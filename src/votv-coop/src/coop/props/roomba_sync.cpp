// coop/props/roomba_sync.cpp -- see coop/props/roomba_sync.h.
//
// prop_roomba_C's graph, from its bytecode (0.9.0n). actionOptionIndex: action 5 flips `active` and calls `use`, which
// runs `upd` (the overlap box's collision and the on/off light), plays the switch sound, `activate` (`move` follows
// `active` after a 1 s delay) and `sound` (the actor tick and the motor loop follow `active`); action 4 opens its
// container while it is off. `loadData` sets `active` from the save and calls `use` too, so `use` is the one place
// every change of `active` passes. ReceiveTick, while `active` and `move`: torque on both wheels and the body, a random
// turn when it is slow, the turn's end, and the brush animation's `a`. The bumper's hit picks a turn; the overlap with a
// trash pile spawns the pile's item and files it into the cleaner's own container.

#include "coop/props/roomba_sync.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_drive_host.h"
#include "coop/props/prop_element_tracker.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <mutex>
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
UE_FACT_FN(kRowUse, "roomba_sync", L"prop_roomba_C", L"use", Declared);
constexpr int kTagTick = 0x524D4201, kTagAction = 0x524D4202, kTagPickup = 0x524D4203, kTagUse = 0x524D4204;  // 'RMB' 1..4

constexpr uint8_t  kActionSwitch = 5;      // EInteractionActions: the cleaner's on/off
constexpr float    kReachCm = 600.f;       // a client's press, from its eye to the cleaner's root
constexpr uint64_t kPressEveryMs = 250;    // a slot's presses, one per this
constexpr size_t   kMaxHeld = 64;          // states held for copies not yet bound, one per cleaner

coop::net::Session* g_session = nullptr;
bool g_watched = false;
bool g_settled = false;

bool IsHost() { return g_session && g_session->running() && g_session->role() == N::Role::Host; }
bool IsClient() { return g_session && g_session->running() && g_session->role() == N::Role::Client; }

// [dev red] roomba_intent_off (client): a press of the switch is never asked of the host.
bool IntentOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::roomba_intent_off);
    return off;
}
// [dev red] roomba_drive_claim_off (host): a running cleaner's drive is never claimed, so no copy follows its pose.
bool DriveClaimOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::roomba_drive_claim_off);
    return off;
}

std::atomic<uint32_t> g_statesSent{0}, g_intentsSwitched{0}, g_statesReceived{0}, g_statesApplied{0}, g_intentsSent{0};
std::atomic<uint32_t> g_heldCount{0}, g_lastStateEid{0};

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

// A joiner's snapshot is the one scan: nothing announces what a joiner missed.
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

// True when the state was sent; a cleaner with no element id yet is not.
bool SendState(void* roomba, bool active, int slot) {
    if (!g_session) return false;
    N::RoombaPayload p{};
    p.eid = EidOf(roomba);
    p.active = active ? 1 : 0;
    if (!p.eid) return false;
    const bool ok = slot >= 0 ? g_session->SendReliableToSlot(static_cast<uint8_t>(slot), N::ReliableKind::RoombaState,
                                                              &p, sizeof(p))
                              : g_session->SendReliable(N::ReliableKind::RoombaState, &p, sizeof(p));
    if (ok) g_statesSent.fetch_add(1);
    UE_LOGI("roomba_sync: HOST cleaner eid %u is %hs -- sent to %hs (ok=%d)", p.eid, active ? "on" : "off",
            slot >= 0 ? "a joiner" : "every client", ok ? 1 : 0);
    return ok;
}

bool Press5(void* roomba) {
    ue_wrap::ParamFrame f(R::FindFunction(R::ClassOf(roomba), L"actionOptionIndex"));
    if (!f.valid()) return false;
    f.Set<uint8_t>(L"action", kActionSwitch);
    return ue_wrap::Call(roomba, f);
}

