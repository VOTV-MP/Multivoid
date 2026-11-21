// coop/interactables/battery_charger_sync.cpp -- see coop/interactables/battery_charger_sync.h.

#include "coop/interactables/battery_charger_sync.h"

#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/class_resolve.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace coop::battery_charger_sync {
namespace {

namespace N  = coop::net;
namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
namespace sg = ue_wrap::script_gate;
using ue_wrap::FVector;

UE_FACT_FN(kRowTick, "battery_charger_sync", nullptr, L"ReceiveTick", AnyClass);
UE_FACT_FN(kRowAction, "battery_charger_sync", nullptr, L"actionOptionIndex", AnyClass);
constexpr int kTag = 0x42434847;   // 'BCHG'

constexpr uint8_t kActionUse = 4;          // the charger's press: actionOptionIndex's action 4 runs putIn
constexpr uint64_t kPollMs = 250;          // the host reads its chargers this often
constexpr uint64_t kChargeGapMs = 2000;    // a moving charge is told at most this often
constexpr float kChargeEps = 0.5f;         // and only once it has moved this much
constexpr uint64_t kParkMs = 10000;        // a row for a charger not bound here yet waits this long
constexpr float kReachCm = 400.f;          // a client's put or take names a charger this near its player

std::atomic<N::Session*> g_session{nullptr};

uint64_t Now() { return ::GetTickCount64(); }

bool IsHost() {
    N::Session* s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->role() == N::Role::Host;
}
bool IsClient() {
    N::Session* s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->role() != N::Role::Host;
}

// The two classes and the charger's fields, per world: a Blueprint class is a new object in every world.
struct Layout {
    uint32_t gen = 0xFFFFFFFFu;
    void* charger = nullptr;
    void* battery = nullptr;   // prop_batts_acc_C, the one battery a charger takes
    int32_t slot[N::kChargerSlots] = {-1, -1, -1, -1};
    int32_t plugged = -1, lookingAt = -1, powered = -1;
    uint8_t poweredMask = 0;
    int32_t energy = -1;       // the battery's
};
Layout g_l;

void Refresh() {
    const uint32_t gen = ue_wrap::world_identity::Generation();
    if (gen != g_l.gen) g_l = Layout{gen};
    if (!g_l.charger) {
        g_l.charger = OI::ClassByName(L"prop_batteryCharger_C");
        if (g_l.charger) {
            const wchar_t* const names[] = {L"b1", L"b2", L"b3", L"b4"};
            for (int i = 0; i < N::kChargerSlots; ++i) g_l.slot[i] = R::FindPropertyOffset(g_l.charger, names[i]);
            g_l.plugged = R::FindPropertyOffset(g_l.charger, L"plugged");
            g_l.lookingAt = R::FindPropertyOffset(g_l.charger, L"lookingAt");
            if (!R::FindBoolProperty(g_l.charger, L"powered", g_l.powered, g_l.poweredMask)) g_l.powered = -1;
        }
    }
    if (!g_l.battery) {
        g_l.battery = OI::ClassByName(L"prop_batts_acc_C");
        if (g_l.battery) g_l.energy = R::FindPropertyOffset(g_l.battery, L"energy");
    }
}

bool Ready() {
    for (int32_t o : g_l.slot)
        if (o < 0) return false;
    return g_l.charger && g_l.plugged >= 0 && g_l.lookingAt >= 0 && g_l.powered >= 0;
}

bool IsCharger(void* obj) {
    if (!obj || !g_l.charger) return false;
    void* const base = g_l.charger;
    return R::IsDescendantOfAny(R::ClassOf(obj), &base, 1, 8);
}

bool IsBattery(void* obj) {
    if (!obj || !g_l.battery) return false;
    void* const base = g_l.battery;
    return R::IsDescendantOfAny(R::ClassOf(obj), &base, 1, 8);
}

template <class T>
T& At(void* obj, int32_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(obj) + off); }

uint32_t EidOf(void* actor) {
    const coop::element::ElementId e = coop::element::Registry::Get().EidForActor(actor);
    return e == coop::element::kInvalidId ? 0u : static_cast<uint32_t>(e);
}

