// coop/props/fireext_sync.cpp -- see coop/props/fireext_sync.h.
//
// prop_fireExt_C's graph, from its bytecode (0.9.0n). playerHandUse_LMB and playerHandRelease_LMB call `fire` with true
// and false. `fire(firing)`: a spray asked for with no charge or while thrusting returns; else `firing` is written,
// eff_ext is set active by it, the actor's tick is enabled by it, and `fireSound` turns the fireExtFire sound on (its
// timeline fading it in) or fades it out. ReceiveTick, while `firing` and not `active`: eff_ext turned to player 0's
// camera, `life` drained by delta / 60 (a spray off at 0), eff_fireExtThr scaled by lerp(0.1, 1, life), and
// `extinguishRay` from the camera to 750 cm along it plus up to 100 cm at random, radius 5-10 cm: a sphere trace that
// calls extinguishFire on every int_objects within 20-30 cm of the hit. addDamage over 5, fireDamage and
// receivedPhyiscsDamage, while not `active`, run `shoot`: `fire(false)`, physicsThruster, eff_fireExtThr and the
// fireExtThrust sound on, `active` true, then a loop each frame draining `life`, scaling the thrust by it and casting
// rays from the nozzle, until `life` is 0, when the three go off. getData and loadData keep `life` and `active`; a load
// with both runs `shoot` again.

#include "coop/props/fireext_sync.h"

#include "fireext_internal.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/fireext_thrust_sync.h"
#include "ue_wrap/actors/fire_extinguisher.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace coop::fireext_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
namespace sg = ue_wrap::script_gate;
namespace CC = ue_wrap::component_calls;
namespace FX = ue_wrap::fire_extinguisher;
namespace P  = ue_wrap::profile;
namespace EL = coop::element;
namespace N  = coop::net;

// The game's own hold change is this player's own hand's end: updateHold rebuilds the hand item (a pick, a drop, a
// quick-slot switch) and the extinguisher the spray was on is gone.
UE_FACT_FN(kRowFire, "fireext_sync", L"prop_fireExt_C", L"fire", Declared);
UE_FACT_FN(kRowUpdateHold, "fireext_sync", L"mainPlayer_C", L"updateHold", Declared);
constexpr int kTagFire = 0x46455801, kTagHold = 0x46455804;  // 'FEX' 1, 4

constexpr const wchar_t* kClass = L"prop_fireExt_C";
constexpr float    kRayLengthCm = 750.f;     // the tick's ray, from the eye along the aim
constexpr float    kRayScatterCm = 100.f;    // plus up to this, at random
constexpr uint64_t kChargeEveryMs = 500;     // a spray's charge, at most once per this while it runs

// A client's "on" of the spray: 8 at once, 4 a second. A first guess, read from the tallies printed at each disconnect.
constexpr N::IntentBudget kSprayBudget{8.0f, 4.0f};

coop::net::Session* g_session = nullptr;
bool g_watched = false;
bool g_settled = false;

}  // namespace

namespace detail {
namespace {
coop::roster_ledger::PerSlotState<SlotLedger> g_ledger;
}  // namespace

SlotLedger& Ledger(int slot) { return g_ledger[slot]; }
coop::net::Session* SessionPtr() { return g_session; }
bool IsHost() { return g_session && g_session->running() && g_session->role() == N::Role::Host; }
bool IsClient() { return g_session && g_session->running() && g_session->role() == N::Role::Client; }

}  // namespace detail

namespace {

using detail::IsClient;
using detail::IsHost;
using detail::Ledger;

// The red arms of the foam drills, each resolved once at its site: no peer casts a hand mirror's ray, and the join snapshot
// sends neither sprays nor thrusts.
bool MirrorRayOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::fireext_mirror_ray_off);
    return off;
}
bool SnapshotOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::fireext_snapshot_off);
    return off;
}

uint64_t NowMs() { return coop::net::NowMs(); }

float Lerp(float a, float b, float t) { return a + (b - a) * t; }

uint32_t EidOf(void* ext) {
    const EL::ElementId eid = EL::Registry::Get().EidForActor(ext);
    return eid == EL::kInvalidId ? 0u : static_cast<uint32_t>(eid);
}

