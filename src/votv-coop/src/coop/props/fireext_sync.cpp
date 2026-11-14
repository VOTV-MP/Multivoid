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

#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include <chrono>
#include <cmath>
#include <random>
#include <vector>

namespace coop::fireext_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
namespace sg = ue_wrap::script_gate;
namespace CC = ue_wrap::component_calls;
namespace EL = coop::element;
namespace N  = coop::net;

UE_FACT_FN(kRowFire, "fireext_sync", L"prop_fireExt_C", L"fire", Declared);
UE_FACT_FN(kRowShoot, "fireext_sync", L"prop_fireExt_C", L"shoot", Declared);
UE_FACT_FN(kRowRay, "fireext_sync", L"prop_fireExt_C", L"extinguishRay", Declared);
constexpr int kTagFire = 0x46455801, kTagShoot = 0x46455802, kTagRay = 0x46455803;  // 'FEX' 1..3

constexpr const wchar_t* kClass = L"prop_fireExt_C";
constexpr float    kRayLengthCm = 750.f;     // the tick's ray, from the eye along the aim
constexpr float    kRayScatterCm = 100.f;    // plus up to this, at random
constexpr uint64_t kScanEveryMs = 100;       // the host's look at the world extinguishers
constexpr uint64_t kLifeEveryMs = 500;       // a thrust's charge, at most once per this
constexpr uint64_t kStateLifeMs = 10000;     // a thrust for a copy not yet made waits this long

coop::net::Session* g_session = nullptr;
bool g_watched = false;
bool g_settled = false;
bool g_applying = false;  // this lane's own calls into an extinguisher: the gate lets them run

bool IsHost() { return g_session && g_session->running() && g_session->role() == N::Role::Host; }
bool IsClient() { return g_session && g_session->running() && g_session->role() == N::Role::Client; }

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

bool IsExtinguisher(void* obj) {
    void* const base = OI::ClassByName(kClass);
    return obj && base && R::IsLive(obj) && R::IsDescendantOfAny(R::ClassOf(obj), &base, 1);
}

void* MemberObj(void* obj, const wchar_t* name) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    return off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const char*>(obj) + off) : nullptr;
}

float* LifeOf(void* ext) {
    const int32_t off = ext ? R::FindPropertyOffset(R::ClassOf(ext), L"life") : -1;
    return off >= 0 ? reinterpret_cast<float*>(static_cast<char*>(ext) + off) : nullptr;
}

bool ReadBool(void* obj, const wchar_t* name, bool& out) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask)) return false;
    out = (static_cast<const uint8_t*>(obj)[off] & mask) != 0;
    return true;
}

void WriteBool(void* obj, const wchar_t* name, bool v) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask)) return;
    uint8_t& b = static_cast<uint8_t*>(obj)[off];
    b = v ? static_cast<uint8_t>(b | mask) : static_cast<uint8_t>(b & ~mask);
}

void SetScale(void* comp, float s) {
    void* const fn = comp ? R::FindDispatchFunctionCached(R::ClassOf(comp), L"SetWorldScale3D") : nullptr;
    ue_wrap::ParamFrame f(fn);
    if (f.valid() && f.Set<ue_wrap::FVector>(L"NewScale", ue_wrap::FVector{s, s, s})) ue_wrap::Call(comp, f);
}

float Lerp(float a, float b, float t) { return a + (b - a) * t; }

uint32_t EidOf(void* ext) {
    const EL::ElementId eid = EL::Registry::Get().EidForActor(ext);
    return eid == EL::kInvalidId ? 0u : static_cast<uint32_t>(eid);
}

void* ExtinguisherOf(uint32_t eid) {
    void* const ext = EL::LiveActorOfType(static_cast<EL::ElementId>(eid), EL::ElementType::Prop);
    return IsExtinguisher(ext) ? ext : nullptr;
}

// ---- what a peer shows ---------------------------------------------------------------------------------------------

// A hand mirror's spray, as the sprayer's own `fire` shows it: the effect, `firing` and the sound through the
// extinguisher's own fireSound. The mirror's tick stays off, so it never aims along this machine's camera, drains its
// charge or casts a ray of its own.
void ShowSpray(void* mirror, bool on) {
    if (!mirror || !R::IsLive(mirror)) return;
    g_applying = true;
    WriteBool(mirror, L"firing", on);
    E::SetActorTickEnabled(mirror, false);
    if (void* eff = MemberObj(mirror, L"eff_ext")) CC::SetActive(eff, on, false);
    CC::CallParamlessNamed(mirror, L"fireSound");
    g_applying = false;
}

