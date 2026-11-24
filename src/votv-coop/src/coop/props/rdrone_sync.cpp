// coop/props/rdrone_sync.cpp -- see coop/props/rdrone_sync.h.
//
// 0.9.0n. prop_rdrone_C: actionOptionIndex action 8, with `energy` not below 0, flips `active` and runs `upd`.
// ReceiveTick, while active, adds the hover force (staticmesh addForce z 980) and a torque toward `look`, and takes
// dt/... off `energy`, switching `active` off through `upd` below 0; while `charging` (the cord plugged) it adds energy
// up to 100. `move(forward, rot, up)` adds one frame's force (the body's flat forward times `forward`, plus `up`) and
// turns `look` by `rot * dt` in world yaw. `upd` shows the blades, the audio and the glow for `active` and `charging`.
// ui_laptop_C's drone sliders call `move` on each value change.
// prop_sled_C: playerSit binds the sitter's move inputs to its pressed flags, turns its tick on and sets `player`;
// playerUnsit undoes it. Its tick, with a `player` and no interface open, pushes the body for each pressed flag.

#include "coop/props/rdrone_sync.h"

#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_drive_host.h"
#include "coop/props/prop_syncer.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/world_instances.h"

#include <windows.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace coop::rdrone_sync {
namespace {

namespace N  = coop::net;
namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace sg = ue_wrap::script_gate;
namespace EL = coop::element;

UE_FACT_FN(kRowTick, "rdrone_sync", L"prop_rdrone_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowAction, "rdrone_sync", L"prop_rdrone_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowMove, "rdrone_sync", L"prop_rdrone_C", L"move", Declared);
UE_FACT_FN(kRowUpd, "rdrone_sync", L"prop_rdrone_C", L"upd", Declared);
UE_FACT_FN(kRowSit, "rdrone_sync", L"prop_sled_C", L"playerSit", Declared);
UE_FACT_FN(kRowUnsit, "rdrone_sync", L"prop_sled_C", L"playerUnsit", Declared);
constexpr int kTagTick = 0x44524E01, kTagAction = 0x44524E02, kTagMove = 0x44524E03, kTagUpd = 0x44524E04;  // 'DRN'
constexpr int kTagSit = 0x534C4401, kTagUnsit = 0x534C4402;                                                 // 'SLD'

constexpr const wchar_t* kDroneClass = L"prop_rdrone_C";
constexpr uint8_t kActSwitch = 8;
constexpr uint8_t kOpMove = 1, kOpSwitch = 2;
constexpr float kReachCm = 600.f;      // a switch press: the sender's body to the drone
constexpr uint64_t kSendMs = 50;       // CLIENT: the summed flight sent at most this often
constexpr uint64_t kMoveGapMs = 30;    // HOST: a sender's flight asks, about thirty a second at most
constexpr float kMaxAxis = 10000.f;    // HOST: the most of each axis one ask may carry (accelerations, in cm/s/s)
constexpr float kEnergyStep = 5.f;     // HOST: energy is sent when it crosses a 5% step
constexpr size_t kMaxHeld = 16;
constexpr int kCap = 64;

N::Session* g_session = nullptr;
bool g_watched = false;
bool g_settled = false;
void* g_applying = nullptr;  // CLIENT: the drone whose `upd` runs for the host's state
uint64_t g_lastAt[N::kMaxPeers] = {};
uint64_t g_lastSwitchAt[N::kMaxPeers] = {};
std::vector<N::RdroneStatePayload> g_held;
void* g_drones[kCap];

// CLIENT: the flight summed since the last send.
struct Pending {
    ue_wrap::CachedObjRef ref;
    float forward = 0.f, rot = 0.f, up = 0.f;
};
std::vector<Pending> g_pending;
uint64_t g_lastSendMs = 0;

// HOST: the last state sent per drone, and whether its drive is claimed.
struct Known {
    ue_wrap::CachedObjRef ref;
    bool active = false;
    int step = -1;
    bool claimed = false;
};
std::vector<Known> g_known;

// The sled this peer's own player rides, from its playerSit to its playerUnsit.
ue_wrap::CachedObjRef g_ridden;

bool IsHost() { return g_session && g_session->running() && g_session->role() == N::Role::Host; }
bool IsClient() { return g_session && g_session->running() && g_session->role() == N::Role::Client; }

uint64_t NowMs() { return ::GetTickCount64(); }

bool IsDrone(void* obj) { return obj && R::IsLive(obj) && R::NameEquals(R::NameOf(R::ClassOf(obj)), kDroneClass); }

R::InstanceOffset s_energy(L"energy");
float* EnergyOf(void* obj) {
    const int32_t off = obj ? s_energy.Of(obj) : -1;
    return off >= 0 ? reinterpret_cast<float*>(static_cast<char*>(obj) + off) : nullptr;
}

bool ReadActive(void* obj, bool& out) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), L"active", off, mask) || off < 0) return false;
    out = (*(static_cast<uint8_t*>(obj) + off) & mask) != 0;
    return true;
}
void WriteActive(void* obj, bool on) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), L"active", off, mask) || off < 0) return;
    uint8_t& b = *(static_cast<uint8_t*>(obj) + off);
    b = on ? static_cast<uint8_t>(b | mask) : static_cast<uint8_t>(b & ~mask);
}

