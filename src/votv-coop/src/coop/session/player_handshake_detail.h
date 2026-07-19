// coop/session/player_handshake_detail.h -- INTERNAL (src-tree) shared header
// for the player_handshake module's THREE translation units:
//   player_handshake.cpp         -- the Join/PlayerJoined/AssignPeerSlot identity
//                                   handshake (owns g_skinBySlot + the nick/guid
//                                   side-tables)
//   player_handshake_prefs.cpp   -- the live display-pref change family
//                                   (SkinChange / NameplateChange / NickColorChange
//                                   announce + forgery-guarded handle + rebroadcast)
//   player_handshake_version.cpp -- the v122 wire version gate (the Paper-pair
//                                   equality validation at the TOP of the Join
//                                   handler + the refuse close/popup/feed line).
//                                   Split 2026-07-19 (modular file-size rule:
//                                   the gate grew player_handshake.cpp 828->965).
// Split 2026-07-05 (modular file-size rule: player_handshake.cpp had grown to
// 1043 LOC when the v103 nick color landed). NOT part of the public module API
// (that is include/coop/session/player_handshake.h); nothing outside these
// TUs may include this.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::player_handshake {

// Store + live-apply a peer's skin: writes the identity handshake's
// g_skinBySlot side-table (owned by player_handshake.cpp) and re-skins the
// slot's puppet if already spawned. Game thread only.
void StoreSkinForSlot(int slot, std::string name);

// Parse one [u8 len][ASCII] skin field. Returns bytes consumed (0 =
// malformed/absent); `out` untouched unless a well-formed non-empty field
// validated as a skin name (IsValidSkinName -- it becomes a LoadObject
// package path component).
size_t ParseSkinField(const uint8_t* p, size_t remaining, std::string* out);

// UTF-8 -> wide (defined in player_handshake.cpp; shared with the version
// gate's nick extraction).
std::wstring FromUtf8(const uint8_t* p, int len);

// The VT-inspired nickname sanitizer (defined in player_handshake.cpp; the
// version gate sanitizes the refused peer's nick for the host feed line).
std::wstring SanitizeNickname(const std::wstring& raw);

// The v122 WIRE VERSION GATE (player_handshake_version.cpp). Runs at the TOP of
// HandleJoinMessage, BEFORE any identity side effect: pure pre-pass over the
// Join payload chain, byte-equality validation of the peer's game target (the
// build half is the packet header's protocol version, equal by construction),
// fail-closed on a malformed chain. Returns true if the Join was REFUSED (the
// caller drops the message): host side Kicks with the reason + posts the deduped
// "<nick> was turned away" feed line; client side Fails the join (popup) and
// lets the host's symmetric gate close the wire. Game thread only.
bool ValidateJoinVersionOrRefuse(coop::net::Session& session, int senderSlot,
                                 const uint8_t* payload, size_t payloadLen);

}  // namespace coop::player_handshake
