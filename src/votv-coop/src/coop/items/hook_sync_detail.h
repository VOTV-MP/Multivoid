// coop/items/hook_sync_detail.h -- sibling-internal state shared by the hook lane's two TUs
// (hook_sync.cpp, the owner half and the mirrors; hook_anchor.cpp, the handoff). The
// coop/net/session_lanes.h shape: private to the src tree, never part of the include surface.

#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "ue_wrap/actors/hook.h"
#include "ue_wrap/core/cached_obj_ref.h"

namespace coop::net { class Session; }

namespace coop::hook_sync::detail {

// (slot, seq) packed. Stable across the anchor handoff on purpose: the anchor moves who may write
// the hook, it does not rename it.
using Key = uint32_t;
inline Key MakeKey(uint8_t slot, uint16_t seq) {
    return (static_cast<uint32_t>(slot) << 16) | seq;
}
inline uint8_t  SlotOf(Key k) { return static_cast<uint8_t>(k >> 16); }
inline uint16_t SeqOf(Key k)  { return static_cast<uint16_t>(k & 0xFFFFu); }

// A hook this peer owns and streams. One row survives past `activeHook` going null, because that
// is the moment the anchor has to be noticed.
struct Owned {
    ue_wrap::CachedObjRef ref;
    uint16_t              seq       = 0;
    ue_wrap::hook::Kind   kind      = ue_wrap::hook::Kind::Hook;
    bool                  committed = false;  // the anchor commit has been sent; stop streaming
    // The change gate's baseline, and its clock. lastPollMs bumps on every poll whether or not
    // anything was sent: bumping it only on a send is the bug atv_sync.cpp:671-674 had to go back
    // and fix, where a quiet entity's stale clock let the gate run its dispatches at the pump rate.
    ue_wrap::hook::State  last{};
    uint64_t              lastPollMs = 0;
    uint64_t              lastSendMs = 0;
};

// A hook some other peer owns (or, once anchored, the host does) that we render.
struct Mirror {
    ue_wrap::CachedObjRef ref;
    ue_wrap::hook::Kind   kind     = ue_wrap::hook::Kind::Hook;
    bool                  anchored = false;  // the host owns it; a peer leaving does not drop it
};

coop::net::Session*                  Session();
void                                 SetSession(coop::net::Session* s);
std::vector<Owned>&                  OwnedHooks();
std::unordered_map<Key, Mirror>&     Mirrors();

// The interceptor's index: the mirror actors, flat. Read once per `hook_C::ReceiveTick` dispatch,
// so it is a short linear scan over pointers rather than a hash -- a session holds a handful of
// hooks, and the scan beats a bucket walk at that size.
std::vector<void*>&                  MirrorActors();
void NoteMirrorActor(void* actor, bool add);

// Build a mirror and register it under `key`; destroys and replaces any mirror already there.
// Returns the actor, or null.
void* InstallMirror(Key key, ue_wrap::hook::Kind kind, const ue_wrap::FVector& aLoc,
                    const ue_wrap::FRotator& aRot, bool anchored);

// Destroy the mirror under `key`, if any.
void DropMirror(Key key);

// Attach a mirror's tail to the puppet of `slot` (or to the local player, for a mirror of a hook
// this peer's own body is carrying -- which happens only on the host, for a client's hook).
void AttachTailToSlotBody(void* mirror, uint8_t slot);

uint64_t NowMs();

}  // namespace coop::hook_sync::detail
