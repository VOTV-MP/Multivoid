// coop/props/roomba_sync.cpp -- see coop/props/roomba_sync.h.
//
// prop_roomba_C's graph, from its bytecode (0.9.0n). actionOptionIndex: action 5 flips `active` and calls `use`, which
// runs `upd` (the overlap box's collision and the on/off light), plays the switch sound, `activate` (`move` follows
// `active` after a 1 s delay) and `sound` (the actor tick and the motor loop follow `active`); action 4 opens its
// container while it is off. ReceiveTick, while `active` and `move`: torque on both wheels and the body, a random turn
// when it is slow, the turn's end, and the brush animation's `a`. The bumper's hit picks a turn; the overlap with a
// trash pile spawns the pile's item and files it into the cleaner's own container.

#include "coop/props/roomba_sync.h"

#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_drive_host.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include <chrono>
#include <cmath>
#include <vector>

namespace coop::roomba_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
namespace sg = ue_wrap::script_gate;
namespace EL = coop::element;
namespace N  = coop::net;

UE_FACT_FN(kRowTick, "roomba_sync", L"prop_roomba_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowAction, "roomba_sync", L"prop_roomba_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowPickup, "roomba_sync", L"prop_roomba_C",
           L"BndEvt__prop_roomba_overlap_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);
constexpr int kTagTick = 0x524D4201, kTagAction = 0x524D4202, kTagPickup = 0x524D4203;  // 'RMB' 1..3

constexpr uint8_t  kActionSwitch = 5;      // EInteractionActions: the cleaner's on/off
constexpr float    kReachCm = 600.f;       // a client's press, from its eye to the cleaner's root
constexpr uint64_t kPressEveryMs = 250;    // a slot's presses, one per this
constexpr uint64_t kStateLifeMs = 10000;   // a state for a copy not yet made waits this long

coop::net::Session* g_session = nullptr;
bool g_watched = false;
bool g_settled = false;

bool IsHost() { return g_session && g_session->running() && g_session->role() == N::Role::Host; }
bool IsClient() { return g_session && g_session->running() && g_session->role() == N::Role::Client; }

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

bool ReadActive(void* roomba, bool& out) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!roomba || !R::FindBoolProperty(R::ClassOf(roomba), L"active", off, mask) || off < 0) return false;
    out = (*(static_cast<uint8_t*>(roomba) + off) & mask) != 0;
    return true;
}

void WriteActive(void* roomba, bool on) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!roomba || !R::FindBoolProperty(R::ClassOf(roomba), L"active", off, mask) || off < 0) return;
    uint8_t& b = *(static_cast<uint8_t*>(roomba) + off);
    b = on ? static_cast<uint8_t>(b | mask) : static_cast<uint8_t>(b & ~mask);
}

uint32_t EidOf(void* roomba) {
    const EL::ElementId eid = EL::Registry::Get().EidForActor(roomba);
    return eid == EL::kInvalidId ? 0u : static_cast<uint32_t>(eid);
}

bool IsLiveRoomba(void* obj) { return obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__"); }

template <class F>
void ForEachRoomba(F&& fn) {
    void* const cls = OI::ClassByName(L"prop_roomba_C");
    if (!cls) return;
    std::vector<void*> found;
    OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
        if (IsLiveRoomba(obj)) static_cast<std::vector<void*>*>(ctx)->push_back(obj);
    }, &found);
    for (void* r : found) fn(r);
}

// ---- the host ----------------------------------------------------------------------------------------------------
struct Known {
    ue_wrap::CachedObjRef ref;
    bool active = false;
    bool claimed = false;
};
std::vector<Known> g_known;
struct Press { int slot; N::RoombaPayload p; };
std::vector<Press> g_presses;
uint64_t g_lastPressMs[N::kMaxPeers] = {};
uint64_t g_nextScanMs = 0;

void SendState(void* roomba, bool active, int slot) {
    if (!g_session) return;
    N::RoombaPayload p{};
    p.eid = EidOf(roomba);
    p.active = active ? 1 : 0;
    if (!p.eid) return;
    const bool ok = slot >= 0 ? g_session->SendReliableToSlot(static_cast<uint8_t>(slot), N::ReliableKind::RoombaState,
                                                              &p, sizeof(p))
                              : g_session->SendReliable(N::ReliableKind::RoombaState, &p, sizeof(p));
    UE_LOGI("roomba_sync: HOST cleaner eid %u is %hs -- sent to %hs (ok=%d)", p.eid, active ? "on" : "off",
            slot >= 0 ? "a joiner" : "every client", ok ? 1 : 0);
}

bool Press5(void* roomba) {
    ue_wrap::ParamFrame f(R::FindFunction(R::ClassOf(roomba), L"actionOptionIndex"));
    if (!f.valid()) return false;
    f.Set<uint8_t>(L"action", kActionSwitch);
    return ue_wrap::Call(roomba, f);
}

