// coop/net/snapshot_send_scope.cpp -- see coop/net/snapshot_send_scope.h.

#include "coop/net/snapshot_send_scope.h"

#include "ue_wrap/core/hot_path_guard.h"  // UE_ASSERT_GAME_THREAD

namespace coop::net {

namespace {
thread_local int t_scopeSlot = -1;
}  // namespace

SnapshotSendScope::SnapshotSendScope(int slot) : previous_(t_scopeSlot) {
    UE_ASSERT_GAME_THREAD("SnapshotSendScope");
    t_scopeSlot = slot;
}

SnapshotSendScope::~SnapshotSendScope() { t_scopeSlot = previous_; }

int SnapshotSendScope::CurrentSlot() { return t_scopeSlot; }

}  // namespace coop::net
