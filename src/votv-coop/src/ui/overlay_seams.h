// ui/overlay_seams.h -- TU-private seam between imgui_overlay.cpp (the host: bring-up, the frame, the
// window messages) and overlay_seams.cpp (the engine seams that deliver each present and each resize).
// Nothing else includes this.

#pragma once

namespace ui::overlay_backend { struct FrameTarget; }

namespace ui::imgui_overlay {

// One frame of overlay work; the target arrives from whichever present seam fired.
void OverlayFrame(const overlay_backend::FrameTarget& t);

// ImGui and the RHI backend are live (the resize bracket acts only then).
bool ImguiReady();

// The bring-up for a target whose own engine seam owns it (Vulkan): ImGui, the Win32 backend and the RHI
// backend come up behind the SEH guard, and the overlay is ready on true. Once only; the DX targets are
// brought up from OverlayFrame itself.
bool BringUpFromSeam(const overlay_backend::FrameTarget& t);

// The backend went down after the bring-up: the overlay is not ready any more (imgui_overlay.cpp).
void OverlayDown();

}  // namespace ui::imgui_overlay

namespace ui::overlay_seams {

// AOB-resolve and MinHook the present and resize seams; `armed` receives how many of the `total`
// seams installed. false = no draw seam resolved, so the overlay cannot draw this run.
bool Install(int& armed, int& total);

// The first call after Install judges the draw seams: when seams were armed and none of them has fired,
// the world is up without a present reaching the overlay, so the draw row of the scan ledger is marked
// missed and one line names each seam resolved / armed / fired. Later calls do nothing. Any thread.
void NoteWorldUp();

}  // namespace ui::overlay_seams