void JudgePresses() {
    std::vector<Press> presses;
    presses.swap(g_presses);
    for (const Press& pr : presses) {
        void* const roomba = EL::LiveActorOfType(static_cast<EL::ElementId>(pr.p.eid), EL::ElementType::Prop);
        bool active = false;
        if (!roomba || !ReadActive(roomba, active) || R::ClassNameOf(roomba) != L"prop_roomba_C") {
            UE_LOGW("roomba_sync: HOST slot %d pressed eid %u, which is no cleaner here -- refused", pr.slot, pr.p.eid);
            continue;
        }
        coop::RemotePlayer& rp = coop::puppet_drive::Puppet(static_cast<uint8_t>(pr.slot));
        ue_wrap::FVector eye{}, at{};
        if (!rp.valid() || !rp.TryGetEyePosition(eye) || !E::TryGetActorLocation(roomba, at)) continue;
        const float dx = eye.X - at.X, dy = eye.Y - at.Y, dz = eye.Z - at.Z;
        if (dx * dx + dy * dy + dz * dz > kReachCm * kReachCm) {
            UE_LOGW("roomba_sync: HOST slot %d pressed cleaner eid %u from out of reach -- refused", pr.slot, pr.p.eid);
            SendState(roomba, active, pr.slot);
            continue;
        }
        if (active == (pr.p.active != 0)) {
            SendState(roomba, active, pr.slot);  // already so: the asker hears the state it missed
            continue;
        }
        const bool ran = Press5(roomba);
        UE_LOGI("roomba_sync: HOST slot %d switched cleaner eid %u %hs (ran=%d)", pr.slot, pr.p.eid,
                pr.p.active ? "on" : "off", ran ? 1 : 0);
    }
}

Known& KnownOf(void* roomba, bool active) {
    for (Known& k : g_known)
        if (k.ref.Is(roomba)) return k;
    g_known.emplace_back();
    g_known.back().ref.Set(roomba);
    g_known.back().active = active;
    return g_known.back();
}

void TickHost() {
    JudgePresses();
    const uint64_t now = NowMs();
    if (now < g_nextScanMs) return;
    g_nextScanMs = now + 100;
    ForEachRoomba([](void* roomba) {
        bool active = false;
        if (!ReadActive(roomba, active)) return;
        Known& k = KnownOf(roomba, !active);  // a cleaner met for the first time is said once
        if (k.active != active) {
            k.active = active;
            SendState(roomba, active, -1);
        }
        // While it runs the host's copy drives, and its pose rides the drive stream; switched off, it coasts to rest.
        if (active) {
            coop::prop_drive_host::Claim(roomba, "roomba");
            k.claimed = true;
        } else if (k.claimed) {
            coop::prop_drive_host::Release(roomba);
            k.claimed = false;
        }
    });
}

// ---- the client --------------------------------------------------------------------------------------------------
struct Pending { N::RoombaPayload p; uint64_t since; };
std::vector<Pending> g_states;
uint64_t g_lastBrushMs = 0;

bool ApplyState(const N::RoombaPayload& p) {
    void* const roomba = EL::LiveActorOfType(static_cast<EL::ElementId>(p.eid), EL::ElementType::Prop);
    bool active = false;
    if (!roomba || !ReadActive(roomba, active)) return false;
    if (active != (p.active != 0)) {
        WriteActive(roomba, p.active != 0);
        void* const use = R::FindFunction(R::ClassOf(roomba), L"use");
        const bool ran = use && R::CallFunction(roomba, use, nullptr);  // its light, sound, box and motor loop
        UE_LOGI("roomba_sync: CLIENT cleaner eid %u is %hs, as the host's (use ran=%d)", p.eid, p.active ? "on" : "off",
                ran ? 1 : 0);
    }
    return true;
}

// The brushes turn in the cleaner's own tick, which a client refuses: the same `a`, advanced 4 turns a second.
void AnimateBrushes() {
    const uint64_t now = NowMs();
    const float dt = g_lastBrushMs ? static_cast<float>(now - g_lastBrushMs) / 1000.f : 0.f;
    g_lastBrushMs = now;
    if (dt <= 0.f || dt > 1.f) return;
    ForEachRoomba([dt](void* roomba) {
        bool active = false;
        if (!ReadActive(roomba, active) || !active) return;
        const int32_t animOff = R::FindPropertyOffset(R::ClassOf(roomba), L"brushAnim");
        const int32_t alphaOff = R::FindPropertyOffset(R::ClassOf(roomba), L"alpha");
        void* const anim = animOff >= 0 ? *reinterpret_cast<void**>(static_cast<char*>(roomba) + animOff) : nullptr;
        if (!anim || alphaOff < 0 || !R::IsLive(anim)) return;
        float& alpha = *reinterpret_cast<float*>(static_cast<char*>(roomba) + alphaOff);
        alpha = std::fmod(alpha + dt * 4.f, 1.f);
        const int32_t aOff = R::FindPropertyOffset(R::ClassOf(anim), L"a");
        if (aOff >= 0) *reinterpret_cast<float*>(static_cast<char*>(anim) + aOff) = alpha;
    });
}

