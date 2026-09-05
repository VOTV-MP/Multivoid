// coop/session/player_handshake.h -- the Player-Element mirror exchange (Join and AssignPeerSlot)
// and the per-slot identity announcements. The host stamps its local Player Element id into
// the AssignPeerSlot it issues, each peer's Join carries its own element id, and the receiver
// binds a mirror for the sender's slot, so every peer's element registry agrees on the id of
// each Player Element. Per-slot identity state (nickname, guid, skin, the join-announced latch)
// lives in the roster ledger; this module owns only the local nickname and the per-slot
// Join-sent latch, a property of the link rather than of the person. Teardown is a ledger row
// transition, which fires on a replacement as well as a departure. Game thread only.

#pragma once

#include "coop/net/session.h"

#include <cstdint>
#include <string>
#include <vector>

namespace coop::player_handshake {

// The display-name cap, with one owner: SanitizeNickname enforces it and the nickname arbiter
// sizes its suffix variants against it, so a variant is never longer than a name the sanitizer
// would accept.
inline constexpr size_t kNickMaxChars = 20;

// Set the local player's requested display name, what the person typed in the browser or the
// ini, sanitized on the way in by the sanitizer that also runs on inbound peer names. Also sets
// the displayed name: until a host says otherwise, what you asked for is what you are.
void SetLocalNickname(const std::wstring& nick);

// Adopt the display name the host assigned us; the host sees every name at once, so uniqueness
// is its call. It arrives on our own roster row. The assigned name is kept, not borrowed:
// adopting writes the displayed name, the requested name and the ini, so the next session asks
// for the assigned name and a later duplicate is the one renamed. That makes the ledger's
// freedom from ghost rows load-bearing, since a rename earned by colliding with your own
// un-reaped row would persist; the ledger's reconcile runs its death pass first.
void AdoptCanonicalNickname(const std::wstring& canonical);

// The local player's displayed name: the host-assigned one once joined, the requested one
// before. Game thread only (a reference to a game-thread string). Every surface that prints
// our name derives from it.
const std::wstring& LocalNickname();

// The requested name, what we ask a host to call us; only the Join payload uses it.
const std::wstring& RequestedNickname();

// Host-side read of a peer's storage GUID by slot: hex(SHA-256(pubkey)[0..16]) of the key the
// peer proved at admission, copied into the roster row when its Join lands. Empty on a client,
// and on a host until the Join arrives. Game thread only.
const std::string& GuidForSlot(int slot);

// True iff `guid` is exactly 32 hex characters. The GUID becomes a host filesystem path
// component (coop_players/<guid>.json), so a remote-supplied one is validated to this charset
// before use, which is what keeps a hostile Join from steering a file write outside that
// directory. Used at the wire boundary and in the path builder. Pure; any thread.
bool IsValidGuid(const std::string& guid);

// The skin name peer `slot` announced (a Join field or a SkinChange); empty until known, in
// which case the puppet spawns with the native body and is re-skinned when the name lands.
// Game thread only.
const std::string& SkinForSlot(int slot);

// Announce the local player's skin change mid-session: client to host, host to every ready
// client. The at-join announce needs no call; the Join builder reads local_body::LocalSkinName.
// Game thread only.
void AnnounceLocalSkin(coop::net::Session& session, const std::string& name);

// Handle a delivered SkinChange ([u8 slot][u8 len][name]). Host: the sender must match the
// slot, then stored, applied to the slot's puppet and rebroadcast to the other clients. Client:
// host-only sender, stored, applied. True when recognised.
bool HandleSkinChange(coop::net::Session& session,
                      const coop::net::Session::ReliableMessage& msg);

// Announce the local plate visibility mid-session, with the same trust and relay shape as the
// skin. The at-join state rides the prefs flags byte of the Join. Game thread only.
void AnnounceLocalNameplate(coop::net::Session& session, bool visible);

// Handle a delivered NameplateChange ([u8 slot][u8 visible]). Host: sender-checked, stored,
// rebroadcast; client: host-only sender, stored. True when recognised.
bool HandleNameplateChange(coop::net::Session& session,
                           const coop::net::Session::ReliableMessage& msg);

// Announce the local nick colour mid-session, the same shape; packed 0 resets to the default.
// The at-join state rides the [has][r][g][b] field of the Join. Game thread only.
void AnnounceLocalNickColor(coop::net::Session& session, uint32_t packed);

// Handle a delivered NickColorChange ([u8 slot][u8 has][r][g][b]), the same trust shape. True
// when recognised.
bool HandleNickColorChange(coop::net::Session& session,
                           const coop::net::Session::ReliableMessage& msg);

// Reset the per-slot caches; called from event_feed's session start, so a stop and start in one
// process sees clean state.
void Reset();

// The per-tick connect-edge Join sender for one slot; event_feed detects the edge and calls
// this. The payload and its built flag are lazy state shared across the slot loop, so the
// conversion and allocation are not paid every pump tick once every slot has our Join. Holds
// off, without latching, while the local Player Element id is not yet allocated (the window
// after AssignPeerSlot, about one pump tick); a no-op for a slot that already has it.
void MaybeSendJoinToSlot(coop::net::Session& session, int slot,
                          std::vector<uint8_t>& joinPayload,
                          bool& joinPayloadBuilt);

// Person-state teardown is driven by the roster ledger's row transition, which fires on a
// replacement as well as a departure; a disconnect callback cannot see a slot recycled between
// two ticks. Registers the module's subscribers once.
void InstallLedgerSubscribers();

// The nickname for a peer slot: a thin read of the ledger row with the placeholder fallback
// applied.
const std::wstring& NicknameForSlot(int slot);

// Host: assert the current roster to every ready client, at about 1 s for the first 10 s after
// a roster change and 5 s after, so a row lost in the join-time burst heals within the seconds
// a joiner is looking at the player list. Game thread, once per pump tick.
void PulseRosterRows(coop::net::Session& session);

// Host: arm the pulse's fast window after a roster change.
void MarkRosterChanged();

// Client: called the instant AssignPeerSlot stamps our slot, to apply roster rows that arrived
// before we knew which slot was ours.
void OnLocalPeerIdStamped(coop::net::Session& session);

// The second phase of the join announcement: the Join announces that a peer is connecting, and
// this, called from the puppet spawn path, announces that it joined, the moment the body is
// visible. Role-aware: on a client, slot 0 is the host whose game we joined. The caller
// announces the client role unconditionally and the host role once the slot is world-ready (a
// pre-world pose can spawn the puppet early; OnClientWorldReady covers that order). Latched
// once per join, so the two seams never repeat. Game thread.
void AnnouncePeerSpawned(net::Role role, int slot);

// Host-side cover for the reverse order, fired when a client's ClientWorldReady lands: if the
// slot's puppet already spawned (a pre-world menu pose), world-ready is the moment it becomes
// the real joiner, so the line is announced now under the same latch. In the normal order this
// is a no-op. Game thread.
void OnClientWorldReady(int slot);

// Handle a delivered Join: parses the sender's element id, then the length-prefixed UTF-8
// nickname, sanitises it, sets the puppet's nameplate and posts the feed entry. True when the
// message was a recognised Join, whatever the validation outcome; false when the payload is too
// short for the header.
bool HandleJoinMessage(coop::net::Session& session,
                       const coop::net::Session::ReliableMessage& msg);

// Handle a delivered AssignPeerSlot: stamps the client's slot and binds the host's mirror
// Player Element when the host element id is present and valid. Dropped on the host, which
// self-assigns. True when recognised; false on a short payload.
bool HandleAssignPeerSlot(coop::net::Session& session,
                          const coop::net::Session::ReliableMessage& msg);

// Host-side cross-peer identity broadcast, called from HandleJoinMessage once the joiner's
// mirror and nick are stored; MTA's initial-data-stream exchange: a PlayerJoined for the
// joiner to every other client, and one for every already-known client to the joiner. No-op on
// a client.
void BroadcastRosterFromHost(coop::net::Session& session,
                                   int joinerSlot,
                                   uint32_t joinerEid,
                                   const std::wstring& joinerNick);

// Client-side handling of a PlayerJoined describing a third peer: range-validates the eid,
// binds the peer's mirror Player Element and caches its nickname, so the puppet spawned later
// on the first relayed pose is born identified. Dropped on the host, which originates these.
// True when recognised; false on a short payload.
bool HandleRosterRow(coop::net::Session& session,
                        const coop::net::Session::ReliableMessage& msg);

// Re-assert every live puppet's ledger skin on a 2 s throttle. ApplySkin early-outs when
// already applied, so a converged slot costs one string compare, and a puppet whose apply
// deferred (the pak still mounting during the join window) heals instead of wearing the wrong
// body all session. Game thread.
void TickSkinConverge();

}  // namespace coop::player_handshake
