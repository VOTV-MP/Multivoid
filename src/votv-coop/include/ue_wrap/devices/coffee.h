// ue_wrap/devices/coffee.h -- reflected access to VOTV's coffee props: the vending machine
// (prop_coffeevending_C), the coffee maker (prop_coffeeMaker_C) and the coffee bag (prop_coffeeBag_C). Which
// class an actor is, and the few fields the lanes read; no network or gameplay logic (principle 7). Game thread.
#pragma once

#include <cstdint>

namespace ue_wrap::coffee {

enum class Device : uint8_t { None, Vending, Maker };

// The action both devices brew or dispense on: their actionOptionIndex(player, action) takes the points
// (the vending machine) or starts the brew (the maker) at this value, and every other value is a look-at
// option the player's aim reads.
inline constexpr uint8_t kBrewAction = 4;

// The device an actor is: its class IS prop_coffeevending_C or prop_coffeeMaker_C, exactly. A subclass,
// another prop, null and a class not loaded yet answer None.
Device DeviceOf(void* actor);

// True iff the actor's class IS prop_coffeeBag_C, exactly. A pointer compare against the class kept for this
// world, so a caller may ask it at contact rate.
bool IsBag(void* actor);

// The three flags of a maker's save record: the ones its brew sets and clears and its fill sets.
struct MakerFlags {
    bool hasCup = false;
    bool hasCoffee = false;
    bool coffeeReady = false;
    bool operator==(const MakerFlags& o) const {
        return hasCup == o.hasCup && hasCoffee == o.hasCoffee && coffeeReady == o.coffeeReady;
    }
};

// Read the maker's flags. False when the actor is no maker or a field did not resolve; `out` is left alone then.
bool ReadMakerFlags(void* maker, MakerFlags& out);

// Read a bag's `amounts`, the count of cups of coffee it still holds. False when the actor is no bag or the field
// did not resolve.
bool ReadBagAmounts(void* bag, int32_t& out);

}  // namespace ue_wrap::coffee
