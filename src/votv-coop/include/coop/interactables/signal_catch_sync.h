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
// Catcher detection (1 Hz poll, claim holder only, since L4): the
// coord_signalData IDENTITY TUPLE (x,y,z,frequency | objectName) CHANGE-edge
// to a non-None state. Derivation (impl-RE SS7/SS8): coord_signalData has
// exactly TWO native writers -- ping-success (:= row) and the delete chain
// (:= None) -- and our wire writes are apply+primed, so a local change under
// the claim IS a catch, definitionally. (The old MovingCount rising-edge
// signature is retired: it was indirect, raced the L4 kill sweep, and ATE a
// catch pinged while dishes already moved.) kind=1 (cleared): the 'Signal
// data deleted' button = objectName -> 'None' edge on ANY peer (unclaimed
// trust, matching the physical button's authority model).
//
// Game thread throughout.

#pragma once

#include "coop/net/protocol.h"
#include "ue_wrap/space_renderer.h"

#include <cstdint>
#include <vector>

namespace coop::net { class Session; }

namespace coop::signal_catch_sync {

void Install(coop::net::Session* session);

// 1 Hz: the catch + cleared detectors and recent-catch TTL pruning. Cheap
// when idle (one struct read).
void Tick();

// Wire ingest (both roles). HOST: validates kind=0 against the desk-claim
// holder, replays locally (incl. the StartMovingAll theater), rebroadcasts to
// everyone but the catcher. CLIENT: replays the identity half only
// (transport-trusted -- clients only receive from the host).
void OnReliable(const coop::net::SkySignalCatchPayload& p, uint8_t senderSlot);

// HOST: if coord_signalData is armed, send the joiner one kind=0 replay for
// the signal IDENTITY (the poses/arm ride dish_sync's snapshot + DishArm rows
// on the same ordered lane, queued right after this).
void QueueConnectBroadcastForSlot(int peerSlot);

// Called by console_state_sync BEFORE applying an assembled SkySignalState
// snapshot: runs the catch detector immediately (an in-flight local catch
// must outrank a stale snapshot row) and strips recently-caught identities
// from `rows` (a snapshot sent before the host processed the catch must not
// resurrect the row).
void NoteIncomingSnapshot(std::vector<ue_wrap::space_renderer::SignalRow>& rows);

void OnDisconnect();

}  // namespace coop::signal_catch_sync