void JudgePresses() {
    if (g_presses.empty()) return;
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
        const bool ran = Press5(roomba);  // `use` runs, and its watch publishes the new state
        if (ran) g_intentsSwitched.fetch_add(1);
        UE_LOGI("roomba_sync: HOST slot %d switched cleaner eid %u %hs (ran=%d)", pr.slot, pr.p.eid,
                pr.p.active ? "on" : "off", ran ? 1 : 0);
    }
}

Known* FindKnown(void* roomba) {
    for (Known& k : g_known)
        if (k.ref.Is(roomba)) return &k;
    return nullptr;
}

// While it runs the host's copy drives, and its pose rides the drive stream; switched off, it coasts to rest.
void Drive(void* roomba, bool active, Known& k) {
    if (active && !DriveClaimOff()) {
        coop::prop_drive_host::Claim(roomba, "roomba");
        k.claimed = true;
    } else if (!active && k.claimed) {
        coop::prop_drive_host::Release(roomba);
        k.claimed = false;
    }
}

// A cleaner met for the first time (its element id was just minted, by a spawn or the save's census): its `active` as it
// stands goes out, and a running one is claimed. A switch before this point is covered by it.
void Announce(void* roomba) {
    bool active = false;
    if (!IsHost() || !ReadActive(roomba, active)) return;
    Known* k = FindKnown(roomba);
    if (!k) {
        for (size_t i = 0; i < g_known.size();)
            if (!g_known[i].ref.Alive()) g_known.erase(g_known.begin() + static_cast<std::ptrdiff_t>(i));
            else ++i;
        g_known.emplace_back();
        g_known.back().ref.Set(roomba);
        k = &g_known.back();
    }
    k->active = active;
    if (g_session->connected()) SendState(roomba, active, -1);
    Drive(roomba, active, *k);
}

// ---- the enrol seam (any thread): the host's prop births, queued for the game thread ------------------------------------
constexpr size_t kBornCap = 16;
struct Born { void* actor; EL::ElementId eid; };
std::mutex g_bornMutex;
Born g_born[kBornCap];
size_t g_bornCount = 0;
std::atomic<size_t> g_bornPending{0};
std::atomic<bool> g_bornFullSaid{false};
std::atomic<void*> g_roombaClass{nullptr};

// The cleaner's class, asked again when the class set or the unlisted classes move.
void RefreshClass() {
    static uint64_t s_set = ~0ull, s_unlist = ~0ull;
    const uint64_t set = OI::ClassSetVersion(), unlist = OI::ClassUnlistVersion();
    if (set == s_set && unlist == s_unlist) return;
    s_set = set;
    s_unlist = unlist;
    g_roombaClass.store(OI::ClassByName(L"prop_roomba_C"), std::memory_order_release);
}

// MarkPropElement's callback, any thread: a cleaner is recognised by its class pointer and queued.
void OnExpressed(void* actor, EL::ElementId eid) {
    void* const cls = g_roombaClass.load(std::memory_order_acquire);
    if (!cls || !actor || R::ClassOf(actor) != cls) return;
    bool full = false;
    {
        std::lock_guard<std::mutex> lk(g_bornMutex);
        if (g_bornCount >= kBornCap) {
            full = true;
        } else {
            g_born[g_bornCount++] = Born{actor, eid};
            g_bornPending.store(g_bornCount, std::memory_order_release);
        }
    }
    if (full && !g_bornFullSaid.exchange(true))
        UE_LOGW("roomba_sync: the birth queue is full (%zu); a cleaner is announced at the next joiner", kBornCap);
}

void DrainBorn() {
    if (g_bornPending.load(std::memory_order_acquire) == 0) return;
    Born taken[kBornCap];
    size_t n = 0;
    {
        std::lock_guard<std::mutex> lk(g_bornMutex);
        for (; n < g_bornCount; ++n) taken[n] = g_born[n];
        g_bornCount = 0;
        g_bornPending.store(0, std::memory_order_release);
    }
    for (size_t i = 0; i < n; ++i)
        if (EL::Registry::Get().EidForActor(taken[i].actor) == taken[i].eid && R::IsLive(taken[i].actor)) Announce(taken[i].actor);
}

void TickHost() {
    RefreshClass();
    DrainBorn();
    JudgePresses();
}

