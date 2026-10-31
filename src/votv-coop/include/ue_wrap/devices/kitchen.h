// ue_wrap/devices/kitchen.h -- the microwave's and the mini oven's controls: the setting and whether it runs.
//
// Each machine heats the food inside it from its own tick or its own latent loop, so what a peer has to agree on is only
// these two. Engine-wrapper layer: no network. Game thread.

#pragma once

#include <cstdint>

namespace ue_wrap::kitchen {

enum class Kind : uint8_t { None = 0, Microwave = 1, MiniOven = 2 };

struct State {
    Kind    kind = Kind::None;
    int32_t mode = 0;        // the microwave's timeMode, the oven's mode: 0 to 4
    bool    active = false;
    float   remaining = 0.f;  // the microwave's seconds left while it runs
};

inline constexpr int32_t kModes = 5;

// The class names of the two machines, for the caller's instance walk.
inline constexpr const wchar_t* kMicrowaveClass = L"prop_microwave_C";
inline constexpr const wchar_t* kMiniOvenClass = L"prop_minioven_C";

Kind KindOf(void* actor);

// False when the actor is not a live machine or a field did not resolve.
bool Read(void* actor, State& out);

// The setting, as the machine's own press leaves it, with the oven's repaint.
bool ApplyMode(void* actor, int32_t mode);

// Run or stop. The microwave's start is its own Wave, which checks its power and door, and takes `remaining` when it
// runs; its stop ends the loop at its next step, with its ding. The oven's is its flag and repaint. Game thread.
bool ApplyActive(void* actor, bool active, float remaining);

}  // namespace ue_wrap::kitchen
