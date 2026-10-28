// coop/interactables/drive_sync.cpp -- see coop/interactables/drive_sync.h.
//
// The slot lane (the rack lane lives in drive_rack_sync, a drive's row in drive_payload_sync):
//   DriveSlotState -- idempotent any-peer slot FSM lines, host canonical.
// Detection = verb dirty-marks at the script-body gate (capture-only, barrier
// emission) + a 1 Hz diff-gated sweep. Apply+prime is GT-atomic.
// This module OWNS the verb watches for the slot and rack chain (putDriveIn is
// a shared slot/rack context) and forwards rack marks to
// drive_rack_sync::MarkDirtyFromVerb().

#include "coop/interactables/drive_sync.h"

#include "coop/element/bind_wait.h"
#include "coop/element/registry.h"
#include "coop/interactables/desk_snd_fx.h"   // ScopedWireApply (the shared desk wire guard)
#include "coop/interactables/drive_rack_sync.h"  // MarkDirtyFromVerb (owner API)
#include "coop/net/session.h"
#include "coop/props/remote_prop.h"           // EndAnyHoldOn: an insert ends the hold on the drive
#include "coop/session/net_pump.h"            // IsInAnnouncedWorld: a client's own edges are its world's

#include "ue_wrap/actors/prop.h"  // IsFrozen, CallAwakeUnfreeze: a conflicting drive's eject
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/desk/drive_chain.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

namespace coop::drive_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace DC = ue_wrap::drive_chain;
namespace sg = ue_wrap::script_gate;
using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};

// ---- the watch tags ----
constexpr int kVerbPutDriveIn = 1;   // driveSlot OR rack context (ctx discriminates)
constexpr int kVerbPulledOut  = 2;
constexpr int kVerbRackTake   = 4;   // getDrive

bool g_verbsRegistered = false;

// ---- dirty marks (set in the VM bracket -- relaxed atomics, drained at Tick) ----
std::atomic<bool> g_slotDirty[DC::kRoleCount] = {};
// Eject capture: at drivePulledOut ENTRY slot.drive is STILL SET -- the occupant is kept, and the empty
// line names it by the eid it has when the line is sent (the latch completion needs it, and the host
// takes an eject only for the drive its own slot holds): a bind landing between the verb and the drain
// is then already in the name. Game thread, like the watch that sets it.
ue_wrap::CachedObjRef g_lastEjected[DC::kRoleCount];

// ---- baselines ----
// The drive is held as well as its eid: the same drive under a new eid is a re-key (the reconcile binding the drive a
// load seated), not a slot edge.
struct SlotBase {
    bool known = false;
    bool occupied = false;
    uint32_t eid = 0;
    ue_wrap::CachedObjRef drive;
};
SlotBase g_slotBase[DC::kRoleCount];

void SetBase(int role, bool occupied, uint32_t eid, void* drive) {
    SlotBase& b = g_slotBase[role];
    b.known = true;
    b.occupied = occupied;
    b.eid = eid;
    b.drive.Set(occupied ? drive : nullptr);
}

// An eid for a log line: a drive the registry has not bound reads "unbound".
const char* EidText(uint32_t eid, char (&buf)[16]) {
    if (eid == coop::element::kInvalidId) return "unbound";
    std::snprintf(buf, sizeof(buf), "%u", eid);
    return buf;
}

bool g_primed = false;
bool g_wasConnected = false;

// ---- held lines (drive actor not resolvable yet -- spawn in flight), waiting in bind_wait ----
// A role's generation moves at every place its held lines go stale: a newer insert line, an eject, a move of the
// slot's base (an edit, a restore, an applied insert). A line is held with the generation it was held under and
// replays only if the role still carries it.
constexpr uint16_t kDriveSlotState = static_cast<uint16_t>(coop::net::ReliableKind::DriveSlotState);
struct HeldSlotLine {
    coop::net::DriveSlotStatePayload line;
    uint32_t roleGen;
};
uint32_t g_roleGen[DC::kRoleCount] = {};

// SetBase for a move that is not a re-key, voiding the role's held lines when the base changes.
void MoveBase(int role, bool occupied, uint32_t eid, void* drive) {
    const SlotBase& b = g_slotBase[role];
    if (!b.known || b.occupied != occupied || b.eid != eid) ++g_roleGen[role];   // a stale base moved: held lines are older
    SetBase(role, occupied, eid, drive);
}

