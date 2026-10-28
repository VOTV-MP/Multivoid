// coop/net/snapshot_send_scope.h -- marks the host's snapshot sends to one joining slot, so the
// send backlog's join gate lets them through while it holds every other Default-lane send to that slot.
//
// MTA sends a joining player its whole map in one entity add, after marking it joined and before any
// broadcast can reach it (CGame.cpp:1407,1431 -> CMapManager.cpp:548). Ours is spread over many ticks,
// so the slot's gate (SendBacklog::OpenJoinGate) stands in for that atomicity, and the sends that ARE the
// add -- the divergence deletes, SnapshotBegin, each drained prop's spawn and its records -- run inside
// this scope. Game thread only; the net thread is never inside one, so a relay is always gated.
// Divergence from MTA, whose BroadcastOnlyJoined drops a broadcast for a not-joined player and delivers
// the state inside the add (CPlayerManager.cpp:159-172): ours defers the broadcast and replays it behind
// the add. MTA's per-element alternative, CPerPlayerEntity, was rejected (see SendBacklog::OpenJoinGate).

#pragma once

namespace coop::net {

class SnapshotSendScope {
public:
    // Every reliable send to `slot` made on this thread until the scope ends skips the join gate.
    // Scopes nest: the destructor restores the slot the enclosing scope set, or none.
    explicit SnapshotSendScope(int slot);
    ~SnapshotSendScope();
    SnapshotSendScope(const SnapshotSendScope&) = delete;
    SnapshotSendScope& operator=(const SnapshotSendScope&) = delete;

    // The slot the calling thread is inside a scope for, or -1 outside any. Any thread.
    static int CurrentSlot();

private:
    int previous_;
};

}  // namespace coop::net
