// ue_wrap/comp_pane.h -- the main desk's refiner (comp) pane surface: the
// decode scalars + the comp_data_0 struct base + the pane repaint / direct
// paints / cue actions. Split out of console_desk 2026-07-19 (one desk
// sub-surface per file); the desk actor + the atlas widget chain stay owned
// by ue_wrap::console_desk (Instance() / AtlasWidget() publics). Principle-7
// engine-wrapper layer -- NO network logic; coop::comp_sync drives the
// mirror through here.
//
// RE (2026-06-12 comp agent pass): the decode ticker is gated ONLY on
// active_comp + comp_isDecodeActive -- NO occupancy condition -- so any
// machine with the flag latched SIMULATES (and completion fires world
// triggers incl. the level-3 theEvil_C spawn). Mirrors therefore stay
// passive: raw scalar writes + direct paints + cue edges; the flag is never
// written true from the wire.

#pragma once

namespace ue_wrap::comp_pane {

struct CompScalars {
    float progress = 0;          // comp_progress      @0x0AA8 (0..100)
    float downloading = 0;       // comp_downloading   @0x0B20 (per-tick inc; the B\s readout)
    bool  decodeActive = false;  // comp_isDecodeActive @0x0AAC (read side)
};
bool ReadCompScalars(CompScalars& out);

// Mirror-side write: progress + downloading ONLY (never the flag).
bool WriteCompScalars(float progress, float downloading);

// The live comp_data_0 struct base (signal_dynamic I/O target). Null when
// unresolved / no world.
void* CompDataPtr();

// CLIENT world-up unlatch: clears comp_isDecodeActive + native wind-down cue
// + "idle" text. Kills the save-transfer's setData->comp_start auto-resume
// (a joiner would otherwise simulate the decode in parallel with the host --
// the pre-existing v56 double-simulation bug). No-op if not latched.
bool UnlatchDecode();

// updComp(bool hasData): the comp pane repaint. Condition semantics are
// "has data" (comp_data_0.size > 0) -- the native callers' meaning (the v64
// DeskState apply passed activeComp here; wrong, moved+fixed in v65).
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