uint32_t EidOf(void* obj) {
    const EL::ElementId eid = EL::Registry::Get().EidForActor(obj);
    return eid == EL::kInvalidId ? 0u : static_cast<uint32_t>(eid);
}

int StepOf(float energy) { return static_cast<int>(std::floor(energy / kEnergyStep)); }

bool SendState(void* drone, int slot) {
    N::RdroneStatePayload p{};
    bool active = false;
    const float* const energy = EnergyOf(drone);
    p.eid = EidOf(drone);
    if (!g_session || !g_session->connected() || !p.eid || !energy || !ReadActive(drone, active)) return false;
    p.energy = *energy;
    p.active = active ? 1 : 0;
    const bool ok = slot >= 0 ? g_session->SendReliableToSlot(static_cast<uint8_t>(slot), N::ReliableKind::RdroneState, &p,
                                                              sizeof(p))
                              : g_session->SendReliable(N::ReliableKind::RdroneState, &p, sizeof(p));
    UE_LOGI("rdrone_sync: HOST drone eid %u is %hs, energy %.1f -- sent to %hs (ok=%d)", p.eid, active ? "on" : "off",
            p.energy, slot >= 0 ? "one slot" : "every client", ok ? 1 : 0);
    return ok;
}

// HOST: the drone's state as it stands, sent when its switch or its energy step changed; its drive claimed while on.
void Publish(void* drone, bool force) {
    bool active = false;
    const float* const energy = EnergyOf(drone);
    if (!energy || !ReadActive(drone, active)) return;
    Known* k = nullptr;
    for (Known& e : g_known)
        if (e.ref.Is(drone)) k = &e;
    if (!k) {
        for (size_t i = 0; i < g_known.size();)
            if (!g_known[i].ref.Alive()) g_known.erase(g_known.begin() + static_cast<std::ptrdiff_t>(i));
            else ++i;
        g_known.emplace_back();
        g_known.back().ref.Set(drone);
        k = &g_known.back();
        force = true;
    }
    const int step = StepOf(*energy);
    if (!force && k->active == active && k->step == step) return;
    k->active = active;
    k->step = step;
    SendState(drone, -1);
    if (active && !k->claimed) {
        coop::prop_drive_host::Claim(drone, "drone");
        k->claimed = true;
    } else if (!active && k->claimed) {
        coop::prop_drive_host::Release(drone);
        k->claimed = false;
    }
}

// ---- the gate --------------------------------------------------------------------------------------------------------
// CLIENT: only the host's drone hovers. HOST: after its tick, the energy step.
sg::Verdict OnTickPre(const sg::Call&) { return IsClient() ? sg::Verdict::Cancel : sg::Verdict::Run; }
void OnTickPost(const sg::Call& c) {
    if (IsHost() && c.object && g_session->connected()) Publish(c.object, false);
}

