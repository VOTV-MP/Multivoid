// coop/signal_catch_sync.h -- v70 (reworked v113/L4): the STOLAS signal-catch
// CONSUME REPLAY.
//
// RE ground truth (votv-stolas-signal-catch-RE-2026-06-12.md +
// votv-dish-impl-RE-2026-07-16.md): the SP chain after a successful ping --
// coord_signalData := gatherSignal.data -> row delete -> download-machine
// reset -> playPingSound -> startMovingTo on EVERY gamemode dish -> (native,
// per peer) arrival -> activeDishes all-false -> dishesStop broadcast -> the
// desk arms formDownload(0,-1) -- ran on the CATCHING peer only.
//
// This module relays the catch as ONE host-validated world event and replays
// its IDENTITY half on every other peer (coord_signalData + sky-row delete +
// machine reset + ping sound). Since L4 (v113) the dish THEATER half is
// host-only: the HOST replays StartMovingAll and streams poses (dish_sync);
// a CLIENT never slews from wire, and its own unpreventable ping slews are
// killed-with-cleanup right after the catch payload is sent
// (dish_sync::KillOwnPingSlews). The download ARM rides the host-authored
// DishArm lane (host polarity) -- the v70 pending-adopt and the joiner
// direct-arm are RETIRED (RULE 2).
//
// Catcher detection (1 Hz poll, UNGATED since v116): the coord_signalData
// IDENTITY TUPLE (x,y,z,frequency | objectName) CHANGE-edge to a non-None
// state. Derivation (impl-RE SS7/SS8 + the v116 tree-wide writer grep):
// coord_signalData has exactly TWO native writers -- ping-success (:= row)
// and the delete chain (:= None) -- and our ONLY wire writers
// (ApplyReplay/connect-seed) prime these baselines, so an unprimed local
// change IS a catch, definitionally. The v63-v115 claim gate is RETIRED
// (RULE 2): a successful ping's own completion releases the desk FSM-hold
// within the same second as the edge (measured 17:04:46/47), so any
// claim-anchored gate loses that race by construction -- and the baseline
// roll-forward then eats the catch permanently (the 17:04 lost-catch root).
// Same retire on the host validator (holder==sender): replaced by the
// recent-TTL dup guard. kind=1 (cleared): the 'Signal data deleted' button =
// objectName -> 'None' edge on ANY peer (unclaimed trust, matching the
// physical button's authority model). kind=2 (v116): host-authored connect
// STATE-SEED -- applied like kind=0, never announced to the activity feed.
//
// v116 feature: every kind=0 catch lands one activity-feed line per peer
// ("You caught signal 'X'" at the catcher; "<nick> caught signal 'X'"
// elsewhere, attributed via the v18 logical-origin stamp).
//
// Game thread throughout.

#pragma once

#include "coop/net/protocol.h"
#include "ue_wrap/desk/space_renderer.h"

#include <cstdint>
#include <vector>

namespace coop::net { class Session; }

namespace coop::signal_catch_sync {

void Install(coop::net::Session* session);

// 1 Hz: the catch + cleared detectors and recent-catch TTL pruning. Cheap
// when idle (one struct read).
void Tick();

// Wire ingest (both roles). HOST: dedups kind=0 via the recent-TTL, replays
// locally (incl. the StartMovingAll theater), rebroadcasts to everyone but
// the catcher, feeds the activity line; drops kind=2 (host-authored only).
// CLIENT: replays the identity half only (transport-trusted -- clients only
// receive from the host; senderSlot = the stamped logical catcher).
void OnReliable(const coop::net::SkySignalCatchPayload& p, uint8_t senderSlot);

// HOST: if coord_signalData is armed, send the joiner one kind=2 STATE-SEED
// for the signal IDENTITY (the poses/arm ride dish_sync's snapshot + DishArm
// rows on the same ordered lane, queued right after this). kind=2 applies
// like a catch but never announces to the activity feed.
void QueueConnectBroadcastForSlot(int peerSlot);

// Called by console_state_sync BEFORE applying an assembled SkySignalState
// snapshot: runs the catch detector immediately (an in-flight local catch
// must outrank a stale snapshot row) and strips recently-caught identities
// from `rows` (a snapshot sent before the host processed the catch must not
// resurrect the row).
void NoteIncomingSnapshot(std::vector<ue_wrap::space_renderer::SignalRow>& rows);

void OnDisconnect();

}  // namespace coop::signal_catch_sync
