// coop/join_curtain.h -- instant-world UPPER layer (SEAM 1): the SHORT curtain.
//
// A full-viewport opaque cover the joining client raises at connect and DISMISSES with a
// smooth alpha-fade when the "primary world is assembled" (SnapshotComplete + spawn-drain --
// NOT full quiescence; see docs/COOP_INSTANT_WORLD_TWO_LAYER.md SEAM 1). It hides the rawest
// connect moment (the client's own save load-in, camera settle, the spawn burst, and the
// local-actor reposition jumps that deferred-spawn cannot hide because those are the engine's
// own actors). Because it lifts at SnapshotComplete (~2s BEFORE quiescence) it adds NO long
// blank screen -- the world fades in already-assembled, and the uncertain tail keeps resolving
// invisibly under mirror_defer.
//
// The cover draws on the ImGui BACKGROUND draw list -- ON TOP of the game world but BEHIND the
// loading panel -- so the panel stays legible while the curtain is up. Pure ImGui (our surface,
// our trigger), NOT the engine ClientSetCameraFade (whose co-op semantics are unpredictable).
//
// Game-thread / render-thread: Show/BeginDismiss/Reset are called from the join lifecycle
// (game thread); Render is called once per frame from the imgui overlay (the DX Present hook).

#pragma once

namespace coop::join_curtain {

// Raise the cover (alpha = 1) -- CLIENT, at connect (alongside mirror_defer::Arm()).
void Show();

// Start the alpha-fade 1->0 (~0.4s) -- at "primary world assembled" (SnapshotComplete + drain,
// alongside mirror_defer::RevealConfirmedAtLift() so the confirmed world fades IN as the cover
// fades OUT). Idempotent (a second call while already fading is ignored).
void BeginDismiss();

// Drop the cover immediately (session teardown / cancel). No fade.
void Reset();

// True while the cover is still drawing (alpha > 0). After the fade completes it returns false.
bool IsActive();

// Draw the full-viewport cover at the current alpha. Call every frame from the imgui overlay;
// no-op when inactive. Uses ImGui::GetTime() for the fade clock.
void Render();

}  // namespace coop::join_curtain
