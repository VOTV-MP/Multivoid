// coop/interactables/signal_catch_sync.h -- the STOLAS signal-catch CONSUME REPLAY. Overview:
// docs/signals.md. Game thread throughout.
//
// A successful ping runs a native chain on the machine that rolled its verdict, which is
// the host alone (desk_ping_sync rolls a client's verdict there): coord_signalData takes
// the gathered row, the sky row is deleted, the download machine resets, the ping sound
// plays, and every gamemode dish slews to the target.
//
// This module relays the host's catch as ONE world event and replays its IDENTITY half --
// the signal data, the sky-row delete, the catch's own two writes to the download machine --
// on every client. The dish THEATER half is the host's: its native slews stream as poses
// (dish_sync), and a client never slews from the wire. The download arm and reset ride the
// host-authored DishArm lane (coop/interactables/download_arm_sync).

#pragma once

#include "coop/net/protocol.h"
#include "ue_wrap/desk/space_renderer.h"

#include <cstdint>
#include <vector>

namespace coop::net { class Session; }

namespace coop::signal_catch_sync {

void Install(coop::net::Session* session);

// 1 Hz: the catch detector, plus recent-catch TTL pruning. Cheap when
// idle (one struct read).
//
// The catch detector is UNGATED: it fires on a change-edge of the coord_signalData
// identity tuple (x, y, z, frequency, objectName) to a non-None state, with no
// claim check in front of it. That is sound because the field has three native
// writers -- ping-success, which assigns the row, the delete chain, which assigns
// None, and the desk's setData, a save's restore on every load of a save -- and the
// restore and our own wire appliers prime these baselines, so an unprimed change on
// the host IS its catch. A client's is no catch of its own, since its verdicts are the
// host's: it is logged and not relayed. The gate is also necessary: a claim-anchored
// gate loses by construction, because the ping's own completion releases the desk
// hold within the same second as the edge, and the baseline then rolls forward over
// the catch permanently.
void Tick();

// Wire ingest, a client's: the host's receive edge refuses a client's copy (client_authorship.h),
// so only the host's catches arrive. Applies both kinds, replaying the identity half (senderSlot is
// the stamped logical catcher), and announces kind 0 only.
//
// kind=0 is a catch and lands one activity-feed line per peer, phrased for the
// catcher or for a watcher from the stamped origin. A clear is not this lane's: the
// host's signal-deleted reset reaches every peer on coop/interactables/download_arm_sync.
void OnReliable(const coop::net::SkySignalCatchPayload& p, uint8_t senderSlot);

// HOST: if coord_signalData holds a caught signal, send the joiner one kind=2 STATE-SEED
// for the signal IDENTITY; a reset since the joiner's capture goes ahead of the desk's
// whole seed, and the dish snapshot and the download's arm row after this
// (download_arm_sync). kind=2 applies like a catch but never announces to the activity feed;
// it also carries the catch of a verdict whose client left before the detector found it.
void QueueConnectBroadcastForSlot(int peerSlot);

// Called by console_state_sync BEFORE applying an assembled SkySignalState
// snapshot: runs the catch detector immediately (an in-flight local catch
// must outrank a stale snapshot row) and strips recently-caught identities
// from `rows` (a snapshot sent before the host processed the catch must not
// resurrect the row).
void NoteIncomingSnapshot(std::vector<ue_wrap::space_renderer::SignalRow>& rows);

void OnDisconnect();

// HOST: the signal at (x, y, z, frequency) that this machine's detector finds next is `slot`'s catch, the verdict of a
// ping the host's desk rolled for that client (desk_ping_sync): it is announced with that slot and sent to every client,
// the slot's own included, stamped as its. One mark at a time, dropped when the slot leaves.
void AttributeCatch(float x, float y, float z, float frequency, uint8_t slot);

// HOST: a peer left; a catch marked as its own is no longer anyone's.
void OnPeerLeft(uint8_t slot);

// HOST: the catches its detector relayed as a client's (AttributeCatch), for the drill.
uint64_t AttributedCatchesRelayed();

// The catches this peer detected and relayed since its session began: a drill's readiness that a caught
// signal it wrote is on the wire, ahead of anything it sends after. Game thread.
uint64_t LocalCatchesRelayed();

}  // namespace coop::signal_catch_sync
