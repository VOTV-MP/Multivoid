// ue_wrap/desk/desk_ping.h -- the main desk's ping machine (analogDScreenTest_C) and its verdict: the run flag, the
// stage and the three accumulators process_coords advances, and spaceRenderer_C::gatherSignal, which the machine calls
// in the same process_coords call that takes it to stage 3 (analogDScreenTest.cpp:1440-1491, spaceRenderer.cpp:542).
// The cheat menu's isntaCatchSignal calls the same verdict outside the machine (:7226). Engine-wrapper layer
// (principle 7), no network or coop state. Game thread, every function. Implementation: src/ue_wrap/desk/desk_ping.cpp.

#pragma once

#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/desk/console_desk.h"

namespace ue_wrap::desk_ping {

// The names a watch on the machine knows its functions by. A class-scoped watch is matched by the pointers it was
// registered with, so every watcher registers and asks with these.
inline constexpr const wchar_t* kDeskClass = L"analogDScreenTest_C";
inline constexpr const wchar_t* kRendererClass = L"spaceRenderer_C";
inline constexpr const wchar_t* kProcessCoords = L"process_coords";  // the desk's per-tick coordinate process
inline constexpr const wchar_t* kInstaCatch = L"isntaCatchSignal";   // the cheat menu's verdict at the cursor
inline constexpr const wchar_t* kVerdict = L"gatherSignal";          // the renderer's roll of the ping
inline constexpr const wchar_t* kLogLine = L"writeToCoordLog_2";     // the desk's coordinate-log append
inline constexpr const wchar_t* kPingSound = L"playPingSound";       // the desk's ping-sound component

// A failed ping's sound, as engine::SoundName names it (analogDScreenTest.cpp:1491).
inline constexpr const wchar_t* kPingFailedSound =
    L"SoundWave /Game/audio/newsounds/effects/newdesk_panelCoord_pingFailed.newdesk_panelCoord_pingFailed";

// coord_isPing: a ping runs on `desk`.
bool ReadPinging(void* desk, bool& out);

// Progress fields only. Mirrors keep coord_isPing clear so their local Blueprint tick cannot start a second scan.
struct Progress { int32_t stage = 0; float outer = 0.f, inner = 0.f, ping = 0.f; };
bool ReadProgress(void* desk, Progress& out);
bool WriteProgress(void* desk, const Progress& value);

// The machine at stage 2 complete -- coord_isPing set, coord_pingStage 2, the three accumulators at 1 -- so the
// process_coords body that reads it next takes it to stage 3 and rolls the verdict (@80250, @79979, @79837, @79809).
bool PrimeVerdict(void* desk);

// The machine idle, as the reset a second after a verdict leaves it: coord_isPing clear, coord_pingStage 0, the three
// accumulators at 0 (@5069).
bool ResetMachine(void* desk);

// A refused gatherSignal leaves its out parameters in the caller's storage as the last call wrote them: `return` and
// `caughtAtLeastOne` are written false there, so the caller takes its no-signal branch.
bool WriteNoVerdict(const script_gate::Call& c);

// gatherSignal's `return`, read after its body from the caller's storage: whether it caught.
bool ReadCaught(const script_gate::Call& c, bool& out);

// gatherSignal's `data`, read after its body from the caller's storage: the signal a catch gathered, which the caller
// copies into the desk's coord_signalData (@79303).
bool ReadVerdictSignal(const script_gate::Call& c, console_desk::CoordSignal& out);

// The sound a playPingSound call plays is the stage-change cue (newdesk_panelCoord_stageChange).
bool IsStageChangeCue(const script_gate::Call& c);

// The desk's own isntaCatchSignal: the verdict at its view coordinate, as the cheat menu's catchSig calls it.
bool CallInstaCatch(void* desk);

// The desk's own playPingSound of the loaded sound `soundName` names (engine::SoundName's form, as kPingFailedSound).
// False when the sound is not loaded or the call does not dispatch.
bool PlayPingSound(void* desk, const wchar_t* soundName);

}  // namespace ue_wrap::desk_ping
