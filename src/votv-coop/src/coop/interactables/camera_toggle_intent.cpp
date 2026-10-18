// coop/interactables/camera_toggle_intent.cpp -- see coop/interactables/camera_toggle_intent.h.

#include "coop/interactables/camera_toggle_intent.h"

#include "coop/element/intent_authority.h"
#include "coop/interactables/interactable_sync.h"  // the camera lane's key for a camera, and its resolve
#include "coop/net/intent_bucket.h"
#include "coop/net/intent_queue.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"  // PerSlotState
#include "coop/session/net_pump.h"      // IsInAnnouncedWorld: a client's own world load runs natively

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/appliance.h"  // CallAction: a player's interaction dispatch, on any actor
#include "ue_wrap/devices/laptop.h"
#include "ue_wrap/devices/portable_pc.h"
#include "ue_wrap/devices/security_camera.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>

namespace coop::camera_toggle_intent {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace sc = ue_wrap::security_camera;
namespace CT = coop::net::camera_toggle;

// The three bodies a client refuses. Declared on their classes, so another class's function of the name is
// never asked: the camera's use (a subclass that inherits the body is matched), the laptop widget's
// camera-light button, and the cursed camera's own tick.
UE_FACT_FN(kRowUse, "camera_toggle_intent", L"prop_camera_bad_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowLightClick, "camera_toggle_intent", L"ui_laptop_C",
           L"BndEvt__umg_laptop_Button_camFl_K2Node_ComponentBoundEvent_31_OnButtonClickedEvent__DelegateSignature",
           Declared);
UE_FACT_FN(kRowCursedTick, "camera_toggle_intent", L"prop_camera_cursed_C", L"ReceiveTick", Declared);

constexpr int kTagUse   = 0x43545541;  // 'CTUA'
constexpr int kTagLight = 0x43544C43;  // 'CTLC'
constexpr int kTagTick  = 0x43545254;  // 'CTRT'

struct Watch { const ue_wrap::facts::Function* row; int tag; };
const Watch kWatches[] = {
    { &kRowUse,        kTagUse },
    { &kRowLightClick, kTagLight },
    { &kRowCursedTick, kTagTick },
};
constexpr int kWatchCount = static_cast<int>(sizeof(kWatches) / sizeof(kWatches[0]));

// The reach a sender's press is judged against. A player's use reaches as far as the look-at trace, whose length
// is mainPlayer.armLength: 200 by default. Twice the default is the reach; the authority adds the target's own
// bounds radius and a 600 uu allowance for the puppet's pose lagging the sender (coop/element/intent_authority),
// so the bound refuses a camera or a laptop the sender cannot be near, never a reach the game itself allows.
constexpr float kReachUU = 400.0f;

// A sender's presses run from a bounded queue at a bounded rate: a press is a click, so the bound is only felt
// by a stalled or flooding client.
constexpr coop::net::IntentBudget kBudget{4.0f, 4.0f};
constexpr size_t   kMaxPending   = 8;
constexpr uint64_t kRefusalSayMs = 10000;
constexpr uint64_t kClientSayMs  = 60000;  // a key's refusal line, once in this long

std::atomic<coop::net::Session*> g_session{nullptr};
// Each watch registers once: a refusal (a full gate table) is final, and said once. The set settles once every
// watch is live, or once no name is left to resolve and the rest are dead for good.
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg[kWatchCount] = {};
bool g_settled = false;

uint64_t g_sent = 0, g_ran = 0, g_converged = 0, g_denied = 0;
uint64_t g_tickRefused = 0, g_unindexed = 0, g_loadNative = 0;
bool     g_tickSaid = false;
bool     g_unknownSaid = false;  // HOST: a request for a key no camera answers to, said once a session
uint64_t g_nextUnindexedSayMs = 0;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

const char* WhatOf(uint8_t bit) { return bit == CT::kActive ? "toggle" : "light"; }
const char* BitName(uint8_t bit) { return bit == CT::kActive ? "active" : "isLight"; }

bool ReadBit(void* cam, uint8_t bit, bool& on) {
    return bit == CT::kActive ? sc::ReadActive(cam, on) : sc::ReadLight(cam, on);
}

// ---- host side -------------------------------------------------------------------------------
// One sender: its queued presses with their bucket, and the latches that clear with its occupant.
struct Slot {
    coop::net::IntentQueue<coop::net::CameraToggleIntentPayload> queue;
    uint64_t nextSayMs = 0;      // the full queue's refusal, said once in kRefusalSayMs
    bool     waitSaid  = false;  // a wait for the body, said once a streak
};
coop::roster_ledger::PerSlotState<Slot> g_slots;

// The camera's state for the requester alone: a request that changed nothing still answers, so a client whose
// copy diverged converges and its button is never dead.
void SendStateTo(coop::net::Session& s, uint8_t slot, const coop::net::WireKey& key, uint8_t bit, bool on) {
    coop::net::KeyedTogglePayload kp{};
    kp.key = key;
    kp.action = on ? 1 : 0;
    s.SendReliableToSlot(slot, bit == CT::kActive ? coop::net::ReliableKind::CameraActiveState
                                                  : coop::net::ReliableKind::CameraLightState,
                         &kp, sizeof(kp));
}

void Deny(uint8_t slot, const coop::net::CameraToggleIntentPayload& p, const std::wstring& key, const char* why) {
    ++g_denied;
    if (g_denied <= 3 || g_denied % 20 == 0)
        UE_LOGI("[CAMERA-INTENT] DENY slot=%u %s key='%ls' -- %s (#%llu)", static_cast<unsigned>(slot), WhatOf(p.bit),
                key.c_str(), why, static_cast<unsigned long long>(g_denied));
}

// The devices whose screen shows the laptop interface widget: the base laptop and every portable PC (the PC is a
// remote terminal to the same widget), the default object left out.
struct Screens { void* v[8]; int n = 0; };

void CollectScreens(Screens& out) {
    ue_wrap::laptop::EnsureResolved();
    if (void* l = ue_wrap::laptop::Instance()) out.v[out.n++] = l;
    ue_wrap::portable_pc::ForEachPc(
        [](void* ctx, void* pc) {
            auto* o = static_cast<Screens*>(ctx);
            if (o->n < 8 && !R::NameStartsWith(R::NameOf(pc), L"Default__")) o->v[o->n++] = pc;
        },
        &out);
}

// The verdict on a request's reach. The use is judged against the camera. The light button is on a screen, and the
// payload names none, so it is judged against every screen device: the first one the sender is within reach of
// passes; else the verdict is the nearest one's (a NoBody on any wins, as the sender then has no pose to measure).
// `screens` is how many devices were asked (0 for the use).
coop::element::IntentSubject Judge(coop::net::Session& s, uint8_t slot, uint8_t bit, void* cam, int& screens) {
    const coop::element::IntentTarget target = coop::element::IntentTarget::ForClientIntent(s, slot, kReachUU);
    screens = 0;
    if (bit != CT::kLight) return target.Authorize(cam);
    Screens devices;
    CollectScreens(devices);
    screens = devices.n;
    coop::element::IntentSubject best;
    bool have = false;
    for (int i = 0; i < devices.n; ++i) {
        const coop::element::IntentSubject sub = target.Authorize(devices.v[i]);
        if (sub || sub.outcome == coop::element::IntentOutcome::NoBody) return sub;
        if (!have || (sub.distUU >= 0.f && (best.distUU < 0.f || sub.distUU < best.distUU))) { best = sub; have = true; }
    }
    return best;
}

// The host's per-request INFO lines are said for a key once a minute, with the number that followed it, so a client
// pressing at its bucket's rate writes one line a minute for a camera and not four a second. `kind` tells the
// lines of one key apart. Cameras the lane indexes are the only keys that reach it, so the map is as small as they are.
struct HostSaid { uint64_t nextMs = 0; uint64_t since = 0; };
std::unordered_map<std::wstring, HostSaid> g_hostSaid;

bool MaySay(const std::wstring& key, wchar_t kind, uint64_t& since) {
    HostSaid& sd = g_hostSaid[key + L'#' + kind];
    const uint64_t now = NowMs();
    if (now < sd.nextMs) { ++sd.since; return false; }
    since = sd.since;
    sd.nextMs = now + kClientSayMs;
    sd.since = 0;
    return true;
}

// The tail a line carries when lines of its key were held back since the last one.
std::string More(uint64_t since) {
    return since ? " (" + std::to_string(since) + " more since the last line)" : std::string();
}

// False keeps the request at its sender's queue head: the host has no body for the sender yet (its first pose
// has not arrived), so its reach cannot be measured, and the request waits for the body rather than being lost.
// Everything else is run or refused, and consumed.
bool Execute(coop::net::Session& s, const coop::net::CameraToggleIntentPayload& p, uint8_t slot) {
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    void* cam = key.empty() ? nullptr : coop::interactable_sync::ResolveCamera(key);
    if (!cam) {
        // An unknown key is counted, and said once a session: a client can name any key it likes.
        if (!g_unknownSaid) {
            g_unknownSaid = true;
            Deny(slot, p, key, "the camera lane indexes no camera by that key (said once a session)");
        } else {
            ++g_denied;
        }
        return true;
    }
    int screens = 0;
    const coop::element::IntentSubject subject = Judge(s, slot, p.bit, cam, screens);
    if (p.bit == CT::kLight && screens == 0) {
        Deny(slot, p, key, "no device in this world shows the laptop interface to measure the light button from");
        return true;
    }
    if (subject.outcome == coop::element::IntentOutcome::NoBody) {
        if (!g_slots[slot].waitSaid) {
            g_slots[slot].waitSaid = true;
            UE_LOGI("[CAMERA-INTENT] slot %u's camera requests wait: the host has no body for it yet",
                    static_cast<unsigned>(slot));
        }
        return false;
    }
    if (!subject) {
        char why[96];
        std::snprintf(why, sizeof why, "%s (%.0f uu of %.0f%s)", coop::element::OutcomeName(subject.outcome),
                      subject.distUU, subject.reachUU, screens ? ", the nearest screen device" : "");
        Deny(slot, p, key, why);
        return true;
    }
    bool before = false;
    if (!ReadBit(cam, p.bit, before)) {
        Deny(slot, p, key, "the camera's bit did not read");
        return true;
    }
    const bool desired = p.desired != 0;
    if (before == desired) {
        ++g_converged;
        SendStateTo(s, slot, p.key, p.bit, before);
        uint64_t since = 0;
        if (MaySay(key, L'n', since))
            UE_LOGI("[CAMERA-INTENT] slot %u's %s on key='%ls' changed nothing: %s is already %d; its state is sent "
                    "to that slot alone%s", static_cast<unsigned>(slot), WhatOf(p.bit),
                    key.c_str(), BitName(p.bit), before ? 1 : 0, More(since).c_str());
        return true;
    }
    // The body Authorize just measured is the sender's puppet, passed as the player of the use. The camera's
    // own body reads only the action.
    bool dispatched = false;
    if (p.bit == CT::kActive) {
        coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
        void* body = (rp && rp->valid()) ? rp->GetActor() : nullptr;
        dispatched = ue_wrap::appliance::CallAction(cam, body, sc::kUseAction);
    } else {
        // What the widget's button does without the widget: write the bit, then upd().
        dispatched = sc::WriteLight(cam, desired) && sc::CallUpd(cam);
    }
    bool after = false;
    const bool readAfter = ReadBit(cam, p.bit, after);
    if (dispatched) ++g_ran; else ++g_denied;
    // The host's own open laptop panel shows the camera's bits too, and the verbs above never reach its updCam.
    if (dispatched) sc::RefreshLaptopsShowing(cam);
    uint64_t since = 0;
    if (MaySay(key, L'r', since))
        UE_LOGI("[CAMERA-INTENT] host ran slot %u's %s on key='%ls': dispatched=%d, %s %d -> %s%s",
                static_cast<unsigned>(slot), WhatOf(p.bit), key.c_str(), dispatched ? 1 : 0, BitName(p.bit),
                before ? 1 : 0, readAfter ? (after ? "1" : "0") : "(unread)", More(since).c_str());
    return true;
}

// ---- the client's gate -----------------------------------------------------------------------
// A key's refusal is said once in kClientSayMs, with the number that followed it; the rest are counted.
struct Said { uint64_t nextMs = 0; uint64_t since = 0; };
std::unordered_map<std::wstring, Said> g_said;

void SayRefusal(const std::wstring& key, uint8_t bit) {
    Said& sd = g_said[key + (bit == CT::kActive ? L"#t" : L"#l")];
    const uint64_t now = NowMs();
    if (now < sd.nextMs) { ++sd.since; return; }
    if (sd.since)
        UE_LOGI("camera: refused a client's own %s on key=%ls, sent as an intent (%llu more since the last line)",
                WhatOf(bit), key.c_str(), static_cast<unsigned long long>(sd.since));
    else
        UE_LOGI("camera: refused a client's own %s on key=%ls, sent as an intent", WhatOf(bit), key.c_str());
    sd.nextMs = now + kClientSayMs;
    sd.since = 0;
}

// Refuses a client's own press of `bit` on `cam` and sends the value that negates its copy to the host. A
// camera the lane does not index has no name the host could resolve: the press is refused and nothing is sent,
// said once a minute, since to the player it is a dead button on one camera.
sg::Verdict Refuse(coop::net::Session& s, void* cam, uint8_t bit) {
    bool cur = false;
    if (!ReadBit(cam, bit, cur)) return sg::Verdict::Run;  // the class did not resolve (said by its wrapper): native
    const std::wstring key = coop::interactable_sync::CameraKey(cam);
    if (key.empty()) {
        ++g_unindexed;
        const uint64_t now = NowMs();
        if (now >= g_nextUnindexedSayMs) {
            g_nextUnindexedSayMs = now + kClientSayMs;
            UE_LOGW("camera: refused a client's own %s on camera %p, which the camera lane does not index (%s) "
                    "-- nothing sent", WhatOf(bit), cam,
                    sc::IsCamera(cam) ? "its key is not in the index yet" : "it has no key of its own");
        }
        return sg::Verdict::Cancel;
    }
    coop::net::CameraToggleIntentPayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.bit = bit;
    p.desired = cur ? 0 : 1;
    if (!s.SendReliable(coop::net::ReliableKind::CameraToggleIntent, &p, sizeof(p))) {
        UE_LOGW("camera: the client's %s on key=%ls was not sent (the session refused it); the camera stays as "
                "the host has it", WhatOf(bit), key.c_str());
        return sg::Verdict::Cancel;
    }
    ++g_sent;
    SayRefusal(key, bit);
    return sg::Verdict::Cancel;
}

sg::Verdict OnPre(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Client || !call.object) return sg::Verdict::Run;
    if (call.tag == kTagTick) {
        // The cursed camera rolls its night flicker with a per-process RNG: the host rolls it, and the result
        // crosses as CameraActiveState. A call on an object of a world this client has not announced ready is
        // that world's load and runs natively. A cursed camera the lane does not index (a key it never took of
        // its own) is no instance of either camera channel, and stays this peer's own.
        if (!coop::net_pump::IsInAnnouncedWorld(call.object)) { ++g_loadNative; return sg::Verdict::Run; }
        if (coop::interactable_sync::CameraKey(call.object).empty()) return sg::Verdict::Run;
        ++g_tickRefused;
        if (!g_tickSaid) {
            g_tickSaid = true;
            UE_LOGI("camera: a client's cursed camera tick is refused -- the host rolls it (said once a session; "
                    "the rest are counted)");
        }
        return sg::Verdict::Cancel;
    }
    if (call.tag == kTagUse) {
        static R::ParamOffset s_action{L"action"};
        const int32_t off = s_action.Of(call.function);
        if (off < 0 || *(call.locals + off) != sc::kUseAction) return sg::Verdict::Run;
        if (!coop::net_pump::IsInAnnouncedWorld(call.object)) { ++g_loadNative; return sg::Verdict::Run; }
        return Refuse(*s, call.object, CT::kActive);
    }
    // The laptop's camera-light button: the camera is the widget's selected one. With none selected the button
    // only plays its sound and repaints, which runs natively.
    void* cam = sc::LaptopCamOf(call.object);
    if (!cam) return sg::Verdict::Run;
    if (!coop::net_pump::IsInAnnouncedWorld(cam)) { ++g_loadNative; return sg::Verdict::Run; }
    return Refuse(*s, cam, CT::kLight);
}

