// ui/overlay_backend_vk_frame.cpp -- the Vulkan implementation of ui/overlay_backend.h, frame half: the
// replacements for the engine's vkQueuePresentKHR and vkDestroySwapchainKHR, the per-swapchain framebuffer
// sets, the ring of render slots, and the proof counters.

// The engine calls Vulkan through function-pointer slots its loader fills once; this swaps two of them. The
// swapped present is the only place that has the image index and the semaphore the present waits on, so the
// WHOLE overlay frame (OverlayFrame) runs inside it, on the thread that runs the engine's Present: that thread
// is the one that submits to the present queue, and the engine serializes its rare other submits against its
// own presents, so our submit needs no queue lock of ours. A frame draws into the image about to be shown:
// our submit waits on the present's one semaphore and signals a per-image "done" semaphore, and the present is
// then made with that one in its wait list. A plain mutex guards the sets, because the swapchain's destroy
// runs on the render thread at a resize while presents run on this one; the present path holds it from the
// lookup through the submit and releases it before the engine's own present call.

// Any failing Vulkan call of ours raises (vk::Check); the outer function takes the mutex through RAII and the
// inner one holds the __try and nothing with a destructor, so its handler can latch the overlay down
// (IsDown) without leaving the mutex locked. A down overlay passes every later present through untouched.

#include "overlay_backend_vk.h"

#include "ui/overlay_backend.h"
#include "overlay_seams.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/vulkan_rhi.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"

