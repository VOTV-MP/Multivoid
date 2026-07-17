// ue_wrap/space_renderer.h -- standalone engine access for AspaceRenderer_C:
// the sky-signal set (the coords-minigame targets), the dish-aim vector, and
// the client roller suppression. Principle-7 engine-wrapper layer -- NO
// network logic; coop::console_state_sync drives the mirror through here.
//
// RE (votv-base-computers-RE-2026-06-11.md SS5.4 + the phase-2 impl pass):
// spaceRenderer is a singleton placed actor. `signals` (TArray<Fstruct_
// signal_spawn>, 0x2C rows) pairs BY INDEX with `signals_a` (TArray<
// ui_signal_C*>, the sky-dome widgets, each carrying its own LifeTime/
// MaxLifetime). `spawnSignal` self-re-arms on a 20-60 s timer and rolls
// everything locally -- the per-peer RNG hole; `addSignal(FVector)` appends
// a row + widget (rolling the non-position fields -- the caller overwrites
// them after); `deleteSignal(ui_signal*)` removes a row by its paired
// widget; `gatherSignal` is the occupant's catch roll.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::space_renderer {

// One sky signal: the Fstruct_signal_spawn POD head + the paired widget's
// rolled/ticking state. `objectName` is the FName's string (wire form).
// `alpha` is THE expiry countdown (ui_signal.reduceLifetime: alpha -=
// dt/lifetime, self-delete at <= 0); `direction` gates catch success on the
// desk (per-peer re-rolls would diverge the minigame) -- phase-2 impl RE.
struct SignalRow {
    float x = 0, y = 0, z = 0;       // coordinates (the cross-peer identity)
    int32_t type = 0;
    float strength = 0;
    float frequency = 0;             // identity tiebreaker
    float frequencySpread = 0;
    float polarity = 0;
    float polaritySpread = 0;
    float alpha = 1.f;               // widget Alpha: the 1->0 expiry countdown
    float lifeTime = 0;              // widget LifeTime: the countdown divisor
    float maxLifetime = 0;           // widget MaxLifetime (rolled 120-240)
    bool  direction = false;         // widget Direction (catch-gate parity)
    std::wstring objectName;
};

// Resolve the class + field offsets + verbs (throttled lazy retry). Game thread.
bool EnsureResolved();

// The live placed spaceRenderer (cached + liveness-checked; re-found on
// level reload). Null until the world is up.
void* Instance();

// Read the full local signal set (rows + paired widget lifetimes). False if
// unresolved / no instance. The caller compares CONTENT excluding lifetimes
// (they tick every frame).
bool ReadSignals(std::vector<SignalRow>& out);

// Set-reconcile the LOCAL signals to `want` (the wire set): delete local rows
// not in `want` (via the game's own deleteSignal -- despawns the widget),
// add missing rows (addSignal(coords) + overwrite the rolled row fields +
// push the widget props incl. the wire lifetimes). Rows are matched by exact
// (x, y, z, frequency) -- wire-copied floats are bit-identical. Game thread.
struct ApplyStats { int added = 0, removed = 0, kept = 0; };
bool ApplySignalSet(const std::vector<SignalRow>& want, ApplyStats& stats);

// HOST: remove one signal by identity (a relayed client catch). True if a
// matching row was found and deleteSignal dispatched.
bool RemoveSignalByIdentity(float x, float y, float z, float frequency);

// CLIENT: kill the local spawnSignal roller timer (K2_ClearTimer(self,
// "spawnSignal") on the Kismet CDO). spawnSignal is the ONLY re-armer, so one
// kill per spaceRenderer instance silences the roller until RestoreRoller.
// Returns true if the clear dispatched (caller re-kills on instance change).
bool KillClientSpawnTimer();

// Restore the native roller on disconnect: one reflected spawnSignal() call
// rolls one signal AND re-arms the BP's own 20-60 s loop (harmless extra
// signal; the world is back to SP behavior).
bool RestoreRoller();

// v115 cursor mirror: zero the glide integrator state (`movement`, a raw
// EX_Let-written FVector2D -- NOT setter-managed) so a residual local glide
// never co-writes against an incoming remote cursor stream. One write at the
// receiver's stream-start edge (desk_cursor_sync).
bool ZeroMovement();

// (The dish aim does NOT live here: spaceRenderer.coords/coords_rot are dead
// bytecode -- the real cursor state is on the ui_coordinates widget; see
// ue_wrap::console_desk::ReadDishAim/WriteDishAim. Phase-2 impl RE.)

}  // namespace ue_wrap::space_renderer