// `use` ran (the press and the save's load both end there): the body has written the new `active`. A cleaner with no
// element id yet is announced at its birth, saying what it reads then; one enrolled before the lane's subscription is
// met here, at its first switch.
void OnUsePost(const sg::Call& c) {
    if (!IsHost() || !c.object) return;
    Known* k = FindKnown(c.object);
    if (!k) {
        if (EidOf(c.object)) Announce(c.object);
        return;
    }
    bool active = false;
    if (!ReadActive(c.object, active) || active == k->active) return;
    k->active = active;
    if (g_session->connected()) SendState(c.object, active, -1);
    Drive(c.object, active, *k);
}

// ---- the client --------------------------------------------------------------------------------------------------
// The copies this client has bound, filled at the bind (OnBound) and dead with the object or the world.
std::vector<ue_wrap::CachedObjRef> g_cleaners;
// The newest state per cleaner whose copy is not bound yet, applied at its bind and released with the world.
std::vector<N::RoombaPayload> g_held;
bool g_heldFullSaid = false;
uint64_t g_lastBrushMs = 0;

bool ApplyTo(void* roomba, const N::RoombaPayload& p) {
    bool active = false;
    if (!ReadActive(roomba, active)) return false;
    if (active != (p.active != 0)) {
        WriteActive(roomba, p.active != 0);
        void* const use = R::FindFunction(R::ClassOf(roomba), L"use");
        const bool ran = use && R::CallFunction(roomba, use, nullptr);  // its light, sound, box and motor loop
        g_statesApplied.fetch_add(1);
        UE_LOGI("roomba_sync: CLIENT cleaner eid %u is %hs, as the host's (use ran=%d)", p.eid, p.active ? "on" : "off",
                ran ? 1 : 0);
    }
    return true;
}

