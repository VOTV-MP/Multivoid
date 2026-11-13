// The desk INPUT lane (docs/signals.md): every input-class scalar -- knob speeds,
// filter toggles, polarity, volume, select, max level, the four unit power
// toggles -- is shared last-writer state, polled per field and shipped as a delta.
//
// A poll rather than a hook, because the desk's verbs dispatch inside the
// Blueprint VM below the ProcessEvent detour; claim-free, because the desk claim
// engages only on the screen's active-interface edge, which the download unit's
// physical buttons never set. The host relays a delta to every peer but its
// author -- an echo would revert a newer local value -- and a receiver primes its
// own baseline for that field in the same game-thread task.
//
// The ping flag rides the wire as bookkeeping and is never written into a
// receiver's machine: it is the run-flag of a latent tick machine, and a raw apply
// wakes a second copy of it on every observer. One ping runs, the presser's, and
// its rising edge holds the desk claim for the run.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct DeskInputPayload;
struct DeskScanEventPayload;
}

namespace coop::desk_input_sync {

void Install(coop::net::Session* session);

// Game thread, per pump tick (internally throttled to the 250 ms poll).
void Tick();

// Wire appliers (event feed drain; GT).
void OnDeskInput(const coop::net::DeskInputPayload& p, uint8_t senderSlot);
void OnDeskScan(const coop::net::DeskScanEventPayload& p, uint8_t senderSlot);

// Re-prime every poll baseline from the CURRENT desk fields -- called by the
// join-adopt apply (console_state_sync) after it seeds the scalars, so the
// seeded values never read as local edges.
void PrimeBaselines();

// Draw the signal view in `spectrogram` mode on the local desk. A desk whose screen widget is not live
// yet (a join adopt can land first) cannot draw it: the mode is kept and applied by the desk's BeginPlay watch
// when the widget comes up. True when drawn now. Game thread.
bool ApplyVisualizerModeOrDefer(bool spectrogram);

// The same for the remap bounds' repaint (updRemap), which the join adopt owes after it wrote them.
bool RepaintRemapOrDefer();

// The same for the refiner's cap label (console_desk::RepaintCapLevel), owed whenever the cap level or the Auto
// Processing module changed by anything but this peer's own cap press: a wire apply, a join adopt, or any
// module plug or unplug, this peer's own included, since the game repaints the label from neither. A desk whose
// screen widget is not assigned yet keeps the repaint owed, and the desk's next watched event (its BeginPlay, a
// local cap press, another cap label repaint) runs it.
bool RepaintCapLevelOrDefer();

// HOST: a peer left -- if it was the live ping setter, clear the attribution (the
// desk-claim deny would otherwise outlive the leaver). No machine write: no peer's
// machine carries a wire-applied ping flag.
void OnPeerLeft(int slot);

// HOST: the slot whose ping FSM is currently running (0 = the host itself),
// 0xFF when none: a client's own, from its deltas, else the one a verdict the
// host's machine runs for. device_occupancy's desk-claim arbitration consults this.
uint8_t PingActiveSlot();

// HOST: the host's machine was just primed with `slot`'s verdict (desk_ping_sync):
// its next rising edge is that client's run, to its falling edge. 0xFF withdraws a
// push whose run never rose.
void AttributePrimedRun(uint8_t slot);

// HOST, connect edge (ConnectReplayForSlot): re-derive the ping attribution from
// the machine's ground truth. A solo host's rising edge is absorbed into the
// unwired baseline (Tick's !connected() branch never diffs), so a client joining
// mid-host-ping would otherwise see no desk hold. Reads the ping flag; TRUE ->
// setter = 0, since only the host's own machine can be running when no peer was
// connected to author a delta.
void SeedPingAttributionFromMachine();

void OnDisconnect();

}  // namespace coop::desk_input_sync