void* ActorOf(uint32_t eid) { return coop::element::LivePropActor(static_cast<coop::element::ElementId>(eid)); }

N::ChargerRow Read(void* ch, uint32_t eid) {
    N::ChargerRow r{};
    r.eid = eid;
    for (int i = 0; i < N::kChargerSlots; ++i) r.slot[i] = At<float>(ch, g_l.slot[i]);
    r.plugged = static_cast<int8_t>(std::clamp(At<int32_t>(ch, g_l.plugged), -1, 4));
    r.powered = (At<uint8_t>(ch, g_l.powered) & g_l.poweredMask) ? 1 : 0;
    return r;
}

bool CallNoArgs(void* obj, const wchar_t* fn) {
    void* const f = obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), fn) : nullptr;
    ue_wrap::ParamFrame frame(f);
    return f && frame.valid() && ue_wrap::Call(obj, frame);
}

void Write(void* ch, const N::ChargerRow& r) {
    for (int i = 0; i < N::kChargerSlots; ++i) At<float>(ch, g_l.slot[i]) = r.slot[i];
    At<int32_t>(ch, g_l.plugged) = r.plugged;
    uint8_t& b = At<uint8_t>(ch, g_l.powered);
    b = r.powered ? static_cast<uint8_t>(b | g_l.poweredMask) : static_cast<uint8_t>(b & ~g_l.poweredMask);
    CallNoArgs(ch, L"upd");   // the slots' meshes and lamps, as the game's own changes run it
}

std::vector<void*> Chargers() {
    std::vector<void*> out;
    if (g_l.charger)
        OI::ForEachInstance(g_l.charger, [](void* ctx, void* obj, int32_t) {
            if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__"))
                static_cast<std::vector<void*>*>(ctx)->push_back(obj);
        }, &out);
    return out;
}

// ---------------------------------------------------------------- HOST

struct Sent {
    N::ChargerRow row{};
    uint64_t at = 0;
    bool any = false;
};
std::unordered_map<void*, Sent> g_sent;
uint64_t g_nextPoll = 0;
uint32_t g_rowsSent = 0, g_puts = 0, g_takes = 0, g_refused = 0;

// Whether `now` is a row to send against what was told: a slot filled or emptied, plugged or powered changed, or a
// charge moved by kChargeEps once kChargeGapMs has passed.
bool Due(const Sent& s, const N::ChargerRow& now, uint64_t t) {
    if (!s.any) return true;
    if (s.row.plugged != now.plugged || s.row.powered != now.powered) return true;
    bool moved = false;
    for (int i = 0; i < N::kChargerSlots; ++i) {
        if ((s.row.slot[i] < 0.f) != (now.slot[i] < 0.f)) return true;
        if (std::fabs(s.row.slot[i] - now.slot[i]) >= kChargeEps) moved = true;
    }
    return moved && t - s.at >= kChargeGapMs;
}

void SendRows(N::Session& s, const std::vector<N::ChargerRow>& rows, int slot) {
    for (size_t i = 0; i < rows.size(); i += N::kChargerRowsMax) {
        N::ChargerStatePayload p{};
        p.count = static_cast<uint8_t>(std::min<size_t>(N::kChargerRowsMax, rows.size() - i));
        std::memcpy(p.rows, rows.data() + i, p.count * sizeof(N::ChargerRow));
        const bool ok = slot < 0 ? s.SendReliable(N::ReliableKind::ChargerState, &p, sizeof(p))
                                 : s.SendReliableToSlot(slot, N::ReliableKind::ChargerState, &p, sizeof(p));
        if (ok) g_rowsSent += p.count;
        else UE_LOGW("battery_charger_sync: %u charger row(s) were not sent", static_cast<unsigned>(p.count));
    }
}

