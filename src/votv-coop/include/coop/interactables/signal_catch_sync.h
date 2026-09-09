// coop/interactables/signal_catch_sync.h -- the STOLAS signal-catch CONSUME REPLAY. Overview:
// docs/signals.md. Game thread throughout.
//
// A successful ping runs a native chain on the CATCHING peer only: coord_signalData
// takes the gathered row, the sky row is deleted, the download machine resets, the
// ping sound plays, and every gamemode dish slews to the target.
//
// This module relays the catch as ONE host-validated world event and replays its
// IDENTITY half -- the signal data, the sky-row delete, the machine reset, the ping
// sound -- on every other peer. The dish THEATER half is host-only: the host
// replays StartMovingAll and streams the poses (dish_sync), while a client never
// slews from the wire and its own unpreventable ping slews are killed with cleanup
// right after the catch payload is sent (dish_sync::KillOwnPingSlews). The download
// arm rides the host-authored DishArm lane.

#pragma once

#include "coop/net/protocol.h"
#include "ue_wrap/desk/space_renderer.h"

#include <cstdint>
#include <vector>

namespace coop::net { class Session; }

namespace coop::signal_catch_sync {

void Install(coop::net::Session* session);

// 1 Hz: the catch and cleared detectors, plus recent-catch TTL pruning. Cheap when
// idle (one struct read).
//
// The catch detector is UNGATED: it fires on a change-edge of the coord_signalData
// identity tuple (x, y, z, frequency, objectName) to a non-None state, with no
// claim check in front of it. That is sound because the field has exactly two
// native writers -- ping-success, which assigns the row, and the delete chain,
// which assigns None -- and our own wire appliers prime these baselines, so an
// unprimed local change IS a catch. It is also necessary: a claim-anchored gate
// loses by construction, because the ping's own completion releases the desk hold
// within the same second as the edge, and the baseline then rolls forward over the
// catch permanently.
void Tick();

// Wire ingest (both roles). HOST: dedups kind=0 through the recent-catch TTL,
// replays locally (the StartMovingAll theater included), rebroadcasts to everyone
// but the catcher, and feeds the activity line; drops kind=2, which is
// host-authored only. CLIENT: replays the identity half only (transport-trusted --
// clients only ever receive from the host; senderSlot is the stamped logical
// catcher).
//
// kind=0 is a catch and lands one activity-feed line per peer, phrased for the
// catcher or for a watcher from the stamped origin. kind=1 is a CLEAR: the 'Signal
// data deleted' button, seen as an objectName-to-None edge on ANY peer, unclaimed,
// matching that physical button's own authority model.
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