void* ExtinguisherOf(uint32_t eid) {
    void* const ext = EL::LiveActorOfType(static_cast<EL::ElementId>(eid), EL::ElementType::Prop);
    return FX::IsExtinguisher(ext) ? ext : nullptr;
}

uint8_t OwnSlot() { return coop::players::Registry::Get().LocalPeerId(); }

// ---- the applied markers -------------------------------------------------------------------------------------------
// One line when a state has reached an actor and its `on` changed since the last line, with the charge of that apply: the
// drills read these instead of a clock, and a charge that only moves prints nothing (the host says it twice a second). A
// spray's eid is 0 (a hand has no element id); a thrust's slot is 0. `seq` numbers each line printed, one count for both
// kinds, so the drills' reads (LastSprayApplied, LastThrustApplied) say whether a line came after the moment they started
// waiting.
struct SprayMark { int8_t on = -1; float charge = -2.f; uint32_t seq = 0; };
SprayMark g_sprayMark[N::kMaxPeers];
struct ThrustMark { uint32_t eid = 0; bool on = false; float charge = -2.f; uint32_t seq = 0; };
constexpr size_t kThrustMarks = 32;
ThrustMark g_thrustMark[kThrustMarks];
size_t g_nextThrustMark = 0;
uint32_t g_markSeq = 0;

void MarkSpray(uint8_t slot, bool on, float charge) {
    if (slot >= N::kMaxPeers) return;
    SprayMark& m = g_sprayMark[slot];
    if (m.on == (on ? 1 : 0)) return;
    m.on = on ? 1 : 0;
    m.charge = charge;
    m.seq = ++g_markSeq;
    UE_LOGI("fireext: applied op=1 slot=%u eid=0 on=%d charge=%.3f", slot, on ? 1 : 0, charge);
}

void MarkThrust(uint32_t eid, bool on, float charge) {
    ThrustMark* m = nullptr;
    for (ThrustMark& t : g_thrustMark)
        if (t.eid == eid) m = &t;
    if (!m) {
        m = &g_thrustMark[g_nextThrustMark];
        g_nextThrustMark = (g_nextThrustMark + 1) % kThrustMarks;
        *m = ThrustMark{eid, !on, -2.f};
    }
    if (m->on == on) return;
    m->on = on;
    m->charge = charge;
    m->seq = ++g_markSeq;
    UE_LOGI("fireext: applied op=0 slot=0 eid=%u on=%d charge=%.3f", eid, on ? 1 : 0, charge);
}

// ---- what a peer shows ---------------------------------------------------------------------------------------------

// A hand mirror's spray, as the sprayer's own `fire` shows it: the effect, `firing` and the sound through the
// extinguisher's own fireSound. The mirror's tick stays off, so it never aims along this machine's camera, drains its
// charge or casts a ray of its own: the rays are CastRays's.
void ShowSpray(void* mirror, uint8_t slot, bool on) {
    if (!mirror || !R::IsLive(mirror)) return;
    FX::WriteSpraying(mirror, on);
    E::SetActorTickEnabled(mirror, false);
    if (void* eff = FX::Member(mirror, P::name::FireExtSprayEffectProp)) CC::SetActive(eff, on, false);
    CC::CallParamlessNamed(mirror, L"fireSound");
    float charge = -1.f;
    FX::ReadCharge(mirror, charge);
    MarkSpray(slot, on, charge);
}

// A world copy's thrust, as the host's `shoot` shows it: the thrust effect scaled by the charge and its sound. The
// thruster stays off: the host's prop moves, and its pose reaches this copy.
void ShowThrust(void* ext, bool on, float life) {
    FX::WriteCharge(ext, life);
    if (on) FX::WriteThrusting(ext, true);
    void* const eff = FX::Member(ext, P::name::FireExtThrustEffectProp);
    void* const snd = FX::Member(ext, P::name::FireExtThrustSoundProp);
    if (eff) {
        CC::SetActive(eff, on, false);
        if (on) CC::SetWorldScale3D(eff, Lerp(0.1f, 1.f, life));
    }
    if (snd) {
        CC::SetActive(snd, on, false);
        if (on) CC::SetVolumeMultiplier(snd, Lerp(0.1f, 2.f, life));
    }
    float charge = -1.f;
    FX::ReadCharge(ext, charge);
    MarkThrust(EidOf(ext), on, charge);
}

