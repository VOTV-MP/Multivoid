// coop/comms/chat_nick_color.h -- what colour a chat line's nick prefix is drawn in.
//
// The colour AXIS has one owner (coop::nick_color) and every surface falls back to
// its OWN default when a peer has no custom pick. This is the CHAT surface's half of
// that contract: the per-slot palette, plus the resolve that picks between it and the
// peer's custom colour.
//
// It lives in the gameplay layer, beside the two modules that COMPOSE chat lines
// (chat_sync, peer_action_feed), because as of 2026-07-29 the colour is resolved once
// when the line is composed and then FROZEN into it -- user: "old chat history is
// essentially a frozen history". Resolving late, at draw time, would repaint a
// two-hour-old message when the peer whose slot has since been recycled picks a new
// colour. That also means the render half needs no palette at all.
//
// Returns ARGB (0xFFRRGGBB), the same packing coop::nick_color uses -- NOT ImGui's
// ImU32, which is ABGR. The render half converts.

#pragma once

#include <cstdint>

#include "coop/player/nick_color.h"

namespace coop::chat_nick_color {

// The per-slot fallback palette (8 entries, wraps). Matches the scoreboard's hue
// family: distinct, readable on dark AND bright scenes at full alpha.
inline constexpr uint32_t kSlotCols[8] = {
    0xFFFFB340u,  // 0 host: amber
    0xFF6ECDFFu,  // 1 sky
    0xFF96FF96u,  // 2 mint
    0xFFFF82A0u,  // 3 rose
    0xFFC896FFu,  // 4 lilac
    0xFFFFF078u,  // 5 lemon
    0xFF78EBDCu,  // 6 teal
    0xFFFFA06Eu,  // 7 coral
};

// The peer's custom nick colour if it has one, else this surface's palette entry.
// Call at COMPOSE time (game thread); the result is stored on the line.
inline uint32_t ForSlot(uint8_t slot) {
    const uint32_t custom = coop::nick_color::PackedForSlot(slot);
    return coop::nick_color::IsCustom(custom) ? custom : kSlotCols[slot % 8u];
}

}  // namespace coop::chat_nick_color
