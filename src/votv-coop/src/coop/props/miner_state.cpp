// coop/props/miner_state.cpp -- a rig's counts, powered flag, points and heat, sent by the host and applied by a client
// (MinerState op 0). See coop/props/miner_sync.h.

#include "coop/props/miner_sync.h"

#include "miner_internal.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/miner_bounds.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/actors/miner.h"
#include "ue_wrap/core/log.h"

#include <cmath>

namespace coop::miner_sync {
namespace {

namespace M  = ue_wrap::miner;
namespace EL = coop::element;
namespace N  = coop::net;

// The points and the heat alone are said at most once a second: they move with every tick of a mining rig.
constexpr uint64_t kDriftEveryMs = 1000;
// A heat that moved less than this is not a change.
constexpr float kHeatEpsilon = 0.01f;

// ---- the host: what was last said of each rig ----------------------------------------------------------------------
// A table of 256, found by the rig's slot and serial (a CachedObjRef), never walked for the world's rigs. An entry lasts as
// long as its rig does, and a place whose rig is gone is free again. A full table of live rigs takes no more: a rig left
// without a place is counted and nothing of it is said but a join snapshot and a granted use, so no rig is ever taken for
// a first sight twice and the table never makes a broadcast of its own.
constexpr size_t kMaxSeen = 256;
struct Seen {
    ue_wrap::CachedObjRef ref;
    M::Fields sent;
    uint64_t driftMs = 0;
    bool used = false;
};
Seen g_seen[kMaxSeen];
uint32_t g_seenFull = 0;
uint32_t g_sendFailed = 0;
uint32_t g_snapshotEidless = 0;

Seen* SeenOf(void* rig) {
    for (Seen& e : g_seen)
        if (e.used && e.ref.Is(rig)) return &e;
    return nullptr;
}

Seen* Claim(void* rig) {
    Seen* place = nullptr;
    for (Seen& e : g_seen) {
        if (e.used && !e.ref.Alive()) e = Seen{};
        if (!e.used && !place) place = &e;
    }
    if (!place) {
        const uint32_t n = ++g_seenFull;
        if ((n & (n - 1)) == 0)
            UE_LOGW("miner_sync: HOST watches %zu rigs already -- a rig beyond them is not watched (#%u)", kMaxSeen, n);
        return nullptr;
    }
    place->used = true;
    place->ref.Set(rig);
    return place;
}

bool Changed(const M::Fields& a, const M::Fields& b) {
    return a.gpus != b.gpus || a.slots != b.slots || a.fans != b.fans || a.powered != b.powered;
}

bool Drifted(const M::Fields& a, const M::Fields& b) {
    return a.points != b.points || std::fabs(a.overheat - b.overheat) > kHeatEpsilon;
}

bool SendFields(uint32_t eid, const M::Fields& f, int toSlot) {
    N::Session* const s = detail::SessionPtr();
    if (!s) return false;
    N::MinerPayload p{};
    p.eid = eid;
    p.op = N::kMinerOpState;
    p.pad = f.powered ? 1 : 0;
    p.gpus = f.gpus;
    p.slots = f.slots;
    p.fans = f.fans;
    p.points = f.points;
    p.overheat = f.overheat;
    const bool ok = toSlot >= 0
        ? s->SendReliableToSlot(static_cast<uint8_t>(toSlot), N::ReliableKind::MinerState, &p, sizeof(p))
        : s->SendReliable(N::ReliableKind::MinerState, &p, sizeof(p));
    if (!ok) detail::CountSendFailed();
    return ok;
}

// ---- the host: a state owed to an asker whose ask was refused ------------------------------------------------------
constexpr size_t kMaxOwed = 4;
struct Owed {
    uint32_t eids[kMaxOwed] = {};
    uint8_t n = 0;
};
coop::roster_ledger::PerSlotState<Owed> g_owed;
bool g_anyOwed = false;
uint32_t g_owedDropped = 0;

// ---- the client: the states waiting for their copy, and what was applied -------------------------------------------
// A state waits until its copy binds (OnBound applies it), is replaced by the host's next state of that rig, is the oldest
// of a full table, or the session ends. A state whose element never binds stays until one of the last three.
constexpr size_t kMaxPending = 32;
struct Pending {
    N::MinerPayload p{};
    uint32_t order = 0;
    bool used = false;
};
Pending g_pending[kMaxPending];
size_t g_pendingCount = 0;
uint32_t g_pendingOrder = 0;
uint32_t g_pendingReplaced = 0;

constexpr size_t kMaxApplied = 8;
struct AppliedSlot {
    uint32_t eid = 0;
    Applied a;
};
AppliedSlot g_applied[kMaxApplied];
size_t g_nextApplied = 0;
uint32_t g_appliedSeq = 0;

void NoteApplied(uint32_t eid, const M::Fields& f) {
    AppliedSlot* slot = nullptr;
    for (AppliedSlot& s : g_applied)
        if (s.eid == eid) slot = &s;
    if (!slot) {
        slot = &g_applied[g_nextApplied];
        g_nextApplied = (g_nextApplied + 1) % kMaxApplied;
    }
    slot->eid = eid;
    slot->a = Applied{++g_appliedSeq, f.gpus, f.slots, f.fans, f.points, f.overheat, f.powered};
}

// The state written onto the copy: the numbers, then the game's own repaints for what changed. The copy's tick stays
// refused, so nothing here mines.
bool Apply(void* rig, const N::MinerPayload& p) {
    M::Fields cur;
    if (!M::Read(rig, cur)) return false;
    M::Fields next;
    next.gpus = p.gpus;
    next.slots = p.slots;
    next.fans = p.fans;
    next.points = p.points;
    next.overheat = p.overheat;
    next.powered = p.pad != 0;
    if (!M::Write(rig, next)) return false;
    if (cur.gpus != next.gpus || cur.slots != next.slots || cur.fans != next.fans) M::CallUpd(rig);
    if (cur.powered != next.powered) M::CallPlugged(rig);
    const bool first = [&] {
        for (const AppliedSlot& s : g_applied)
            if (s.eid == p.eid) return false;
        return true;
    }();
    NoteApplied(p.eid, next);
    if (first || Changed(cur, next) || (next.points == 0 && cur.points != 0))
        UE_LOGI("miner_sync: CLIENT applied state eid %u: %d GPUs in %d slots, %d coolers, points %d, powered %d", p.eid,
                next.gpus, next.slots, next.fans, next.points, next.powered ? 1 : 0);
    return true;
}

Pending* PendingOf(uint32_t eid) {
    for (Pending& e : g_pending)
        if (e.used && e.p.eid == eid) return &e;
    return nullptr;
}

void RetirePending(Pending* e) {
    if (!e || !e->used) return;
    *e = Pending{};
    --g_pendingCount;
}

}  // namespace

namespace detail {

uint32_t EidOf(void* rig) {
    const EL::ElementId eid = EL::Registry::Get().EidForActor(rig);
    return eid == EL::kInvalidId ? 0u : static_cast<uint32_t>(eid);
}

void* RigOfEid(uint32_t eid) {
    if (eid == 0) return nullptr;
    void* const rig = EL::LiveActorOfType(static_cast<EL::ElementId>(eid), EL::ElementType::Prop);
    return M::IsMiner(rig) ? rig : nullptr;
}

void CountSendFailed() {
    const uint32_t n = ++g_sendFailed;
    if ((n & (n - 1)) == 0) UE_LOGW("miner_sync: a send failed (#%u)", n);
}

void SendState(void* rig, int toSlot) {
    M::Fields now;
    const uint32_t eid = EidOf(rig);
    if (!eid || !M::Read(rig, now)) return;
    SendFields(eid, now, toSlot);
    if (toSlot >= 0) return;
    if (Seen* e = SeenOf(rig)) {
        e->sent = now;
        e->driftMs = N::NowMs();
    }
}

// A refused ask is owed the host's own state of the rig, which corrects what the client's copy predicted: MTA sends the
// server's value back to a client whose write it cancelled (MTA CGame.cpp:2775-2790).
void OweState(uint8_t slot, uint32_t eid) {
    if (slot == 0 || eid == 0) return;
    Owed& o = g_owed[slot];
    for (uint8_t i = 0; i < o.n; ++i)
        if (o.eids[i] == eid) return;
    if (o.n >= kMaxOwed) {
        const uint32_t n = ++g_owedDropped;
        if ((n & (n - 1)) == 0) UE_LOGW("miner_sync: HOST owes slot %u a fifth rig's state at once -- not kept (#%u)", slot, n);
        return;
    }
    o.eids[o.n++] = eid;
    g_anyOwed = true;
}

void FlushOwed() {
    if (!g_anyOwed) return;
    g_anyOwed = false;
    for (int slot = 1; slot < g_owed.size(); ++slot) {
        Owed& o = g_owed[slot];
        for (uint8_t i = 0; i < o.n; ++i)
            if (void* rig = RigOfEid(o.eids[i])) SendState(rig, slot);
        o.n = 0;
    }
}

// The end of a rig's own tick, which runs about every 217 ms for each rig: a table lookup and a compare of six numbers,
// nothing else while the rig is as it was said.
void OnRigTickPost(void* rig) {
    N::Session* const s = SessionPtr();
    if (!s || !IsHost()) return;
    M::Fields now;
    if (!M::Read(rig, now)) return;
    Seen* e = SeenOf(rig);
    if (!e) {
        const uint32_t eid = EidOf(rig);
        if (!eid) return;  // no element id yet: nothing is latched, the next tick looks again
        e = Claim(rig);
        if (!e) return;  // a full table: counted, and nothing is said of this rig
        e->sent = now;
        e->driftMs = N::NowMs();
        if (s->AnyWorldReadyPeer()) SendFields(eid, now, -1);
        return;
    }
    if (RecordsWait()) NoteRecordSeen(rig, now);
    const bool counts = Changed(now, e->sent);
    if (!counts) {
        if (!Drifted(now, e->sent)) return;
        const uint64_t t = N::NowMs();
        if (t - e->driftMs < kDriftEveryMs) return;
    }
    if (!s->AnyWorldReadyPeer()) {  // a joiner is shown every rig at its ready edge, so nothing is sent to nobody
        e->sent = now;
        e->driftMs = N::NowMs();
        return;
    }
    const uint32_t eid = EidOf(rig);
    if (!eid) return;
    SendFields(eid, now, -1);
    e->sent = now;
    e->driftMs = N::NowMs();
}

void SendSnapshot(int slot) {
    static const bool withheld = coop::config::ResolveFlag(coop::config_registry::rows::miner_snapshot_off);
    if (!IsHost() || slot < 0) return;
    if (withheld) {
        UE_LOGI("miner_sync: HOST snapshot withheld from slot %d (miner_snapshot_off)", slot);
        return;
    }
    struct Walk {
        int slot;
        uint32_t sent = 0, eidless = 0;
    } w{slot};
    M::ForEachRig([](void* ctx, void* rig) {
        Walk* walk = static_cast<Walk*>(ctx);
        M::Fields f;
        if (!M::Read(rig, f)) return;
        const uint32_t eid = EidOf(rig);
        if (!eid) {
            ++walk->eidless;
            return;
        }
        if (SendFields(eid, f, walk->slot)) ++walk->sent;
    }, &w);
    g_snapshotEidless += w.eidless;
    UE_LOGI("miner_sync: HOST snapshot to slot %d: %u rigs sent, %u without an element id skipped", slot, w.sent, w.eidless);
}

void ApplyOrPark(const N::MinerPayload& p) {
    if (void* const rig = RigOfEid(p.eid)) {
        if (Apply(rig, p)) {
            if (g_pendingCount > 0) RetirePending(PendingOf(p.eid));
            return;
        }
    }
    if (Pending* e = PendingOf(p.eid)) {
        e->p = p;
        e->order = ++g_pendingOrder;
        return;
    }
    Pending* slot = nullptr;
    for (Pending& e : g_pending)
        if (!e.used) {
            slot = &e;
            break;
        }
    if (!slot) {  // the host's state is its latest and it says it again at any change: the oldest waiting one gives way
        slot = &g_pending[0];
        for (Pending& e : g_pending)
            if (e.order < slot->order) slot = &e;
        const uint32_t n = ++g_pendingReplaced;
        if ((n & (n - 1)) == 0)
            UE_LOGW("miner_sync: CLIENT waits on %zu rigs' states already -- the oldest is replaced (#%u)", kMaxPending, n);
        RetirePending(slot);
    }
    slot->used = true;
    slot->p = p;
    slot->order = ++g_pendingOrder;
    ++g_pendingCount;
}

// A prop copy was bound to element id `eid` (coop/props/remote_prop's bind funnel): the state waiting for it is applied to
// that copy. A copy whose fields do not read yet keeps the state waiting for the host's next one.
void OnBound(uint32_t eid, void* actor) {
    if (g_pendingCount == 0 || !actor) return;
    Pending* const e = PendingOf(eid);
    if (e && Apply(actor, e->p)) RetirePending(e);
}

void ClearState() {
    if (g_sendFailed || g_seenFull || g_pendingReplaced || g_snapshotEidless)
        UE_LOGI("miner_sync: session end -- sends failed %u, rig ticks past a full table %u, waiting states replaced %u, rigs "
                "skipped by a snapshot for no element id %u", g_sendFailed, g_seenFull, g_pendingReplaced, g_snapshotEidless);
    for (Seen& e : g_seen) e = Seen{};
    for (Pending& e : g_pending) e = Pending{};
    for (AppliedSlot& a : g_applied) a = AppliedSlot{};
    g_seenFull = g_sendFailed = g_snapshotEidless = 0;
    g_pendingCount = 0;
    g_pendingOrder = g_pendingReplaced = 0;
    g_nextApplied = 0;
    g_appliedSeq = 0;
    g_anyOwed = false;
    g_owedDropped = 0;
    for (int slot = 0; slot < g_owed.size(); ++slot) g_owed[slot] = Owed{};
}

}  // namespace detail

Applied LastApplied(uint32_t eid) {
    for (const AppliedSlot& s : g_applied)
        if (s.eid == eid && eid != 0) return s.a;
    return Applied{};
}

uint32_t SnapshotEidless() { return g_snapshotEidless; }

bool HostOwesState(uint8_t slot) { return g_owed[slot].n > 0; }

}  // namespace coop::miner_sync
