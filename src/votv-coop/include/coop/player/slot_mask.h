// coop/player/slot_mask.h -- a set of player slots. One type sized by the slot count, so a per-slot
// owed set, holder set or "said it once" latch is the width of the ceiling and never a hand-rolled
// `1u << slot` on a type that fits four. A slot at or above kMaxPeers asserts in debug and is ignored
// in release: never a shift past the width, never a wrap onto another slot. Header-only and trivially
// copyable, so it sits in a plain global or a struct member like the integer it replaces.

#pragma once

#include "coop/player/slot_count.h"

#include <cassert>
#include <cstdint>
#include <type_traits>

namespace coop::players {

static_assert(kMaxPeers <= 32, "SlotMask holds 32 slots; a wider ceiling widens its storage");

class SlotMask {
public:
    constexpr void Set(int slot) {
        if (InRange(slot)) bits |= 1u << slot;
    }
    constexpr void Clear(int slot) {
        if (InRange(slot)) bits &= ~(1u << slot);
    }
    constexpr bool Has(int slot) const {
        return InRange(slot) && (bits & (1u << slot)) != 0;
    }
    constexpr bool Any() const { return bits != 0; }
    constexpr bool None() const { return bits == 0; }
    constexpr void ClearAll() { bits = 0; }

    // Whole-set operations, in place: Keep is the intersection with `other`, Add the union.
    constexpr void Keep(SlotMask other) { bits &= other.bits; }
    constexpr void Add(SlotMask other) { bits |= other.bits; }

    // The set slots, ascending. The callback may change the set; the walk reads a copy.
    template <typename Fn>
    void ForEach(Fn&& fn) const {
        const uint32_t snapshot = bits;
        for (int slot = 0; slot < kMaxPeers; ++slot)
            if (snapshot & (1u << slot)) fn(slot);
    }

private:
    uint32_t bits = 0;

    static constexpr bool InRange(int slot) {
        const bool ok = slot >= 0 && slot < kMaxPeers;
        assert(ok && "SlotMask: slot outside [0, kMaxPeers)");
        return ok;
    }
};

static_assert(sizeof(SlotMask) == 4, "SlotMask is one 32-bit word");
static_assert(std::is_trivially_copyable_v<SlotMask>);

}  // namespace coop::players