void TickClient() {
    const uint64_t now = NowMs();
    for (size_t i = 0; i < g_states.size();) {
        const bool done = ApplyState(g_states[i].p);
        if (done || now - g_states[i].since > kStateLifeMs) {
            if (!done) UE_LOGW("roomba_sync: CLIENT cleaner eid %u's state found no copy in 10 s -- dropped", g_states[i].p.eid);
            g_states.erase(g_states.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    AnimateBrushes();
}

// ---- the gate ----------------------------------------------------------------------------------------------------
// The tick and the pickup: a client's copy neither drives nor picks up; the host's does both.
sg::Verdict OnClientRefused(const sg::Call&) { return IsClient() ? sg::Verdict::Cancel : sg::Verdict::Run; }

// The actions: a client's switch press goes to the host, a drill's call as the key makes it included. The client applies
// the host's state through `use`, never through this body.
sg::Verdict OnActionPre(const sg::Call& c) {
    if (!IsClient() || !c.object || !c.function || !c.locals) return sg::Verdict::Run;
    const int32_t off = R::FindParamOffset(c.function, L"action");
    if (off < 0 || c.locals[off] != kActionSwitch) return sg::Verdict::Run;
    bool active = false;
    N::RoombaPayload p{};
    p.eid = EidOf(c.object);
    if (!p.eid || !ReadActive(c.object, active)) return sg::Verdict::Cancel;
    const uint64_t now = NowMs();
    static uint64_t s_lastMs = 0;
    if (now - s_lastMs < kPressEveryMs) return sg::Verdict::Cancel;
    s_lastMs = now;
    p.active = active ? 0 : 1;
    const bool ok = g_session->SendReliable(N::ReliableKind::RoombaIntent, &p, sizeof(p));
    UE_LOGI("roomba_sync: CLIENT pressed cleaner eid %u's switch -- asked the host for %hs (ok=%d)", p.eid,
            p.active ? "on" : "off", ok ? 1 : 0);
    return sg::Verdict::Cancel;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session = session;
    if (g_watched) return;
    g_watched = true;
    const bool t = sg::WatchName(kRowTick, kTagTick, &OnClientRefused, nullptr);
    const bool a = sg::WatchName(kRowAction, kTagAction, &OnActionPre, nullptr);
    const bool p = sg::WatchName(kRowPickup, kTagPickup, &OnClientRefused, nullptr);
    if (!t || !a || !p) UE_LOGE("roomba_sync: a watch was refused (tick=%d action=%d pickup=%d)", t ? 1 : 0, a ? 1 : 0, p ? 1 : 0);
}

void Tick() {
    if (IsHost()) TickHost();
    else if (IsClient()) TickClient();
    if (!g_watched || g_settled) return;
    sg::ResolvePendingNames();
    if (!sg::NameWatchSettled(kRowTick, kTagTick) || !sg::NameWatchSettled(kRowAction, kTagAction) ||
        !sg::NameWatchSettled(kRowPickup, kTagPickup))
        return;
    g_settled = true;
    const bool t = sg::NameWatchLive(kRowTick, kTagTick), a = sg::NameWatchLive(kRowAction, kTagAction),
               p = sg::NameWatchLive(kRowPickup, kTagPickup);
    if (t && a && p) UE_LOGI("roomba_sync: the watches on prop_roomba_C's tick, actions and pickup are live");
    else UE_LOGE("roomba_sync: a watch is dead (tick=%d action=%d pickup=%d) -- a client's cleaner runs its own",
                 t ? 1 : 0, a ? 1 : 0, p ? 1 : 0);
}

void SendJoinSnapshotForSlot(int slot) {
    if (!IsHost() || slot < 0) return;
    ForEachRoomba([slot](void* roomba) {
        bool active = false;
        if (ReadActive(roomba, active)) SendState(roomba, active, slot);
    });
}

void OnIntent(int senderSlot, const coop::net::RoombaPayload& p) {
    if (!IsHost() || senderSlot <= 0 || senderSlot >= static_cast<int>(N::kMaxPeers) || g_presses.size() >= 64) return;
    const uint64_t now = NowMs();
    if (now - g_lastPressMs[senderSlot] < kPressEveryMs) return;
    g_lastPressMs[senderSlot] = now;
    g_presses.push_back({senderSlot, p});
}

void OnState(const coop::net::RoombaPayload& p) {
    if (!IsClient() || g_states.size() >= 64) return;
    g_states.push_back({p, NowMs()});
}

void OnDisconnect() {
    g_known.clear();
    g_presses.clear();
    g_states.clear();
    g_lastBrushMs = 0;
}

}  // namespace coop::roomba_sync