// A world copy's thrust, as the host's `shoot` shows it: the thrust effect scaled by the charge and its sound. The
// thruster stays off: the host's prop moves, and its pose reaches this copy.
void ShowThrust(void* ext, bool on, float life) {
    if (float* l = LifeOf(ext)) *l = life;
    if (on) WriteBool(ext, L"active", true);
    void* const eff = MemberObj(ext, L"eff_fireExtThr");
    void* const snd = MemberObj(ext, L"fireExtThrust");
    if (eff) {
        CC::SetActive(eff, on, false);
        if (on) SetScale(eff, Lerp(0.1f, 1.f, life));
    }
    if (snd) {
        CC::SetActive(snd, on, false);
        if (on) CC::SetVolumeMultiplier(snd, Lerp(0.1f, 2.f, life));
    }
}

// ---- the sprays ----------------------------------------------------------------------------------------------------
struct Spray {
    bool on = false;
    ue_wrap::CachedObjRef shown;  // the mirror the spray was put on
};
Spray g_spray[N::kMaxPeers];
uint32_t g_rays[N::kMaxPeers] = {};  // HOST: the rays cast for a slot's spray, logged as it ends
bool g_ownOn = false;  // this peer's own hand's spray, as last told

uint8_t OwnSlot() { return coop::players::Registry::Get().LocalPeerId(); }

void SendSpray(uint8_t slot, bool on, int toSlot) {
    N::FireExtPayload p{};
    p.op = N::kFireExtOpSpray;
    p.slot = slot;
    p.on = on ? 1 : 0;
    if (toSlot >= 0) g_session->SendReliableToSlot(toSlot, N::ReliableKind::FireExtState, &p, sizeof(p));
    else g_session->SendReliable(N::ReliableKind::FireExtState, &p, sizeof(p));
}

// The spray shown on the slot's current mirror, and the effect turned along the sprayer's aim while it runs.
void TendSprays() {
    const uint8_t own = OwnSlot();
    for (uint8_t s = 0; s < N::kMaxPeers; ++s) {
        Spray& sp = g_spray[s];
        if (s == own) continue;
        void* const mirror = coop::hand_item::MirrorActorForSlot(s);
        const bool ext = IsExtinguisher(mirror);
        if (!sp.on) {
            if (ext && sp.shown.Is(mirror)) ShowSpray(mirror, false);
            sp.shown.Set(nullptr);
            continue;
        }
        if (!ext) continue;
        if (!sp.shown.Is(mirror)) {
            ShowSpray(mirror, true);
            sp.shown.Set(mirror);
        }
        coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(s);
        void* const eff = MemberObj(mirror, L"eff_ext");
        if (rp && rp->valid() && eff) E::SetComponentWorldRotation(eff, rp->GetSyncedAimRotation());
    }
}

std::mt19937& Rng() {
    static std::mt19937 rng{std::random_device{}()};
    return rng;
}

// HOST: the game's own extinguishing ray for a client's spray, cast from its mirror along the sender's aim each frame,
// as the sprayer's tick casts it along its camera. Only the host's world puts a fire out.
void CastRays() {
    for (uint8_t s = 1; s < N::kMaxPeers; ++s) {
        if (!g_spray[s].on) continue;
        if (!coop::hand_item::HeldClassIs(s, kClass)) {
            g_spray[s].on = false;
            SendSpray(s, false, -1);
            UE_LOGI("fireext_sync: HOST slot %u's hand no longer holds an extinguisher -- its spray ends", s);
            continue;
        }
        void* const mirror = coop::hand_item::MirrorActorForSlot(s);
        coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(s);
        ue_wrap::FVector eye{};
        if (!IsExtinguisher(mirror) || !rp || !rp->valid() || !rp->TryGetEyePosition(eye)) continue;
        const ue_wrap::FVector dir = rp->GetSyncedAimDirection();
        std::uniform_real_distribution<float> unit(-1.f, 1.f), scatter(0.f, kRayScatterCm), radius(5.f, 10.f);
        ue_wrap::FVector r{unit(Rng()), unit(Rng()), unit(Rng())};
        const float len = std::sqrt(r.X * r.X + r.Y * r.Y + r.Z * r.Z);
        const float k = len > 1e-3f ? scatter(Rng()) / len : 0.f;
        const ue_wrap::FVector end{eye.X + dir.X * kRayLengthCm + r.X * k, eye.Y + dir.Y * kRayLengthCm + r.Y * k,
                                   eye.Z + dir.Z * kRayLengthCm + r.Z * k};
        ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(mirror), L"extinguishRay"));
        if (!f.valid()) continue;
        f.Set<ue_wrap::FVector>(L"inputPin", eye);
        f.Set<ue_wrap::FVector>(L"inputPin2", end);
        f.Set<float>(L"radius", radius(Rng()));
        g_applying = true;
        if (ue_wrap::Call(mirror, f)) ++g_rays[s];
        g_applying = false;
    }
}