// CLIENT: the switch is the host's to judge.
sg::Verdict OnActionPre(const sg::Call& c) {
    if (!IsClient() || !c.object || !c.function || !c.locals) return sg::Verdict::Run;
    static R::ParamOffset s_action(L"action");
    const int32_t off = s_action.Of(c.function);
    if (off < 0 || c.locals[off] != kActSwitch) return sg::Verdict::Run;
    N::RdroneAskPayload p{};
    p.eid = EidOf(c.object);
    p.op = kOpSwitch;
    if (p.eid) {
        const bool ok = g_session->SendReliable(N::ReliableKind::RdroneAsk, &p, sizeof(p));
        UE_LOGI("rdrone_sync: CLIENT pressed drone eid %u's switch -- asked of the host (ok=%d)", p.eid, ok ? 1 : 0);
    }
    return sg::Verdict::Cancel;
}

// CLIENT: the flight is summed for the host; the yaw turns this copy's `look` too, so the pilot's camera answers now.
sg::Verdict OnMovePre(const sg::Call& c) {
    if (!IsClient() || !c.object || !c.function || !c.locals) return sg::Verdict::Run;
    static R::ParamOffset s_fwd(L"forward"), s_rot(L"rot"), s_up(L"up");
    const int32_t fo = s_fwd.Of(c.function), ro = s_rot.Of(c.function), uo = s_up.Of(c.function);
    if (fo < 0 || ro < 0 || uo < 0) return sg::Verdict::Cancel;
    float& fwd = *reinterpret_cast<float*>(c.locals + fo);
    float& up = *reinterpret_cast<float*>(c.locals + uo);
    const float rot = *reinterpret_cast<const float*>(c.locals + ro);
    Pending* pd = nullptr;
    for (Pending& e : g_pending)
        if (e.ref.Is(c.object)) pd = &e;
    if (!pd && g_pending.size() < 8) {
        g_pending.emplace_back();
        g_pending.back().ref.Set(c.object);
        pd = &g_pending.back();
    }
    if (pd && std::isfinite(fwd) && std::isfinite(rot) && std::isfinite(up)) {
        pd->forward += fwd;
        pd->rot += rot;
        pd->up += up;
    }
    fwd = 0.f;  // this copy adds no force: the host's drone flies
    up = 0.f;
    return sg::Verdict::Run;
}

// HOST: its switch or its energy changed. CLIENT: the body run for the host's state asks nothing.
void OnUpdPost(const sg::Call& c) {
    if (IsHost() && c.object && g_session->connected()) Publish(c.object, false);
}

// Every peer: its own player sat on a sled, or got up.
void OnSitPost(const sg::Call& c) {
    if (!c.object || !c.function || !c.locals) return;
    static R::ParamOffset s_player(L"player");
    const int32_t off = s_player.Of(c.function);
    void* const player = off >= 0 ? *reinterpret_cast<void**>(c.locals + off) : nullptr;
    if (!player || player != coop::players::Registry::Get().Local()) return;
    g_ridden.Set(c.object);
    UE_LOGI("rdrone_sync: this player sat on a sled -- its ride is this machine's to sync");
}
void OnUnsitPost(const sg::Call& c) {
    if (c.object && g_ridden.Is(c.object)) {
        g_ridden.Reset();
        UE_LOGI("rdrone_sync: this player got off its sled -- the sled syncs on until it rests");
    }
}

// CLIENT: the host's state on this copy, shown through the drone's own `upd`.
bool TakeState(const N::RdroneStatePayload& p) {
    void* const drone = EL::LiveActorOfType(static_cast<EL::ElementId>(p.eid), EL::ElementType::Prop);
    float* const energy = EnergyOf(drone);
    bool active = false;
    if (!IsDrone(drone) || !energy || !ReadActive(drone, active)) return false;
    *energy = p.energy;
    if (active != (p.active != 0)) {
        WriteActive(drone, p.active != 0);
        void* const upd = R::FindDispatchFunctionCached(R::ClassOf(drone), L"upd");
        g_applying = drone;
        const bool ran = upd && R::CallFunction(drone, upd, nullptr);
        g_applying = nullptr;
        UE_LOGI("rdrone_sync: CLIENT drone eid %u is %hs, as the host's (upd ran=%d)", p.eid, p.active ? "on" : "off",
                ran ? 1 : 0);
    }
    return true;
}