// ---- the sprays ----------------------------------------------------------------------------------------------------
struct Spray {
    bool on = false;
    ue_wrap::CachedObjRef shown;  // the mirror the spray was put on
    // The charge of the extinguisher in this slot's hand: HOST, the one it drains while the slot sprays (-1 unknown, until
    // the slot's first spray says it); CLIENT, the host's last word, written into that hand's copy here.
    float charge = -1.f;
    uint64_t drainedMs = 0;  // HOST: the last drain
    uint64_t saidMs = 0;     // HOST: the last charge sent while it sprays
    // HOST: the slot's hold (hand_item::HoldGeneration) the count was started under: another hold is another item.
    uint32_t holdGen = 0;
    // The aim last written to the shown mirror's spray effect; unknown until written for the mirror now shown.
    ue_wrap::FRotator aim{};
    bool aimWritten = false;
};
Spray g_spray[N::kMaxPeers];
uint32_t g_rays[N::kMaxPeers] = {};  // the rays this peer cast for a slot's spray, logged as it ends
// This peer's own hand's spray, as its `fire` last left it, and the hand actor it was read from: a latch of a hand that
// is gone or another says nothing of the one in hand. Kept with or without a session and across one: a host that sprays
// before any client connects still owes a joiner its spray, and its release to everyone.
bool g_ownOn = false;
ue_wrap::CachedObjRef g_ownActor;

// A spray's state goes to every ready peer at any distance, where MTA relays a claim to the cameras near it
// (CGame.cpp:2930-2945): at most 16 peers, and each peer's own ray needs every spray.
void SendSpray(uint8_t slot, bool on, int toSlot, float charge) {
    N::FireExtPayload p{};
    p.op = N::kFireExtOpSpray;
    p.slot = slot;
    p.on = on ? 1 : 0;
    p.life = charge < 0.f ? 0.f : charge;
    p.charged = charge < 0.f ? 0 : 1;
    if (toSlot >= 0) g_session->SendReliableToSlot(toSlot, N::ReliableKind::FireExtState, &p, sizeof(p));
    else g_session->SendReliable(N::ReliableKind::FireExtState, &p, sizeof(p));
}

// The spray shown on the slot's current mirror, and the effect turned along the sprayer's aim while it runs. A slot with no
// spray on and nothing shown costs no lookup.
void TendSprays() {
    const uint8_t own = OwnSlot();
    for (uint8_t s = 0; s < N::kMaxPeers; ++s) {
        Spray& sp = g_spray[s];
        if (s == own || (!sp.on && !sp.shown.Get())) continue;
        void* const mirror = coop::hand_item::MirrorActorForSlot(s);
        const bool ext = FX::IsExtinguisher(mirror);
        if (!sp.on) {
            if (ext && sp.shown.Is(mirror)) {
                ShowSpray(mirror, s, false);
                UE_LOGI("fireext_sync: slot %u's spray ended here (%u rays cast)", s, g_rays[s]);
                g_rays[s] = 0;
            }
            sp.shown.Set(nullptr);
            sp.aimWritten = false;
            continue;
        }
        if (!ext) continue;
        if (!sp.shown.Is(mirror)) {
            ShowSpray(mirror, s, true);
            sp.shown.Set(mirror);
            sp.aimWritten = false;
        }
        coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(s);
        void* const eff = FX::Member(mirror, P::name::FireExtSprayEffectProp);
        if (!rp || !rp->valid() || !eff) continue;
        // The effect is turned only when the synced aim moved: the write is a reflected call and dirties the transform.
        const ue_wrap::FRotator aim = rp->GetSyncedAimRotation();
        if (sp.aimWritten && aim.Pitch == sp.aim.Pitch && aim.Yaw == sp.aim.Yaw && aim.Roll == sp.aim.Roll) continue;
        if (E::SetComponentWorldRotation(eff, aim)) {
            sp.aim = aim;
            sp.aimWritten = true;
        }
    }
}

std::mt19937& Rng() {
    static std::mt19937 rng{std::random_device{}()};
    return rng;
}

