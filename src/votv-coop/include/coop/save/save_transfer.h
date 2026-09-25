// coop/save/save_transfer.h -- the save-transfer join bootstrap: the host's save to the joining
// client. A menu-mode joining client does not generate its own fresh world (its first-run
// spawns and litter would never match the host's): it connects at the menu, requests the
// host's save, receives it chunked on the bulk lane, writes it as the ephemeral slot
// zcoop_<pid>, loads it through the game's own story load (the engine places every prop at
// rest, with the host's keys), then announces world-ready, and the host runs the connect
// replay (the key diff, the snapshot bracket, the state broadcasts) as a thin true-up for
// whatever moved since the save was written. The zcoop_ prefix is one the game's save menu
// never lists, per instance so same-machine peers cannot collide. The slot lives for the
// session (the game re-reads it after the first load); a boot sweep removes stale zcoop_*
// older than an hour, never a concurrent sibling's. Clients are
// blocked from saving during coop, so the game never refreshes it; only the slot file is
// transferred, never the global progression file; a copy of the bytes mid-session is not
// prevented. The host side runs on the game thread; the client side spans the net thread (the
// chunk and begin sinks) and the harness join loop (the polls), behind one small mutex.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::save_transfer {

// The ephemeral client-side slot name, per instance: zcoop_<pid>. The prefix is outside the
// game's menu-listed families, so it never shows in the load menu; the game mode comes from
// the begin payload (the host's), threaded through the story load's forced mode, since the
// prefix match cannot map an unknown prefix.
std::wstring CoopSlotName();

// Register the chunk and begin sinks with the session and remember the session pointer for
// sends. Once at harness boot, before any session starts.
void Install(coop::net::Session* session);

// Host side, game thread.

// The slot name the host's world was loaded from (harness boot or the host picker set it), empty
// for a New Game that has no file yet. The transfer reads the slot's .sav under the save directory
// fresh per request.
void SetHostSlot(const std::wstring& slot);

// The host slot name; the per-player inventory keys its per-save directory on it. Empty until
// SetHostSlot. Game thread.
const std::wstring& HostSlot();

// How many times SetHostSlot has run. A process names the slot once per world it loads to host, so
// this tells state that belongs to ONE loaded world (coop/player/player_profile_store) that
// another has taken its place. Game thread.
uint32_t HostSlotSerial();

// A client asked for the save. Arms the slot's stream; the file read happens in TickHost under
// the torn-read guard (the game writes saves non-atomically in place, so the file is trusted
// only when its size and mtime are stable across consecutive polls and two full reads are
// CRC-identical). A missing file sends a zero-byte begin: the fresh-world fallback.
void OnRequest(int peerSlot);

// The host pump: per active slot, the stable-read attempt until the blob is captured, then
// chunk sends paced by send-buffer backpressure (a failed send stops the pass; retried next
// tick). From the net pump's tick on the host.
void TickHost();

// A peer left mid-stream: drop its pump state (the disconnect edge).
void CancelForSlot(int peerSlot);

// Whether the host has taken `peerSlot`'s world for this connection's join, live or from the file on
// disk: from that instant a change the host makes reaches the joiner only through the lanes' own
// replays at its world-ready. False again when the slot leaves. Host, game thread.
bool WorldTakenFor(int peerSlot);

// Client side.

enum class ClientState : int {
    Idle = 0,         // not armed (an env or script client, or the host role)
    WaitingBegin,     // armed and the request sent or queued; nothing received yet
    Receiving,        // Begin seen and/or chunks flowing
    ReadySlotWritten,  // blob complete, CRC ok, the zcoop slot written: load it
    NoSaveAvailable,  // the host has no save (a zero-byte begin): the fresh-world fallback
    Failed,           // CRC mismatch or write failure: the fresh-world fallback, logged
};

// Arm the transfer (a menu-mode browser join only; before the session starts). Env and script
// clients that already booted a world never arm: they keep the fresh-world and true-up
// baseline and the host never streams to them.
void ClientArm();

// The connect edge (client): send the request once if armed. Idempotent.
void ClientNoteConnected();

// The begin arrived. Net thread: the session's receive path diverts it to this module's begin
// sink, deliberately the same thread as the chunk sink, since an announce processed on a
// different thread from its payload opened an unbounded pre-begin window. Everything this
// reaches, including the CRC check and the slot write, already runs on the net thread when
// the final chunk completes the transfer.
void OnBegin(const coop::net::SaveTransferBeginPayload& p);

// Poll the state machine; the harness's join wait loop drives the join on it.
ClientState GetClientState();

// Download progress for the loading screen, in bytes; the total is 0 until the begin.
void GetProgress(uint32_t& doneBytes, uint32_t& totalBytes);

// The host's game mode for the transferred save (from the begin; 0 is the story default). The
// harness threads it into the story load's forced mode for the zcoop slot.
uint8_t ReceivedGameMode();

// Boot-time sweep: delete stale zcoop_*.sav older than an hour (crash leftovers), never a
// fresh one (a concurrent same-machine sibling may be mid-join).
void CleanupStaleSlotsAtBoot();

// Full client-side reset and delete this instance's zcoop_<pid>.sav.
void OnDisconnect();

}  // namespace coop::save_transfer