void HostTick(N::Session& s, bool force) {
    const uint64_t t = Now();
    if (!force && t < g_nextPoll) return;
    g_nextPoll = t + kPollMs;
    if (!s.AnyWorldReadyPeer()) return;
    std::vector<N::ChargerRow> rows;
    for (void* ch : Chargers()) {
        const uint32_t eid = EidOf(ch);
        if (!eid) continue;
        const N::ChargerRow now = Read(ch, eid);
        Sent& sent = g_sent[ch];
        if (sent.any && sent.row.eid != eid) sent = Sent{};
        if (!Due(sent, now, t)) continue;
        sent.row = now;
        sent.at = t;
        sent.any = true;
        rows.push_back(now);
    }
    if (!rows.empty()) SendRows(s, rows, -1);
    if (g_sent.size() > 64)
        for (auto it = g_sent.begin(); it != g_sent.end();) it = R::IsLive(it->first) ? std::next(it) : g_sent.erase(it);
}

bool InReach(int slot, void* actor) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
    FVector p{}, a{};
    if (!rp || !rp->valid() || !rp->TryGetLocation(p) || !E::TryGetActorLocation(actor, a)) return false;
    const float x = p.X - a.X, y = p.Y - a.Y, z = p.Z - a.Z;
    return x * x + y * y + z * z <= kReachCm * kReachCm;
}

// A refused put's battery, beside the sender at its charge: its hand gave it up when the put ran on its copy.
void Refund(int slot, float charge) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
    FVector at{};
    if (!rp || !rp->valid() || !rp->TryGetLocation(at) || !g_l.battery) return;
    at.Z += 60.f;
    void* const a = E::BeginDeferredSpawn(g_l.battery, at, {});
    if (a && g_l.energy >= 0) At<float>(a, g_l.energy) = charge;
    if (a) E::FinishDeferredSpawn(a, at, {});
    UE_LOGI("battery_charger_sync: slot %d's refused battery spawned beside it at %.1f (%d)", slot, charge, a ? 1 : 0);
}

// ---------------------------------------------------------------- CLIENT

struct Parked {
    N::ChargerRow row;
    uint64_t until;
};
std::unordered_map<uint32_t, Parked> g_parked;
uint32_t g_applied = 0, g_asked = 0, g_given = 0;

bool ApplyRow(const N::ChargerRow& r) {
    void* const ch = ActorOf(r.eid);
    if (!ch || !IsCharger(ch)) return false;
    Write(ch, r);
    ++g_applied;
    return true;
}

void Ask(uint32_t eid, uint8_t op, uint8_t slot, float charge) {
    N::Session* s = g_session.load(std::memory_order_acquire);
    N::ChargerIntentPayload p{};
    p.eid = eid;
    p.op = op;
    p.slot = slot;
    p.charge = charge;
    if (s && s->SendReliable(N::ReliableKind::ChargerIntent, &p, sizeof(p))) ++g_asked;
    else UE_LOGW("battery_charger_sync: this client's %s of slot %u on charger eid=%u was not sent",
                 op == N::kChargerPut ? "put" : "take", static_cast<unsigned>(slot), eid);
}

// ---------------------------------------------------------------- the watches

void* PlayerOf(const sg::Call& c) {
    const int32_t off = c.locals ? R::FindParamOffset(c.function, L"player") : -1;
    void* p = nullptr;
    if (off >= 0) std::memcpy(&p, c.locals + off, sizeof(p));
    return p;
}

uint8_t ActionOf(const sg::Call& c) {
    const int32_t off = c.locals ? R::FindParamOffset(c.function, L"action") : -1;
    return off >= 0 ? c.locals[off] : 0xFF;
}

// A client's charger runs no charge of its own: the host's rows carry it.
sg::Verdict OnTickPre(const sg::Call& c) {
    if (!c.object || !IsClient() || !IsCharger(c.object)) return sg::Verdict::Run;
    return sg::Verdict::Cancel;
}