// ---- cadence ----
Clock::time_point g_nextSweep{};
Clock::time_point g_nextStats{};

// ---- counters (60 s line -- dead matchers visible) ----
std::atomic<uint64_t> g_cMarksSlot{0};
uint64_t g_cSlotSent = 0, g_cSlotApplied = 0;
uint64_t g_cLatchCompleted = 0;
uint64_t g_cGrabKept = 0;  // this player's grab left alone inside a replayed insert
uint64_t g_cPrimedOffWorld = 0;  // CLIENT: slot edges outside the world it announced, primed
uint64_t g_cRekeyed = 0;         // a slot's drive under a new eid, primed
uint64_t g_cEjectRefused = 0;    // HOST: client ejects naming another drive, answered
std::set<uint32_t> g_wrongClassSaid;  // eids a slot line named that are no drive here (said once a session)

// ---- the local player is never the subject of a replayed insert ----
// The slot's putDriveIn, and the eraser's handler of the slot's driveIn, end the grab of the machine's
// own player (getMainPlayer()->dropGrabObject()): in single player that is the inserter, letting go of
// the drive the slot takes. A replayed insert is another player's, so while one runs here this
// player's dropGrabObject is refused and its grab stays. The scope is the replay's own, not the gate's
// IsBodyActive(putDriveIn): this player's own insert runs the same body, and there the grab must end.
// It restores what it found, so a nested replay cannot close an outer one. Game thread.
constexpr int kReplayDropTag = 0x44524750;  // 'DRGP'
UE_FACT_FN(kRowPutDriveIn, "drive_sync", nullptr, L"putDriveIn", AnyClass);
UE_FACT_FN(kRowDrivePulledOut, "drive_sync", nullptr, L"drivePulledOut", AnyClass);
UE_FACT_FN(kRowGetDrive, "drive_sync", nullptr, L"getDrive", AnyClass);
UE_FACT_FN(kRowDropGrabObject, "drive_sync", L"mainPlayer_C", L"dropGrabObject", Declared);
bool g_replayingInsert = false;
struct ReplayingInsert {
    ReplayingInsert() : outer_(g_replayingInsert) { g_replayingInsert = true; }
    ~ReplayingInsert() { g_replayingInsert = outer_; }
    ReplayingInsert(const ReplayingInsert&) = delete;
    ReplayingInsert& operator=(const ReplayingInsert&) = delete;
private:
    bool outer_;
};

// The lane's own apply mirrors a slot by calling the watched verbs; a mark on it would stash a stale eject id and
// re-announce what was just applied. Scoped to this lane's own calls: another caller of ours, a drill standing in for a
// player, marks like the game.
bool g_laneApplying = false;
struct LaneApply {
    LaneApply() : outer_(g_laneApplying) { g_laneApplying = true; }
    ~LaneApply() { g_laneApplying = outer_; }
    LaneApply(const LaneApply&) = delete;
    LaneApply& operator=(const LaneApply&) = delete;
private:
    bool outer_;
};

sg::Verdict OnDropGrabPre(const sg::Call&) {
    if (!g_replayingInsert) return sg::Verdict::Run;
    if (g_cGrabKept++ == 0)
        UE_LOGI("drive_sync: a replayed insert left this player's grab alone -- its dropGrabObject refused (first "
                "refusal; the rest are counted)");
    return sg::Verdict::Cancel;
}

// --------------------------------------------------------------------------
// helpers

bool IsHost() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Host;
}

// eid -> live actor: coop::element::LivePropActor (O(1) Registry::Get; the
// promoted canonical resolve idiom).
using coop::element::LivePropActor;

// --------------------------------------------------------------------------
// the verb watches (capture-only: relaxed marks + one stashed eid read)

