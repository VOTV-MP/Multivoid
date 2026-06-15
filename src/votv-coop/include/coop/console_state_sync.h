// coop/console_state_sync.h -- v64 base-computers PHASE 2 increment 1: the
// screen-STATE mirror for the signal-catcher subsystem (RE doc SS4-5).
//
// THREE channels (one module -- they share the spaceRenderer/desk substrate
// and the v63 claim context):
//
//   SKY SIGNALS (SkySignalState=52). The coords-minigame targets were pure
//   per-peer RNG (spaceRenderer.spawnSignal re-arms every 20-60 s and rolls
//   everything locally) -- host gusty, client barren, the one roller hole the
//   RE doc flagged. HOST = the only roller (its native timer keeps running);
//   a CLIENT kills its own roller timer (K2_ClearTimer; restored by one
//   reflected spawnSignal() on disconnect -- which re-arms the BP's own
//   loop), keeps its widget lifetimes wire-driven (keepalive on every apply,
//   so only wire snapshots remove rows), and reconciles its set to the host's
//   snapshot (delete-not-in-wire / add-missing). MTA shape: server-rolled
//   pickups/world events mirrored at the RESULT level. The CATCH consume
//   replay (SkySignalCatch=53) lives in coop/signal_catch_sync since v70;
//   this module hands incoming snapshots through its recent-catch filter.
//
//   DESK SCALARS (DeskState=54). The 4-screen desk's live-visible fields
//   (download/refine/playback/coords activity). Streamed ~1 Hz on-change by
//   the desk-claim OWNER while claimed (host-relayed); edge-broadcast by ANY
//   peer on a discrete flip while unclaimed (the physical buttons don't
//   require entering); adopt=1 connect snapshot from the host (whose
//   dlDecoded/dlPolarity hand off to signal_catch_sync's pending adopt).
//   Receivers raw-write + run the desk's own upd* repaint chain. Echo-guarded
//   by a last-applied compare (house pattern).
//
//   DESK LOG LINES (DeskLogLine=65, v70 -- replaces the v64 coordLog tail
//   window, RULE 2). Producer-side EXACT diff of coord_coordLog2Text (one
//   local monotone text), complete CRLF lines only, the self-regenerating
//   ANIMATED lines (CDOWN/AREA SCAN/APPROXIMATION/ANALYSIS/CR:[) filtered by
//   prefix; only the 9 one-shot event lines ride the wire (host-relayed).
//   Receivers append via writeToCoordLog_2 (native CRLF + repaint + 1000-cap)
//   and advance their own diff baseline past the applied text (echo-proof).
//
//   DISH AIM (DishAimState=55). The ui_coordinates cursor state (+ the v70
//   Direction catch-gate toggle) streamed ~3 Hz by the claim owner while
//   claimed; receivers raw-write + reflected repaint (nothing else writes
//   them on a non-occupant peer -- the coords screen + the physical dishes
//   render from them).
//
// Game thread throughout (Tick from subsystems::TickGameplay, OnReliable
// from the event_feed drain).

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::console_state_sync {

// Store the session. Idempotent; called per tick from subsystems::Install.
void Install(coop::net::Session* session);

// Per-tick: resolve substrate (throttled); HOST polls the sky-signal set
// (~1 Hz, broadcast on change) + the CLIENT enforces its roller suppression;
// the desk-claim owner streams desk scalars + dish aim; every peer polls its
// desk for unclaimed button edges (~1 Hz, edge-broadcast).
void Tick();

// Wire ingest. (SkySignalCatch ingests in coop/signal_catch_sync since v70.)
void OnSkySignalState(const coop::net::SkySignalStatePayload& p, uint8_t senderSlot);
void OnDeskState(const coop::net::DeskStatePayload& p, uint8_t senderSlot);
void OnDishAim(const coop::net::DishAimStatePayload& p, uint8_t senderSlot);
void OnDeskLogLine(const coop::net::DeskLogLinePayload& p, uint8_t senderSlot);

// Re-baseline the unclaimed desk edge detector to the CURRENT desk scalars so
// a programmatic apply (the signal_catch_sync replay's scalar fallout) never
// reads as a local button edge and echo-broadcasts (the house lastKnown
// pattern, callable cross-module).
void PrimeDeskEdgeDetector();

// HOST: send the current sky-signal snapshot + desk scalars (adopt=1) to a
// joining peer (world-ready replay).
void QueueConnectBroadcastForSlot(int peerSlot);

// Aggregate teardown: clear mirrors/edge state; a CLIENT restores its native
// sky-signal roller (one reflected spawnSignal() re-arms the BP loop).
void OnDisconnect();

}  // namespace coop::console_state_sync
