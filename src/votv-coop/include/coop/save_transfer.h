// coop/save_transfer.h -- v56 save-transfer join bootstrap (host save -> joining client).
//
// THE architecture fix for divergent client worlds (user mandate 2026-06-10: "pull
// all objects data at connecting time and place/spawn objects naturally"): a
// menu-mode joining client no longer generates its own fresh world (whose first-run
// gib spawns put office walls in the air and whose RNG litter never matches). It
// connects AT THE MENU, requests the HOST's save, receives it chunked on the Bulk
// lane, writes it as the EPHEMERAL slot `s_coop_dl`, loads it through the game's own
// LoadStorySave (the engine places every prop naturally, at rest, with the HOST's
// keys), then announces ClientWorldReady -- at which point the host runs the connect
// replay (snapshot bracket + state broadcasts) as a thin all-exact-key true-up for
// whatever moved since the save was written.
//
// NO-STEAL lifecycle (user requirement): the slot is `zcoop_<pid>.sav` -- a prefix
// the game's save MENU never lists, per-instance so same-machine peers can't
// collide. It lives for the SESSION (the game re-reads the slot for sub-level
// subsaves -- deleting right after load would break those; design-workflow B5
// verdict), then is deleted at disconnect, and a boot sweep removes stale zcoop_*
// older than 1 h (never a concurrent sibling's). Clients are already blocked from
// saving during coop (save_guard policy 2026-05-30), so the game never refreshes
// it. data.sav (global progression) is never transferred. Honest limit: bytes on
// a client's disk can be copied by a determined user mid-session; this is
// hygiene + deterrence, not DRM.
//
// Threading: the HOST side (OnRequest / TickHost / CancelForSlot) runs on the game
// thread (event_feed + net_pump). The CLIENT side spans three threads -- the bulk
// sink (net thread), OnBegin (game thread), the harness join poll (timeline
// thread) -- so client state sits behind one small mutex. File I/O only; the one
// engine interaction (loading the slot) is the HARNESS's, not ours (principle 7).

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::save_transfer {

// The ephemeral client-side slot name, per-instance: "zcoop_<pid>". The zcoop_
// prefix is OUTSIDE the game's menu-listed families (s_/b_), so it never shows
// in the load menu; GameMode comes from SaveTransferBeginPayload.gameMode (the
// host's), threaded through LoadStorySave's forceGameMode (the prefix-match
// can't map an unknown prefix).
std::wstring CoopSlotName();

// Register the bulk sink with the session + remember the session pointer for
// sends. Call once at harness boot, before any session starts.
void Install(coop::net::Session* session);

// ---- HOST side (game thread) ---------------------------------------------------

// The slot name the host's world was loaded from (harness boot / Host-Game picker
// set this). The transfer reads `<SaveGames>\<slot>.sav` fresh per request.
void SetHostSlot(const std::wstring& slot);

// Read the host slot name (v73 per-player inventory: keys the per-save
// <SaveGames>\<slot>\coop_players\ dir). Empty until SetHostSlot runs. Game thread.
const std::wstring& HostSlot();

// A client asked for the save (event_feed SaveTransferRequest). Arms the slot's
// stream; the actual FILE READ happens in TickHost under the torn-read guard
// (VOTV writes saves non-atomically in place -- save_guard.h: 4 in-place writes,
// no temp+rename -- so the file is only trusted when size+mtime are stable
// across two polls AND two full reads CRC-identical; design-workflow B1).
// A missing file sends SaveTransferBegin{totalBytes=0} -> fresh-world fallback.
void OnRequest(int peerSlot);

// Host pump: per active slot, run the stable-read attempt (until the blob is
// captured) then chunk sends paced by send-buffer backpressure (a failed send
// stops the pass; retried next tick). Called from net_pump::Tick on the host.
void TickHost();

// Peer left mid-stream -- drop its pump state (net_pump disconnect edge).
void CancelForSlot(int peerSlot);

// ---- CLIENT side ----------------------------------------------------------------

enum class ClientState : int {
    Idle = 0,         // not armed (env/autotest flow, or host role)
    WaitingBegin,     // armed + request sent (or queued) -- nothing received yet
    Receiving,        // Begin seen and/or chunks flowing
    ReadySlotWritten, // blob complete + CRC ok + s_coop_dl.sav written -- LOAD IT
    NoSaveAvailable,  // host has no save (totalBytes=0) -> fresh-world fallback
    Failed,           // CRC mismatch / write failure -> fresh-world fallback (logged)
};

// Arm the transfer (menu-mode browser join only; call BEFORE StartCoopSession).
// Env/autotest clients that already booted a world never arm -- they keep the
// fresh-world + true-up baseline and the host never streams to them.
void ClientArm();
bool ClientArmed();

// Connect edge reached (net_pump, client): send the SaveTransferRequest once if
// armed. Idempotent.
void ClientNoteConnected();

// SaveTransferBegin arrived (event_feed, game thread).
void OnBegin(const coop::net::SaveTransferBeginPayload& p);

// Poll the state machine (harness timeline thread drives the join on it).
ClientState GetClientState();

// Download progress for the loading screen (bytes). total==0 until Begin.
void GetProgress(uint32_t& doneBytes, uint32_t& totalBytes);

// The host's GameMode for the transferred save (from Begin; 0=story default).
// The harness threads it into LoadStorySave's forceGameMode for the zcoop slot.
uint8_t ReceivedGameMode();

// Boot-time sweep: delete stale zcoop_*.sav older than ~1 h (crash leftovers),
// NEVER a fresh one (a concurrent same-machine sibling may be mid-join).
void CleanupStaleSlotsAtBoot();

// Full client-side reset + delete THIS instance's zcoop_<pid>.sav (net_pump
// aggregate disconnect -- the end of the no-steal window).
void OnDisconnect();

}  // namespace coop::save_transfer
