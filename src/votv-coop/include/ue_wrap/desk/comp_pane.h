// ue_wrap/desk/comp_pane.h -- the main desk's refiner (comp) pane surface: the
// decode scalars + the comp_data_0 struct base + the pane repaint / direct
// paints / cue actions -- one desk sub-surface per file, so the desk actor
// and the atlas widget chain stay owned
// by ue_wrap::console_desk (Instance() / AtlasWidget() publics). Principle-7
// engine-wrapper layer -- NO network logic; coop::comp_sync drives the
// mirror through here.
//
// The decode ticker is gated on isDreaming, active_comp and comp_isDecodeActive,
// and on nothing else -- no occupancy condition -- so any awake
// machine with the flag latched SIMULATES (and completion fires world
// triggers incl. the level-3 theEvil_C spawn). Mirrors therefore stay
// passive: raw scalar writes + direct paints + cue edges; the flag is never
// written true from the wire.

#pragma once

namespace ue_wrap::comp_pane {

struct CompScalars {
    float progress = 0;          // comp_progress      (0..100)
    float downloading = 0;       // comp_downloading   (per-tick inc; the B\s readout)
    bool  decodeActive = false;  // comp_isDecodeActive (read side)
};
bool ReadCompScalars(CompScalars& out);

// Mirror-side write: progress + downloading ONLY (never the flag).
bool WriteCompScalars(float progress, float downloading);

// The live comp_data_0 struct base (signal_dynamic I/O target). Null when
// unresolved / no world.
void* CompDataPtr();

// CLIENT world-up unlatch: clears comp_isDecodeActive + native wind-down cue
// + "idle" text. Kills the save-transfer's setData->comp_start auto-resume,
// without which a joiner simulates the decode in parallel with the host.
// No-op if not latched.
bool UnlatchDecode();

// updComp(bool hasData): the comp pane repaint. Condition semantics are
// "has data" (comp_data_0.size > 0), which is what the native callers mean by
// it -- not whether the pane is active.
bool UpdComp(bool hasData);

// Direct paints for the two texts nothing repaints on a passive mirror
// (text_comp_progress only repaints inside the decode-active tick chain;
// text_comp_process only inside comp_start/comp_stop/completion).
bool PaintCompProgress(float progress);
bool PaintCompProcess(const wchar_t* text);

// Decode ambience on WIRE edges -- the comp_start/comp_stop cue actions
// minus the state latch: rising -> the computerWorking_Cue loop; falling ->
// the computerWorking_end wind-down; completion -> the prog/Done beep.
bool CompCueStart();
bool CompCueStop();
bool CompBeepDone(bool maxed);

}  // namespace ue_wrap::comp_pane
