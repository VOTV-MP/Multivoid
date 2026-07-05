// coop/player_handshake.h -- Player-Element mirror exchange (Join + AssignPeerSlot).
//
// Gameplay/network layer (principle 7). Extracted from event_feed.cpp on
// 2026-05-29 (C5) to keep event_feed below the 800 LOC soft cap after the
// A4 v13 wire migration pushed it to 841 LOC. The handshake is its own
// subsystem: it owns the local + per-slot nickname state, the per-slot
// Join-sent latch, and the two reliable kinds (Join, AssignPeerSlot) that
// carry the mirror Element ids between peers.
//
// Mirror exchange model (A4 / v13):
//   - Host -> Client (AssignPeerSlot): host stamps its local Player
//     Element id (hostElementId) when issuing the slot assignment.
//     Client EstablishMirrorForSlot(0, hostElementId).
//   - Peer -> Peer (Join): each peer's Join payload prepends its own
//     senderElementId (uint32). Receiver EstablishMirrorForSlot(
//     senderPeerSlot, senderElementId).
//   - Result: both peers' element::Registry agree on the same id for
//     each Player Element.
//
// State ownership split with event_feed:
//   - player_handshake owns: g_localNick, g_remoteNickBySlot, g_joinSentBySlot.
//   - event_feed owns: g_lastConnectedBySlot (connect/disconnect edge
//     detector for the "<X> left the game" feed message).
//   - On a per-slot disconnect, event_feed calls NicknameForSlot for the
//     hud message and OnSlotDisconnected to reset our Join-sent latch so
//     a reconnect re-announces.
//
// Game thread only (it reads/writes UE engine objects via Puppet().SetNickname,
// chat_feed::Push, players::Registry, and reads our process-local cfg).

#pragma once

#include "coop/net/session.h"

#include <cstdint>
#include <string>
#include <vector>