namespace ui::overlay_backend::vk {
namespace {

constexpr uint32_t kMaxImages    = 16;
constexpr size_t   kMaxSets      = 4;      // KeepSwapChain makes two coexist at a resize
constexpr uint64_t kStallMs      = 5000;   // above any legitimate stall of a frame's GPU work
constexpr uint64_t kSlotCheckMs  = 1000;
constexpr uint64_t kFenceWaitNs  = 2000000000ull;
constexpr uint32_t kWindowFrames = 60;

using PresentFn = VkResult (*)(VkQueue, const VkPresentInfoKHR*);
using DestroyFn = void (*)(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*);

// ---- the loader slots -------------------------------------------------------------------------------

struct Slot {
    const char*    name;
    std::uintptr_t addr;
    void*          saved;
    void*          ours;
    bool           swapped;
    bool           warned;
};
Slot g_present = {"vkQueuePresentKHR"};
Slot g_destroy = {"vkDestroySwapchainKHR"};

// Written before the slots are swapped, read by the replacements.
PresentFn g_savedPresent = nullptr;
DestroyFn g_savedDestroy = nullptr;
std::atomic<bool> g_stopped{false};  // the slots are being put back: in-flight replacements pass through
uint64_t g_lastSlotCheck = 0;

thread_local void* t_viewport = nullptr;  // the FVulkanViewport* of the engine Present running on this thread

// ---- the per-swapchain sets and the ring ---------------------------------------------------------------

struct SwapSet {
    VkSwapchainKHR chain;   // null: the entry is free
    uint32_t       count;
    uint32_t       w, h;    // the framebuffers' extent: the engine's InternalWidth/Height (min of requested and image)
    VkImageView    views[kMaxImages];
    VkFramebuffer  fbs[kMaxImages];
    VkSemaphore    done[kMaxImages];  // signalled by our submit for image i; the present waits on it
};

// Bits of the proof windows a frame counted into.
enum : uint8_t { kWinFrames = 1, kWinReinit = 2 };

struct RingSlot {
    VkCommandBuffer cmd;
    VkFence         fence;
    bool            inFlight;
    uint64_t        submitTick;
    uint8_t         windows;
};

struct Window {
    const char* name;
    uint8_t     bit;
    bool        active;
    bool        printed;
    // worstMs (the verdict's worstFenceMs) runs from a submit to the present at which its fence was first seen
    // signalled, so it floors at one frame time and grows with any gap between presents: an upper bound on
    // the GPU's latency, not a measurement of it.
    uint32_t    open, submitted, signalled, presentsOk, worstMs;
};

// What one present decided, for the ops it drives (EnsureTarget, Render) and for its after-effects.
struct Current {
    rhi::ViewportState st;
    SwapSet*           set;
    VkQueue            queue;
    const VkPresentInfoKHR* info;
    uint32_t           imageIndex;
    uint64_t           now;
    uint8_t            windows;
    VulkanFrame        vf;  // vf.waitOut: the semaphore the present must wait on once our submit has gone in
};

std::mutex g_mu;  // not recursive: destroy never runs inside our present
SwapSet    g_sets[kMaxSets];
RingSlot   g_ring[kMaxImages];
VkCommandPool g_pool = VK_NULL_HANDLE;
uint32_t   g_ringCount = 0;
Window     g_win[2] = {{"frames", kWinFrames, true, false, 0, 0, 0, 0, 0},
                       {"reinit", kWinReinit, false, false, 0, 0, 0, 0, 0}};
Current    g_cur{};
uint32_t   g_skipped = 0;
uint32_t   g_unreadPresents = 0;  // presents passed through because the viewport could not be read
uint32_t   g_badImagePresents = 0;  // presents passed through because the image index was out of range
bool       g_warnedWaits = false;
bool       g_drillDone = false;

// The dev drill (vk_overlay_drill=reinit): read once, on first use.
bool DrillReinitOn() {
    static const bool on = coop::config::ResolveEnum(::coop::config_registry::rows::vk_overlay_drill) == "reinit";
    return on;
}

struct FrameResult {
    VkSemaphore wait;     // the semaphore to present on, or null: present untouched
    uint8_t     windows;  // the proof windows this present counted into
};

VkDevice Dev() { return As<VkDevice>(Ctx().st.vkDevice); }

// ---- sets -------------------------------------------------------------------------------------------

SwapSet* FindSet(VkSwapchainKHR chain) {
    for (SwapSet& s : g_sets)
        if (s.chain == chain) return &s;
    return nullptr;
}

void DestroyFramebuffers(SwapSet& s) {
    const Context& c = Ctx();
    for (uint32_t i = 0; i < s.count; ++i) {
        if (s.fbs[i]) c.fns.vkDestroyFramebuffer(Dev(), s.fbs[i], nullptr);
        s.fbs[i] = VK_NULL_HANDLE;
    }
}

void FreeSet(SwapSet& s) {
    const Context& c = Ctx();
    DestroyFramebuffers(s);
    for (uint32_t i = 0; i < s.count; ++i) {
        if (s.views[i]) c.fns.vkDestroyImageView(Dev(), s.views[i], nullptr);
        if (s.done[i]) c.fns.vkDestroySemaphore(Dev(), s.done[i], nullptr);
    }
    UE_LOGI("[VK-OVERLAY] set freed: swapchain=0x%llx images=%u",
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(s.chain)), s.count);
    std::memset(&s, 0, sizeof s);
}

void BuildFramebuffers(SwapSet& s, const rhi::ViewportState& st) {
    const Context& c = Ctx();
    s.w = st.internalW;
    s.h = st.internalH;
    for (uint32_t i = 0; i < s.count; ++i) {
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = c.renderPass;
        fci.attachmentCount = 1;
        fci.pAttachments = &s.views[i];
        fci.width = s.w;
        fci.height = s.h;
        fci.layers = 1;
        Check(c.fns.vkCreateFramebuffer(Dev(), &fci, nullptr, &s.fbs[i]));
    }
}

// A swapchain the engine created that we have not seen: its images, a view and a "done" semaphore per image,
// and framebuffers at the extent the engine itself blits to.
SwapSet* BuildSet(const rhi::ViewportState& st) {
    SwapSet* s = FindSet(VK_NULL_HANDLE);
    if (!s) {
        MarkDown("more swapchains alive at once than the overlay keeps sets for");
        return nullptr;
    }
    const Context& c = Ctx();
    VkImage images[kMaxImages] = {};
    uint32_t n = st.imageCount;
    Check(c.fns.vkGetSwapchainImagesKHR(Dev(), As<VkSwapchainKHR>(st.vkSwapchain), &n, images));
    if (n != st.imageCount) {
        MarkDown("the swapchain's image count differs from the engine's");
        return nullptr;
    }
    s->chain = As<VkSwapchainKHR>(st.vkSwapchain);  // first, so a failure part-way is still freed by its destroy
    s->count = n;
    for (uint32_t i = 0; i < n; ++i) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = images[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = static_cast<VkFormat>(c.st.vkFormat);
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        Check(c.fns.vkCreateImageView(Dev(), &vci, nullptr, &s->views[i]));
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        Check(c.fns.vkCreateSemaphore(Dev(), &sci, nullptr, &s->done[i]));
    }
    BuildFramebuffers(*s, st);
    UE_LOGI("[VK-OVERLAY] set built: swapchain=0x%llx images=%u extent=%ux%u",
            static_cast<unsigned long long>(st.vkSwapchain), n, s->w, s->h);
    return s;
}

// ---- the ring ---------------------------------------------------------------------------------------

bool AnyInFlight() {
    for (uint32_t i = 0; i < g_ringCount; ++i)
        if (g_ring[i].inFlight) return true;
    return false;
}

void DestroyRing() {
    const Context& c = Ctx();
    for (uint32_t i = 0; i < g_ringCount; ++i)
        if (g_ring[i].fence) c.fns.vkDestroyFence(Dev(), g_ring[i].fence, nullptr);
    if (g_pool) c.fns.vkDestroyCommandPool(Dev(), g_pool, nullptr);
    std::memset(g_ring, 0, sizeof g_ring);
    g_pool = VK_NULL_HANDLE;
    g_ringCount = 0;
}

// One command buffer and one fence (created signalled) per image of the swapchain.
void CreateRing(uint32_t n) {
    const Context& c = Ctx();
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = c.st.queueFamily;
    Check(c.fns.vkCreateCommandPool(Dev(), &pci, nullptr, &g_pool));
    VkCommandBuffer cmds[kMaxImages] = {};
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = n;
    Check(c.fns.vkAllocateCommandBuffers(Dev(), &cai, cmds));
    g_ringCount = n;  // from here DestroyRing frees whatever the loop below made
    for (uint32_t i = 0; i < n; ++i) {
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        g_ring[i].cmd = cmds[i];
        Check(c.fns.vkCreateFence(Dev(), &fi, nullptr, &g_ring[i].fence));
    }
}

// Which fences of ours the GPU has finished, observed without waiting; a fence still unsignalled five
// seconds after its submit puts the overlay down.
void PollFences(uint64_t now) {
    const Context& c = Ctx();
    for (uint32_t i = 0; i < g_ringCount; ++i) {
        RingSlot& s = g_ring[i];
        if (!s.inFlight) continue;
        const VkResult r = c.fns.vkGetFenceStatus(Dev(), s.fence);
        if (r == VK_SUCCESS) {
            s.inFlight = false;
            const uint32_t ms = static_cast<uint32_t>(now - s.submitTick);
            for (Window& w : g_win) {
                if (!(s.windows & w.bit)) continue;
                ++w.signalled;
                if (ms > w.worstMs) w.worstMs = ms;
            }
        } else if (r == VK_NOT_READY) {
            if (now - s.submitTick > kStallMs) {
                MarkDown("the GPU never finished our work (a fence stayed unsignalled for 5 s)");
                return;
            }
        } else {
            Check(r);
        }
    }
}

// ---- the proof windows ------------------------------------------------------------------------------

// A window's verdict: the first 60 presents in which a frame was due to be submitted, then every submit of
// its frames observed finished.
void PrintVerdicts() {
    for (Window& w : g_win) {
        if (!w.active || w.printed || w.open < kWindowFrames) continue;
        bool pending = false;
        for (uint32_t i = 0; i < g_ringCount; ++i)
            if (g_ring[i].inFlight && (g_ring[i].windows & w.bit)) pending = true;
        if (pending) continue;
        w.printed = true;
        const bool pass = w.submitted == kWindowFrames && w.signalled == kWindowFrames && w.presentsOk == kWindowFrames;
        if (pass)
            UE_LOGI("[VK-OVERLAY] %s PASS: submitted=%u signalled=%u presentsOk=%u worstFenceMs=%u", w.name, w.submitted,
                    w.signalled, w.presentsOk, w.worstMs);
        else
            UE_LOGE("[VK-OVERLAY] %s FAIL: submitted=%u signalled=%u presentsOk=%u worstFenceMs=%u", w.name, w.submitted,
                    w.signalled, w.presentsOk, w.worstMs);
        if (w.bit == kWinFrames) UE_LOGI("[VK-OVERLAY] texture uploads=%u", Uploads());
        ue_wrap::log::Flush();
    }
}

void NotePresentResult(uint8_t windows, VkResult r) {
    if (!windows || (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)) return;
    for (Window& w : g_win)
        if (windows & w.bit) ++w.presentsOk;
}

// Once a second: does each slot still hold our replacement? Logs once per slot if not.
void CheckSlots(uint64_t now) {
    if (now - g_lastSlotCheck < kSlotCheckMs) return;
    g_lastSlotCheck = now;
    for (Slot* s : {&g_present, &g_destroy}) {
        if (!s->swapped || s->warned) continue;
        void* v = rhi::ReadSlot(s->addr);
        if (v == s->ours) continue;
        s->warned = true;
        UE_LOGW("[VK-OVERLAY] slot %s no longer holds our replacement (now %p): the overlay may stop drawing", s->name, v);
    }
}

// ---- re-init ----------------------------------------------------------------------------------------

// The image count or the format changed: the engine idled the device before it rebuilt the swapchain, so the
// ring's fences are the only work of ours in flight. They are waited (bounded; this is not the steady state),
// then the sets, the ring, the backend and our render pass go and are built again.
void Reinit(const rhi::ViewportState& st) {
    Context& c = Ctx();
    UE_LOGI("[VK-OVERLAY] re-init: images %u -> %u, format %d -> %d", c.st.imageCount, st.imageCount, c.st.vkFormat,
            st.vkFormat);
    if (g_ringCount) {
        VkFence fences[kMaxImages] = {};
        for (uint32_t i = 0; i < g_ringCount; ++i) fences[i] = g_ring[i].fence;
        const VkResult wr = c.fns.vkWaitForFences(Dev(), g_ringCount, fences, VK_TRUE, kFenceWaitNs);
        if (wr != VK_SUCCESS) Check(wr == VK_TIMEOUT ? VK_ERROR_DEVICE_LOST : wr);
    }
    for (SwapSet& s : g_sets)
        if (s.chain) FreeSet(s);
    DestroyRing();
    ShutdownBackend();
    if (IsDown()) return;  // the backend could not be shut down cleanly: it is not rebuilt on top of itself
    c.st = st;
    RingReset();
    if (!InitBackend()) MarkDown("the ImGui Vulkan backend could not be rebuilt");
}

// ---- one present --------------------------------------------------------------------------------------

// The frame itself, under the mutex and the inner __try. Returns the semaphore to present on (null: none).
FrameResult RunFrameBody(void* viewport, VkQueue queue, const VkPresentInfoKHR* info) {
    const Context& c = Ctx();
    const uint64_t now = ::GetTickCount64();
    g_cur = {};
    PollFences(now);
    if (IsDown()) return {};
    CheckSlots(now);
    PrintVerdicts();
    if (!rhi::ReadViewport(viewport, g_cur.st)) {
        // Transient while the engine recreates the swapchain; a count that keeps climbing is a defect.
        if (++g_unreadPresents == 1 || g_unreadPresents % 300 == 0)
            UE_LOGW("[VK-OVERLAY] %u present(s) passed untouched: the viewport could not be read", g_unreadPresents);
        return {};
    }
    const rhi::ViewportState& st = g_cur.st;
    // Only the viewport's own swapchain on the present queue is ours; anything else goes through untouched.
    if (st.vkSwapchain != reinterpret_cast<std::uintptr_t>(info->pSwapchains[0]) ||
        reinterpret_cast<std::uintptr_t>(queue) != c.st.vkQueue)
        return {};
    if (info->waitSemaphoreCount != 1 || !info->pWaitSemaphores || !info->pImageIndices) {
        if (!g_warnedWaits) {
            g_warnedWaits = true;
            UE_LOGW("[VK-OVERLAY] a present with %u wait semaphores passes untouched: the overlay draws only into "
                    "presents with exactly one",
                    info->waitSemaphoreCount);
        }
        return {};
    }
    const uint32_t image = info->pImageIndices[0];
    if (image >= st.imageCount) {
        if (++g_badImagePresents == 1 || g_badImagePresents % 300 == 0)
            UE_LOGW("[VK-OVERLAY] %u present(s) passed untouched: image index %u of %u", g_badImagePresents, image,
                    st.imageCount);
        return {};
    }
    if (st.vkFormat != c.st.vkFormat || st.imageCount != c.st.imageCount) {
        Reinit(st);
        if (IsDown()) return {};
    } else if (g_win[0].printed && !g_drillDone && DrillReinitOn()) {
        // The drill runs the same function on a live session, where the device was not idled by the engine:
        // the queue is, so no present of ours still waits on a semaphore the rebuild destroys.
        g_drillDone = true;
        Check(c.fns.vkQueueWaitIdle(As<VkQueue>(c.st.vkQueue)));
        Reinit(st);
        if (IsDown()) return {};
        g_win[1].active = true;
    }
    g_cur.queue = queue;
    g_cur.info = info;
    g_cur.imageIndex = image;
    g_cur.now = now;
    g_cur.vf = {viewport, const_cast<VkPresentInfoKHR*>(info), queue, image, nullptr};
    FrameTarget t{Rhi::Vulkan, reinterpret_cast<HWND>(st.hwnd), nullptr, &g_cur.vf};
    imgui_overlay::OverlayFrame(t);
    return {reinterpret_cast<VkSemaphore>(g_cur.vf.waitOut), g_cur.windows};
}

FrameResult RunFrameGuarded(void* viewport, VkQueue queue, const VkPresentInfoKHR* info) {
    __try {
        return RunFrameBody(viewport, queue, info);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        MarkDown("a fault or a failed Vulkan call in the present path");
        // Once our submit went in it consumed the engine's wait semaphore: the present must wait on ours.
        return {reinterpret_cast<VkSemaphore>(g_cur.vf.waitOut), g_cur.windows};
    }
}

FrameResult RunFrameLocked(void* viewport, VkQueue queue, const VkPresentInfoKHR* info) {
    std::lock_guard<std::mutex> lock(g_mu);
    return RunFrameGuarded(viewport, queue, info);
}

VkResult SwappedPresent(VkQueue queue, const VkPresentInfoKHR* info) {
    void* const viewport = t_viewport;
    if (!viewport || !info || info->swapchainCount != 1 || !info->pSwapchains ||
        g_stopped.load(std::memory_order_relaxed) || IsDown())
        return g_savedPresent(queue, info);
    const FrameResult fr = RunFrameLocked(viewport, queue, info);
    VkResult r;
    if (fr.wait) {
        // A copy of the engine's info with our semaphore as its one wait: pNext and the rest pass through.
        VkPresentInfoKHR patched = *info;
        patched.waitSemaphoreCount = 1;
        patched.pWaitSemaphores = &fr.wait;
        r = g_savedPresent(queue, &patched);
    } else {
        r = g_savedPresent(queue, info);
    }
    NotePresentResult(fr.windows, r);
    return r;
}

// ---- destroy ----------------------------------------------------------------------------------------

void DestroyGuarded(VkDevice device, VkSwapchainKHR chain) {
    __try {
        if (reinterpret_cast<std::uintptr_t>(device) != Ctx().st.vkDevice) return;
        if (SwapSet* s = FindSet(chain)) FreeSet(*s);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        MarkDown("a fault while freeing a destroyed swapchain's set");
    }
}

void DestroyLocked(VkDevice device, VkSwapchainKHR chain) {
    std::lock_guard<std::mutex> lock(g_mu);
    DestroyGuarded(device, chain);
}

// The engine has idled the device before it destroys a swapchain, so no frame of ours is using its set.
void SwappedDestroy(VkDevice device, VkSwapchainKHR chain, const VkAllocationCallbacks* alloc) {
    if (chain && !g_stopped.load(std::memory_order_relaxed)) DestroyLocked(device, chain);
    g_savedDestroy(device, chain, alloc);
}

// ---- the slots ------------------------------------------------------------------------------------------

// Is `v` the loader's own function for the Vulkan entry point `name`: its export or the instance's answer?
bool IsLoaderPointer(void* v, const char* name, rhi::Handle instance) {
    return v && (v == reinterpret_cast<void*>(rhi::LoaderExport(name)) ||
                 v == reinterpret_cast<void*>(rhi::GetInstanceProcAddr(instance, name)));
}

}  // namespace