sg::Verdict OnVerbEntry(const sg::Call& b) {
    if (g_laneApplying) return sg::Verdict::Run;
    switch (b.tag) {
        case kVerbPutDriveIn: {
            const int role = DC::RoleOfSlotActor(b.object);
            if (role >= 0) {
                g_slotDirty[role].store(true, std::memory_order_relaxed);
                g_cMarksSlot.fetch_add(1, std::memory_order_relaxed);
            } else if (DC::IsRackClass(R::ClassOf(b.object))) {
                coop::drive_rack_sync::MarkDirtyFromVerb();
            }
            break;
        }
        case kVerbPulledOut: {
            const int role = DC::RoleOfSlotActor(b.object);
            if (role >= 0) {
                g_lastEjected[role].Set(DC::SlotDrive(b.object));  // still set at ENTRY (measured)
                g_slotDirty[role].store(true, std::memory_order_relaxed);
                g_cMarksSlot.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        }
        case kVerbRackTake:
            if (DC::IsRackClass(R::ClassOf(b.object)))
                coop::drive_rack_sync::MarkDirtyFromVerb();
            break;
        default: break;
    }
    return sg::Verdict::Run;
}

// --------------------------------------------------------------------------
// slot lane

coop::net::DriveSlotStatePayload Line(int role, bool occupied, uint32_t eid) {
    coop::net::DriveSlotStatePayload p{};
    p.role = static_cast<uint8_t>(role);
    p.occupied = occupied ? 1 : 0;
    p.censusIdx = 0;
    p.driveEid = eid;
    return p;
}

// This peer's own edge: a client's to the host, the host's to every client.
void AnnounceSlot(int role, bool occupied, uint32_t eid) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    const coop::net::DriveSlotStatePayload p = Line(role, occupied, eid);
    s->SendReliable(coop::net::ReliableKind::DriveSlotState, &p, sizeof(p));
    ++g_cSlotSent;
    char buf[16];
    UE_LOGI("drive_sync: slot role=%d %s eid=%s announced", role, occupied ? "INSERT" : "EJECT", EidText(eid, buf));
}

// HOST: a client's line it accepted, to every other client whose world is ready; a joiner still loading gets the slot
// in its seed. A client's line is never relayed as it arrives, since the host judges it first: MTA relays an accepted
// element-data change to all but its source (CGame.cpp:2761-2768).
void RelaySlot(int role, bool occupied, uint32_t eid, uint8_t sourceSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    const coop::net::DriveSlotStatePayload p = Line(role, occupied, eid);
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        if (slot == sourceSlot || !s->IsSlotWorldReady(slot)) continue;
        s->SendReliableToSlot(slot, coop::net::ReliableKind::DriveSlotState, &p, sizeof(p));
    }
}

// HOST: the slot as it stands, to the source of a line it refused, whom no other client heard: MTA answers a refused
// element-data change to its source alone (CGame.cpp:2779-2793).
void AnswerSlot(int role, uint8_t sourceSlot, bool occupied, uint32_t eid) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    const coop::net::DriveSlotStatePayload p = Line(role, occupied, eid);
    s->SendReliableToSlot(sourceSlot, coop::net::ReliableKind::DriveSlotState, &p, sizeof(p));
}

// Read a slot's live state; diff vs baseline; announce the edge. `announce`
// false = prime-only (the connect seed).
//
// Two changes are primed rather than announced, because neither is anyone's edit of the slot. A client's slot outside
// the world it announced world-ready in -- before the announce, and while a world change waits for its re-announce --
// is the host's save being restored: the menu join connects at the menu, before any slot exists, and the host's seed
// converges the loaded slots at world-ready. And the drive a slot holds under a new eid is the reconcile binding it,
// which changes only the eid's spelling: the role's held lines still replay, since its generation does not move.
void ProcessSlot(int role, bool announce) {
    void* slot = DC::SlotActor(role);
    if (!slot) return;
    void* drive = DC::SlotDrive(slot);
    const uint32_t eid = drive ? static_cast<uint32_t>(
        coop::element::Registry::Get().EidForActor(drive)) : 0;
    const bool occupied = drive != nullptr;
    SlotBase& b = g_slotBase[role];
    if (b.known && b.occupied == occupied && b.eid == eid) return;
    void* ejected = g_lastEjected[role].Get();
    g_lastEjected[role].Reset();
    const uint32_t ejectEid = ejected ? static_cast<uint32_t>(
        coop::element::Registry::Get().EidForActor(ejected)) : 0;
    const bool rekey = b.known && b.occupied && occupied && b.drive.Is(drive);
    const uint32_t was = b.eid;
    SetBase(role, occupied, eid, drive);
    if (!rekey) ++g_roleGen[role];   // a local edge or a host-save restore moved the slot: its held lines are older
    if (!announce) return;
    char a[16], z[16];
    if (rekey) {
        if (++g_cRekeyed <= 20)
            UE_LOGI("drive_sync: slot role=%d's drive re-keyed eid=%s -> %s -- primed, not an edge", role,
                    EidText(was, a), EidText(eid, z));
        return;
    }
    if (!IsHost() && !coop::net_pump::IsInAnnouncedWorld(slot)) {
        if (++g_cPrimedOffWorld <= 20)
            UE_LOGI("drive_sync: slot role=%d %s eid=%s outside the announced world -- the host's save being "
                    "restored, primed", role, occupied ? "holds" : "emptied", EidText(occupied ? eid : ejectEid, a));
        return;
    }
    AnnounceSlot(role, occupied, occupied ? eid : ejectEid);
}

