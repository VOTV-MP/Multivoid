// coop/props/container_act_selftest_overlap.cpp -- the overlap put's share of the container act selftest: the order the host's
// judge reads its terms in, the refusal latch's key and its clears, the wait table's cap, drop and releases, and the
// unread wait's release at a bind and its expiry. Pure: no engine call, no session (container_overlap_pure.h). Called from
// RunSelftest (container_act_selftest.cpp).

#include "coop/props/container_act_selftest_internal.h"
#include "coop/props/container_overlap_pure.h"

#include <cstring>
#include <set>
#include <vector>

namespace coop::props::container_act_host::selftest {
namespace {

namespace P = coop::props::container_act_host::overlap::pure;

// A reference to a fake object: live until the check kills it.
std::set<void*> g_dead;
struct FakeRef {
    void* p = nullptr;
    void Set(void* x) { p = x; }
    void* Raw() const { return p; }
    bool Alive() const { return p && g_dead.count(p) == 0; }
    bool Is(void* x) const { return x && x == p && Alive(); }
};

void* Obj(int n) { return reinterpret_cast<void*>(static_cast<uintptr_t>(0x1000 + n * 16)); }

P::Inputs Known(bool armed, bool needsReach) {
    P::Inputs in;
    in.needsReach = needsReach;
    in.armedKnown = true;
    in.armed = armed;
    return in;
}

void CheckOrder(Checks& check) {
    P::Inputs in;
    check(P::Decide(in).need == P::Need::Armed, "overlap order: the box's arming is read first");
    in = Known(false, true);
    P::Decision d = P::Decide(in);
    check(d.judged == P::Judged::Refuse && d.tally == Tally::PutOverlapUnarmed && d.need == P::Need::Nothing,
          "overlap order: an unarmed box refuses before any other term is read");

    in = Known(true, true);
    check(P::Decide(in).need == P::Need::Reach, "overlap order: a held or claimed driver's reach is read after the arming");
    in.reachKnown = true;
    in.reach = P::ReachRead::OutOfReach;
    d = P::Decide(in);
    check(d.judged == P::Judged::Refuse && d.tally == Tally::PutOverlapReach, "overlap order: out of reach refuses");
    in.reach = P::ReachRead::Unread;
    d = P::Decide(in);
    check(d.judged == P::Judged::Refuse && d.tally == Tally::PutOverlapUnread && !d.transient,
          "overlap order: a reach that does not read refuses as unread");
    in.reach = P::ReachRead::Ok;
    check(P::Decide(in).need == P::Need::Hold, "overlap order: the hold is read after the reach");

    in = Known(true, false);
    check(P::Decide(in).need == P::Need::Hold, "overlap order: an elected throw has no reach term");

    in.holdKnown = true;
    in.held = true;
    in.gatesKnown = true;
    in.gate = P::GateRead::NoMesh;
    d = P::Decide(in);
    check(d.judged == P::Judged::Wait && d.tally == Tally::PutOverlapHeld && !d.transient && d.need == P::Need::Nothing,
          "overlap order: a client's hold waits before the gates and the fit are read");

    in.held = false;
    in.gatesKnown = false;
    check(P::Decide(in).need == P::Need::Gates, "overlap order: the gates are read after the hold");
    const P::GateRead gates[] = {P::GateRead::NoMesh, P::GateRead::IgnoreSaveUnread, P::GateRead::Ignored,
                                 P::GateRead::CanPickupUnread, P::GateRead::CannotPickup};
    bool allRefuse = true;
    for (const P::GateRead g : gates) {
        in.gatesKnown = true;
        in.gate = g;
        d = P::Decide(in);
        allRefuse = allRefuse && d.judged == P::Judged::Refuse && d.tally == Tally::PutOverlapGate && d.need == P::Need::Nothing;
    }
    check(allRefuse, "overlap order: each gate that does not pass refuses before the fit is read");

    in.gate = P::GateRead::Pass;
    check(P::Decide(in).need == P::Need::Fit, "overlap order: the fit, which reads the whole container, is read last");
    in.fitKnown = true;
    in.fit = P::FitRead::Unread;
    d = P::Decide(in);
    check(d.judged == P::Judged::Wait && d.tally == Tally::PutOverlapUnread && d.transient,
          "overlap order: a fit read that does not come waits for its edge");
    in.fit = P::FitRead::Refused;
    d = P::Decide(in);
    check(d.judged == P::Judged::Refuse && d.tally == Tally::PutOverlapFit, "overlap order: a fit that refuses refuses");
    in.fit = P::FitRead::Fits;
    d = P::Decide(in);
    check(d.judged == P::Judged::Pass && d.need == P::Need::Nothing, "overlap order: every term passing is a pass");
}

void CheckLatch(Checks& check) {
    g_dead.clear();
    P::LatchTable<FakeRef> t;
    const uint32_t keyA = P::DriveKey(2, 5, 0);
    check(P::DriveKey(2, 5, 0) != P::DriveKey(3, 5, 0) && P::DriveKey(2, 5, 0) != P::DriveKey(2, 6, 0) &&
              P::DriveKey(2, 5, 0) != P::DriveKey(2, 5, 1),
          "latch: the drive key differs by slot, row generation and hold generation");

    Tally got = Tally::kCount;
    check(!t.Add(Obj(1), 7, keyA, Tally::PutOverlapFit) && t.Hit(Obj(1), 7, keyA, got) && got == Tally::PutOverlapFit,
          "latch: a refusal is kept under its prop, container and drive key");
    check(!t.Hit(Obj(1), 7, P::DriveKey(2, 6, 0), got), "latch: a new drive of the row is a new key");
    check(!t.Hit(Obj(1), 7, P::DriveKey(2, 5, 1), got), "latch: a new hold is a new key");
    check(!t.Hit(Obj(1), 8, keyA, got) && !t.Hit(Obj(2), 7, keyA, got), "latch: another container or prop is another key");

    t.Add(Obj(2), 7, keyA, Tally::PutOverlapReach);
    t.Add(Obj(3), 7, keyA, Tally::PutOverlapUnread);
    check(t.Size() == 1, "latch: a reach or unread refusal is not kept");

    t.Add(Obj(4), 9, keyA, Tally::PutOverlapGate);
    t.ClearContainer(7);
    check(!t.Hit(Obj(1), 7, keyA, got) && t.Hit(Obj(4), 9, keyA, got),
          "latch: a container's change clears its latches and no other's");

    t.Add(Obj(1), 7, keyA, Tally::PutOverlapFit);
    t.Add(Obj(1), 7, keyA, Tally::PutOverlapUnarmed);
    check(t.Size() == 2 && t.Hit(Obj(1), 7, keyA, got) && got == Tally::PutOverlapUnarmed,
          "latch: the pair's newer refusal replaces the older");

    g_dead.insert(Obj(4));
    check(!t.Hit(Obj(4), 9, keyA, got), "latch: a prop that is gone is not a hit");
    t.Add(Obj(5), 9, keyA, Tally::PutOverlapFit);
    check(t.Size() == 2, "latch: a row of a prop that is gone leaves at the next add");

    t.Clear();
    g_dead.clear();
    bool evicted = false;
    for (int i = 0; i < static_cast<int>(P::kTableRows); ++i) evicted = evicted || t.Add(Obj(10 + i), 1, keyA, Tally::PutOverlapFit);
    check(!evicted && t.Size() == P::kTableRows, "latch: the table holds its eight rows without an eviction");
    check(t.Add(Obj(30), 1, keyA, Tally::PutOverlapFit) && t.Size() == P::kTableRows && !t.Hit(Obj(10), 1, keyA, got) &&
              t.Hit(Obj(30), 1, keyA, got),
          "latch: a ninth row evicts the oldest and says so");
}

void CheckWaits(Checks& check) {
    g_dead.clear();
    P::WaitTable<FakeRef> t;
    using Added = P::WaitTable<FakeRef>::Added;
    check(P::kUnreadWaitMs == 2000, "waits: the unread wait is bounded at 2000 ms");

    bool anyDropped = false;
    for (int i = 0; i < static_cast<int>(P::kTableRows); ++i)
        anyDropped = anyDropped || t.Add(Obj(i), Obj(100 + i), 50 + i, P::WaitKind::Hold, 0) == Added::NewDroppedOldest;
    check(!anyDropped && t.Size() == P::kTableRows, "waits: the table holds its eight rows without a drop");
    check(t.Add(Obj(40), Obj(140), 90, P::WaitKind::Hold, 0) == Added::NewDroppedOldest && t.Size() == P::kTableRows &&
              t.TakeHold(50).empty() && t.TakeHold(90).size() == 1,
          "waits: a ninth row drops the oldest and says so");

    t.Clear();
    check(t.Add(Obj(1), Obj(101), 7, P::WaitKind::Hold, 0) == Added::New &&
              t.Add(Obj(1), Obj(101), 7, P::WaitKind::Hold, 0) == Added::Merged && t.Size() == 1,
          "waits: a pair already waiting is kept once");
    t.Add(Obj(2), Obj(101), 7, P::WaitKind::Hold, 0);
    t.Add(Obj(3), Obj(102), 8, P::WaitKind::Hold, 0);
    std::vector<P::WaitRow<FakeRef>> released = t.TakeHold(7);
    check(released.size() == 2 && t.Size() == 1, "waits: a hold's end releases that container's waiting puts and no other's");
    check(t.TakeHold(7).empty(), "waits: a released put is not released twice");

    t.Clear();
    t.Add(Obj(1), Obj(101), 7, P::WaitKind::Bind, 5000);
    t.Add(Obj(2), Obj(101), 7, P::WaitKind::Read, 5000);
    t.Add(Obj(3), Obj(101), 7, P::WaitKind::Hold, 0);
    check(t.HasEdgeRows() && t.TakeReady().empty() && t.TakeHold(9).empty(), "waits: a bind or read row waits for its edge");
    t.MarkBound(Obj(2));
    check(t.TakeReady().empty(), "waits: another prop's bind releases nothing, and a bind does not release a read row");
    t.MarkBound(Obj(1));
    std::vector<P::WaitRow<FakeRef>> ready = t.TakeReady();
    check(ready.size() == 1 && ready[0].prop.Raw() == Obj(1), "waits: the bind of the unresolved prop releases its wait");
    t.MarkChanged(8);
    check(t.TakeReady().empty(), "waits: another container's change releases nothing");
    t.MarkChanged(7);
    ready = t.TakeReady();
    check(ready.size() == 1 && ready[0].prop.Raw() == Obj(2), "waits: the container's change releases a read wait, not a hold wait");
    check(t.Size() == 1 && !t.HasEdgeRows(), "waits: the hold row stays and has no edge to time out");

    t.Clear();
    const uint64_t t0 = 10000;
    t.Add(Obj(1), Obj(101), 7, P::WaitKind::Bind, t0 + P::kUnreadWaitMs);
    t.Add(Obj(2), Obj(101), 7, P::WaitKind::Hold, 0);
    check(t.TakeExpired(t0 + P::kUnreadWaitMs - 1).empty(), "waits: an unread wait is not expired a millisecond early");
    t.MarkBound(Obj(1));
    check(t.TakeExpired(t0 + 60000).empty(), "waits: a wait whose edge came is not expired");
    t.Clear();
    t.Add(Obj(1), Obj(101), 7, P::WaitKind::Read, t0 + P::kUnreadWaitMs);
    t.Add(Obj(2), Obj(101), 7, P::WaitKind::Hold, 0);
    std::vector<P::WaitRow<FakeRef>> gone = t.TakeExpired(t0 + P::kUnreadWaitMs);
    check(gone.size() == 1 && gone[0].prop.Raw() == Obj(1) && t.Size() == 1,
          "waits: an unread wait expires at 2000 ms and a hold wait never does");

    t.Clear();
    t.Add(Obj(1), Obj(101), 7, P::WaitKind::Hold, 0);
    t.Add(Obj(2), Obj(101), 8, P::WaitKind::Bind, t0);
    t.Add(Obj(3), Obj(102), 9, P::WaitKind::Hold, 0);
    check(t.TakeContainerActor(Obj(101)).size() == 2 && t.Size() == 1,
          "waits: a container's end of play drops its waits of every kind");

    t.Clear();
    t.Add(Obj(1), Obj(101), 7, P::WaitKind::Read, t0);
    check(t.Add(Obj(1), Obj(101), 7, P::WaitKind::Hold, 0) == Added::Merged && !t.HasEdgeRows() && t.TakeHold(7).size() == 1,
          "waits: a read wait that meets a hold becomes a hold wait");
}

}  // namespace

void RunOverlapSelftest(Checks& check) {
    CheckOrder(check);
    CheckLatch(check);
    CheckWaits(check);
}

}  // namespace coop::props::container_act_host::selftest