void SetViewport(void* viewport) { t_viewport = viewport; }

bool ResolveSlots() {
    g_present.addr = rhi::ResolveLoaderSlot(g_present.name);
    g_destroy.addr = rhi::ResolveLoaderSlot(g_destroy.name);
    for (const Slot* s : {&g_present, &g_destroy})
        if (!s->addr) UE_LOGW("[VK-OVERLAY] the loader slot of %s was not found in the image", s->name);
    return g_present.addr && g_destroy.addr;
}

bool SwapSlots() {
    const rhi::Handle instance = Ctx().st.vkInstance;
    for (Slot* s : {&g_present, &g_destroy}) {
        void* const held = s->addr ? rhi::ReadSlot(s->addr) : nullptr;
        if (!IsLoaderPointer(held, s->name, instance)) {
            UE_LOGE("[VK-OVERLAY] slot %s holds %p, which is not the loader's own function (another swapper?): "
                    "overlay down",
                    s->name, held);
            return false;
        }
        s->saved = held;
    }
    g_savedPresent = reinterpret_cast<PresentFn>(g_present.saved);  // published before the swap
    g_savedDestroy = reinterpret_cast<DestroyFn>(g_destroy.saved);
    g_present.ours = reinterpret_cast<void*>(&SwappedPresent);
    g_destroy.ours = reinterpret_cast<void*>(&SwappedDestroy);
    for (Slot* s : {&g_present, &g_destroy}) {
        // Only while the slot still holds the value checked above: a swapper that wrote in between keeps its pointer.
        if (!rhi::SwapSlot(s->addr, s->saved, s->ours)) {
            UE_LOGE("[VK-OVERLAY] slot %s changed between the check and the swap (now %p, checked %p): not "
                    "swapped, overlay down",
                    s->name, rhi::ReadSlot(s->addr), s->saved);
            RestoreSlots();  // a slot already swapped goes back, and the replacements pass through
            return false;
        }
        s->swapped = true;
    }
    UE_LOGI("[VK-OVERLAY] slots swapped: %s %p -> %p, %s %p -> %p", g_present.name, g_present.saved, g_present.ours,
            g_destroy.name, g_destroy.saved, g_destroy.ours);
    return true;
}