// The game's own extinguishing ray for another player's spray, cast from this peer's mirror of its hand along the sprayer's
// synced aim each frame, as the sprayer's tick casts it along its camera. Every peer casts it: while fire runs on every
// peer separately, this peer's own ray is the only way this peer's fire goes out. Transitional until fire itself is the
// host's (the header says what that change deletes).
void CastRays() {
    if (MirrorRayOff()) return;
    const uint8_t own = OwnSlot();
    for (uint8_t s = 0; s < N::kMaxPeers; ++s) {
        if (s == own || !g_spray[s].on) continue;
        void* const mirror = coop::hand_item::MirrorActorForSlot(s);
        coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(s);
        ue_wrap::FVector eye{};
        if (!FX::IsExtinguisher(mirror) || !rp || !rp->valid() || !rp->TryGetEyePosition(eye)) continue;
        const ue_wrap::FVector dir = rp->GetSyncedAimDirection();
        std::uniform_real_distribution<float> unit(-1.f, 1.f), scatter(0.f, kRayScatterCm), radius(5.f, 10.f);
        ue_wrap::FVector r{unit(Rng()), unit(Rng()), unit(Rng())};
        const float len = std::sqrt(r.X * r.X + r.Y * r.Y + r.Z * r.Z);
        const float k = len > 1e-3f ? scatter(Rng()) / len : 0.f;
        const ue_wrap::FVector end{eye.X + dir.X * kRayLengthCm + r.X * k, eye.Y + dir.Y * kRayLengthCm + r.Y * k,
                                   eye.Z + dir.Z * kRayLengthCm + r.Z * k};
        if (FX::CastExtinguishRay(mirror, eye, end, radius(Rng()))) ++g_rays[s];
    }
}

// The host's charge for a slot's hand, written into this peer's own hand or its mirror of that slot's.
void WriteCharge(uint8_t slot, float charge) {
    FX::WriteCharge(slot == OwnSlot() ? coop::hand_item::LocalHandActor() : coop::hand_item::MirrorActorForSlot(slot), charge);
}

// HOST: a client's spray drains the host's count of its charge at the game's own rate (a second's spray, 1/60), and the
// count goes to every peer twice a second and as the spray ends: the sprayer's hand and every mirror of it take it. A
// spray whose charge runs out ends. The host's own hand drains in its own game, and its start and end say its charge.
void DrainCharges() {
    const uint64_t now = NowMs();
    for (uint8_t s = 1; s < N::kMaxPeers; ++s) {
        Spray& sp = g_spray[s];
        if (!sp.on || sp.charge < 0.f) continue;
        sp.charge -= static_cast<float>(now - sp.drainedMs) / 60000.f;
        sp.drainedMs = now;
        if (sp.charge <= 0.f) {
            sp.charge = 0.f;
            sp.on = false;
            SendSpray(s, false, -1, 0.f);
            UE_LOGI("fireext_sync: HOST slot %u's extinguisher is empty -- its spray ends", s);
            continue;
        }
        if (now - sp.saidMs >= kChargeEveryMs) {
            sp.saidMs = now;
            SendSpray(s, true, -1, sp.charge);
            WriteCharge(s, sp.charge);  // the host's own mirror of that hand shows it too
        }
    }
}

// ---- the client's thrusts ------------------------------------------------------------------------------------------
// The latest state per copy that is not here yet: a fixed table, a state waits for its copy with no clock, and goes when
// applied, at disconnect, or when an off arrives with no charge (the host's thrust ended for an extinguisher that is gone, so
// no copy is coming). An off that carries a charge waits like any state: it is the charge of a copy that is not here yet.
struct Pending {
    bool used = false;
    N::FireExtPayload p{};
};
constexpr size_t kMaxPending = 32;
Pending g_pending[kMaxPending];
size_t g_pendingCount = 0;
uint32_t g_pendingRefused = 0;

Pending* PendingOf(uint32_t eid) {
    for (Pending& e : g_pending)
        if (e.used && e.p.eid == eid) return &e;
    return nullptr;
}

void RetirePending(Pending* e) {
    if (!e) return;
    *e = Pending{};
    --g_pendingCount;
}

void ApplyThrust(void* ext, const N::FireExtPayload& p) {
    ShowThrust(ext, p.on != 0, p.life);
    if (p.on) detail::ForgetAsk(p.eid);
}

