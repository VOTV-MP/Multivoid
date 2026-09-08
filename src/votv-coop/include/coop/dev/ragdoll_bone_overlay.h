// coop/dev/ragdoll_bone_overlay.h -- dev skeleton ESP for the NATIVE ragdoll bodies.
//
// The game's ragdoll is a separate spawned actor (AplayerRagdoll_C) whose simulating skeletal
// mesh is invisible in the main view -- ragdollMode sets SetVisibleInSceneCaptureOnly on it, so
// it renders only in mirrors -- which is why debugging its motion needs a visualizer. This
// projects every bone of every ACTIVE ragdoll body and draws the bone-to-parent skeleton and
// joint dots as an ImGui overlay: the game thread publishes under a mutex, ui/hud.cpp draws it.
//
// Two targets per update: the local player's own native ragdoll (mainPlayer.ragdollActor) and
// every remote peer's mirror body (RemotePlayer::RagdollBody), each lit while that body is
// ragdolling. The mirror body's only coupled bone is the pelvis, and extending that is the point.
//
// Dev-gated host-only and off by default. Off is one atomic load per tick; on with no active
// ragdoll, a few cheap reads and a controller call at 30 Hz; the per-bone GetSocketLocation and
// ProjectWorldToScreen run only while a body is live.
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

// Game-thread tick, in the harness composite pump next to object_overlay::Update: collect the
// active ragdoll meshes, take the bone points, project, publish. Self-clears once on disable;
// one atomic load per tick while off.
void Update();

// Render thread: copy the published snapshot (mutex).
void GetSnapshot(Snapshot& out);

// Menu checkbox state, role-aware: reports OFF while a connected client.
bool IsEnabled();
void SetEnabled(bool on);

}  // namespace coop::dev::ragdoll_bone_overlay