void RestoreSlots() {
    g_stopped.store(true, std::memory_order_relaxed);
    int restored = 0;
    for (Slot* s : {&g_present, &g_destroy}) {
        if (!s->swapped) continue;
        s->swapped = false;
        if (rhi::RestoreSlot(s->addr, s->ours, s->saved))
            ++restored;
        else
            UE_LOGW("[VK-OVERLAY] slot %s now holds %p, not our replacement: another swapper owns it, so it is left "
                    "in place and ours passes through",
                    s->name, rhi::ReadSlot(s->addr));
    }
    if (restored) UE_LOGI("[VK-OVERLAY] slots restored: %d", restored);
}

void EnsureTarget(const FrameTarget&) {
    g_cur.set = nullptr;
    if (IsDown()) return;
    const rhi::ViewportState& st = g_cur.st;
    if (g_ringCount != st.imageCount) {
        if (g_ringCount) DestroyRing();
        CreateRing(st.imageCount);
    }
    SwapSet* s = FindSet(As<VkSwapchainKHR>(st.vkSwapchain));
    if (!s) {
        s = BuildSet(st);
    } else if (s->w != st.internalW || s->h != st.internalH) {
        // The framebuffers follow the engine's extent; they are rebuilt only with none of our work in flight.
        if (AnyInFlight()) return;
        DestroyFramebuffers(*s);
        BuildFramebuffers(*s, st);
    }
    g_cur.set = s;
}