// The brushes turn in the cleaner's own tick, which a client refuses, and nothing announces the turn: the same `a`,
// advanced 4 turns a second, for each bound copy that runs.
void AnimateBrushes() {
    const uint64_t now = NowMs();
    const float dt = g_lastBrushMs ? static_cast<float>(now - g_lastBrushMs) / 1000.f : 0.f;
    g_lastBrushMs = now;
    if (dt <= 0.f || dt > 1.f || g_cleaners.empty()) return;
    static R::InstanceOffset s_brushAnim(L"brushAnim"), s_alpha(L"alpha"), s_a(L"a");
    for (size_t i = 0; i < g_cleaners.size();) {
        if (!g_cleaners[i].Alive()) {
            g_cleaners.erase(g_cleaners.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        void* const roomba = g_cleaners[i++].Get();
        bool active = false;
        if (!roomba || !ReadActive(roomba, active) || !active) continue;
        const int32_t animOff = s_brushAnim.Of(roomba), alphaOff = s_alpha.Of(roomba);
        void* const anim = animOff >= 0 ? *reinterpret_cast<void**>(static_cast<char*>(roomba) + animOff) : nullptr;
        if (!anim || alphaOff < 0 || !R::IsLive(anim)) continue;
        float& alpha = *reinterpret_cast<float*>(static_cast<char*>(roomba) + alphaOff);
        alpha = std::fmod(alpha + dt * 4.f, 1.f);
        const int32_t aOff = s_a.Of(anim);
        if (aOff >= 0) *reinterpret_cast<float*>(static_cast<char*>(anim) + aOff) = alpha;
    }
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
    if (IntentOff()) {
        UE_LOGW("roomba_sync: [dev red] roomba_intent_off -- cleaner eid %u's press is NOT asked of the host", p.eid);
        return sg::Verdict::Cancel;
    }
    const bool ok = g_session->SendReliable(N::ReliableKind::RoombaIntent, &p, sizeof(p));
    if (ok) g_intentsSent.fetch_add(1);
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
    const bool u = sg::WatchName(kRowUse, kTagUse, nullptr, &OnUsePost);
    if (!t || !a || !p || !u)
        UE_LOGE("roomba_sync: a watch was refused (tick=%d action=%d pickup=%d use=%d)", t ? 1 : 0, a ? 1 : 0, p ? 1 : 0, u ? 1 : 0);
    if (!coop::prop_element_tracker::SubscribeHostPropExpressed(&OnExpressed))
        UE_LOGE("roomba_sync: the prop lane's subscriber table is full -- the host's cleaners are not announced at their birth");
}

void Tick() {
    if (IsHost()) TickHost();
    else if (IsClient()) AnimateBrushes();
    if (!g_watched || g_settled) return;
    sg::ResolvePendingNames();
    if (!sg::NameWatchSettled(kRowTick, kTagTick) || !sg::NameWatchSettled(kRowAction, kTagAction) ||
        !sg::NameWatchSettled(kRowPickup, kTagPickup) || !sg::NameWatchSettled(kRowUse, kTagUse))
        return;
    g_settled = true;
    const bool t = sg::NameWatchLive(kRowTick, kTagTick), a = sg::NameWatchLive(kRowAction, kTagAction),
               p = sg::NameWatchLive(kRowPickup, kTagPickup), u = sg::NameWatchLive(kRowUse, kTagUse);
    if (t && a && p && u) UE_LOGI("roomba_sync: the watches on prop_roomba_C's tick, actions, pickup and use are live");
    else UE_LOGE("roomba_sync: a watch is dead (tick=%d action=%d pickup=%d use=%d) -- a client's cleaner runs its own, "
                 "the host publishes no switch", t ? 1 : 0, a ? 1 : 0, p ? 1 : 0, u ? 1 : 0);
}

void SendJoinSnapshotForSlot(int slot) {
    if (!IsHost() || slot < 0) return;
    ForEachRoomba([slot](void* roomba) {
        if (!FindKnown(roomba)) {  // never met (enrolled before the lane's subscription): met now, its running drive claimed
            Announce(roomba);
            return;
        }
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
    if (!IsClient()) return;
    g_statesReceived.fetch_add(1);
    g_lastStateEid.store(p.eid);
    void* const roomba = EL::LiveActorOfType(static_cast<EL::ElementId>(p.eid), EL::ElementType::Prop);
    if (roomba && ApplyTo(roomba, p)) return;
    // Its copy is not bound yet: the newest state per cleaner waits for the bind.
    for (N::RoombaPayload& h : g_held)
        if (h.eid == p.eid) {
            h = p;
            return;
        }
    if (g_held.size() >= kMaxHeld) {
        if (!g_heldFullSaid) {
            g_heldFullSaid = true;
            UE_LOGW("roomba_sync: CLIENT %zu cleaners' states are held for copies not bound -- cleaner eid %u's is dropped", kMaxHeld, p.eid);
        }
        return;
    }
    g_held.push_back(p);
    g_heldCount.store(static_cast<uint32_t>(g_held.size()));
}

void OnBound(uint32_t eid, void* actor, const wchar_t* cls) {
    if (!IsClient() || !actor || !cls || _wcsicmp(cls, L"prop_roomba_C") != 0) return;
    bool listed = false;
    for (const ue_wrap::CachedObjRef& r : g_cleaners) listed = listed || r.Is(actor);
    if (!listed) {
        g_cleaners.emplace_back();
        g_cleaners.back().Set(actor);
    }
    for (size_t i = 0; i < g_held.size(); ++i) {
        if (g_held[i].eid != eid) continue;
        const N::RoombaPayload held = g_held[i];
        g_held.erase(g_held.begin() + static_cast<std::ptrdiff_t>(i));
        g_heldCount.store(static_cast<uint32_t>(g_held.size()));
        ApplyTo(actor, held);
        break;
    }
}

void OnClientWorldReady() {
    g_cleaners.clear();
    g_held.clear();
    g_heldCount.store(0);
    g_heldFullSaid = false;
}

void OnDisconnect() {
    OnClientWorldReady();
    g_known.clear();
    g_presses.clear();
    g_lastBrushMs = 0;
}

Counters Read() {
    return {g_statesSent.load(), g_intentsSwitched.load(), g_statesReceived.load(), g_statesApplied.load(),
            g_intentsSent.load(), g_heldCount.load(), g_lastStateEid.load()};
}

}  // namespace coop::roomba_sync
