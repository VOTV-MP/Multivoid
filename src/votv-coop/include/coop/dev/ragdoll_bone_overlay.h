// coop/dev/ragdoll_bone_overlay.h -- dev skeleton ESP for the NATIVE ragdoll bodies.
//
// The game's ragdoll is a separate spawned actor (AplayerRagdoll_C) whose simulating
// SkeletalMesh is INVISIBLE in the main view (ragdollMode sets SetVisibleInSceneCaptureOnly
// on it -- it renders only in mirrors); it activates on the C key ("ragdoll mode"), faints,
// and trip/fall knockdowns. Debugging its motion (and our v22 mirror-body coupling -- today
// pelvis-only) therefore needs an explicit visualizer: this module projects EVERY bone of
// every ACTIVE ragdoll body to the screen and draws the bone->parent skeleton lines +
// joint dots as an ImGui overlay (the object_overlay pattern: game thread collects +
// projects + publishes a snapshot under a mutex; ui/hud.cpp draws it).
//
// Targets per update:
//   - the LOCAL player's native ragdoll (mainPlayer.ragdollActor -> SkeletalMesh) -- lit
//     while YOU are ragdolled (drawn from the ragdoll's own head-camera view),
//   - every remote peer's MIRROR ragdoll body (RemotePlayer::RagdollBody) -- lit while a
//     peer is ragdolling, i.e. the exact body whose only coupled bone is the pelvis today
//     (the visualizer is the diagnostic for extending that to every bone).
//
// Dev-gated (host-only via coop::dev_gate, like every dev overlay); OFF by default; force-on
// via [dev] ragdoll_bone_overlay=1. COST while ON with an active ragdoll: ~2 UFunction calls
// per bone per update at 30 Hz (GetSocketLocation + ProjectWorldToScreen) -- a dev-diagnostic
// budget. OFF = one atomic load per tick; ON with no active ragdoll = a handful of cheap
// reads + one controller call at 30 Hz; the per-bone cost only runs while a body is live.
#pragma once

#include <cstdint>

namespace coop::dev::ragdoll_bone_overlay {

inline constexpr int kMaxLines = 512;  // ~bones per body x active bodies; kel rig fits easily

struct Line {
    float   x1, y1, x2, y2;  // screen-space bone -> parent segment
    uint8_t kind;            // 0 = LOCAL native ragdoll, 1 = remote peer's mirror body
};

struct Snapshot {
    int  count = 0;
    Line lines[kMaxLines];
    char status[96] = {};
};

// Read [dev] ragdoll_bone_overlay once at boot (force-enable). Render/menu thread safe.
void InitFromIni();

// Game-thread tick (the harness composite pump, next to object_overlay::Update):
// collect active ragdoll meshes -> bone points -> project -> publish. Self-clears
// once on disable; one atomic load per tick while off.
void Update();

// Render thread: copy the published snapshot (mutex).
void GetSnapshot(Snapshot& out);

// Menu checkbox state (role-aware: reports OFF while a connected client, dev_gate).
bool IsEnabled();
void SetEnabled(bool on);

}  // namespace coop::dev::ragdoll_bone_overlay