namespace {

// Records the ImGui draw data into the slot's buffer on the image's framebuffer and submits it: waits on the
// present's one semaphore, signals the image's "done" one. Raises on any Vulkan error.
VkSemaphore RecordAndSubmit(RingSlot& slot, const SwapSet& set, ImDrawData* draw) {
    const Context& c = Ctx();
    const rhi::DeviceFns& f = c.fns;
    VkCommandBuffer cmd = slot.cmd;
    Check(f.vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(f.vkBeginCommandBuffer(cmd, &bi));
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = c.renderPass;
    rbi.framebuffer = set.fbs[g_cur.imageIndex];
    rbi.renderArea = {{0, 0}, {set.w, set.h}};
    f.vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    if (draw->Textures)  // the textures the backend is about to create or update, each with a queue wait
        for (const ImTextureData* tex : *draw->Textures)
            if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates) NoteUploads(1);
    ImGui_ImplVulkan_RenderDrawData(draw, cmd);
    RingAdvance();  // ImGui's own buffer ring stepped on that call (it returns early only on a zero-size display)
    f.vkCmdEndRenderPass(cmd);
    Check(f.vkEndCommandBuffer(cmd));

    const VkSemaphore done = set.done[g_cur.imageIndex];
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = g_cur.info->pWaitSemaphores;
    si.pWaitDstStageMask = &stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &done;
    Check(f.vkResetFences(Dev(), 1, &slot.fence));
    Check(f.vkQueueSubmit(g_cur.queue, 1, &si, slot.fence));
    return done;
}

// The inner step: its handler latches the overlay down, so a failure here never reaches the host's own
// handler (which would close the surfaces) and the frame is presented with the engine's own semaphore.
VkSemaphore RecordGuarded(RingSlot& slot, const SwapSet& set, ImDrawData* draw) {
    __try {
        return RecordAndSubmit(slot, set, draw);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        MarkDown("a Vulkan call failed while drawing");
        return VK_NULL_HANDLE;
    }
}

// This present is one the proof windows count: the first kWindowFrames presents of a window in which a frame
// was due to be drawn.
void OpenWindows() {
    for (Window& w : g_win) {
        if (!w.active || w.open >= kWindowFrames) continue;
        ++w.open;
        g_cur.windows |= w.bit;
    }
}

}  // namespace

