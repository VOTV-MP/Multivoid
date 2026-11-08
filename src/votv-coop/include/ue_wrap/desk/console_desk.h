// ue_wrap/desk/console_desk.h -- engine access for the four-screen main desk
// (analogDScreenTest_C): the live-visible scalar set the DeskState lane mirrors, the
// screen-refresh chain and the coords-screen log append. Engine-wrapper layer, no network
// logic; console_state_sync drives the mirror through here. The desk is a singleton (the
// gamemode's analogPanels, one placed actor). Its persisted state rides the save transfer at
// join; the live divergence is the scalar set below (download, refine, playback and coords
// activity). The desk blueprint ticks on every peer. On a client its detection needle takes no
// step of its own (desk_sim_sync parks the step's input), so the needle is the host's, and the
// other mirror writes are convergence nudges on the discrete states, not a fight with the local
// sim. Every field offset resolves through reflection.

#pragma once

#include <cstdint>
#include <string>

namespace ue_wrap::signal_dynamic { struct Row; }

namespace ue_wrap::console_desk {

// The live-visible scalar set, the DeskState payload's typed twin. The decode scalars are not
// here; they ride the host's CompState stream, its machine the refiner's one simulator. compMaxLevel
// stays, a claim-owner button edit rather than simulator state. canDL is not here: it is canSaveSignal's
// latch, which the desk's tick runs only on the tick whose own increment carries `decoded` to
// its size, so a mirrored input never converges it; desk_sim_sync carries the host's canDL and
// runs canSaveSignal on a change.
struct Scalars {
  float  dlPoFilterOffset = 0;  // DL_poFilterOffset
  float  dlFrFilterOffset = 0;  // DL_FrFilterOffset
  float  dlPoFilterSpeed = 0;  // DL_poFilterSpeed
  float  dlFrFilterSpeed = 0;  // DL_FrFilterSpeed
  float  dlDownloading = 0;  // DL_downloading (float; 0 = idle)
  float  dlResDetecPercent = 0;  // DL_resDetecPercent (the detection needle)
  float  coordCooldown = 0;  // coord_cooldown
  int32_t playVolume = 0;  // play_volume (int32 in the BP)
  int32_t dlPolarityDir = 0;  // DL_PolarityDir
  int32_t compMaxLevel = 0;  // comp_maxLevel
  int32_t playSelectIndex = 0;  // play_selectIndex
  bool  dlActiveFrFilter = false;  // DL_activeFrFilter
  bool  dlActivePoFilter = false;  // DL_activePoFilter
  // The four unit power flags are read here and never written: each peer's own setPower writes them
  // through the desk's powerChanged, the one writer the game has.
  bool  activePlay = false;  // active_play
  bool  activeDownload = false;  // active_download
  bool  activeCoords = false;  // active_coords
  bool  activeComp = false;  // active_comp
  bool  coordIsPing = false;  // coord_isPing
  float remapMin = 0;  // remapValue.X, the signal view's lower bound
  float remapMax = 0;  // remapValue.Y, its upper bound
  bool  visualizerMode = false;  // visualizerMode, graph or spectrogram (ApplyVisualizerMode draws it)
};

// The range the game's own updRemap clamps both remap bounds to. A wire value is clamped to it at the
// receive edge, so a poll baseline never holds a number the desk will not.
constexpr float kRemapLimit = 10.0f;
constexpr float ClampRemap(float v) { return v < -kRemapLimit ? -kRemapLimit : (v > kRemapLimit ? kRemapLimit : v); }

// Resolve the desk class, the singleton instance, every field offset and the refresh
// UFunctions. A throttled (2 s) lazy retry; idempotent. Game thread.
bool EnsureResolved();

// The live placed desk actor, cached and liveness-checked, re-found on a level reload. Null
// when the class or instance has not loaded.
void* Instance();

// The console's parameterless painter verbs, one bit each. A caller names the ones the field it
// changed actually needs, because the chain is NOT side-effect free: `updToggles` ends by nulling
// the local player's `lookAtComponent` (the game's own look-at invalidation, see the chain in
// console_desk.cpp), so painting the whole console for one changed scalar rebuilds every action
// button under that peer's crosshair. The map below is read off the verbs' bytecode, not their
// names -- `kPaintText` and `kPaintCoordCoords` read no scalar at all (hover state and the coords
// commit), which is why no field names them.
enum Painter : uint32_t {
    kPaintNone           = 0u,
    kPaintText           = 1u << 0,   // updText            -- hover text; no scalar
    kPaintToggles        = 1u << 1,   // updToggles         -- DL_activeFrFilter/PoFilter, active_download
    kPaintPolarity       = 1u << 2,   // updPolarity        -- DL_PolarityDir
    kPaintVolume         = 1u << 3,   // updVolume          -- play_volume
    kPaintCoordLights    = 1u << 4,   // updCoordLights     -- active_coords
    kPaintPlaybackLights = 1u << 5,   // updPlaybackLights  -- active_play
    kPaintPolarityLights = 1u << 6,   // updPolarityLights  -- DL_PolarityDir, active_download, canDL, physMods
    kPaintMaxLevelLights = 1u << 7,   // updMaxLevelLights  -- comp_maxLevel, active_comp, physMods
    kPaintCoordCoords    = 1u << 8,   // updateCoordCoords  -- the coords commit; no scalar
    kPaintRemap          = 1u << 9,   // updRemap           -- remapValue
    kPaintAll            = (1u << 10) - 1u,
};

bool ReadScalars(Scalars& out);

// The detected frequency and polarity data floats, the decode's view of the matched signal's
// frequency and polarity, distinct from the filter-knob offsets in Scalars. Diagnostic,
// read-only; not on the DeskState wire. False if unresolved.
bool ReadFreqPolData(float& frData, float& poData);

// Raw-write the scalar set but the four unit power flags, then run the desk's own refresh verbs
// named by `painters` (the Painter bits above) so the screens and LEDs repaint from the new fields.
// `kPaintAll` is right for a whole-set apply such as a join adopt; a caller that changed ONE field
// names only that field's painters, because the chain is not side-effect free -- see the Painter
// comment and the chain itself in console_desk.cpp. `kPaintNone` writes the fields and paints
// nothing, which is the correct answer for a scalar no verb reads. Game thread.
bool WriteScalars(const Scalars& in, uint32_t painters);

// The tail (the last maxChars) of the live coords-screen event log, the PING and FOUND lines
// the catching peer generates locally. The log the screen renders is coord_coordLog2Text
// through writeToCoordLog_2, self-capped at 1000 chars; the older log field and writer are
// dead.
std::wstring ReadCoordLogTail(size_t maxChars);

// The allocation-free twin of ReadCoordLogTail equality: true if the live log's tail equals
// `expected`, compared in place against the engine string with no wstring build. The 1 Hz log
// producer's steady-state check: the log is unchanged almost every poll, and building a
// kilobyte string per poll just to discover that was the one unconditional allocation on the
// path. A length-only check is not sound: at the cap, append-and-trim keeps the length pinned
// while the content changes.
bool CoordLogTailEquals(const std::wstring& expected, size_t maxChars);

// Append `suffix` to the coords log through the desk's own writeToCoordLog_2, engine-side
// string handling; engine strings are never written raw.
bool AppendCoordLog(const std::wstring& suffix);

// The line a running writeToCoordLog_2 call appends, its parameter read through the gate's frame (`function` and
// `locals` of a script-gate PRE): every write, the game's own and AppendCoordLog's, whatever the cap later trims.
// Empty when the parameter does not resolve or the frame is null. Game thread.
std::wstring ReadCoordLogWrite(void* function, const uint8_t* locals);

// The coords-panel cursor state lives in ue_wrap/desk/coords_panel; the two desk-half seams it
// consumes are below.

// The atlas widget instance (the desk's Widget, liveness-checked), the desk-half seam the
// refiner pane's text-block chain consumes. Null while the desk or atlas is not live.
void* AtlasWidget();

// The raw desk widget to atlas coordinates-slot value (the atlas liveness-checked, the widget
// pointer unvalidated); coords_panel's instance chain does the class-validate and cache half.
// Null while the desk or atlas is not live.
void* AtlasUiCoordsSlot();

// Dispatch the desk's updateCoordCoords, the azimuth and altitude text repaint; coords_panel's
// committed-apply tail. False on unresolved.
bool CallUpdateCoordCoords();

// Replay the desk's native intComs_unfocused, the reset on release; it dims rather than hides.
bool CallIntComsUnfocused();

// The signal-catch consume surface (signal_catch_sync). The caught-signal struct is the global
// catch truth: the gamemode's signal-data getter returns it, and the downloader needles and
// accrual derive from it per tick on every peer.

// The desk's caught-signal struct, typed, the object name as a wire-able string.
struct CoordSignal {
  float x = 0, y = 0, z = 0;  // coordinates (the cross-peer identity)
    int32_t type = 0;
    float strength = 0;
  float frequency = 0;  // identity tiebreaker
    float frequencySpread = 0;
    float polarity = 0;
    float polaritySpread = 0;
  std::wstring objectName;  // FName rendered; 'None' when unarmed
};
bool ReadCoordSignal(CoordSignal& out);
// Any signal-spawn struct at `data`, the desk's or the `data` a gatherSignal call gathered into, read as
// ReadCoordSignal reads the desk's.
bool ReadSignalAt(const void* data, CoordSignal& out);
// Raw member writes, plain data plus a string-to-FName.
bool WriteCoordSignal(const CoordSignal& in);

// The ping success's own two writes, and nothing else: the space-object struct to its literal (the text a minted
// "none", the name None, the meshes and actor null, the rotators and enums as the literal has them) and the dynamic
// download data to its empty literal (signal_dynamic's live writer). No init and no repaint, as the catch has none.
bool WriteCatchReset();

// The reflected formDownload(decoded, polarity): rebuilds the download data from the objects
// table row named by the caught signal's object name, plus the download-init screen state. The
// native arm on dish stop is formDownload(0, -1). A native no-op when the object name is None,
// since the table lookup fails.
bool ArmDownloadFromSignal(float decoded, int32_t polarity);

// The download progress (decoded and polarity) the joiner adopt carries. False if unresolved.
bool ReadDownloadProgress(float& decoded, int32_t& polarity);

// The download identity's raw FName bits, for a change compare (a drill's); never rendered to a string.
bool ReadDLSignalKey(uint64_t& out);

// A running call's parameters, written through the gate's frame (`function` and `locals` of a script-gate PRE):
// formDownload's decoded and polarity, and initDownloadSignal's polarity, found by name on the function handed over.
// False, having written nothing, when a parameter does not resolve or the frame is null. Game thread.
bool WriteFormDownloadArgs(void* function, uint8_t* locals, float decoded, int32_t polarity);
bool WriteInitDownloadPolarity(void* function, uint8_t* locals, int32_t polarity);

// The download's dynamic data whole, without its photo: what the download screen shows. False if unresolved.
bool ReadDownloadRow(ue_wrap::signal_dynamic::Row& out);

// The gamemode's deleteActiveSignal, the game's whole un-arm: the desk's "Signal data deleted" reset (its only
// caller; its init rolls the polarity unless a gate PRE writes one), the renderer's deleteSignalActor, then, when the
// renderer's signal camera and its trigger are both valid, the trigger's runTrigger(0) and the camera field cleared
// (mainGamemode.cpp:9633-9670). False when unresolved or the call failed.
bool CallDeleteActiveSignal();

// The host-authoritative download-simulation output vector (desk_sim_sync). The download rate
// formula rolls unseeded random terms per tick and integrates the filter offsets from per-peer
// frame time, so these outputs diverge across peers even with identical knob inputs
// (measured). The host owns the sim and streams this vector at about 10 Hz, interpolated like
// the cursor; the client overwrites its own, whose local sim self-accrues garbage the
// overwrite hides. The knob intents (speeds, active, direction) stay occupant-authored through
// DeskState and the host integrates the offset from them, so this vector is host-down only,
// one author. The frequency and polarity data are streamed too, not relied on to converge on
// their own: they read a filter-size upgrade that has no live sync lane.
struct SimOutputs {
  float decoded = 0;  // DL_SignalDownloadDLData.decoded (download progress)
  float resDetec = 0;  // DL_resDetecPercent (the detection needle)
  float rate = 0;  // DL_downloading (per-tick rate; 0 = idle)
  float frData = 0;  // DL_frData (frequency-match,)
  float poData = 0;  // DL_poData (polarity-match,)
  float frOffset = 0;  // DL_FrFilterOffset (knob position)
  float poOffset = 0;  // DL_poFilterOffset
    // coord_cooldown is not in the sim vector: the 10 Hz overwrite erased a client presser's
    // charge. It rides the DeskInput charge events and the native per-peer decay.
};
bool ReadSimOutputs(SimOutputs& out);
// Raw-write the sim outputs. Every field above is painted by the desk widget's own tick, and not
// one of them is read by any verb in the WriteScalars refresh chain -- measured over every verb in it, updRemap included (see
// the chain's comment in console_desk.cpp). This function used to pulse that chain at about 3 Hz
// "for the refresh-only display fields"; there were none, and the pulse cost a visible defect, so
// it is gone rather than conditioned. The write is not a crossing's apply: the needle's crossing
// and canDL run their painters from the host's edges (desk_sim_sync).
bool WriteSimOutputs(const SimOutputs& in);

// True while the download data's mesh is a live object, the machine armed; the joiner's
// pending adopt applies on this edge.
bool DownloadMeshValid();

// The desk-input apply surface (desk_input_sync): the claim-free field-granular input lane's
// engine writes.

// The maximum cooldown, the scan-charge target: the shift scan charges the cooldown exactly
// to it, and the dots charge to half of it, so the poll's scan classifier threshold is half
// plus a little. False if unresolved.
bool ReadMaxCooldown(float& out);

// DL_precMult, the average of the dishes' precision that mainGamemode.setPrec writes and the
// download's rate is computed from. False if unresolved.
bool ReadPrecMult(float& out);

// Live-apply the play volume the way the atlas's volume setter does: the raw field write is
// the caller's (WriteScalars); this adds the sound's volume multiplier, the value over 10
// clamped to 0.1 through 5. Game thread.
bool ApplyPlayVolumeEffects(int32_t value);

// Draw the signal view in `spectrogram` mode, as the desk's red button does: setCanvas, which writes
// visualizerMode, then endCanvas. False if unresolved or the desk's screen widget is not live yet
// (nothing is written then; the caller retries). Game thread.
bool ApplyVisualizerMode(bool spectrogram);

// Run the desk's updRemap alone (the SARV text and the visualizer materials from remapValue). False if
// unresolved or the desk's screen widget is not live yet (nothing runs then; the caller retries).
// Game thread.
bool RepaintRemap();

// Repaint the refiner's cap label from comp_maxLevel and the modules: the cap button's tag (the hover's
// "Cap level [N]", or "Upgrade Auto Processing to use" without the module) and the screen's text_comp_maxlvl
// ("[N]", or "NaN"). Only the cap button's own body and the desk's upgraded_pcUpgrades write them, so a value
// written from outside the press, or a module plugged or pulled, leaves them stale; this runs
// upgraded_pcUpgrades, the game's own repaint, which otherwise re-derives only what the save's upgrades already
// say. It ends by nulling the local player's lookAtComponent, as the press does, so it runs on a change, never
// on a clock. False if unresolved or the desk's screen widget is not live yet (nothing runs then; the caller
// retries). Game thread.
bool RepaintCapLevel();

// The cap label as this peer shows it: the cap button's first tag and the screen's cap text. False if either
// is not readable. Diagnostic; game thread.
bool ReadCapLabel(std::wstring& tag, std::wstring& text);

// The deck-playback replay surface (deck_play_sync): the reflected desk playSignal and
// stopSound, parameterless. playSignal reads the selected index and gates on active play, a
// valid index and decoded at least size internally; the caller pre-checks the
// divergence-capable gates and holds the audio-seam wire guard. Game thread.
bool CallDeckPlaySignal();
bool CallDeckStopSound();

// The shift-scan accepted-branch visual for a mirror: the reflected spawnDirs, whose arrows
// regenerate from the wire-mirrored signals. The beep does not play here; the presser's
// organic ping sound rides the desk sound-effect lane.
// Never a search replay: its cooldown gate would refuse on per-peer decay jitter.
// Null-guarded on the widget; false (the caller logs once) if the coordinates widget is not
// live yet.
bool PlayScanEffects();

// The refiner pane surface lives in ue_wrap/desk/comp_pane; the desk-half seams it consumes
// are Instance and AtlasWidget above.

}  // namespace ue_wrap::console_desk