// A client's press on a charger: a put runs here as the game's does and is told, with the battery's charge read before
// it goes; a take is cancelled and asked.
sg::Verdict OnActionPre(const sg::Call& c) {
    if (!c.object || !IsClient() || !Ready() || !IsCharger(c.object) || ActionOf(c) != kActionUse) return sg::Verdict::Run;
    void* const player = PlayerOf(c);
    if (!player || !coop::players::Registry::Get().IsLocal(player)) return sg::Verdict::Run;
    const int32_t slot = At<int32_t>(c.object, g_l.lookingAt);
    if (slot < 0 || slot >= N::kChargerSlots) return sg::Verdict::Run;
    const uint32_t eid = EidOf(c.object);
    if (!eid) return sg::Verdict::Run;
    if (At<float>(c.object, g_l.slot[slot]) >= 0.f) {
        Ask(eid, N::kChargerTake, static_cast<uint8_t>(slot), 0.f);
        UE_LOGI("battery_charger_sync: this client asked to take slot %d of charger eid=%u", slot, eid);
        return sg::Verdict::Cancel;
    }
    E::MainPlayerGrabState gs{};
    void* const held = E::ReadMainPlayerGrabState(player, gs) ? gs.holdingActor : nullptr;
    if (!IsBattery(held) || g_l.energy < 0) return sg::Verdict::Run;   // the game's own hint: it needs a battery
    const float charge = std::clamp(At<float>(held, g_l.energy), 0.f, 100.f);
    Ask(eid, N::kChargerPut, static_cast<uint8_t>(slot), charge);
    UE_LOGI("battery_charger_sync: this client put a battery at %.1f into slot %d of charger eid=%u -- told the host",
            charge, slot, eid);
    return sg::Verdict::Run;
}

bool g_watchesRegistered = false, g_watchesSettled = false;

void SettleWatches() {
    if (g_watchesSettled) return;
    if (!g_watchesRegistered) {
        g_watchesRegistered = true;
        const bool a = sg::WatchName(kRowTick, kTag, &OnTickPre, nullptr);
        const bool b = sg::WatchName(kRowAction, kTag, &OnActionPre, nullptr);
        if (!a || !b) {
            g_watchesSettled = true;
            UE_LOGE("battery_charger_sync: the gate took no watch on %ls -- a client's chargers run on their own",
                    !a ? kRowTick.Name() : kRowAction.Name());
            return;
        }
    }
    sg::ResolvePendingNames();
    if (!sg::NameWatchSettled(kRowTick, kTag) || !sg::NameWatchSettled(kRowAction, kTag)) return;
    g_watchesSettled = true;
    if (sg::NameWatchLive(kRowTick, kTag) && sg::NameWatchLive(kRowAction, kTag))
        UE_LOGI("battery_charger_sync: the watches on %ls and %ls are live", kRowTick.Name(), kRowAction.Name());
    else UE_LOGE("battery_charger_sync: a watch settled dead -- a client's chargers run on their own");
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    N::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !s->connected()) return;
    SettleWatches();
    Refresh();
    if (!Ready()) return;
    if (s->role() == N::Role::Host) return HostTick(*s, false);
    if (g_parked.empty()) return;
    const uint64_t t = Now();
    for (auto it = g_parked.begin(); it != g_parked.end();)
        it = (ApplyRow(it->second.row) || t >= it->second.until) ? g_parked.erase(it) : std::next(it);
}

void OnPeerWorldReady(int slot) {
    N::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || !IsHost()) return;
    Refresh();
    if (!Ready()) return;
    std::vector<N::ChargerRow> rows;
    for (void* ch : Chargers())
        if (const uint32_t eid = EidOf(ch)) rows.push_back(Read(ch, eid));
    if (!rows.empty()) SendRows(*s, rows, slot);
    UE_LOGI("battery_charger_sync: slot %d's world is up -- %zu charger(s) sent to it", slot, rows.size());
}

