// coop/player/hand_item.h -- the hotbar HAND-ITEM display axis.
//
// The item shown in a player's hand is NOT a world entity: VOTV's updateHold destroys and
// respawns a fresh local actor on every quick-slot switch. Riding the WORLD-prop pipeline
// (PropSpawn/PropPose/PropRelease) therefore produced a peer's held item updating late or never
// -- the host's pre-quiescence express gate suppressed every host hand item forever, the client
// dropped its 60 Hz keyed poses as unmatched, and every switch left physics-release litter and
// a fresh key/mirror churn.
//
// The model is MTA's: a ped's current weapon is PLAYER STATE attached to the ped locally on
// every machine, so the hand item is part of the player's EXPRESSION like skin and nick colour.
// The owner polls mainPlayer.holding_actor (Aprop_C only -- the trash clump/pile carry keeps its
// own lane) and broadcasts a small reliable HandItem{class, name} on change; every peer keeps a
// display-only mirror. Nothing goes per-tick on the wire, switch latency is one reliable
// message, and late join replays per-slot state. All functions game-thread only.

#pragma once

#include "coop/net/session.h"

#include <cstdint>

namespace coop::hand_item {

// Owner side, per tick (from local_streams::Tick). `local` is the local mainPlayer (cached for
// LocalHandActor's fresh read); `holdingProp` is the local player's holding_actor IF it is a
// live Aprop_C descendant, else nullptr. Announces EDGE-INSTANT on change -- a quick-slot switch
// is one synchronous updateHold call, bytecode-proven, so a null IS the stow.
//
// At the hand edge, an ex-hand actor that SURVIVED the edge was RELEASED into the world (an
// R-drop or quick-slot place is not a spawn; the game re-uses the view actor), so it is expressed
// as a world prop right there through trash_collect_sync::EnsureHeldItemBroadcast. The R-pickup
// destroy side belongs to the K2_DestroyActor Func seam in prop_lifecycle.
void TickOwner(coop::net::Session& session, void* local, void* holdingProp);

// Receiver side, per tick (same site): lazily spawn/replace/destroy the display mirrors, so a
// state arriving before the puppet exists (join) or a puppet respawn are absorbed without event
// ordering. A mirror has physics and collision off and its spawn echo suppressed, and it is
// RE-COMMANDED every tick onto the puppet's head anchor and synced view basis with the
// owner-measured view-relative transform. It is never attached to a component: the native arms
// chain (socket weapon_R, camera-anchored through the arms_lag spring arm) is hidden and does
// not tick on a puppet, so the look has to be reproduced rather than inherited.
void TickMirrors();

// event_feed router entry. Parses, forgery-guards, stores, host-rebroadcasts.
bool HandleHandItem(coop::net::Session& session,
                    const coop::net::Session::ReliableMessage& msg);

// Host -> a just-world-ready joiner: replay every slot's current hand state
// (subsystems::ConnectReplayForSlot; session = the one cached by TickOwner).
void ReplayPeerStatesToSlot(int slot);

// The axis boundary for the world-prop census: the LOCAL player's live hotbar hand actor (a
// fresh mainPlayer.holding_actor read, Aprop_C-gated), or nullptr. prop_census's seed walk
// skips this actor -- it is player expression, never a world entity, and adopting it makes the
// safety census express an incremental PropSpawn that lands on the peer as a frozen duplicate
// at the puppet. Fresh read rather than the announce latch: the census runs earlier in the pump
// tick than TickOwner, and updateHold can have respawned the actor in between.
void* LocalHandActor();

// The FULL hand-axis boundary, ONE owner: true for the local hand actor AND for every live
// remote mirror. LocalHandActor above answers only for the local half, and a census that asks
// it alone adopts the mirrors. Callers: prop_drop_intent on both enqueue AND drain -- the
// drain-time re-check is load-bearing, because holding_actor is not yet written when
// FinishSpawn returns -- and rng_roll_census's channel exclusion. prop_census's seed walk takes
// the same set the cheaper way, hoisting CollectHandAxisActors once per walk. Game thread only.
bool IsHandAxisActor(void* actor);

// Snapshot the current hand-axis actors (local hand + live remote mirrors)
// into out[]; returns the count (<= 1 + kMaxPeers). For per-walk hoisting.
size_t CollectHandAxisActors(void* out[], size_t cap);

// Session lifecycle.
void Reset();                          // destroy all mirrors + clear states
void OnSlotDisconnected(uint8_t slot); // destroy that slot's mirror + state

}  // namespace coop::hand_item
