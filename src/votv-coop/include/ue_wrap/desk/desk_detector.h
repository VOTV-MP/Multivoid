// ue_wrap/desk/desk_detector.h -- the main desk's detection needle (analogDScreenTest_C): the multiplier its step
// reads, the canDL latch canSaveSignal keeps, the game's own painters of both, and the ubergraph's path after the
// step. The needle loop resumes ExecuteUbergraph_analogDScreenTest at kLoopEntry; after the step, at kPostStepEntry,
// come the texts, the crossing check, the crossing block and setFullyProcessedSignalObject (the desk's CFG). Entering
// there runs that code as the step's own fall-through runs it. Engine-wrapper layer (principle 7), no network or coop
// state. Game thread, every function. Implementation: src/ue_wrap/desk/desk_detector.cpp.

#pragma once

#include <cstdint>

namespace ue_wrap::desk_detector {

// The ubergraph entries: the loop's resume (the Delay(0) at @60420) and the statement after the step (@4128).
inline constexpr int32_t kLoopEntry = 3541;
inline constexpr int32_t kPostStepEntry = 4128;

// DL_detectorMultiplier, the step's only input no code writes: its one reader is the step's multiply.
bool ReadMultiplier(void* desk, float& out);
bool WriteMultiplier(void* desk, float value);

// The desk's DL_resDetecPercent (the needle) and canDL (canSaveSignal's latch).
bool ReadNeedle(void* desk, float& out);
bool WriteNeedle(void* desk, float value);
bool ReadCanDL(void* desk, bool& out);

// The signal object's fullyProcessed, which setFullyProcessedSignalObject writes: gamemode.objectRenderer's
// signalObjectActor. False when the chain does not read (no signal object on screen).
bool ReadFullyProcessed(void* desk, bool& out);

// The game's own painters, dispatched on the desk: canSaveSignal (canDL, updPolarityLights, the tutorial text; it
// repaints only on a change) and setFullyProcessedSignalObject (the signal object's fullyProcessed).
bool CallCanSaveSignal(void* desk);
bool CallSetFullyProcessedSignalObject(void* desk);

// The ubergraph's UFunction, for a gate watch; null when it does not resolve.
void* Ubergraph(void* desk);

// The EntryPoint a call of the ubergraph starts from, read from its frame's `locals` at the gate's pre: the
// ubergraph's locals are one persistent frame every entry shares, so a read after a nested entry sees that one.
bool ReadEntry(void* ubergraph, const uint8_t* locals, int32_t& out);

// Run the ubergraph from kPostStepEntry, when its bytecode holds the layout the entry rests on: EX_PushExecutionFlow
// at kPostStepEntry with the target that setFullyProcessedSignalObject's call opens. The layout is read once per
// ubergraph and said once. True when the entry dispatched.
bool EnterPostStep(void* desk);

}  // namespace ue_wrap::desk_detector