void RegisterWatches() {
    for (int i = 0; i < kWatchCount; ++i) {
        if (g_reg[i] != Reg::Pending) continue;
        if (sg::WatchName(*kWatches[i].row, kWatches[i].tag, &OnPre, nullptr)) {
            g_reg[i] = Reg::Registered;
            continue;
        }
        g_reg[i] = Reg::Refused;
        UE_LOGE("[CAMERA-INTENT] the script-body gate refused the watch on '%ls' -- for the rest of this process "
                "that camera press runs only where it is made", kWatches[i].row->Name());
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    RegisterWatches();
}

void Tick(coop::net::Session& session) {
    sg::ResolvePendingNames();
    if (!g_settled) {
        int live = 0, pending = 0;
        for (int i = 0; i < kWatchCount; ++i) {
            if (g_reg[i] == Reg::Pending) ++pending;
            else if (g_reg[i] == Reg::Registered && sg::NameWatchLive(*kWatches[i].row, kWatches[i].tag)) ++live;
        }
        if (live == kWatchCount) {
            g_settled = true;
            UE_LOGI("[CAMERA-INTENT] the camera press gates are live: use, light button and cursed tick");
        } else if (pending == 0 && sg::PendingNameCount() == 0) {
            g_settled = true;
            UE_LOGE("[CAMERA-INTENT] %d of %d camera press gates are dead (refused, or resolved into a full table) "
                    "-- those presses run only where they are made", kWatchCount - live, kWatchCount);
        }
    }
    if (!session.running() || session.role() != coop::net::Role::Host) return;
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        Slot& sl = g_slots[slot];
        sl.queue.Serve(kBudget, NowMs, [&](const coop::net::CameraToggleIntentPayload& p) {
            if (!Execute(session, p, slot)) return coop::net::Served::Wait;  // a wait runs nothing: no token spent
            sl.waitSaid = false;  // a consumed request ends the wait's streak
            return coop::net::Served::Done;
        });
    }
}

