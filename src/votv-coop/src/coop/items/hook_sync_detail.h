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

// (space, slot, seq) packed. The pair still names the hook across the handoff -- the anchor moves
// who may write it, not what it is called -- but the ANCHORED form lives in its own space.
//
// It has to. A departing peer's anchored rows are deliberately kept, because the host owns those
// now; a replacement peer takes the same slot and starts at seq 1; and a single key space would
// then have that peer's first hook DESTROY the anchored mirror of a hook the host still owns and
// still has in its save. Two spaces, and the two can never collide.
using Key = uint32_t;
inline constexpr uint32_t kAnchoredSpace = 1u << 31;
inline Key MakeKey(uint8_t slot, uint16_t seq, bool anchored = false) {
    return (anchored ? kAnchoredSpace : 0u) | (static_cast<uint32_t>(slot) << 16) | seq;
}
inline uint8_t  SlotOf(Key k)      { return static_cast<uint8_t>((k >> 16) & 0xFFu); }
inline uint16_t SeqOf(Key k)       { return static_cast<uint16_t>(k & 0xFFFFu); }
inline bool     IsAnchored(Key k)  { return (k & kAnchoredSpace) != 0; }

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
    // The tail attach is retried until it lands: a peer's puppet may not exist yet when its first
    // hook state arrives, and an attach that silently never happened leaves the cable pinned to
    // the mirror's own root for the hook's whole life.
    bool                  tailAttached = false;
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

// Destroy a hook this peer OWNS, by seq, and drop its row. The answer to a host refusal: without
// it a refused commit leaves the owner holding a real hook that every other peer has dropped.
// True if a row was found.
bool DropOwnedBySeq(uint16_t seq);

// Attach a mirror's tail to the puppet of `slot`. FALSE when that peer has no live puppet yet,
// which is not a failure and is why the caller records the answer and retries: an attach that
// silently never happened leaves the cable pinned to the mirror's own root for the hook's life.
bool AttachTailToSlotBody(void* mirror, uint8_t slot);

uint64_t NowMs();

}  // namespace coop::hook_sync::detail
