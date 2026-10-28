// ui/overlay_backend_vk.h -- TU-private seam between the overlay's two Vulkan translation units
// (overlay_backend_vk.cpp: bring-up, the ImGui backend, textures, the draw selftest; and
// overlay_backend_vk_frame.cpp: the swapped present and destroy, the per-swapchain sets, the ring), the
// engine seam in overlay_seams.cpp and the RHI dispatcher in overlay_backend.cpp (the ops table and the
// slot restore at shutdown). Nothing else includes this.

#pragma once

#include "overlay_backend_internal.h"
#include "ue_wrap/engine/vulkan_rhi.h"

#include <cstdint>

namespace ui::overlay_backend::vk {

namespace rhi = ue_wrap::vulkan_rhi;

// A 64-bit handle the engine reader returned, as the Vulkan type it is.
template <class T>
inline T As(rhi::Handle h) { return reinterpret_cast<T>(static_cast<std::uintptr_t>(h)); }

// The dev red arms (config vk_overlay_red): which half of the proof is made to fail.
enum class Red : uint8_t { None, SelfTest, Present };

// What the bring-up verified and the backend was built on; refreshed by a re-init. Render-thread owned.
struct Context {
    rhi::ViewportState st;          // the viewport as last read at a bring-up or re-init
    rhi::DeviceFns     fns;         // the device-level entry points, filled once
    VkRenderPass       renderPass;  // ours: one colour attachment, the swapchain format
    Red                red;
};

// ---- overlay_backend_vk.cpp ----

const BackendOps& Ops();
Context& Ctx();

// CheckVkResultFn of the ImGui backend and the check on every call of ours: an error result (negative)
// records itself and raises an SEH exception. Success codes, VK_NOT_READY included, pass.
void Check(VkResult result);

// The overlay is down for the run: one line, and the ImGui host stops treating the overlay as ready.
void MarkDown(const char* why);
bool IsDown();

// Builds our render pass and the ImGui backend from Ctx().st; the counterpart destroys them. The first
// is also the re-init's second half. Both raise on a Vulkan error.
bool InitBackend();
void ShutdownBackend();

// Our ring of render slots steps exactly when ImGui's own buffer ring does (every RenderDrawData that
// passes its zero-size check). The selftest steps it once.
uint32_t RingIndex();
void RingAdvance();
void RingReset();

// Texture uploads the ImGui backend ran. Each waits on the present queue until the GPU has finished its
// work there, so the count says how often a run stalls the present on one.
void NoteUploads(uint32_t n);
uint32_t Uploads();

// ---- overlay_backend_vk_frame.cpp ----

// The viewport (FVulkanViewport*) of the engine Present now running on this thread; null outside one.
void SetViewport(void* viewport);

// The two loader slots: resolved at Init (false names what is missing), swapped last in the bring-up
// (false = a slot is not the loader's own pointer, or the swap did not take), put back at shutdown.
bool ResolveSlots();
bool SwapSlots();
void RestoreSlots();

// The Vulkan backend's ensureTarget and render ops.
void EnsureTarget(const FrameTarget& t);
void Render(const FrameTarget& t);

}  // namespace ui::overlay_backend::vk