void OnIntent(int slot, const coop::net::ChargerIntentPayload& p) {
    N::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || !IsHost()) return;
    Refresh();
    void* const ch = ActorOf(p.eid);
    const bool put = p.op == N::kChargerPut;
    const char* why = nullptr;
    if (!Ready()) why = "the charger's fields did not resolve";
    else if (!ch || !IsCharger(ch)) why = "the host holds no such charger";
    else if (!InReach(slot, ch)) why = "out of the sender's reach";
    else if (put && At<float>(ch, g_l.slot[p.slot]) >= 0.f) why = "the slot is full";
    else if (!put && At<float>(ch, g_l.slot[p.slot]) < 0.f) why = "the slot is empty";
    if (why) {
        ++g_refused;
        UE_LOGI("battery_charger_sync: slot %d's %s of slot %u on charger eid=%u refused: %s", slot, put ? "put" : "take",
                static_cast<unsigned>(p.slot), p.eid, why);
        if (put) Refund(slot, p.charge);
        g_sent.erase(ch);   // the sender's copy ran its own put: the host's row is owed to it again
        HostTick(*s, true);
        return;
    }
    int32_t& plugged = At<int32_t>(ch, g_l.plugged);
    float& charge = At<float>(ch, g_l.slot[p.slot]);
    float said = 0.f;   // the charge put in, or the charge the taken battery carries
    if (put) {
        charge = std::clamp(p.charge, 0.f, 100.f);
        said = charge;
        ++plugged;
        ++g_puts;
    } else {
        N::ChargerGivePayload g{};
        g.eid = p.eid;
        g.slot = p.slot;
        g.charge = std::clamp(charge, 0.f, 100.f);
        said = g.charge;
        charge = -1.f;
        --plugged;
        ++g_takes;
        if (!s->SendReliableToSlot(slot, N::ReliableKind::ChargerGive, &g, sizeof(g)))
            UE_LOGW("battery_charger_sync: the battery taken for slot %d was not sent to it", slot);
    }
    CallNoArgs(ch, L"upd");
    UE_LOGI("battery_charger_sync: slot %d's %s of slot %u on charger eid=%u ran here (charge %.1f)", slot,
            put ? "put" : "take", static_cast<unsigned>(p.slot), p.eid, said);
    HostTick(*s, true);
}

void OnState(const coop::net::ChargerStatePayload& p) {
    if (!IsClient()) return;
    Refresh();
    if (!Ready()) return;
    for (int i = 0; i < p.count; ++i)
        if (!ApplyRow(p.rows[i])) g_parked[p.rows[i].eid] = Parked{p.rows[i], Now() + kParkMs};
}

// The host's take for this client: the battery into this player's inventory, as the game's take spawns and stores it.
void OnGive(const coop::net::ChargerGivePayload& p) {
    if (!IsClient()) return;
    Refresh();
    void* const me = coop::players::Registry::Get().Local();
    FVector at{};
    if (!me || !g_l.battery || !E::TryGetActorLocation(me, at)) return;
    void* const a = E::BeginDeferredSpawn(g_l.battery, at, {});
    if (a && g_l.energy >= 0) At<float>(a, g_l.energy) = p.charge;
    void* const spawned = a && E::FinishDeferredSpawn(a, at, {}) ? a : nullptr;
    void* const fn = spawned ? R::FindDispatchFunctionCached(R::ClassOf(me), L"putObjectInventory2") : nullptr;
    ue_wrap::ParamFrame f(fn);
    const bool stored = fn && f.valid() && f.Set<void*>(L"inputPin", spawned) && f.Set<bool>(L"noNotify", false) &&
                        ue_wrap::Call(me, f);
    ++g_given;
    UE_LOGI("battery_charger_sync: the host gave this player slot %u's battery of charger eid=%u at %.1f (stored %d)",
            static_cast<unsigned>(p.slot), p.eid, p.charge, stored ? 1 : 0);
}

void OnDisconnect() {
    if (g_rowsSent || g_applied || g_asked)
        UE_LOGI("battery_charger_sync: session end -- host rows sent %u, puts %u, takes %u, refused %u; client rows "
                "applied %u, asks %u, given %u",
                g_rowsSent, g_puts, g_takes, g_refused, g_applied, g_asked, g_given);
    g_sent.clear();
    g_parked.clear();
    g_nextPoll = 0;
    g_rowsSent = g_puts = g_takes = g_refused = g_applied = g_asked = g_given = 0;
}

}  // namespace coop::battery_charger_sync
