// coop/session/player_handshake_detail.h -- INTERNAL, src-tree only: the shared header for the
// player_handshake module's three translation units. Nothing outside them may include it; the
// public module API is include/coop/session/player_handshake.h.
//
//   player_handshake.cpp          the Join, RosterRow and AssignPeerSlot identity handshake. The
//                                 per-slot identity fields themselves live in the roster ledger's
//                                 Row.
//   player_handshake_prefs.cpp    the live display-pref change family -- skin, nameplate and nick
//                                 colour: announce, forgery-guarded handle, rebroadcast.
//   player_handshake_version.cpp  the wire version gate: byte-equality on the version pair at the
//                                 top of the Join handler, plus the refuse close, the popup and the
//                                 feed line.
//
// The three are separate files because one was over the size rule, not because the boundary is
// deep; the module is one concept split for readability.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::net { class Session; }

namespace coop::player_handshake {

// Store + live-apply a peer's skin: writes the roster ledger's Row::skin
// (change-gated) and re-skins the slot's puppet if already spawned.
// Game thread only.
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

// wide -> UTF-8 (defined in player_handshake.cpp; the roster TU builds the same
// length-prefixed nick field the Join payload uses).
std::vector<uint8_t> ToUtf8(const std::wstring& w);

// Display prefs: the per-slot flags byte carried by Join and RosterRow, where bit 0 is "nameplate
// visible". Defined in player_handshake.cpp, which owns the bit layout.
uint8_t PrefsFlagsForSlot(int slot);
void StorePrefsFlagsForSlot(int slot, uint8_t flags);

// Nick colour: the self-describing [u8 has][r][g][b] field appended after the prefs byte. Defined
// in player_handshake.cpp.
void AppendNickColorField(std::vector<uint8_t>& out, uint32_t packed);
size_t ParseNickColorField(const uint8_t* p, size_t remaining, int slot);

// The WIRE VERSION GATE (player_handshake_version.cpp). Runs at the TOP of HandleJoinMessage,
// before any identity side effect: a pure pre-pass over the Join payload chain, byte-equality
// validation of the peer's game target -- the build half is the packet header's protocol version,
// equal by construction -- and fail-closed on a malformed chain.
//
// Returns true when the Join was REFUSED, and the caller drops the message. On the host that means
// a Kick with the reason plus the deduped "<nick> was turned away" feed line; on a client it fails
// the join with a popup and lets the host's symmetric gate close the wire. Game thread only.
bool ValidateJoinVersionOrRefuse(coop::net::Session& session, int senderSlot,
                                 const uint8_t* payload, size_t payloadLen);

// Register the roster TU's own ledger subscriber (arms the repair pulse on any
// occupancy change). Called from InstallLedgerSubscribers so there is ONE
// registration door for the whole module.
void InstallRosterPulseSubscriber();

}  // namespace coop::player_handshake