namespace coop::player_handshake {

// Set the local player's display name. Sanitized on the way in (see
// SanitizeNickname inside the .cpp -- the same sanitizer that runs on
// inbound peer nicknames, so both ends agree on the displayable form).
void SetLocalNickname(const std::wstring& nick);

// Read the local player's (sanitized) display name. Game thread only (returns a
// reference to the game-thread-owned string). Used by the roster snapshot.
const std::wstring& LocalNickname();

// v73 (per-player inventory): set the local player's durable identity GUID (32 hex chars
// from harness::config::ReadPlayerGuid). Seeded once at boot, appended to our Join so the
// HOST can key this peer's inventory file (coop_players/<guid>.json). ASCII; not displayed.
void SetLocalGuid(const std::string& guid);

// HOST-side read of the GUID a peer sent in its Join, by peer slot. Empty until that peer's
// Join lands (or if it sent none -> first-join/empty inventory). Game thread only.
const std::string& GuidForSlot(int slot);

// True iff `guid` is exactly 32 hex chars ([0-9a-fA-F]) -- the durable-identity format. The GUID
// becomes a HOST FILESYSTEM PATH COMPONENT (coop_players/<guid>.json), so a remote-supplied GUID
// MUST be validated to this charset before use: it is the only thing that keeps a hostile/tampered
// Join (guid="..\\..\\evil") from steering a host file write outside coop_players (path traversal).
// Hex is inherently filesystem-safe (no '.', '/', '\\', ':'). Used at the wire boundary AND in
// PlayerFilePath (defense in depth). Pure; any thread.
bool IsValidGuid(const std::string& guid);

// v93 skins: the skin name peer `slot` announced (Join field / SkinChange).
// Empty until known -> the puppet spawns native kel and is re-skinned when the
// name lands. Game thread only.
const std::string& SkinForSlot(int slot);

// v93 skins: announce the LOCAL player's skin change mid-session. Client ->
// host (slot 0); host -> every ready client (slot=0 payload). The at-join
// announce needs no call -- MaybeSendJoinToSlot reads local_body::LocalSkinName
// when building the Join payload. Game thread only.
void AnnounceLocalSkin(coop::net::Session& session, const std::string& name);

// v93 skins: handle a delivered SkinChange ([u8 slot][u8 len][name]). Host:
// sender forgery-checked (slot == senderPeerSlot), stored, applied to the
// slot's puppet, rebroadcast to the other clients. Client: host-only sender
// accepted, stored, applied. Returns true when recognized.
bool HandleSkinChange(coop::net::Session& session,
                      const coop::net::Session::ReliableMessage& msg);

// v94 nameplate pref: announce the LOCAL player's plate visibility mid-session
// (same trust/relay shape as AnnounceLocalSkin). The at-join state rides the
// prefs flags byte in the Join payload. Game thread only.
void AnnounceLocalNameplate(coop::net::Session& session, bool visible);

// v94: handle a delivered NameplateChange ([u8 slot][u8 visible]). Host:
// forgery-checked + stored (coop::nameplate) + rebroadcast; client: host-only
// sender, stored. Returns true when recognized.
bool HandleNameplateChange(coop::net::Session& session,
                           const coop::net::Session::ReliableMessage& msg);

// v103 nick color (12f): announce the LOCAL player's nick color mid-session
// (same trust/relay shape as AnnounceLocalNameplate; packed 0 = reset to
// default). The at-join state rides the [has][r][g][b] field in the Join
// payload. Game thread only.
void AnnounceLocalNickColor(coop::net::Session& session, uint32_t packed);

// v103: handle a delivered NickColorChange ([u8 slot][u8 has][r][g][b]). Host:
// forgery-checked + stored (coop::nick_color) + rebroadcast; client: host-only
// sender, stored. Returns true when recognized.
bool HandleNickColorChange(coop::net::Session& session,
                           const coop::net::Session::ReliableMessage& msg);

// Reset per-slot caches. Called from event_feed::OnSessionStart so a
// Session::Stop()/Start() in the same process sees clean state.
void Reset();

// Per-tick connect-edge Join sender for one slot. event_feed iterates
// over slots, detects the connect edge, and calls this. joinPayload +
// joinPayloadBuilt are lazy-build state shared across the loop so we
// don't pay the UTF-8 conversion + heap alloc every 8 ms once Join has
// already been sent to every slot.
//
// Sender holds off (returns without sending and without latching) when
// the local Player Element id isn't allocated yet (boot/seed race
// window after AssignPeerSlot, before EnsurePlayerElement_ runs). The
// guard is bounded (~1 net pump tick = ~8 ms at 125 Hz). Send is also
// no-op if the slot has already received our Join.
void MaybeSendJoinToSlot(coop::net::Session& session, int slot,
                          std::vector<uint8_t>& joinPayload,
                          bool& joinPayloadBuilt);

// Per-slot disconnect edge: clear the Join-sent latch so a reconnect
// re-announces. Called from event_feed once it has consumed the
// nickname for the "<X> left the game" hud message.
void OnSlotDisconnected(int slot);

// Read-only access to the nickname cached for a peer slot. Returns
// "Remote player" placeholder when no Join has landed for that slot
// yet. Used by event_feed::Update to build the per-slot disconnect
// hud message ("<X> left the game").
const std::wstring& NicknameForSlot(int slot);

// Two-phase join announcement (2026-06-15, seam moved 2026-07-03): the Join handshake announces
// "<nick> is connecting to the game" (connected, not loaded/spawned yet); net_pump calls THIS the
// moment the peer's puppet actually SPAWNS -- the visible appearance the "<nick> joined the game"
// line must coincide with (user 2026-07-03; the old host announce on ClientWorldReady+5s ran ~6 s
// before the puppet in the measured live flow). Role-aware: on a CLIENT, slot 0 is the HOST whose
// game WE joined ("Joined <host>'s game", kept at +5 s -- own loading screen, user 2026-06-21).
// net_pump calls it for the CLIENT role unconditionally (gated on the client's own
// g_worldReadyAnnounced) and for the HOST role once IsSlotWorldReady(slot) holds (a pre-world
// menu/loading pose can spawn the puppet early -- user 2026-06-17; that order is covered by
// OnClientWorldReady below). Joiner lines are latched once per join (cleared on slot disconnect),
// so the two seams never repeat. Game thread (chat_feed push).
void AnnouncePeerSpawned(net::Role role, int slot);

// HOST-side reverse-order cover for the join line: fired from event_feed when a client's
// ClientWorldReady reliable lands. If the slot's puppet ALREADY spawned (pre-world menu pose --
// the 2026-06-17 case), the body is standing here and world-ready is the moment it becomes the
// real joiner -> announce now (same once-per-join latch). Normal flow (spawn after world-ready,
// the measured live order) leaves this a no-op; the spawn seam announces. Game thread.
void OnClientWorldReady(int slot);

// Handle a delivered reliable Join message. Parses the v13 prefix
// (senderElementId), then the nickname (UTF-8 length-prefixed),
// sanitizes the nickname, sets the puppet's nameplate, and posts the
// chat_feed entry. Returns true if the message was a recognized Join
// (regardless of validation outcome); returns false if payloadLen is
// too short for the header (caller may log).
bool HandleJoinMessage(coop::net::Session& session,
                       const coop::net::Session::ReliableMessage& msg);

// Handle a delivered reliable AssignPeerSlot message. Stamps the
// client's LocalPeerId and (v13) installs the host's mirror Player
// Element via EstablishMirrorForSlot when hostElementId is present
// and valid. Drops on host side (host self-assigns). Returns true if
// the message was recognized; false on payload-too-short.
bool HandleAssignPeerSlot(coop::net::Session& session,
                          const coop::net::Session::ReliableMessage& msg);

// HOST-side: cross-peer identity broadcast for the host-relay topology
// (PR-FOUNDATION Tier 2 T2-1). Called from HandleJoinMessage once the
// host has established the joiner's mirror + stored its nick. Performs
// the MTA InitialDataStream two-way exchange:
//   (1) sends PlayerJoined{joiner} to every OTHER connected client, and
//   (2) sends PlayerJoined{X} to the joiner for every already-known
//       client X (X != joiner, X != host).
// No-op unless this peer is the host. `joinerSlot` is the slot whose
// Join just arrived; `joinerEid` its Player Element id; `joinerNick`
// its (already-sanitized) nickname.
void BroadcastPlayerJoinedFromHost(coop::net::Session& session,
                                   int joinerSlot,
                                   uint32_t joinerEid,
                                   const std::wstring& joinerNick);

// CLIENT-side: handle a delivered reliable PlayerJoined message
// describing a THIRD peer (another client). Range-validates the eid,
// installs the peer's mirror Player Element via EstablishMirrorForSlot,
// and caches its nickname so the puppet (spawned later on the first
// relayed pose) is born identified. Drops on host side (host originates
// these; never receives them). Returns true if recognized; false on
// payload-too-short.
bool HandlePlayerJoined(coop::net::Session& session,
                        const coop::net::Session::ReliableMessage& msg);

}  // namespace coop::player_handshake