void OnSlotLine(const coop::net::DriveSlotStatePayload& p, uint8_t senderSlot);

// A slot line held in bind_wait runs again once its drive is bound -- unless the role's generation moved on since
// it was held, when the world passed it by and it is dropped, never replayed stale.
void ReplaySlotLine(const uint8_t* bytes, size_t n, int senderSlot) {
    HeldSlotLine held{};
    if (n != sizeof held) return;
    std::memcpy(&held, bytes, sizeof held);
    if (held.line.role >= DC::kRoleCount) return;
    if (g_roleGen[held.line.role] != held.roleGen) {
        UE_LOGW("drive_sync: held slot line role=%u dropped (a newer line or move for this role came since)",
                held.line.role);
        return;
    }
    OnSlotLine(held.line, static_cast<uint8_t>(senderSlot));
}

// --------------------------------------------------------------------------
// slot line apply

void OnSlotLine(const coop::net::DriveSlotStatePayload& p, uint8_t senderSlot) {
    if (p.role >= DC::kRoleCount) return;
    void* slot = DC::SlotActor(p.role);
    if (!slot) return;
    void* cur = DC::SlotDrive(slot);
    const uint32_t curEid = cur ? static_cast<uint32_t>(
        coop::element::Registry::Get().EidForActor(cur)) : 0;

    if (p.occupied) {
        if (cur && curEid == p.driveEid) {  // already true: prime-only no-op
            MoveBase(p.role, true, curEid, cur);
            return;
        }
        void* drive = LivePropActor(p.driveEid);
        if (!drive) {
            // Held for the drive's bind, with the role's generation after this line's own bump: a newer line or
            // a move of the slot since voids it at replay -- never resurrect a stale occupant. This is the one
            // module that holds several lines per eid (coalesce=false), each judged by its own role's generation;
            // the others keep the latest state of an eid. Not deferred: no bind is coming for this eid, so there
            // is nothing to wait for.
            ++g_roleGen[p.role];
            HeldSlotLine held{};
            held.line = p;
            held.roleGen = g_roleGen[p.role];
            if (!coop::element::bind_wait::Hold(p.driveEid, kDriveSlotState, senderSlot, &ReplaySlotLine, &held,
                                                sizeof held, /*coalesce=*/false))
                coop::element::bind_wait::NoteNeverBound(kDriveSlotState, p.driveEid);
            return;
        }
        if (!DC::IsDriveClass(R::ClassOf(drive))) {
            // A live actor of another class: an identity defect to see, not a wait.
            if (g_wrongClassSaid.insert(p.driveEid).second)
                UE_LOGW("drive_sync: slot role=%u line names eid=%u, a live actor that is not a prop_drive_C -- "
                        "dropped", p.role, p.driveEid);
            return;
        }
        if (cur && curEid != p.driveEid) {
            // Conflict: locally captured a DIFFERENT drive. The HOST is
            // canonical: it answers its source with its state; a client converges to
            // the incoming line (eject ours, insert theirs).
            if (IsHost()) {
                AnswerSlot(p.role, senderSlot, true, curEid);
                return;
            }
            coop::desk_snd_fx::ScopedWireApply guard;
            LaneApply apply;
            DC::CallDrivePulledOut(slot);
            // Or it stays frozen in the port beside the new one. No grab took it out, so no hold of
            // the prop lane will unfreeze it; a drive already free needs nothing.
            if (ue_wrap::prop::IsFrozen(cur)) ue_wrap::prop::CallAwakeUnfreeze(cur);
            DC::CompleteEjectLatch(slot, cur);
            ++g_cLatchCompleted;
        }
        {
            coop::desk_snd_fx::ScopedWireApply guard;
            // The slot takes the drive out of whoever's hand held it, ending that hold: closed
            // here, so a pose of it still in flight cannot pull the drive back out of the slot.
            coop::remote_prop::EndAnyHoldOn(drive);
            ReplayingInsert replaying;
            LaneApply apply;
            DC::CallPutDriveIn(slot, drive);
            MoveBase(p.role, true, p.driveEid, drive);
        }
        ++g_cSlotApplied;
        if (IsHost()) RelaySlot(p.role, true, p.driveEid, senderSlot);
        UE_LOGI("drive_sync: slot role=%u INSERT eid=%u applied (from slot %u)",
                p.role, p.driveEid, senderSlot);
    } else {
        // A client's eject names the drive it saw leave its copy of the slot. One naming another drive, or none, is a
        // stale view of this slot and not an edit of it: the host keeps its drive, and the role's held lines with
        // it, and answers the source with the slot as it stands.
        if (IsHost() && cur && p.driveEid != curEid) {
            ++g_cEjectRefused;
            char a[16], z[16];
            UE_LOGI("drive_sync: slot role=%u eject from slot %u names eid=%s, not this slot's drive eid=%s -- refused, "
                    "the slot answered to its source", p.role, senderSlot, EidText(p.driveEid, a), EidText(curEid, z));
            AnswerSlot(p.role, senderSlot, true, curEid);
            return;
        }
        // An eject voids the role's held insert lines, the already-empty one as much as the rest: the slot's base
        // can read the same after an eject as when the insert was held, so a stale insert must not put the
        // drive back in a slot it has left.
        ++g_roleGen[p.role];
        if (!cur) {  // already empty: prime + belt latch completion
            SetBase(p.role, false, 0, nullptr);
            DC::CompleteEjectLatch(slot, LivePropActor(p.driveEid));
            return;
        }
        // A slot freezes the drive it takes, and in the game only a grab takes one out: the drive's
        // playerTryToGrab ejects it, and the grab's playerGrabbed_pre unfreezes it. The line is
        // that grab on another machine, and the grab's own hold reaches the drive through the prop
        // lane: its first pose runs the same unfreeze here, its release leaves the holder's flags.
        bool frozen = false;
        {
            coop::desk_snd_fx::ScopedWireApply guard;
            LaneApply apply;
            DC::CallDrivePulledOut(slot);
            frozen = ue_wrap::prop::IsFrozen(cur);  // 0 once the hold's first pose has unfrozen it
            DC::CompleteEjectLatch(slot, cur);
            SetBase(p.role, false, 0, nullptr);
        }
        ++g_cSlotApplied;
        ++g_cLatchCompleted;
        if (IsHost()) RelaySlot(p.role, false, curEid, senderSlot);
        UE_LOGI("drive_sync: slot role=%u EJECT applied (was eid=%u, from slot %u, frozen=%d)",
                p.role, curEid, senderSlot, frozen ? 1 : 0);
    }
}