void TickPending() {
    if (g_pendingCount == 0) return;
    for (Pending& e : g_pending) {
        if (!e.used) continue;
        void* const ext = ExtinguisherOf(e.p.eid);
        if (!ext) continue;
        ApplyThrust(ext, e.p);
        RetirePending(&e);
    }
}

void OnThrustState(const N::FireExtPayload& p) {
    Pending* const waiting = PendingOf(p.eid);
    if (void* const ext = ExtinguisherOf(p.eid)) {
        ApplyThrust(ext, p);
        RetirePending(waiting);
        return;
    }
    if (!p.on && !p.charged) {
        RetirePending(waiting);
        return;
    }
    if (waiting) {
        waiting->p = p;
        return;
    }
    if (g_pendingCount >= kMaxPending) {
        const uint32_t n = ++g_pendingRefused;
        if ((n & (n - 1)) == 0)
            UE_LOGW("fireext_sync: CLIENT %zu thrusts already wait for their copy -- the one for eid %u is dropped (#%u)",
                    kMaxPending, p.eid, n);
        return;
    }
    for (Pending& e : g_pending)
        if (!e.used) {
            e.used = true;
            e.p = p;
            ++g_pendingCount;
            return;
        }
}

// ---- the gate ------------------------------------------------------------------------------------------------------

// This peer's own hand's spray, after its `fire`: latched always; when connected, what it now shows goes to the host, or
// from the host to everyone. MTA carries the fire button as state in every keysync, so a lost edge heals on the next one
// (CNetAPI.cpp:571-719); the pose stream here has no use bit and the hand axis is reliable, so the edge is sent once.
void OnFirePost(const sg::Call& c) {
    if (!c.object || !coop::hand_item::IsLocalHandViewActor(c.object)) return;
    bool firing = false;
    if (!FX::ReadSpraying(c.object, firing)) return;
    if (g_ownOn && !g_ownActor.Is(c.object)) g_ownOn = false;  // the latch of a hand that is gone or another starts fresh
    if (firing == g_ownOn) return;
    g_ownOn = firing;
    g_ownActor.Set(c.object);
    if (!g_session || !g_session->connected()) return;
    float life = -1.f;
    FX::ReadCharge(c.object, life);
    if (IsHost()) {
        SendSpray(0, firing, -1, life);
        UE_LOGI("fireext_sync: HOST this player's spray %hs -- sent", firing ? "began" : "ended");
    } else if (IsClient()) {
        N::FireExtPayload p{};
        p.op = N::kFireExtOpSprayAsk;
        p.on = firing ? 1 : 0;
        p.life = life < 0.f ? 1.f : life;  // this hand's charge as it starts or stops; the host's word follows
        p.charged = 1;
        const bool ok = g_session->SendReliable(N::ReliableKind::FireExtIntent, &p, sizeof(p));
        UE_LOGI("fireext_sync: CLIENT this player's spray %hs -- told the host (ok=%d)", firing ? "began" : "ended",
                ok ? 1 : 0);
    }
}

// This player's own hold changed (a pick, a drop, a quick-slot switch): the extinguisher its spray was on is gone, so the
// spray ends, said with no charge (the host keeps its own count).
void OnUpdateHoldPost(const sg::Call& c) {
    if (!g_ownOn || !c.object || c.object != coop::players::Registry::Get().Local()) return;
    g_ownOn = false;
    g_ownActor.Set(nullptr);
    if (!g_session || !g_session->connected()) return;
    if (IsHost()) {
        SendSpray(0, false, -1, -1.f);
    } else if (IsClient()) {
        N::FireExtPayload p{};
        p.op = N::kFireExtOpSprayAsk;
        g_session->SendReliable(N::ReliableKind::FireExtIntent, &p, sizeof(p));
    }
    UE_LOGI("fireext_sync: this player's hold changed -- its spray ends");
}

// HOST: the hand lane applied a state of `slot`, or settled one owed: a hold that is no longer the extinguisher the count was
// started under (another class, or another hold of one) ends that slot's spray and forgets its count, said to everyone.
void OnHandApplied(uint8_t slot) {
    if (!IsHost() || slot == 0 || slot >= N::kMaxPeers) return;
    Spray& sp = g_spray[slot];
    if (coop::hand_item::HeldClassIs(slot, kClass) && sp.holdGen == coop::hand_item::HoldGeneration(slot)) return;
    if (sp.on) {
        sp.on = false;
        SendSpray(slot, false, -1, sp.charge);
        UE_LOGI("fireext_sync: HOST slot %u's hand no longer holds an extinguisher -- its spray ends", slot);
    }
    sp.charge = -1.f;
}

