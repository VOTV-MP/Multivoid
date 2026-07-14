// ue_wrap/console_desk.h -- standalone engine access for the 4-screen main
// desk (AanalogDScreenTest_C): the live-visible scalar set the v64 DeskState
// channel mirrors, the screen-refresh (upd*) chain, and the coords-screen log
// append. Principle-7 engine-wrapper layer -- NO network logic;
// coop::console_state_sync drives the mirror through here.
//
// RE (votv-base-computers-RE-2026-06-11.md SS4.1 + the phase-2 impl pass):
// the desk is a SINGLETON (gamemode.analogPanels; one placed actor). Its
// PERSISTED state rides the v56 save-transfer at join (gatherData/setData <->
// saveSlot.analogPanelsData); the LIVE divergence is the scalar set below
// (download/refine/playback/coords activity). The desk BP ticks on EVERY
// peer, re-deriving the continuous fields (e.g. the detection needle) from
// the local spaceRenderer signals + dish aim -- both wire-synced in v64 -- so
// mirror writes are convergence nudges on the discrete states, not a fight
// with the local sim.
//
// Every field offset resolves via FindPropertyOffset (recook-robust); the
// header-dump values are documented for reference only.

#pragma once

#include <cstdint>
#include <string>

namespace ue_wrap::console_desk {

// The live-visible scalar set (DeskStatePayload's typed twin). The comp
// DECODE scalars are NOT here -- they ride the v65 CompState stream from the
// simulating peer (coop/comp_sync); compMaxLevel stays (a claim-owner button
// edit, not simulator state). canDL left in v70 (RULE 2): it is DERIVED --
// canSaveSignal recomputes it per detector pulse from resDetect + decoded >=
// size (@313), so mirroring the INPUTS converges it natively and a wire write
// just fights the local recompute.
struct Scalars {
    float   dlPoFilterOffset = 0;   // DL_poFilterOffset  @0x09F4
    float   dlFrFilterOffset = 0;   // DL_FrFilterOffset  @0x09F8
    float   dlPoFilterSpeed = 0;    // DL_poFilterSpeed   @0x09FC
    float   dlFrFilterSpeed = 0;    // DL_FrFilterSpeed   @0x0A00
    float   dlDownloading = 0;      // DL_downloading     @0x0A2C (float; 0 = idle)
    float   dlResDetecPercent = 0;  // DL_resDetecPercent @0x0970 (the detection needle)
    float   coordCooldown = 0;      // coord_cooldown     @0x0A80
    int32_t playVolume = 0;         // play_volume        @0x06D0 (int32 in the BP)
    int32_t dlPolarityDir = 0;      // DL_PolarityDir     @0x0A30
    int32_t compMaxLevel = 0;       // comp_maxLevel      @0x0AA0
    int32_t playSelectIndex = 0;    // play_selectIndex   @0x06C4
    bool    dlActiveFrFilter = false;  // DL_activeFrFilter @0x0A04
    bool    dlActivePoFilter = false;  // DL_activePoFilter @0x0A05
    bool    activePlay = false;        // active_play       @0x06E8
    bool    activeDownload = false;    // active_download   @0x0A34
    bool    activeCoords = false;      // active_coords     @0x0A35
    bool    activeComp = false;        // active_comp       @0x0B41
    bool    coordIsPing = false;       // coord_isPing      @0x0A7C
};

// Resolve the desk class + the singleton instance + every field offset + the
// refresh UFunctions. Throttled (2 s) lazy retry; idempotent. Game thread.
bool EnsureResolved();

// The live placed desk actor (cached + liveness-checked; re-found on level
// reload). Null when the class/instance hasn't loaded.
void* Instance();

bool ReadScalars(Scalars& out);

// Raw-write the scalar set, then run the desk's own parameterless upd*
// refresh chain so the screens/LEDs repaint from the new fields. Game thread.
bool WriteScalars(const Scalars& in);

// The TAIL (last maxChars) of coord_coordLog2Text -- the LIVE coords-screen
// event log ("PING..."/"FOUND..." lines the catching peer generates locally).
// (Phase-2 impl RE: coord_coordLogText + writeToCoordLog are DEAD -- the
// log the screen renders is coord_coordLog2Text via writeToCoordLog_2,
// self-capped at 1000 chars.)
std::wstring ReadCoordLogTail(size_t maxChars);

// Allocation-free twin of ReadCoordLogTail equality: true iff the live log's
// last-maxChars tail equals `expected`, compared IN PLACE against the engine
// FString (no wstring build). The v70 1 Hz log producer's steady-state check
// (the log is unchanged almost every poll; building a ~1 KB wstring per poll
// just to discover that was the one unconditional allocation on the path --
// perf audit 2026-06-12 IMPROVE-1). A length-only fast check is NOT sound
// here: at the BP's 1000-char cap, append+trim keeps the length pinned while
// the content changes.
bool CoordLogTailEquals(const std::wstring& expected, size_t maxChars);

// Append `suffix` to the coords log via the desk's own writeToCoordLog_2(B)
// (engine-side FString handling -- we never write engine FStrings raw).
bool AppendCoordLog(const std::wstring& suffix);

// The coords-panel cursor state (the REAL dish aim -- the ui_coordinates
// widget fields; spaceRenderer.coords is dead bytecode). Receivers raw-write
// + the reflected updCursorLocations() repaint, which also rotates the
// physical pingDishes (setRot).
struct DishAim {
    float viewX = 0, viewY = 0;   // viewCoordinate
    float c0X = 0, c0Y = 0;       // Coordinate_0
    float c1X = 0, c1Y = 0;       // Coordinate_1
    float c2X = 0, c2Y = 0;       // Coordinate_2
    int32_t selected = 0;         // the selected cursor index
    uint8_t direction = 0;        // Direction @0x441 -- the catch-gate toggle (v70)
};
bool ReadDishAim(DishAim& out);
// v109: the LIVE cursor and the COMMITTED locks are now SEPARATE writes (see the
// .cpp). WriteCursorOnly = viewCoordinate memcpy, NO dispatch (the 60Hz interpolated
// stream; the widget's own Tick repaints). WriteDishCommitted = the discrete locks +
// updCursorLocations repaint (commit rate). WriteDishAim (wrote both + dispatched at
// 3Hz) is RETIRED -- two authors on viewCoordinate is the dupe shape (RULE 2).
bool WriteCursorOnly(float viewX, float viewY);
bool WriteDishCommitted(const DishAim& in);
// v109: replay the desk's native intComs_unfocused (reset-on-release; dims, not hides).
bool CallIntComsUnfocused();

// ---- The v70 signal-catch consume surface (coop/signal_catch_sync) ----
// RE: votv-stolas-signal-catch-RE-2026-06-12.md SS1.3/SS2 + the @33832
// delete-chain disassembly. coord_signalData (@0x0A38, Fstruct_signal_spawn
// 0x2C) is THE global catch truth: gamemode.getSignalSData returns it; the
// downloader needles + accrual derive from it per tick on every peer.

// The desk's caught-signal struct, typed (objectName as a wire-able string).
struct CoordSignal {
    float x = 0, y = 0, z = 0;       // coordinates (the cross-peer identity)
    int32_t type = 0;
    float strength = 0;
    float frequency = 0;             // identity tiebreaker
    float frequencySpread = 0;
    float polarity = 0;
    float polaritySpread = 0;
    std::wstring objectName;         // FName rendered; 'None' when unarmed
};
bool ReadCoordSignal(CoordSignal& out);
// Raw member writes (POD + StringToFName -- the OverwriteRow precedent).
bool WriteCoordSignal(const CoordSignal& in);
// The @34134 reset values: zero vec/type/strength/frequency, freqSpread 0.5,
// polarity 0, polSpread 0.5, objectName None.
bool ClearCoordSignal();

// The @33832-34222 'Signal data deleted' machine reset, minus the log line
// (DeskLogLine carries it from the pressing peer): DL_signalDownloadData
// .signalName := None + .mesh := null (the two LOAD-BEARING members -- mesh
// validity gates the per-tick accrual @66736 and the playSignall screen; the
// FText/rotator display members are NOT raw-zeroed, an FText raw-zero is a
// dangling-shared-ref crash risk and nothing reads them once mesh is invalid)
// + DL_resDetecPercent/DL_frData/DL_poData := 0 + reflected
// initDownloadSignal({0,0}, 0, -1) (rebuilds DL_SignalDownloadDLData properly
// with engine-side assignment + repaints). Also the catch replay's screen
// reset (@10050/@10244 zeroed the same structs).
bool ResetDownloadMachine();

// Reflected formDownload(decoded, polarity): rebuilds DL_signalDownloadData
// from the list_objects DataTable row named by coord_signalData.objectName +
// initDownloadSignal screen state. The dishesStop handler's native arm is
// formDownload(0, -1); the joiner catch-up passes the host's live progress.
// No-ops natively when objectName is 'None' (the DataTable lookup fails).
bool ArmDownloadFromSignal(float decoded, int32_t polarity);

// DL_SignalDownloadDLData.{decoded, polarity} (the download progress the
// joiner adopt carries). False if unresolved.
bool ReadDownloadProgress(float& decoded, int32_t& polarity);

// Raw DL_resDetecPercent write (the joiner adopt's needle catch-up -- applied
// AFTER the machine arms; while the mesh is invalid the native @4602 pulse
// zeroes it again).
bool WriteResDetect(float v);

// True while DL_signalDownloadData.mesh is a live object -- the download
// machine is ARMED (the joiner's pending adopt applies on this edge).
bool DownloadMeshValid();

// playPingSound(newdesk_panelCoord_pingSuccess) -- the catch-replay presence
// cue (@10027).
bool PlayPingSuccess();

// ---- The refiner (comp) pane (v65, coop/comp_sync) ----
// RE (2026-06-12 comp agent pass): the decode ticker is gated ONLY on
// active_comp + comp_isDecodeActive -- NO occupancy condition -- so any
// machine with the flag latched SIMULATES (and completion fires world
// triggers incl. the level-3 theEvil_C spawn). Mirrors therefore stay
// passive: raw scalar writes + direct paints + cue edges; the flag is never
// written true from the wire.

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

}  // namespace ue_wrap::console_desk
