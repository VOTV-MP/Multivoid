// coop/nick_color.h -- per-player nickname COLOR pref (12f, 2026-07-05).
//
// Gameplay/network layer (principle 7). The COLOR AXIS has ONE owner: this
// module ([[feedback-one-owner-order-axis]]). Every surface that draws a nick
// (ImGui nameplate, chat feed nick prefix, scoreboard row) READS from here and
// falls back to its OWN default when the slot has no custom color (nameplate =
// white, chat = the per-slot palette, scoreboard = the role color). The wire
// layer (player_handshake) parses Join/PlayerJoined/NickColorChange and stores
// into this module -- exactly the coop::nameplate v94 visibility shape.
//
// Packed form on both the wire and in the store: 0 = NO custom color (use the
// surface default); custom = 0xFF000000 | (r<<16) | (g<<8) | b -- the 0xFF
// marker byte keeps a pure-black custom pick (0xFF000000) distinct from the
// "unset" 0.
//
// The whole store is ATOMIC (any-thread): writers span the game thread (wire
// handlers), the render thread (the F1 color picker request path) and the
// bringup thread (session-start reset via player_handshake::Reset).

#pragma once

#include <cstdint>
#include <string>

#include "coop/player/players_registry.h"  // kMaxPeers

namespace coop::net { class Session; }

namespace coop::nick_color {

// Pack helpers (shared by the UI picker and the wire build/parse).
inline uint32_t Pack(uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000u | (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) | b;
}
inline bool IsCustom(uint32_t packed) { return packed != 0; }
inline uint8_t R(uint32_t p) { return static_cast<uint8_t>(p >> 16); }
inline uint8_t G(uint32_t p) { return static_cast<uint8_t>(p >> 8); }
inline uint8_t B(uint32_t p) { return static_cast<uint8_t>(p); }

// Boot-time init from multivoid.ini nick_color= (harness, before the pump
// ticks). 0 = no custom color persisted.
void SetInitialLocal(uint32_t packed);

// Boot-time init from the RAW multivoid.ini nick_color= value (harness passes
// ReadIniValue("nick_color", "unset")). v103 (12f) semantics: key ABSENT (a new
// identity) = custom WHITE by default (user 2026-07-05); an explicitly EMPTY
// value (the user unchecked "Custom nickname color") = the per-surface defaults
// (chat palette / role colors); RRGGBB hex = that color. Owns the parse so the
// harness boot glue stays parse-free (MODULARIZATION_PLAN Tier-C, s27).
void SetInitialLocalFromIniHex(const std::string& hex);

// The local pref (0 = default). Any thread (atomic) -- the F1 picker reads it.
uint32_t LocalPacked();

// UI entry (render thread): persist to multivoid.ini, apply locally, announce
// to the session (host: broadcast; client: to host for rebroadcast). packed=0
// resets to the surface defaults everywhere.
void RequestLocal(uint32_t packed);

// Wire store: peer `slot` announced its color. Any thread (atomic slots).
void StoreForSlot(int slot, uint32_t packed);
uint32_t PackedForSlot(int slot);  // 0 = default

// Session wiring + lifecycle edges (same seams as coop::nameplate: subsystems
// installs, player_handshake::Reset resets on the bringup thread, disconnect
// clears the slot so a reuse never inherits the departed peer's color).
void Install(coop::net::Session* session);
void ResetSlots();
void OnSlotDisconnected(int slot);

}  // namespace coop::nick_color