template <class F>
void ForEachWorldExtinguisher(F&& fn) {
    void* const cls = OI::ClassByName(kClass);
    if (!cls) return;
    std::vector<void*> found;
    OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
        if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__"))
            static_cast<std::vector<void*>*>(ctx)->push_back(obj);
    }, &found);
    for (void* e : found)
        if (!coop::hand_item::IsHandAxisActor(e)) fn(e);
}

// HOST: what the ledger of `slot` counted, as one line.
void PrintLedger(int slot) {
    const detail::SlotLedger& l = Ledger(slot);
    uint32_t any = l.onsTaken + l.onsDropped;
    for (uint32_t n : l.asks) any += n;
    if (any == 0) return;
    char buf[320];
    int len = std::snprintf(buf, sizeof(buf), "sprays on taken=%u dropped=%u; runaway asks", l.onsTaken, l.onsDropped);
    for (size_t r = 0; r < detail::kReasons && len > 0 && len < static_cast<int>(sizeof(buf)); ++r)
        if (l.asks[r])
            len += std::snprintf(buf + len, sizeof(buf) - static_cast<size_t>(len), " %s=%u",
                                 detail::ReasonName(static_cast<detail::Reason>(r)), l.asks[r]);
    UE_LOGI("fireext_sync: HOST slot %d's intents over its stay: %s", slot, buf);
}

// HOST: a client's start or stop of its spray. The charge is the host's count: the hand's own word starts it, and never
// raises it; it belongs to the hold it started under, and another hold starts afresh. An "off" takes no token and is always
// applied (a dropped one would spray on forever), lowers the count and says nothing unless the state changes. An "on" takes
// one from the sender's spray bucket, and past it is dropped, with no line and no answer. A refusal after the token answers
// the sender only; its own hand keeps spraying, which only its own world shows.
// MTA: the hand's class is the client's own announce and the host keeps no grant, where the server holds the weapon type
// and applies the client's weapon data only when it matches (CPlayerPuresyncPacket.cpp:214-252); a hotbar item has no host
// record to hold. The host counts the charge and never raises it, where MTA takes the client's ammo as sent (:270-275).
// MTA drops a refused claim with no answer (CGame.cpp:2535-2540); this answers the sender, whose log names the refusal.
// Residual: a client that cycles its hand's class re-seeds its count (the hand's class is its own word), so it can keep a
// spray on with each peer casting the game's ray from its puppet's eye every frame; that changes each peer's own world near
// the client's claimed pose. Bounded by the spray budget (one token per "on"), the hand record's own budget and the cost, per
// spraying slot per frame: the puppet's eye read, the extinguishRay call and the game's trace script, 15 slots at most.
void OnSprayAsk(uint8_t s, const N::FireExtPayload& p) {
    Spray& sp = g_spray[s];
    const bool on = p.on != 0;
    detail::SlotLedger& led = Ledger(s);
    if (on) {
        if (!led.spray.Take(kSprayBudget, NowMs())) {
            ++led.onsDropped;
            return;
        }
        ++led.onsTaken;
        // The hold announced ahead of this "on" on the one ordered lane may still be owed by the hand lane's pacing: it is
        // applied now, so the spray is judged on the hand it follows.
        if (coop::hand_item::HandStateOwed(s)) coop::hand_item::ApplyOwedNow(*g_session, s);
        if (!coop::hand_item::HeldClassIs(s, kClass)) {
            UE_LOGW("fireext_sync: HOST slot %u sprays with no extinguisher in its hand -- refused", s);
            SendSpray(s, false, s, -1.f);
            return;
        }
    }
    const uint32_t gen = coop::hand_item::HoldGeneration(s);
    if (sp.holdGen != gen) {
        sp.charge = -1.f;
        sp.holdGen = gen;
    }
    float said = -1.f;  // the hand's word; an "on" with none says 0, which never raises the count
    if (p.charged && std::isfinite(p.life)) said = std::fmin(std::fmax(p.life, 0.f), 1.f);
    else if (on) said = 0.f;
    if (said >= 0.f) sp.charge = sp.charge < 0.f ? said : std::fmin(sp.charge, said);
    if (!on) {
        if (!sp.on) return;
        sp.on = false;
        SendSpray(s, false, -1, sp.charge);
        UE_LOGI("fireext_sync: HOST slot %u's spray ended, charge %.3f -- shown and sent", s, sp.charge);
        return;
    }
    if (sp.charge <= 0.f) {
        UE_LOGI("fireext_sync: HOST slot %u sprays an empty extinguisher -- refused", s);
        SendSpray(s, false, s, 0.f);
        return;
    }
    if (sp.on) return;
    sp.on = true;
    sp.drainedMs = sp.saidMs = NowMs();
    SendSpray(s, true, -1, sp.charge);
    UE_LOGI("fireext_sync: HOST slot %u's spray began, charge %.3f -- shown and sent", s, sp.charge);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session = session;
    if (!coop::hand_item::SubscribeHandApplied(&OnHandApplied))
        UE_LOGE("fireext_sync: the hand lane's subscriber table is full -- a hold that stops being an extinguisher does not end "
                "its spray");
    if (g_watched) return;
    g_watched = true;
    const bool f = sg::WatchName(kRowFire, kTagFire, nullptr, &OnFirePost);
    const bool h = sg::WatchName(kRowUpdateHold, kTagHold, nullptr, &OnUpdateHoldPost);
    if (!f || !h) UE_LOGE("fireext_sync: a watch was refused (fire=%d updateHold=%d)", f ? 1 : 0, h ? 1 : 0);
}

