// coop/player/puppet_identity_internal.h -- what puppet_identity.cpp (the stub rows, the arming, the per-world
// resolve) and puppet_identity_scan.cpp (the six native seams) share.

#pragma once

#include <cstdint>

namespace coop::puppet_identity {

// A row's index in Counters(), and the refusal count and log line it owns.
enum Row : uint8_t {
    kWispW, kWispO, kWispP, kKelura, kOrborb, kWellTpBegin, kWellTpEnd, kDreamBoulder, kMapTransition,
    kTriggerOpenLevel, kTriggerSpikes, kDreamClimb, kDreamJump, kDreamRunFin, kDreamRunWok, kDreamfillDoor,
    kGrunt, kShower, kBirch, kErie, kSkullzone,
    kRowCount
};

// RemoveActors' predicate: drop the puppet bodies. `ctx` is unused.
bool IsPuppetBodyFn(void* ctx, void* actor);

// Count a refusal and say it once per row per world: the toucher's slot when `actor` is in hand, else unknown.
void NoteRefused(Row row, void* actor);

// Is `cls` still the class listed at `listing`, the identity (ObjectIndex ClassListingId) it had when the
// module resolved from it: false once it was unloaded, or loaded again, since. Reads nothing through `cls`.
// A refusal that fails here leaves the call alone until the next Tick re-resolves, so a class unloaded or
// loaded again is served as SP would for that one frame.
bool ClassStands(void* cls, uint64_t listing);

// Is the grunt stub's marked scope on this thread's chain, and its class not unloaded since the resolve.
bool InGruntScope();

// The same for the wisp_p stub: a puppet's touch of a purple wisp is running on the host.
bool InWispPScope();

namespace scan {

// Install each native seam not yet installed, at most once per `generation` (it moves with each per-world
// resolve), and arm the post hooks for `puppets` registered puppets.
void InstallPump(int generation, int puppets);

// Arm the three post hooks while a puppet exists. Idempotent.
void Rearm(int puppets);

// Re-resolve the scan rows whose own class changed (another class, or the same one listed again). `unlistVersion`
// is the number of class unloads seen: no lookup runs while every class is held and the number stands; a row
// whose class stands is not decoded again whatever else unloaded. True when any row was looked at again.
bool Resolve(uint64_t unlistVersion);

// A window is the stretch of one creature's touch by a puppet in which its native pre hooks are armed: the
// grunt's (SetControlRotation) and the purple wisp's (PlaySound2D, AddToViewport).
enum Window : uint8_t { kGruntWindow, kWispPWindow };
void ArmWindow(Window window, bool on);
bool WindowArmed(Window window);

// Forget every resolved pointer and disarm both windows.
void Clear();

}  // namespace scan
}  // namespace coop::puppet_identity
