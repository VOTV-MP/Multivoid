// coop/player/hand_item.h -- the hotbar HAND-ITEM display axis (v105).
//
// RULE-1 root fix for "peer's held item updates late/never" (user 2026-07-06;
// host log 12:40:05 proof): the item shown in a player's hand is NOT a world
// entity -- VOTV's updateHold destroys + respawns a fresh local actor on every
// quick-slot switch -- yet it rode the WORLD-prop pipeline
// (PropSpawn/PropPose/PropRelease). That mismatch produced: the host's
// PRE-QUIESCENCE express gate suppressing every host hand item forever (the
// client dropped 60 Hz keyed poses -- 'no local match' x1809), physics-release
// litter/dupes on switch-away, and a fresh key/mirror churn per switch.
//
// The proper model (MTA shape: a ped's current weapon is PLAYER STATE attached
// to the ped locally on every machine): the hand item is part of the player's
// EXPRESSION, like skin/nick color.
//   - owner polls mainPlayer.holding_actor (Aprop_C only -- the trash
//     clump/pile carry stays on its own lane) and broadcasts a small reliable
//     HandItem{class, name} on change;
//   - every peer keeps a DISPLAY-ONLY mirror (physics off, collision off,
//     spawn-echo suppressed) attached to the puppet's `weapon` component at
//     the NATIVE hold transform (updateHold @1628 attaches holding_actor to
//     `weapon` -- socket weapon_R of the FP arms rig, camera-anchored via the
//     arms_lag spring arm -- at rel loc(0,0,0) rot(0,180,0); hands-on
//     2026-07-06 13:43: the old puppet-ROOT+offset attach read as carry/grab);
//   - nothing per-tick on the wire; switch latency = one reliable message;
//   - late-join: the host stores per-slot state and replays it in the
//     ConnectReplayForSlot fanout.
//
// The axis BOUNDARY is enforced at the world-prop census too: the owner's
// live hand actor is NOT a world entity, so prop_element_tracker::SeedWalk_
// consults LocalHandActor() and never adopts it (hands-on 2026-07-06 13:44:00:
// the ~20s safety census adopted the host's in-hand rock -> incremental
// PropSpawn -> a frozen dupe rock at the puppet on the client for 4s).
//
// All functions game-thread only.

#pragma once

#include "coop/net/session.h"

#include <cstdint>

namespace coop::hand_item {

// Owner side, per tick (from local_streams::Tick). `local` is the local
// mainPlayer (cached for LocalHandActor's fresh read); `holdingProp` is the
// local player's holding_actor IF it is a live Aprop_C descendant, else
// nullptr. Announces EDGE-INSTANT on change (a quick-slot switch is one
// synchronous updateHold call -- bytecode-proven -- so a null IS the stow).
// v106: at the hand edge, an ex-hand actor that SURVIVED the edge was
// RELEASED into the world (R-drop / quick-slot place is not a spawn -- the
// game re-uses the view actor) -- it is expressed as a world prop right
// there via trash_collect_sync::EnsureHeldItemBroadcast. The R-pickup
// destroy side is owned by the K2_DestroyActor Func seam (prop_lifecycle).
void TickOwner(coop::net::Session& session, void* local, void* holdingProp);

// Receiver side, per tick (same site): lazily spawn/replace/destroy the
// display mirrors so a state that arrives before the puppet exists (join) or
// a puppet respawn are absorbed without event ordering.
void TickMirrors();

// event_feed router entry. Parses, forgery-guards, stores, host-rebroadcasts.
bool HandleHandItem(coop::net::Session& session,
                    const coop::net::Session::ReliableMessage& msg);

// Host -> a just-world-ready joiner: replay every slot's current hand state
// (subsystems::ConnectReplayForSlot; session = the one cached by TickOwner).
void ReplayPeerStatesToSlot(int slot);

// The v105 axis boundary for the world-prop census: the LOCAL player's live
// hotbar hand actor (a fresh mainPlayer.holding_actor read, Aprop_C-gated),
// or nullptr. prop_element_tracker::SeedWalk_ skips this actor -- it is
// player expression, never a world entity. Fresh read (not the announce
// latch): the census runs earlier in the pump tick than TickOwner, and
// updateHold can have respawned the actor in between.
void* LocalHandActor();

// Session lifecycle.
void Reset();                          // destroy all mirrors + clear states
void OnSlotDisconnected(uint8_t slot); // destroy that slot's mirror + state

}  // namespace coop::hand_item
