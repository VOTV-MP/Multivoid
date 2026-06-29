// coop/signal_catch_sync.h -- v70: the STOLAS signal-catch CONSUME REPLAY.
//
// RE ground truth (votv-stolas-signal-catch-RE-2026-06-12.md): the SP chain
// after a successful ping -- coord_signalData := gatherSignal.data -> row
// delete -> download-machine reset -> playPingSound -> startMovingTo on EVERY
// gamemode dish -> (native, per peer) arrival -> activeDishes all-false ->
// dishesStop broadcast -> the desk arms formDownload(0,-1) -- ran on the
// CATCHING peer only. Every other peer's downloader said NO SIGNAL forever
// (DL_signalDownloadData.mesh never valid) and its dishes never slewed.
//
// This module relays the catch as ONE host-validated world event and replays
// it on every other peer; the native dish-arrival chain then re-arms the
// downloader, the letter-board lamps, and the signalFound trigger everywhere
// with zero extra wire. MTA shape: a one-shot world event relayed host-
// authoritatively + local simulation of its deterministic consequences (the
// CClientExplosionManager / projectile precedent).
//
// Catcher detection (1 Hz poll, claim holder only): a row vanished from
// `signals` AND a dish isMoving count ROSE in the same poll window -- the
// dish edge is the catch-success signature (expiry deletes rows without
// moving dishes; a FAILED ping writes coord_signalData but moves no dish;
// and unlike the RE doc's struct-changed AND-detector this also catches a
// success on an identity the struct already pointed at from an earlier
// failed ping). kind=1 (cleared): the 'Signal data deleted' button =
// coord_signalData.objectName -> 'None' edge on ANY peer (unclaimed trust,
// matching the physical button's authority model).
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

// 1 Hz: the catch + cleared detectors, the pending download-adopt apply, and
// recent-catch TTL pruning. Cheap when idle (one struct read + one array
// walk; dish reads only while holding the desk claim).
void Tick();

// Wire ingest (both roles). HOST: validates kind=0 against the desk-claim
// holder, replays locally, rebroadcasts to everyone but the catcher. CLIENT:
// replays (transport-trusted -- clients only receive from the host).
void OnReliable(const coop::net::SkySignalCatchPayload& p, uint8_t senderSlot);

// HOST: if coord_signalData is armed, send the joiner one kind=0 replay so
// its dishes slew (or, with no dish in flight, its downloader arms directly);
// the DeskState adopt's dlDecoded/dlPolarity then true up its progress.
void QueueConnectBroadcastForSlot(int peerSlot);

// Called by console_state_sync BEFORE applying an assembled SkySignalState
// snapshot: runs the catch detector immediately (an in-flight local catch
// must outrank a stale snapshot row) and strips recently-caught identities
// from `rows` (a snapshot sent before the host processed the catch must not
// resurrect the row).
void NoteIncomingSnapshot(std::vector<ue_wrap::space_renderer::SignalRow>& rows);

// Called by console_state_sync's OnDeskState on an adopt=1 snapshot: queue
// the host's download progress; applied via formDownload(decoded, polarity)
// + the resDetect raw write once OUR download machine arms (mesh valid --
// before that, the native mesh-invalid pulse would zero resDetect again).
// Safe to leave armed: coord_signalData=='None' implies host decoded==0/-1
// (the clear chain rebuilds DLData zeroed), so a stale pending is an
// idempotent no-op over a fresh arm.
void SetPendingDownloadAdopt(float decoded, int32_t polarity, float resDetect);

void OnDisconnect();

}  // namespace coop::signal_catch_sync
