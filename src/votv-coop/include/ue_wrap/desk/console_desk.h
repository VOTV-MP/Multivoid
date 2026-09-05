// ue_wrap/desk/console_desk.h -- engine access for the four-screen main desk
// (analogDScreenTest_C): the live-visible scalar set the DeskState lane mirrors, the
// screen-refresh chain and the coords-screen log append. Engine-wrapper layer, no network
// logic; console_state_sync drives the mirror through here. The desk is a singleton (the
// gamemode's analogPanels, one placed actor). Its persisted state rides the save transfer at
// join; the live divergence is the scalar set below (download, refine, playback and coords
// activity). The desk blueprint ticks on every peer, re-deriving the continuous fields (the
// detection needle) from the local sky signals and dish aim, both wire-synced, so mirror
// writes are convergence nudges on the discrete states, not a fight with the local sim. Every
// field offset resolves through reflection.

#pragma once

#include <cstdint>
#include <string>

namespace ue_wrap::console_desk {

// The live-visible scalar set, the DeskState payload's typed twin. The decode scalars are not
// here; they ride the CompState stream from the simulating peer. compMaxLevel stays, a
// claim-owner button edit rather than simulator state. canDL is derived natively from the
// detection and decode values, so mirroring the inputs converges it and a wire write would
// just fight the local recompute.
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
  bool  activePlay = false;  // active_play
  bool  activeDownload = false;  // active_download
  bool  activeCoords = false;  // active_coords
  bool  activeComp = false;  // active_comp
  bool  coordIsPing = false;  // coord_isPing
};

// Resolve the desk class, the singleton instance, every field offset and the refresh
// UFunctions. A throttled (2 s) lazy retry; idempotent. Game thread.
bool EnsureResolved();

// The live placed desk actor, cached and liveness-checked, re-found on a level reload. Null
// when the class or instance has not loaded.
void* Instance();

bool ReadScalars(Scalars& out);

// The detected frequency and polarity data floats, the decode's view of the matched signal's
// frequency and polarity, distinct from the filter-knob offsets in Scalars. Diagnostic,
// read-only; not on the DeskState wire. False if unresolved.
bool ReadFreqPolData(float& frData, float& poData);

// Raw-write the scalar set, then run the desk's own parameterless refresh chain so the
// screens and LEDs repaint from the new fields. Game thread.
bool WriteScalars(const Scalars& in);

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
// Raw member writes, plain data plus a string-to-FName.
bool WriteCoordSignal(const CoordSignal& in);
// The native reset values: zero vector, type, strength and frequency, a frequency spread of
// 0.5, polarity 0, a polarity spread of 0.5, object name None.
bool ClearCoordSignal();

// The native signal-deleted machine reset, minus the log line (the DeskLogLine lane carries it
// from the pressing peer): the download data's signal name set to None and its mesh to null
// (the two load-bearing members: mesh validity gates the per-tick accrual and the play screen;
// the text and rotator display members are not raw-zeroed, since a raw-zeroed text is a
// dangling shared reference and nothing reads them once the mesh is invalid), the detection
// and the frequency and polarity data zeroed, then the reflected download init, which rebuilds
// the download struct properly with engine-side assignment and repaints. Also the catch
// replay's screen reset.
bool ResetDownloadMachine();

// The reflected formDownload(decoded, polarity): rebuilds the download data from the objects
// table row named by the caught signal's object name, plus the download-init screen state. The
// native arm on dish stop is formDownload(0, -1); the joiner catch-up passes the host's live
// progress. A native no-op when the object name is None, since the table lookup fails.
bool ArmDownloadFromSignal(float decoded, int32_t polarity);

// The download progress (decoded and polarity) the joiner adopt carries. False if unresolved.
bool ReadDownloadProgress(float& decoded, int32_t& polarity);

// The download identity's raw FName bits for the host arm poll's change compare. Compare only;
// never rendered to a string.
bool ReadDLSignalKey(uint64_t& out);

// The reflected renderer's deleteSignalActor, the display-actor half of the native un-arm
// chain; the machine reset alone leaves the rendered signal object alive.
bool DeleteSignalActor();

// The raw detection write for the joiner adopt's needle catch-up, applied after the machine
// arms; while the mesh is invalid the native pulse zeroes it again.
bool WriteResDetect(float v);

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
// Raw-write the sim outputs and repaint the screens (the WriteScalars refresh chain) only when
// `repaint`: the interpolation stream raw-writes every tick for smoothness (the widget's own
// tick repaints the self-painting fields) and pulses the full repaint at about 3 Hz for the
// refresh-only display fields, never a per-frame repaint storm.
bool WriteSimOutputs(const SimOutputs& in, bool repaint);

// True while the download data's mesh is a live object, the machine armed; the joiner's
// pending adopt applies on this edge.
bool DownloadMeshValid();

// The desk-input apply surface (desk_input_sync): the claim-free field-granular input lane's
// engine writes.

// The maximum cooldown, the scan-charge target: the shift scan charges the cooldown exactly
// to it, and the dots charge to half of it, so the poll's scan classifier threshold is half
// plus a little. False if unresolved.
bool ReadMaxCooldown(float& out);

// Apply one power toggle with its native setter-event side effects, replicated reflected: the
// hum's activation, the light's visibility, and per unit the play stop, the download's
// play-signal refresh, or the refiner's console flag and materials. The native fused setter
// runs all five units' blocks including an unconditional sound stop, too broad for a
// per-field apply, hence the replication. Raw-write the field through WriteScalars first;
// this adds only the side effects. Units: 0 play, 1 download, 2 coords, 3 refiner. Game
// thread.
bool ApplyActiveToggleEffects(int unit, bool value);

// Live-apply the play volume the way the atlas's volume setter does: the raw field write is
// the caller's (WriteScalars); this adds the sound's volume multiplier, the value over 10
// clamped to 0.1 through 5. Game thread.
bool ApplyPlayVolumeEffects(int32_t value);

// The deck-playback replay surface (deck_play_sync): the reflected desk playSignal and
// stopSound, parameterless. playSignal reads the selected index and gates on active play, a
// valid index and decoded at least size internally; the caller pre-checks the
// divergence-capable gates and holds the audio-seam wire guard. Game thread.
bool CallDeckPlaySignal();
bool CallDeckStopSound();
// The desk's fin UFunction, the audio-finished delegate callback (no direct blueprint callers,
// so delegate-only dispatch); the deck lane's dispatch bracket target. Null until resolved.
void* DeckFinFn();

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
