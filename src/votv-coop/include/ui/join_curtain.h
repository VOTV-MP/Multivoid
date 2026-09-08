// ui/join_curtain.h -- the instant-world UPPER layer: the SHORT curtain.
//
// A full-viewport opaque cover the joining client raises at connect and dismisses with a smooth
// alpha fade once the primary world is assembled -- SnapshotComplete plus the spawn drain, which is
// NOT full quiescence. It hides the rawest connect moment: the client's own save load-in, the
// camera settle, the spawn burst, and the local-actor reposition jumps that deferred spawning
// cannot hide, because those actors are the engine's own. Lifting at SnapshotComplete, about two
// seconds before quiescence, adds no long blank screen: the world fades in already assembled and
// the uncertain tail keeps resolving invisibly under mirror_defer.
//
// The cover draws on the ImGui BACKGROUND draw list -- on top of the game world but behind the
// loading panel -- so the panel stays legible while the curtain is up. Pure ImGui, our surface and
// our trigger, not the engine's ClientSetCameraFade, whose co-op semantics are unpredictable. Show,
// BeginDismiss and Reset are called from the join lifecycle on the game thread; Render is called
// once per frame from the imgui overlay, on the DX Present hook.

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
