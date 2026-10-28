// ue_wrap/engine/vulkan_rhi.h -- what the game's Vulkan renderer holds, read from the engine's own
// objects, and the loader's function-pointer slots the renderer calls through.
//
// Engine-wrapper layer (principle 7): no network, gameplay or co-op state. The overlay's Vulkan half
// needs the instance, physical device, device, present queue, swapchain, extent, image count and
// back-buffer format. The engine keeps them in FVulkanViewport and the objects it points to
// (VulkanViewport.h, VulkanDevice.h, VulkanSwapChain.h); a hook on FVulkanViewport::Present sees the
// viewport as `this`, and ReadViewport reads the rest from it. The swapchain does not store its format
// outside the HDR path: the viewport's EPixelFormat, written back by the swapchain constructor, indexes
// the engine's GPixelFormats table, whose PlatformFormat is the VkFormat (VulkanSwapChain.cpp).
//
// The renderer calls vkQueuePresentKHR, vkQueueSubmit and vkDestroySwapchainKHR through slots in the
// game image that the engine's loader fills once at start; ResolveLoaderSlot finds one by the function's
// name, and SwapSlot / RestoreSlot put a replacement in and take it out. Vulkan types come from the Khronos
// headers; VK_NO_PROTOTYPES keeps every function undeclared, so each is fetched at run time.
#pragma once

#include <vulkan/vulkan_core.h>

#include <cstddef>
#include <cstdint>

// Nothing here installs a hook. Every resolver records its outcome in the scan ledger and is meant for
// the game thread; ReadViewport and the slot calls are safe on any thread once the resolvers have run.

namespace ue_wrap::vulkan_rhi {

// A Vulkan handle: VkInstance, VkDevice, VkQueue and the non-dispatchable ones are all 64 bits wide on
// a 64-bit target (Vulkan 1.0 specification, "Object Model").
using Handle = std::uint64_t;
// PFN_vkVoidFunction: what vkGet*ProcAddr returns, to be cast to the function's own type.
using ProcAddr = void (*)();

// Every value ReadViewport read, as plain values. The pointers are the engine's objects (kept as
// addresses, never dereferenced by a caller), the handles are the Vulkan ones.
struct ViewportState {
    std::uintptr_t viewport;       // FVulkanViewport*
    std::uintptr_t device;         // FVulkanDevice*
    std::uintptr_t rhi;            // FVulkanDynamicRHI*
    std::uintptr_t presentQueue;   // FVulkanQueue*
    std::uintptr_t swapChain;      // FVulkanSwapChain*
    std::uintptr_t hwnd;           // the game window
    Handle         vkInstance;
    Handle         vkPhysicalDevice;
    Handle         vkDevice;
    Handle         vkQueue;        // the present queue's VkQueue
    Handle         vkSwapchain;
    Handle         vkSurface;
    std::uint32_t  queueFamily;
    std::uint32_t  queueIndex;
    std::uint32_t  internalW;      // the swapchain's size (the image extent)
    std::uint32_t  internalH;
    std::uint32_t  imageCount;     // the swapchain's real image count
    std::int32_t   pixelFormat;    // EPixelFormat
    std::int32_t   vkFormat;       // VkFormat of the back buffer: GPixelFormats[pixelFormat].PlatformFormat
};

// FVulkanViewport::Present's entry, from a stretch of its body (sdk_profile.h); 0 when absent.
// Searches the .text section only. The scan ledger's VulkanPresent row.
std::uintptr_t ResolvePresent();

// The GPixelFormats table, from the swapchain constructor's read of it; 0 when absent. Resolved once and
// kept, so a caller on the render thread never scans. The scan ledger's VulkanPixelFormats row.
std::uintptr_t ResolvePixelFormatTable();

// The address of the loader slot that holds the renderer's pointer to the function `name` (a Vulkan
// entry point, e.g. "vkQueuePresentKHR"), or 0. The name is found once in the image as a NUL-bounded
// string; exactly one rip-relative lea in .text may name it; the slot is the store that follows the
// lookup call after that lea. More or fewer than one string or lea, or no store: 0.
std::uintptr_t ResolveLoaderSlot(const char* name);

// Reads `viewport` (the `this` of FVulkanViewport::Present) into `out`. False when any read faults, a
// pointer or a handle is implausible, the pixel format or image count is out of range, or the pixel-format
// table is unresolved; `out` is then not to be used. Any thread.
bool ReadViewport(void* viewport, ViewportState& out);

// vkGetInstanceProcAddr(instance, name) through the loader's export; null when vulkan-1.dll is not
// mapped or the loader does not know the name. Instance 0 answers the global entry points.
ProcAddr GetInstanceProcAddr(Handle instance, const char* name);

// vkGetDeviceProcAddr(device, name), itself fetched through vkGetInstanceProcAddr(instance, ...).
ProcAddr GetDeviceProcAddr(Handle instance, Handle device, const char* name);

// The loader's own export of `name`; null when vulkan-1.dll is not mapped or does not export it.
ProcAddr LoaderExport(const char* name);

// The pointer a loader slot holds right now (a plain aligned read; the slot comes from ResolveLoaderSlot).
void* ReadSlot(std::uintptr_t slot);

// The device-level entry points the overlay calls itself (the ImGui backend loads its own), listed once.
// Fetched from vkGetDeviceProcAddr on the engine's device, so a call goes straight to the driver.
// VK_NOT_READY from vkGetFenceStatus is a success code, so a caller reads the result rather than treating
// every non-zero value as an error.
#define UE_WRAP_VK_DEVICE_FNS(X) \
    X(vkGetDeviceQueue) X(vkGetSwapchainImagesKHR) X(vkQueueSubmit) X(vkQueueWaitIdle) \
    X(vkCreateRenderPass) X(vkDestroyRenderPass) \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) \
    X(vkCreateFence) X(vkDestroyFence) X(vkResetFences) X(vkGetFenceStatus) X(vkWaitForFences) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandBuffer) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdCopyImageToBuffer) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkMapMemory) X(vkUnmapMemory)

struct DeviceFns {
#define UE_WRAP_VK_DECLARE(name) PFN_##name name;
    UE_WRAP_VK_DEVICE_FNS(UE_WRAP_VK_DECLARE)
#undef UE_WRAP_VK_DECLARE
};

// Fills `out` from vkGetDeviceProcAddr(device, ...), the latter itself taken from the instance. False when
// any entry is missing or the lookup faults; `*missing` (when non-null) then names the first one. `out` is
// then not to be used. Resolved once per device, never on a per-frame path.
bool FillDeviceFns(Handle instance, Handle device, DeviceFns& out, const char** missing);

// Stores `ours` into the slot only while it still holds `expected`, the value the caller checked
// (InterlockedCompareExchangePointer). False: the slot held something else, and nothing was written. A
// caller that replaces a slot calls `expected` from its replacement.
bool SwapSlot(std::uintptr_t slot, void* expected, void* ours);

// Stores `saved` back into the slot only while it still holds `ours`. False: another swapper replaced ours
// since, and the slot is left as it is. Either way the replacement may still be running on another thread,
// so it must stay callable.
bool RestoreSlot(std::uintptr_t slot, void* ours, void* saved);

}  // namespace ue_wrap::vulkan_rhi