void Tick() {
    if (g_session && g_session->running() && g_session->connected()) {
        if (IsHost()) DrainCharges();
        else if (IsClient()) TickPending();
        CastRays();
        TendSprays();
    }
    if (!g_watched || g_settled) return;
    sg::ResolvePendingNames();
    if (!sg::NameWatchSettled(kRowFire, kTagFire) || !sg::NameWatchSettled(kRowUpdateHold, kTagHold)) return;
    g_settled = true;
    const bool f = sg::NameWatchLive(kRowFire, kTagFire), h = sg::NameWatchLive(kRowUpdateHold, kTagHold);
    if (f && h) UE_LOGI("fireext_sync: the watches on prop_fireExt_C's fire and mainPlayer_C's updateHold are live");
    else UE_LOGE("fireext_sync: a watch is dead (fire=%d updateHold=%d) -- this player's spray is not told", f ? 1 : 0, h ? 1 : 0);
}

void SayThrustByEid(uint32_t eid, bool on, float charge, int toSlot) {
    if (!IsHost() || eid == 0 || !g_session->connected()) return;
    N::FireExtPayload p{};
    p.eid = eid;
    p.op = N::kFireExtOpThrust;
    p.on = on ? 1 : 0;
    const bool known = std::isfinite(charge) && charge >= 0.f;
    p.life = known ? std::fmin(charge, 1.f) : 0.f;
    p.charged = (on || known) ? 1 : 0;  // 0 only for the end of a thrust whose extinguisher is gone: no copy is coming
    if (toSlot >= 0) g_session->SendReliableToSlot(toSlot, N::ReliableKind::FireExtState, &p, sizeof(p));
    else g_session->SendReliable(N::ReliableKind::FireExtState, &p, sizeof(p));
}

void SayThrust(void* ext, bool on, float charge, int toSlot) { SayThrustByEid(EidOf(ext), on, charge, toSlot); }

Applied LastSprayApplied(uint8_t slot) {
    if (slot >= N::kMaxPeers) return {};
    const SprayMark& m = g_sprayMark[slot];
    return {m.seq, m.on == 1, m.charge};
}

Applied LastThrustApplied(uint32_t eid) {
    for (const ThrustMark& t : g_thrustMark)
        if (eid != 0 && t.eid == eid) return {t.seq, t.on, t.charge};
    return {};
}

bool SprayToldOn(uint8_t slot) { return slot < N::kMaxPeers && g_spray[slot].on; }

bool ThrustWaits(uint32_t eid) { return eid != 0 && PendingOf(eid) != nullptr; }