// ---- the thrusts (host) --------------------------------------------------------------------------------------------
struct Known {
    ue_wrap::CachedObjRef ref;
    bool on = false;
    float life = 1.f;
    bool said = false;
    uint64_t lifeAt = 0;
};
std::vector<Known> g_known;
uint64_t g_nextScanMs = 0;

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

bool ReadThrust(void* ext, bool& on, float& life) {
    bool active = false;
    const float* l = LifeOf(ext);
    if (!l || !ReadBool(ext, L"active", active)) return false;
    life = *l;
    on = active && life > 0.f;
    return true;
}

void SendThrust(void* ext, bool on, float life, int slot) {
    N::FireExtPayload p{};
    p.eid = EidOf(ext);
    if (!p.eid) return;
    p.op = N::kFireExtOpThrust;
    p.on = on ? 1 : 0;
    p.life = life;
    if (slot >= 0) g_session->SendReliableToSlot(slot, N::ReliableKind::FireExtState, &p, sizeof(p));
    else g_session->SendReliable(N::ReliableKind::FireExtState, &p, sizeof(p));
}

Known& KnownOf(void* ext) {
    for (Known& k : g_known)
        if (k.ref.Is(ext)) return k;
    g_known.emplace_back();
    g_known.back().ref.Set(ext);
    return g_known.back();
}