void OnCameraToggleIntent(coop::net::Session& session, const coop::net::CameraToggleIntentPayload& payload,
                          uint8_t senderSlot) {
    if (session.role() != coop::net::Role::Host) return;
    if (senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    Slot& sl = g_slots[senderSlot];
    auto payloadCopy = payload;
    if (!sl.queue.Push(std::move(payloadCopy), kMaxPending)) {
        const uint64_t now = NowMs();
        if (now >= sl.nextSayMs) {
            sl.nextSayMs = now + kRefusalSayMs;
            UE_LOGW("[CAMERA-INTENT] slot %u queue full (%zu) -- refusing its camera requests until it drains",
                    static_cast<unsigned>(senderSlot), sl.queue.size());
        }
        ++g_denied;
    }
}

uint64_t SentCount() { return g_sent; }

void OnDisconnect() {
    if (g_sent || g_ran || g_converged || g_denied || g_tickRefused || g_unindexed || g_loadNative)
        UE_LOGI("[CAMERA-INTENT] session end -- sent=%llu ran=%llu changed-nothing=%llu denied=%llu cursed ticks "
                "refused=%llu unindexed refused=%llu own load run natively=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_ran),
                static_cast<unsigned long long>(g_converged), static_cast<unsigned long long>(g_denied),
                static_cast<unsigned long long>(g_tickRefused), static_cast<unsigned long long>(g_unindexed),
                static_cast<unsigned long long>(g_loadNative));
    g_sent = g_ran = g_converged = g_denied = g_tickRefused = g_unindexed = g_loadNative = 0;
    g_tickSaid = false;
    g_unknownSaid = false;
    g_nextUnindexedSayMs = 0;
    g_said.clear();
    g_hostSaid.clear();
}

}  // namespace coop::camera_toggle_intent
