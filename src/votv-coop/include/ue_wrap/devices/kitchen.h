// ue_wrap/devices/kitchen.h -- the controls of the microwave, the mini oven and the grill.
//
// Each machine heats the food inside it from its own tick or its own latent loop, so what a peer has to agree on is only
// these. Engine-wrapper layer: no network. Game thread.

#pragma once

#include <cstdint>

namespace ue_wrap::kitchen {

enum class Kind : uint8_t { None = 0, Microwave = 1, MiniOven = 2, Grill = 3 };

struct State {
    Kind    kind = Kind::None;
    int32_t mode = 0;         // the microwave's timeMode and the oven's mode, 0 to 4; the grill's coals, 0 to 16
    bool    active = false;
    float   remaining = 0.f;  // the microwave's seconds left while it runs; the grill's burn through its current coal
    bool    lid = false;      // the grill's
    bool    doorOpen = false;  // the microwave's `opened`, which its start refuses
};

inline constexpr int32_t kModes = 5;
inline constexpr int32_t kMaxCoals = 16;

// The class names of the machines, for the caller's instance walk.
inline constexpr const wchar_t* kMicrowaveClass = L"prop_microwave_C";
inline constexpr const wchar_t* kMiniOvenClass = L"prop_minioven_C";
inline constexpr const wchar_t* kGrillClass = L"prop_bbq_C";

Kind KindOf(void* actor);

// False when the actor is not a live machine or a field did not resolve.
bool Read(void* actor, State& out);

// The setting, as the machine's own press leaves it, with the oven's repaint. The grill's coals are its count alone.
bool ApplyMode(void* actor, int32_t mode);

// Run or stop. The microwave's start is its own Wave, which checks its power and door, and takes `remaining` when it
// runs; its stop is its own forceOpenDoor, with its ding. The oven's and the grill's are the flag and repaint, the
// grill's without its ignite sound, and `remaining` is the grill's burn. Game thread.
bool ApplyActive(void* actor, bool active, float remaining);

// The microwave's door, through the functions its door's delegates call: openDoor stops a running one instead of
// marking it open, as the game's does. Game thread.
bool ApplyDoor(void* actor, bool open);

// The grill's lid, as its setLid leaves it, without the lid prop its press makes. Game thread.
bool ApplyLid(void* actor, bool lid);

}  // namespace ue_wrap::kitchen