void SendPending() {
    const uint64_t now = NowMs();
    if (g_pending.empty() || now - g_lastSendMs < kSendMs) return;
    g_lastSendMs = now;
    for (Pending& pd : g_pending) {
        void* const drone = pd.ref.Get();
        if (!drone || (pd.forward == 0.f && pd.rot == 0.f && pd.up == 0.f)) continue;
        N::RdroneAskPayload p{};
        p.eid = EidOf(drone);
        p.op = kOpMove;
        p.forward = pd.forward;
        p.rot = pd.rot;
        p.up = pd.up;
        pd.forward = pd.rot = pd.up = 0.f;
        if (p.eid) g_session->SendReliable(N::ReliableKind::RdroneAsk, &p, sizeof(p));
    }
    for (size_t i = 0; i < g_pending.size();)
        if (!g_pending[i].ref.Alive()) g_pending.erase(g_pending.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
}

// Every peer: its own player's ride is its to sync while it lasts.
void ClaimRide() {
    void* const sled = g_ridden.Get();
    if (!sled) return;
    static R::InstanceOffset s_player(L"player");
    const int32_t off = s_player.Of(sled);
    void* const rider = off >= 0 ? *reinterpret_cast<void**>(static_cast<char*>(sled) + off) : nullptr;
    if (!rider || rider != coop::players::Registry::Get().Local()) {
        g_ridden.Reset();
        return;
    }
    coop::prop_syncer::Claim(sled, N::PropSyncReason::Ride);
}

float Clamp(float v) { return std::isfinite(v) ? (v > kMaxAxis ? kMaxAxis : v < -kMaxAxis ? -kMaxAxis : v) : 0.f; }

}  // namespace

void Install(N::Session* session) {
    g_session = session;
    if (g_watched) return;
    g_watched = true;
    const bool t = sg::WatchName(kRowTick, kTagTick, &OnTickPre, &OnTickPost);
    const bool a = sg::WatchName(kRowAction, kTagAction, &OnActionPre, nullptr);
    const bool m = sg::WatchName(kRowMove, kTagMove, &OnMovePre, nullptr);
    const bool u = sg::WatchName(kRowUpd, kTagUpd, nullptr, &OnUpdPost);
    const bool s = sg::WatchName(kRowSit, kTagSit, nullptr, &OnSitPost);
    const bool us = sg::WatchName(kRowUnsit, kTagUnsit, nullptr, &OnUnsitPost);
    if (!t || !a || !m || !u || !s || !us)
        UE_LOGE("rdrone_sync: a watch was refused (tick=%d action=%d move=%d upd=%d sit=%d unsit=%d)", t, a, m, u, s, us);
}

void Tick() {
    if (IsClient()) SendPending();
    if (IsHost() || IsClient()) ClaimRide();
    if (!g_watched || g_settled) return;
    sg::ResolvePendingNames();
    const struct { const ue_wrap::facts::Function* row; int tag; } rows[] = {
        {&kRowTick, kTagTick}, {&kRowAction, kTagAction}, {&kRowMove, kTagMove},
        {&kRowUpd, kTagUpd},   {&kRowSit, kTagSit},       {&kRowUnsit, kTagUnsit}};
    for (const auto& r : rows)
        if (!sg::NameWatchSettled(*r.row, r.tag)) return;
    g_settled = true;
    int live = 0;
    for (const auto& r : rows) live += sg::NameWatchLive(*r.row, r.tag) ? 1 : 0;
    if (live == 6) UE_LOGI("rdrone_sync: the watches on the drone and the sled are live");
    else UE_LOGE("rdrone_sync: %d of 6 watches on the drone and the sled are live -- one runs on each machine alone", live);
}

