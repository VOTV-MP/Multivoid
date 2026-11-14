// ui/join_curtain.h -- the instant-world UPPER layer: the SHORT curtain.
//
// A full-viewport opaque cover the joining client raises when its world STARTS LOADING and fades
// out once the primary world is assembled (SnapshotComplete plus the spawn drain, which is not
// full quiescence). It hides the client's own save load-in, the camera settle, the spawn burst and
// the reposition jumps the engine makes with its own actors, and the wait for the host's bracket,
// which is the host's to spend and can be long. Lifting about two seconds before quiescence costs
// no blank screen: the world fades in assembled and the tail resolves under mirror_defer.
//
// It draws on the ImGui BACKGROUND draw list -- over the world, under the loading panel -- so the
// panel stays legible above it. Ours, not the engine's ClientSetCameraFade, whose coop semantics
// are unpredictable. Every writer is one lock-free atomic store, so any thread may raise it;
// Render runs per frame on the DX Present hook. Lifecycle and measurements: docs/join.md.
// Show, BeginDismiss and Reset touch no ImGui state (a window that never presented has no context);
// Render, inside a frame, owns the transitions that read the fade's ImGui clock.

#pragma once

#include <cstdint>

namespace coop::join_curtain {

// Raise the cover (alpha = 1) -- CLIENT, at its world load (`join_progress::BeginWorldLoad`), and
// again at the bracket for an in-gameplay join that never loads one (alongside
// `mirror_defer::Arm()`). A plain store, so the second call is free.
void Show();

// Record that the cover is to fade 1->0 (~0.4s) -- at "primary world assembled" (SnapshotComplete +
// drain, alongside mirror_defer::RevealConfirmedAtLift() so the confirmed world fades IN as the cover
// fades OUT). Only records: the fade starts at the next frame Render draws, which stamps its start.
// Idempotent (a second call while already requested or fading is ignored). Game thread; no ImGui call.
void BeginDismiss();

// Drop the cover immediately (session teardown / cancel). No fade.
void Reset();

// True while the cover is up, has a dismissal recorded that no frame has taken yet, or is still fading
// (alpha > 0). False once Render has finished the fade. The overlay's render gate reads it, so a recorded
// dismissal keeps frames coming until Render takes it.
bool IsActive();

// True while the cover is up or its dismissal is recorded and the fade has not begun, so Render has not
// yet taken it. False while fading and once hidden. Any thread.
bool AwaitingFade();

// How many Render calls have finished (a frame the cover had its say in). A reader that sees the count rise
// sees that frame's transition. Any thread.
uint64_t RenderPasses();

// Draw the full-viewport cover at the current alpha. Call every frame from the imgui overlay, inside the
// frame; no-op when inactive. Uses ImGui::GetTime() for the fade clock.
void Render();

}  // namespace coop::join_curtain