void PrimeAll() {
    for (int r = 0; r < DC::kRoleCount; ++r) {
        g_slotBase[r] = {};
        g_lastEjected[r].Reset();
        ProcessSlot(r, /*announce*/false);
    }
}

}  // namespace

// --------------------------------------------------------------------------

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_verbsRegistered) return;
    if (!DC::EnsureResolved()) return;
    // One cb serves all three verb names; the tags discriminate. putDriveIn
    // covers BOTH the slot FSM and the rack (the context's class discriminates).
    const bool ok =
        sg::WatchName(kRowPutDriveIn,     kVerbPutDriveIn, &OnVerbEntry, nullptr) &&
        sg::WatchName(kRowDrivePulledOut, kVerbPulledOut,  &OnVerbEntry, nullptr) &&
        sg::WatchName(kRowGetDrive,       kVerbRackTake,   &OnVerbEntry, nullptr) &&
        sg::WatchName(kRowDropGrabObject, kReplayDropTag, &OnDropGrabPre, nullptr);
    if (ok) {
        g_verbsRegistered = true;
        UE_LOGI("drive_sync: 3 verb watches live (dirty-marks armed at the script-body gate), and the replayed "
                "insert's grab guard");
    }
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (!DC::EnsureResolved()) return;
    if (!g_verbsRegistered) Install(s);
    sg::ResolvePendingNames();

    // Connect seed: (re-)prime silently at the connected rising edge so the
    // state AT connect is the ground truth (no pre-connect edge storms).
    const bool conn = s->connected();
    if (!g_primed || (conn && !g_wasConnected)) {
        PrimeAll();
        g_primed = true;
        UE_LOGI("drive_sync: baselines primed (%s)", conn ? "connect seed" : "boot");
    }
    g_wasConnected = conn;
    if (!conn) return;

    // Barrier drain: verb dirty-marks -> immediate diff-gated processing.
    for (int r = 0; r < DC::kRoleCount; ++r)
        if (g_slotDirty[r].exchange(false, std::memory_order_relaxed))
            ProcessSlot(r, /*announce*/true);

    const auto now = Clock::now();
    if (now >= g_nextSweep) {  // the 1 Hz slot sweep
        g_nextSweep = now + std::chrono::seconds(1);
        for (int r = 0; r < DC::kRoleCount; ++r) ProcessSlot(r, /*announce*/true);
    }

    if (now >= g_nextStats) {
        g_nextStats = now + std::chrono::seconds(60);
        UE_LOGI("drive_sync: 60s marks slot=%llu | sent slot=%llu | applied slot=%llu | latchFix=%llu "
                "grabKept=%llu | primed offWorld=%llu rekey=%llu | ejectRefused=%llu",
                (unsigned long long)g_cMarksSlot.load(std::memory_order_relaxed), (unsigned long long)g_cSlotSent,
                (unsigned long long)g_cSlotApplied, (unsigned long long)g_cLatchCompleted,
                (unsigned long long)g_cGrabKept, (unsigned long long)g_cPrimedOffWorld,
                (unsigned long long)g_cRekeyed, (unsigned long long)g_cEjectRefused);
    }
}