void SendJoinSnapshotForSlot(int slot) {
    if (!IsHost() || slot <= 0) return;
    const int n = ue_wrap::world_instances::Find(kDroneClass, g_drones, kCap);
    int sent = 0;
    for (int i = 0; i < n; ++i)
        if (IsDrone(g_drones[i]) && SendState(g_drones[i], slot)) ++sent;
    if (sent) UE_LOGI("rdrone_sync: HOST sent %d drones to joining slot %d", sent, slot);
}

void OnAsk(int senderSlot, const N::RdroneAskPayload& p) {
    if (!IsHost() || senderSlot <= 0 || senderSlot >= static_cast<int>(N::kMaxPeers)) return;
    void* const drone = EL::LiveActorOfType(static_cast<EL::ElementId>(p.eid), EL::ElementType::Prop);
    if (!IsDrone(drone)) {
        UE_LOGW("rdrone_sync: HOST slot %d asked op %u of eid %u, which is no drone here -- refused", senderSlot, p.op, p.eid);
        return;
    }
    const uint64_t now = NowMs();
    if (p.op == kOpMove) {
        if (now - g_lastAt[senderSlot] < kMoveGapMs) return;  // a flood of flight is dropped, not answered
        g_lastAt[senderSlot] = now;
        ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(drone), L"move"));
        if (!f.valid()) return;
        f.Set<float>(L"forward", Clamp(p.forward));
        f.Set<float>(L"rot", Clamp(p.rot));
        f.Set<float>(L"up", Clamp(p.up));
        ue_wrap::Call(drone, f);
        return;
    }
    if (now - g_lastSwitchAt[senderSlot] < 250) return;
    g_lastSwitchAt[senderSlot] = now;
    coop::RemotePlayer* const rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(senderSlot));
    void* const body = rp ? rp->actor() : nullptr;
    ue_wrap::FVector bodyAt{}, at{};
    const bool reach = body && E::TryGetActorLocation(body, bodyAt) && E::TryGetActorLocation(drone, at) &&
                       (bodyAt.X - at.X) * (bodyAt.X - at.X) + (bodyAt.Y - at.Y) * (bodyAt.Y - at.Y) +
                               (bodyAt.Z - at.Z) * (bodyAt.Z - at.Z) <= kReachCm * kReachCm;
    if (!reach) {
        UE_LOGW("rdrone_sync: HOST slot %d's switch of drone eid %u refused: its body is not within 6 m", senderSlot, p.eid);
        SendState(drone, senderSlot);
        return;
    }
    ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(drone), L"actionOptionIndex"));
    if (!f.valid()) return;
    f.Set<void*>(L"player", nullptr);
    f.Set<uint8_t>(L"action", kActSwitch);
    const bool ran = ue_wrap::Call(drone, f);  // its `upd` is watched: the state goes to every client
    UE_LOGI("rdrone_sync: HOST slot %d pressed drone eid %u's switch (ran=%d)", senderSlot, p.eid, ran ? 1 : 0);
}

void OnState(const N::RdroneStatePayload& p) {
    if (!IsClient() || TakeState(p)) return;
    for (N::RdroneStatePayload& h : g_held)
        if (h.eid == p.eid) {
            h = p;
            return;
        }
    if (g_held.size() < kMaxHeld) g_held.push_back(p);
}

void OnBound(uint32_t eid, void* actor) {
    if (!IsClient() || !actor || g_held.empty()) return;
    for (size_t i = 0; i < g_held.size(); ++i) {
        if (g_held[i].eid != eid) continue;
        const N::RdroneStatePayload h = g_held[i];
        g_held.erase(g_held.begin() + static_cast<std::ptrdiff_t>(i));
        TakeState(h);
        return;
    }
}

void OnClientWorldReady() {
    g_held.clear();
    g_pending.clear();
    g_ridden.Reset();
    g_applying = nullptr;
}

void OnDisconnect() {
    OnClientWorldReady();
    g_known.clear();
    for (uint64_t& t : g_lastAt) t = 0;
    for (uint64_t& t : g_lastSwitchAt) t = 0;
}

}  // namespace coop::rdrone_sync
