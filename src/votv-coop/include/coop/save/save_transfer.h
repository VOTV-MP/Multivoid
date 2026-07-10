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

#include "coop/element/element.h"  // ElementId (v86 Path 1c save-time pile map)
#include "coop/net/protocol.h"
#include "ue_wrap/types.h"  // ue_wrap::FVector

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

// v86 Path 1c: the SAVE-TIME position of keyless chipPile `eid` for `peerSlot`,
// captured at the blob instant (OnRequest). Returns false (out untouched) for a
// stale-fallback join, an unseeded/post-save pile, or an out-of-range slot. The
// connect-replay snapshot builder (prop_snapshot) stamps it onto the pile's
// PropSpawn so the client twin-destroy reconciles a host-moved-in-window pile.
// Game thread.
bool TryGetSaveTimePileXform(int peerSlot, coop::element::ElementId eid, ue_wrap::FVector& out);

// docs/piles/09 (4th mirror-identity instance): record the PRE-GRAB position of pile `eid` into
// every active join slot's blob pile map. Called by OnPileGrabPre (host, game thread) at the
// InpActEvt PRE edge, BEFORE the BP morphs the pile -> clump (so the pos == its save/native pos).
// No-op outside a join. Lets the kToPile LAND convert carry the save-time key for the client.
void RecordGrabTimePileXform(coop::element::ElementId eid, const ue_wrap::FVector& preGrabLoc);

// docs/piles/09: like TryGetSaveTimePileXform but searches ALL active join slots (BroadcastConvert
// is a single fan-out, not per-joiner; a pile eid is unique). Game thread.
bool TryGetSaveTimePileXformAnySlot(coop::element::ElementId eid, ue_wrap::FVector& out);

// b3 (v90) + F1 (2026-07-09): at `peerSlot`'s world-ready, send a PropSnapPos position correction for every
// save-authoritative entity -- chipPILE and KEYED prop -- whose CURRENT host actor position diverges from THIS
// joiner's save-time position (a pile/prop the host MOVED during the join window). A pile carries no position
// in the connect snapshot; a keyed prop DOES, but the joiner's loadObjects RECREATES it at the save pos AFTER,
// clobbering it -- both stale on the joiner, both fixed by re-asserting the host's pos at quiescence. Iterates
// g_blobPileXforms + g_blobKeyedXforms[peerSlot], resolves each eid's live host actor, position-compares, and
// SendReliableToSlot's the diverged. Called from ConnectReplayForSlot AFTER the gate opened. Game thread.
void FlushDivergedSavePositionsForSlot(int peerSlot);

// scope A (kerfur off->active dup retire, 2026-06-24): the SAVE-TIME position of OFF-form kerfur `eid`,
// captured at the blob instant (OnRequest), searched across ALL active peer slots' blob maps (the host
// turn-on broadcast that carries this is a single fan-out, not per-joiner, and a kerfur off-prop's host
// eid is unique). Returns false (out untouched) if no slot captured this eid (stale-fallback join, a
// kerfur bought after the save, or one already ACTIVE at every blob instant). The host stamps it onto the
// KerfurConvert at BindFormActor so the joining client retires its stale local off-prop at the exact key.
// Game thread.
bool TryGetSaveTimeKerfurXformAnySlot(coop::element::ElementId eid, ue_wrap::FVector& out);

// R2 (2026-06-17, MTA Packet_EntityRemove): send EXPLICIT per-key PropDestroy to
// `peerSlot` for every keyed prop its save-transfer BLOB contained that the host's
// LIVE world no longer has (e.g. a prop the host grabbed/destroyed/converted during
// the ~30-60s the joiner spent downloading + loading). Diffs the key-set captured
// at blob-capture (OnRequest, LIVE-capture path) against the current live key-set.
// This replaces the divergence sweep's destructive "unclaimed -> infer-delete" for
// the save-transfer case (where every kerfur-dupe regression lived). Per-slot (the
// divergence is specific to THIS joiner's blob), Bulk lane. Call from
// ConnectReplayForSlot BEFORE prop_snapshot::TriggerForSlot so removes precede the
// snapshot's adds. No-op if no live-capture baseline was taken (stale-fallback join).
// Game thread.
void SendBlobDivergenceDeletes(int peerSlot);

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