float HostSprayCount(uint8_t slot) { return IsHost() && slot < N::kMaxPeers ? g_spray[slot].charge : -1.f; }

// The walk of the world's extinguishers happens here, once per joiner (cold): a thrusting one is adopted into the thrust lane's
// held list and sent on, any other with charge under 1 is sent off with its charge.
void SendJoinSnapshotForSlot(int slot) {
    if (!IsHost() || slot <= 0 || SnapshotOff()) return;
    ForEachWorldExtinguisher([slot](void* ext) {
        bool thrusting = false;
        float charge = 0.f;
        if (!FX::ReadThrusting(ext, thrusting) || !FX::ReadCharge(ext, charge)) return;
        if (thrusting && charge > 0.f) {
            coop::fireext_thrust_sync::Adopt(ext);
            SayThrust(ext, true, charge, slot);
        } else if (charge < 1.f) {
            SayThrust(ext, false, charge, slot);
        }
    });
    // The host's own spray, from the hand as it is now: the latch alone would tell a joiner of a spray that is not running.
    void* const hand = coop::hand_item::LocalHandActor();
    bool spraying = false;
    if (FX::ReadSpraying(hand, spraying) && spraying) {
        float life = -1.f;
        FX::ReadCharge(hand, life);
        SendSpray(0, true, slot, life);
    }
    for (uint8_t s = 1; s < N::kMaxPeers; ++s)
        if (g_spray[s].on && s != slot) SendSpray(s, true, slot, g_spray[s].charge);
}

void OnIntent(int senderSlot, const coop::net::FireExtPayload& p) {
    if (!IsHost() || senderSlot <= 0 || senderSlot >= static_cast<int>(N::kMaxPeers)) return;
    if (p.op == N::kFireExtOpRunawayAsk) detail::OnRunawayAsk(senderSlot, p);
    else if (p.op == N::kFireExtOpSprayAsk) OnSprayAsk(static_cast<uint8_t>(senderSlot), p);
}

void OnState(const coop::net::FireExtPayload& p) {
    if (!IsClient()) return;
    if (p.op == N::kFireExtOpThrust) {
        OnThrustState(p);
        return;
    }
    if (p.op != N::kFireExtOpSpray || p.slot >= N::kMaxPeers) return;
    if (p.charged == 1 && std::isfinite(p.life)) {
        g_spray[p.slot].charge = p.life;
        WriteCharge(p.slot, p.life);
    }
    if (p.slot == OwnSlot()) {
        float charge = -1.f;
        FX::ReadCharge(coop::hand_item::LocalHandActor(), charge);
        MarkSpray(p.slot, p.on != 0, charge);
        if (!p.on && g_ownOn) UE_LOGI("fireext_sync: CLIENT the host ended this player's spray, charge %.3f", p.life);
        return;
    }
    g_spray[p.slot].on = p.on != 0;
}

void OnPeerLeft(uint8_t slot) {
    if (slot >= N::kMaxPeers) return;
    Spray& sp = g_spray[slot];
    if (IsHost() && sp.on) SendSpray(slot, false, -1, sp.charge);  // the leaver's spray is said off to everyone
    PrintLedger(slot);  // counted on the host alone: empty elsewhere
    Ledger(slot) = detail::SlotLedger{};
    sp = Spray{};
    g_rays[slot] = 0;
    g_sprayMark[slot] = SprayMark{};
}

void OnDisconnect() {
    for (int s = 1; s < static_cast<int>(N::kMaxPeers); ++s) PrintLedger(s);  // counted on the host alone: empty elsewhere
    for (uint8_t s = 0; s < N::kMaxPeers; ++s) {
        g_spray[s] = Spray{};
        g_rays[s] = 0;
        g_sprayMark[s] = SprayMark{};
        Ledger(s) = detail::SlotLedger{};
    }
    // g_ownOn and g_ownActor stay: the local hand still sprays or not whatever the session did, and the next fire reads it
    // against the hand actor the latch was taken from.
    for (Pending& e : g_pending) e = Pending{};
    g_pendingCount = 0;
    g_pendingRefused = 0;
    for (ThrustMark& t : g_thrustMark) t = ThrustMark{};
    g_nextThrustMark = 0;
    detail::ClearAsks();
    detail::ClearAnswers();
}

}  // namespace coop::fireext_sync
