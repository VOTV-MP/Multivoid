// coop/comms/chat_nick_color.h -- what colour a chat line's nick prefix is drawn in.
//
// The colour AXIS has one owner (coop::nick_color) and every surface falls back to its OWN default
// when a peer has no custom pick. This is the CHAT surface's half of that contract: the per-slot
// palette, plus the resolve that picks between it and the peer's custom colour.
//
// It lives in the gameplay layer, beside the two modules that COMPOSE chat lines (chat_sync and
// peer_action_feed), because the colour is resolved once when the line is composed and then FROZEN
// into it -- an old chat line keeps the colour it was drawn with. Resolving late, at draw
// time, would repaint a
// two-hour-old message when the peer whose slot has since been recycled picks a new colour. That
// also means the render half needs no palette at all.
//
// Returns ARGB (0xFFRRGGBB), the same packing coop::nick_color uses -- NOT ImGui's ImU32, which is
// ABGR. The render half converts.

#pragma once

#include <cstdint>

#include "coop/player/nick_color.h"

namespace coop::chat_nick_color {

// The fallback palette of the first eight slots. Matches the scoreboard's hue family: distinct,
// readable on dark AND bright scenes at full alpha. Slots past the eighth take these colours with
// their hue turned (PaletteFor), so no two slots of a full session share one.
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

// `argb` with its hue turned by `turn` of a full circle (0..1), saturation and value kept (HSV).
// constexpr, so the palette below is checked by a static_assert.
constexpr uint32_t TurnHue(uint32_t argb, float turn) {
    const float r = static_cast<float>((argb >> 16) & 0xFFu) / 255.0f;
    const float g = static_cast<float>((argb >> 8) & 0xFFu) / 255.0f;
    const float b = static_cast<float>(argb & 0xFFu) / 255.0f;
    const float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    const float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    const float chroma = mx - mn;
    float sextant = 0.0f;  // the hue in sixths of the circle, 0..6
    if (chroma > 0.0f) {
        if (mx == r)      sextant = (g - b) / chroma;
        else if (mx == g) sextant = (b - r) / chroma + 2.0f;
        else              sextant = (r - g) / chroma + 4.0f;
        if (sextant < 0.0f) sextant += 6.0f;
    }
    sextant += turn * 6.0f;
    while (sextant >= 6.0f) sextant -= 6.0f;
    const int   whole = static_cast<int>(sextant);
    const float frac = sextant - static_cast<float>(whole);
    const float p = mn;
    const float q = mx - chroma * frac;
    const float t = mx - chroma * (1.0f - frac);
    float outR = mx, outG = p, outB = q;  // sextant 5
    switch (whole) {
        case 0: outR = mx; outG = t;  outB = p;  break;
        case 1: outR = q;  outG = mx; outB = p;  break;
        case 2: outR = p;  outG = mx; outB = t;  break;
        case 3: outR = p;  outG = q;  outB = mx; break;
        case 4: outR = t;  outG = p;  outB = mx; break;
        default: break;
    }
    const auto byte = [](float v) { return static_cast<uint32_t>(v * 255.0f + 0.5f); };
    return 0xFF000000u | (byte(outR) << 16) | (byte(outG) << 8) | byte(outB);
}

// The turn of the second ring (slots 8..15): three quarters of a circle, the step that keeps the
// closest pair of the sixteen colours furthest apart (by a perceptual RGB distance).
inline constexpr float kSecondRingTurn = 0.75f;

// A slot's fallback colour: the palette entry for the first eight, the same entry turned for the
// next eight.
constexpr uint32_t PaletteFor(uint8_t slot) {
    return slot < 8u ? kSlotCols[slot] : TurnHue(kSlotCols[slot % 8u], kSecondRingTurn);
}

constexpr bool PaletteIsDistinct(uint8_t slots) {
    for (uint8_t a = 0; a < slots; ++a)
        for (uint8_t b = static_cast<uint8_t>(a + 1u); b < slots; ++b)
            if (PaletteFor(a) == PaletteFor(b)) return false;
    return true;
}
static_assert(PaletteIsDistinct(16), "two of the sixteen slots would draw a nick in one colour");

// The peer's custom nick colour if it has one, else this surface's palette entry.
// Call at COMPOSE time (game thread); the result is stored on the line.
inline uint32_t ForSlot(uint8_t slot) {
    const uint32_t custom = coop::nick_color::PackedForSlot(slot);
    return coop::nick_color::IsCustom(custom) ? custom : PaletteFor(slot);
}

}  // namespace coop::chat_nick_color