void OnDriveSlotState(const coop::net::DriveSlotStatePayload& p, uint8_t senderSlot) {
    if (!DC::EnsureResolved()) return;
    OnSlotLine(p, senderSlot);
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    if (!DC::EnsureResolved()) return;
    // Slot lines.
    int lines = 0;
    for (int r = 0; r < DC::kRoleCount; ++r) {
        void* slot = DC::SlotActor(r);
        if (!slot) continue;
        ++lines;
        void* drive = DC::SlotDrive(slot);
        coop::net::DriveSlotStatePayload p{};
        p.role = static_cast<uint8_t>(r);
        p.occupied = drive ? 1 : 0;
        p.driveEid = drive ? static_cast<uint32_t>(
            coop::element::Registry::Get().EidForActor(drive)) : 0;
        s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::DriveSlotState, &p, sizeof(p));
    }
    // (The rows ride drive_payload_sync's seed and the rack canonicals drive_rack_sync's, called right after this
    // one in subsystems -- the slot lines -> rows -> racks order.)
    UE_LOGI("drive_sync: connect seed -> joiner slot %d (%d of %d slot lines)", peerSlot, lines, DC::kRoleCount);
}

uint64_t AnnouncedCount() { return g_cSlotSent; }

void OnDisconnect() {
    for (int r = 0; r < DC::kRoleCount; ++r) {
        g_slotDirty[r].store(false, std::memory_order_relaxed);
        g_lastEjected[r].Reset();
        g_slotBase[r] = {};
    }
    g_primed = false;
    g_wasConnected = false;
    g_replayingInsert = false;
    g_cGrabKept = 0;
    g_cPrimedOffWorld = g_cRekeyed = g_cEjectRefused = 0;
    g_wrongClassSaid.clear();
    g_session.store(nullptr, std::memory_order_release);
    UE_LOGI("drive_sync: teardown (slot baselines cleared)");
}

}  // namespace coop::drive_sync