void ScanThrusts() {
    const uint64_t now = NowMs();
    if (now < g_nextScanMs) return;
    g_nextScanMs = now + kScanEveryMs;
    for (size_t i = 0; i < g_known.size();) {  // the destroyed
        if (!g_known[i].ref.Get()) g_known.erase(g_known.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
    ForEachWorldExtinguisher([now](void* ext) {
        bool on = false;
        float life = 0.f;
        if (!ReadThrust(ext, on, life)) return;
        Known& k = KnownOf(ext);
        const bool changed = !k.said ? on : on != k.on;
        const bool drained = std::fabs(life - k.life) > 0.01f && now - k.lifeAt >= kLifeEveryMs;
        if (!changed && !(k.said && drained)) {
            if (!k.said) { k.said = true; k.life = life; }
            return;
        }
        SendThrust(ext, on, life, -1);
        if (changed)
            UE_LOGI("fireext_sync: HOST extinguisher eid %u's thrust %hs (charge %.2f) -- sent", EidOf(ext),
                    on ? "began" : "ended", life);
        k.on = on;
        k.life = life;
        k.said = true;
        k.lifeAt = now;
    });
}

// ---- the client's thrusts ------------------------------------------------------------------------------------------
struct Pending { N::FireExtPayload p; uint64_t since; };
std::vector<Pending> g_states;
struct Shown { uint32_t eid; bool on; };
std::vector<Shown> g_shown;  // the thrust each copy last showed, for the log

bool ShownChanged(uint32_t eid, bool on) {
    for (Shown& s : g_shown)
        if (s.eid == eid) {
            const bool changed = s.on != on;
            s.on = on;
            return changed;
        }
    if (g_shown.size() < 256) g_shown.push_back({eid, on});
    return on;
}

void TickClientThrusts() {
    const uint64_t now = NowMs();
    for (size_t i = 0; i < g_states.size();) {
        void* const ext = ExtinguisherOf(g_states[i].p.eid);
        if (ext) {
            g_applying = true;
            ShowThrust(ext, g_states[i].p.on != 0, g_states[i].p.life);
            g_applying = false;
            if (ShownChanged(g_states[i].p.eid, g_states[i].p.on != 0))
                UE_LOGI("fireext_sync: CLIENT extinguisher eid %u's thrust %hs (charge %.2f), as the host's",
                        g_states[i].p.eid, g_states[i].p.on ? "began" : "ended", g_states[i].p.life);
        } else if (now - g_states[i].since <= kStateLifeMs) {
            ++i;
            continue;
        }
        g_states.erase(g_states.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

// ---- the gate ------------------------------------------------------------------------------------------------------

// This peer's own hand's spray, after its `fire`: what it now shows goes to the host, or from the host to everyone.
void OnFirePost(const sg::Call& c) {
    if (g_applying || !g_session || !g_session->connected() || !c.object) return;
    if (!coop::hand_item::IsLocalHandViewActor(c.object)) return;
    bool firing = false;
    if (!ReadBool(c.object, L"firing", firing) || firing == g_ownOn) return;
    g_ownOn = firing;
    if (IsHost()) {
        SendSpray(0, firing, -1);
        UE_LOGI("fireext_sync: HOST this player's spray %hs -- sent", firing ? "began" : "ended");
    } else if (IsClient()) {
        N::FireExtPayload p{};
        p.op = N::kFireExtOpSprayAsk;
        p.on = firing ? 1 : 0;
        const bool ok = g_session->SendReliable(N::ReliableKind::FireExtIntent, &p, sizeof(p));
        UE_LOGI("fireext_sync: CLIENT this player's spray %hs -- told the host (ok=%d)", firing ? "began" : "ended",
                ok ? 1 : 0);
    }
}

// The thrust and the ray: a client's copy runs neither. The host's prop thrusts, and only the host's rays put a fire
// out; what they change reaches every peer.
sg::Verdict OnClientRefusedPre(const sg::Call&) {
    return IsClient() && !g_applying ? sg::Verdict::Cancel : sg::Verdict::Run;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session = session;
    if (g_watched) return;
    g_watched = true;
    const bool f = sg::WatchName(kRowFire, kTagFire, nullptr, &OnFirePost);
    const bool s = sg::WatchName(kRowShoot, kTagShoot, &OnClientRefusedPre, nullptr);
    const bool r = sg::WatchName(kRowRay, kTagRay, &OnClientRefusedPre, nullptr);
    if (!f || !s || !r)
        UE_LOGE("fireext_sync: a watch was refused (fire=%d shoot=%d ray=%d)", f ? 1 : 0, s ? 1 : 0, r ? 1 : 0);
}

void Tick() {
    if (g_session && g_session->running() && g_session->connected()) {
        if (IsHost()) {
            CastRays();
            ScanThrusts();
        } else if (IsClient()) {
            TickClientThrusts();
        }
        TendSprays();
    }
    if (!g_watched || g_settled) return;
    sg::ResolvePendingNames();
    if (!sg::NameWatchSettled(kRowFire, kTagFire) || !sg::NameWatchSettled(kRowShoot, kTagShoot) ||
        !sg::NameWatchSettled(kRowRay, kTagRay))
        return;
    g_settled = true;
    const bool f = sg::NameWatchLive(kRowFire, kTagFire), s = sg::NameWatchLive(kRowShoot, kTagShoot),
               r = sg::NameWatchLive(kRowRay, kTagRay);
    if (f && s && r) UE_LOGI("fireext_sync: the watches on prop_fireExt_C's fire, shoot and extinguishRay are live");
    else UE_LOGE("fireext_sync: a watch is dead (fire=%d shoot=%d ray=%d) -- a client's extinguisher runs its own",
                 f ? 1 : 0, s ? 1 : 0, r ? 1 : 0);
}

void SendJoinSnapshotForSlot(int slot) {
    if (!IsHost() || slot <= 0) return;
    ForEachWorldExtinguisher([slot](void* ext) {
        bool on = false;
        float life = 0.f;
        if (ReadThrust(ext, on, life) && (on || life < 1.f)) SendThrust(ext, on, life, slot);
    });
    if (g_ownOn) SendSpray(0, true, slot);
    for (uint8_t s = 1; s < N::kMaxPeers; ++s)
        if (g_spray[s].on && s != slot) SendSpray(s, true, slot);
}

void OnIntent(int senderSlot, const coop::net::FireExtPayload& p) {
    if (!IsHost() || senderSlot <= 0 || senderSlot >= static_cast<int>(N::kMaxPeers) || p.op != N::kFireExtOpSprayAsk)
        return;
    const uint8_t s = static_cast<uint8_t>(senderSlot);
    const bool on = p.on != 0;
    if (on && !coop::hand_item::HeldClassIs(s, kClass)) {
        UE_LOGW("fireext_sync: HOST slot %u sprays with no extinguisher in its hand -- refused", s);
        SendSpray(s, false, senderSlot);
        return;
    }
    if (g_spray[s].on == on) return;
    g_spray[s].on = on;
    SendSpray(s, on, -1);
    UE_LOGI("fireext_sync: HOST slot %u's spray %hs -- shown and sent (%u rays cast along its aim)", s,
            on ? "began" : "ended", on ? 0u : g_rays[s]);
    g_rays[s] = 0;
}

void OnState(const coop::net::FireExtPayload& p) {
    if (!IsClient()) return;
    if (p.op == N::kFireExtOpSpray) {
        if (p.slot >= N::kMaxPeers) return;
        if (p.slot == OwnSlot()) {
            if (!p.on && g_ownOn) UE_LOGW("fireext_sync: CLIENT the host ended this player's spray");
            return;
        }
        g_spray[p.slot].on = p.on != 0;
        return;
    }
    if (p.op != N::kFireExtOpThrust || g_states.size() >= 64) return;
    g_states.push_back({p, NowMs()});
}

void OnPeerLeft(uint8_t slot) {
    if (slot >= N::kMaxPeers) return;
    g_spray[slot].on = false;
    g_spray[slot].shown.Set(nullptr);
}

void OnDisconnect() {
    for (Spray& s : g_spray) {
        s.on = false;
        s.shown.Set(nullptr);
    }
    g_ownOn = false;
    g_known.clear();
    g_states.clear();
    g_shown.clear();
}

}  // namespace coop::fireext_sync