void Render(const FrameTarget& t) {
    ImDrawData* draw = ImGui::GetDrawData();
    if (!g_cur.set || IsDown() || !draw) return;
    // Mirrors ImGui's own early return: such a call advances neither ring.
    if (static_cast<int>(draw->DisplaySize.x * draw->FramebufferScale.x) <= 0 ||
        static_cast<int>(draw->DisplaySize.y * draw->FramebufferScale.y) <= 0)
        return;
    // Nothing to draw (the HUD idle): no pass over the whole image, no submit, no ImGui call, so neither ring
    // moves and the present goes through with the engine's own semaphore.
    if (draw->TotalVtxCount == 0) return;
    if (Ctx().red == Red::Present) {  // the red arm: present with the engine's semaphore, submit nothing
        OpenWindows();
        return;
    }
    RingSlot& slot = g_ring[RingIndex()];
    if (slot.inFlight) {
        // The slot's previous frame is still on the GPU: skip this overlay frame, never wait for it.
        if (++g_skipped == 1 || g_skipped % 600 == 0)
            UE_LOGW("[VK-OVERLAY] skipped %u overlay frame(s): the ring slot's fence was still unsignalled", g_skipped);
        return;
    }
    OpenWindows();
    const VkSemaphore done = RecordGuarded(slot, *g_cur.set, draw);
    if (!done) return;
    slot.inFlight = true;
    slot.submitTick = g_cur.now;
    slot.windows = g_cur.windows;
    for (Window& w : g_win)
        if (g_cur.windows & w.bit) ++w.submitted;
    t.vk->waitOut = done;
}

}  // namespace ui::overlay_backend::vk
