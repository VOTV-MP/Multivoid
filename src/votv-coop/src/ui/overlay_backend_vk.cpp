// ui/overlay_backend_vk.cpp -- the Vulkan implementation of ui/overlay_backend.h, bring-up half: the
// verification of what the engine seam read, the ImGui Vulkan backend and our render pass, the skin-preview
// textures, the draw selftest, and the ops table. The swapped present and destroy, the per-swapchain
// framebuffers and the ring live in overlay_backend_vk_frame.cpp.
//
// The game owns the Vulkan device, so everything here runs on its render thread inside the engine's own
// Present call (ImGui is driven from there for DX alike). Every engine read and every Vulkan call of the
// bring-up sits under SEH: a wrong handle faults inside the loader. A failing call of ours or of the ImGui
// backend raises one SEH exception (Check); a function that holds a destructor-bearing local never holds a
// __try, so each guarded step is its own small function.

#include "overlay_backend_vk.h"

#include "ui/overlay_backend.h"
#include "overlay_seams.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/vulkan_rhi.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"  // RegisterUserTexture
#include "backends/imgui_impl_vulkan.h"

namespace ui::overlay_backend::vk {
namespace {

constexpr DWORD    kVkFailure     = 0xE0564B01u;  // the SEH code Check raises
constexpr uint32_t kMaxImages     = 16;           // the engine reader refuses more
constexpr uint32_t kSelfTestSide  = 64;           // the selftest image is kSelfTestSide x kSelfTestSide
constexpr uint64_t kSelfTestWaitNs = 2000000000ull;
constexpr size_t   kMaxTextures   = 64;           // skin previews held at once
// Combined-image-sampler descriptors of the backend's own pool: every preview, plus the font atlas (twice while
// it is rebuilt) and spare, so a preview can never be what exhausts the pool.
constexpr uint32_t kPoolHeadroom  = 8;
constexpr uint32_t kPoolSize      = static_cast<uint32_t>(kMaxTextures) + kPoolHeadroom;

Context            g_ctx{};
std::atomic<bool>  g_down{false};
std::atomic<int>   g_lastResult{0};
uint32_t           g_ring = 0;
uint32_t           g_uploads = 0;  // render thread only

// ---- checks -----------------------------------------------------------------------------------

// A refused bring-up: the one line, the overlay down for the run, nothing swapped.
void Refuse(const char* why) {
    if (g_down.exchange(true)) return;
    UE_LOGE("[VK-OVERLAY] bring-up refused: %s -- overlay down for this run, presents untouched", why);
    ue_wrap::log::Flush();
}

// ---- verification: every read of the engine checked through the API ---------------------------------

// All of it may fault. Plain values only, no object with a destructor.
bool VerifyUnguarded(const FrameTarget& t, Context& c, char* why, size_t cap) {
    rhi::ViewportState& st = c.st;
    if (!t.vk || !rhi::ReadViewport(t.vk->viewport, st)) {
        std::snprintf(why, cap, "the viewport read was refused (a fault or an implausible value) at %p",
                      t.vk ? t.vk->viewport : nullptr);
        return false;
    }
    HWND hwnd = reinterpret_cast<HWND>(st.hwnd);
    DWORD pid = 0;
    if (!::IsWindow(hwnd) || ::GetWindowThreadProcessId(hwnd, &pid) == 0 || pid != ::GetCurrentProcessId()) {
        std::snprintf(why, cap, "window handle 0x%llx is not a window of this process",
                      static_cast<unsigned long long>(st.hwnd));
        return false;
    }
    if (st.internalW == 0 || st.internalH == 0 || st.imageCount < 2) {
        std::snprintf(why, cap, "swapchain extent %ux%u with %u images is not drawable", st.internalW,
                      st.internalH, st.imageCount);
        return false;
    }
    if (reinterpret_cast<std::uintptr_t>(t.vk->queue) != st.presentQueue) {
        std::snprintf(why, cap, "Present's queue argument %p is not the device's present queue 0x%llx",
                      t.vk->queue, static_cast<unsigned long long>(st.presentQueue));
        return false;
    }

    const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        rhi::GetInstanceProcAddr(st.vkInstance, "vkEnumeratePhysicalDevices"));
    const auto families = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        rhi::GetInstanceProcAddr(st.vkInstance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    if (!enumerate || !families) {
        std::snprintf(why, cap, "the loader does not know %s",
                      enumerate ? "vkGetPhysicalDeviceQueueFamilyProperties" : "vkEnumeratePhysicalDevices");
        return false;
    }
    VkPhysicalDevice devices[16] = {};
    uint32_t deviceCount = 16;
    const VkResult er = enumerate(As<VkInstance>(st.vkInstance), &deviceCount, devices);
    bool listed = false;
    if (er == VK_SUCCESS || er == VK_INCOMPLETE)
        for (uint32_t i = 0; i < deviceCount; ++i)
            if (reinterpret_cast<std::uintptr_t>(devices[i]) == st.vkPhysicalDevice) listed = true;
    if (!listed) {
        std::snprintf(why, cap, "instance 0x%llx does not list physical device 0x%llx (enumerate result %d)",
                      static_cast<unsigned long long>(st.vkInstance),
                      static_cast<unsigned long long>(st.vkPhysicalDevice), static_cast<int>(er));
        return false;
    }

    VkQueueFamilyProperties props[32] = {};
    uint32_t familyCount = 32;
    families(As<VkPhysicalDevice>(st.vkPhysicalDevice), &familyCount, props);
    if (st.queueFamily >= familyCount || !(props[st.queueFamily].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
        std::snprintf(why, cap,
                      "present queue is not graphics-capable (r.Vulkan.AllowPresentOnComputeQueue): "
                      "family %u of %u",
                      st.queueFamily, familyCount);
        return false;
    }

    const char* missing = nullptr;
    if (!rhi::FillDeviceFns(st.vkInstance, st.vkDevice, c.fns, &missing)) {
        std::snprintf(why, cap, "device entry point %s is missing", missing ? missing : "?");
        return false;
    }
    VkQueue queue = VK_NULL_HANDLE;
    c.fns.vkGetDeviceQueue(As<VkDevice>(st.vkDevice), st.queueFamily, st.queueIndex, &queue);
    if (reinterpret_cast<std::uintptr_t>(queue) != st.vkQueue) {
        std::snprintf(why, cap, "vkGetDeviceQueue(family %u, index %u) is 0x%llx, the engine read 0x%llx",
                      st.queueFamily, st.queueIndex,
                      static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(queue)),
                      static_cast<unsigned long long>(st.vkQueue));
        return false;
    }
    uint32_t images = 0;
    const VkResult ir = c.fns.vkGetSwapchainImagesKHR(As<VkDevice>(st.vkDevice), As<VkSwapchainKHR>(st.vkSwapchain),
                                                       &images, nullptr);
    if (ir != VK_SUCCESS || images != st.imageCount || images > kMaxImages) {
        std::snprintf(why, cap, "the swapchain has %u images (result %d), the engine read %u", images,
                      static_cast<int>(ir), st.imageCount);
        return false;
    }
    return true;
}

bool VerifyGuarded(const FrameTarget& t, Context& c, char* why, size_t cap) {
    __try {
        return VerifyUnguarded(t, c, why, cap);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::snprintf(why, cap, "a fault while checking the engine's handles against the API");
        return false;
    }
}

// ---- the draw selftest ------------------------------------------------------------------------------

// What the selftest created, so one teardown frees whatever exists after a failure part-way.
struct SelfTestObjs {
    VkImage        image;
    VkDeviceMemory imageMem;
    VkImageView    view;
    VkRenderPass   pass;
    VkFramebuffer  fb;
    VkBuffer       buf;
    VkDeviceMemory bufMem;
    VkCommandPool  pool;
    VkFence        fence;
};

// The pixel an opaque pure red (255, 0, 0, 255) leaves in the first four bytes of a texel of `format`, read
// as one little-endian dword; false for a format the selftest has no layout for.
bool ExpectedPixel(VkFormat format, uint32_t& pixel) {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM:          pixel = 0xFF0000FFu; return true;
        case VK_FORMAT_B8G8R8A8_UNORM:          pixel = 0xFFFF0000u; return true;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: pixel = 0xC00003FFu; return true;
        default: return false;
    }
}

uint32_t PickMemoryType(const VkPhysicalDeviceMemoryProperties& mem, uint32_t typeBits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
        if ((typeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0xFFFFFFFFu;
}

// Builds the offscreen target, renders one real ImGui frame (a solid red rectangle) through the same backend
// into it, and reads the middle pixel back. Raises on any Vulkan error; the caller frees `o`. Returns the pixel.
uint32_t SelfTestRender(SelfTestObjs& o, Context& c, bool drawIt) {
    const rhi::DeviceFns& f = c.fns;
    const VkDevice dev = As<VkDevice>(c.st.vkDevice);
    const VkFormat format = static_cast<VkFormat>(c.st.vkFormat);
    const auto memProps = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        rhi::GetInstanceProcAddr(c.st.vkInstance, "vkGetPhysicalDeviceMemoryProperties"));
    VkPhysicalDeviceMemoryProperties mem{};
    memProps(As<VkPhysicalDevice>(c.st.vkPhysicalDevice), &mem);

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {kSelfTestSide, kSelfTestSide, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    Check(f.vkCreateImage(dev, &ici, nullptr, &o.image));
    VkMemoryRequirements req{};
    f.vkGetImageMemoryRequirements(dev, o.image, &req);
    uint32_t type = PickMemoryType(mem, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == 0xFFFFFFFFu) type = PickMemoryType(mem, req.memoryTypeBits, 0);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    Check(f.vkAllocateMemory(dev, &mai, nullptr, &o.imageMem));
    Check(f.vkBindImageMemory(dev, o.image, o.imageMem, 0));

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = o.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    Check(f.vkCreateImageView(dev, &vci, nullptr, &o.view));

    // Its own pass (clear, end in TRANSFER_SRC): a pipeline built against our PRESENT_SRC pass is usable in any
    // compatible pass, and the target is not a presentable image.
    VkAttachmentDescription att{};
    att.format = format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = 0;
    dep.dstSubpass = VK_SUBPASS_EXTERNAL;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1;
    rpi.pAttachments = &att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    rpi.dependencyCount = 1;
    rpi.pDependencies = &dep;
    Check(f.vkCreateRenderPass(dev, &rpi, nullptr, &o.pass));
    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = o.pass;
    fci.attachmentCount = 1;
    fci.pAttachments = &o.view;
    fci.width = kSelfTestSide;
    fci.height = kSelfTestSide;
    fci.layers = 1;
    Check(f.vkCreateFramebuffer(dev, &fci, nullptr, &o.fb));

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = static_cast<VkDeviceSize>(kSelfTestSide) * kSelfTestSide * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    Check(f.vkCreateBuffer(dev, &bci, nullptr, &o.buf));
    f.vkGetBufferMemoryRequirements(dev, o.buf, &req);
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = PickMemoryType(mem, req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mai.memoryTypeIndex == 0xFFFFFFFFu) Check(VK_ERROR_FEATURE_NOT_PRESENT);
    Check(f.vkAllocateMemory(dev, &mai, nullptr, &o.bufMem));
    Check(f.vkBindBufferMemory(dev, o.buf, o.bufMem, 0));

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = c.st.queueFamily;
    Check(f.vkCreateCommandPool(dev, &pci, nullptr, &o.pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = o.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    Check(f.vkAllocateCommandBuffers(dev, &cai, &cmd));
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    Check(f.vkCreateFence(dev, &fi, nullptr, &o.fence));

    // One real ImGui frame: a solid foreground rectangle over the whole target.
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(kSelfTestSide), static_cast<float>(kSelfTestSide));
    io.DeltaTime = 1.0f / 60.0f;
    if (io.Fonts->Fonts.Size == 0) io.Fonts->AddFontDefaultBitmap();  // the host's fonts load after the backend
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(255, 0, 0, 255));
    ImGui::Render();

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(f.vkBeginCommandBuffer(cmd, &bi));
    VkClearValue clear{};
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = o.pass;
    rbi.framebuffer = o.fb;
    rbi.renderArea = {{0, 0}, {kSelfTestSide, kSelfTestSide}};
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;
    f.vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    if (drawIt) {
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
        RingAdvance();  // the backend's own buffer ring stepped on that call
    }
    f.vkCmdEndRenderPass(cmd);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {kSelfTestSide, kSelfTestSide, 1};
    f.vkCmdCopyImageToBuffer(cmd, o.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, o.buf, 1, &region);
    Check(f.vkEndCommandBuffer(cmd));

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    Check(f.vkQueueSubmit(As<VkQueue>(c.st.vkQueue), 1, &si, o.fence));
    const VkResult wr = f.vkWaitForFences(dev, 1, &o.fence, VK_TRUE, kSelfTestWaitNs);
    if (wr != VK_SUCCESS) Check(wr == VK_TIMEOUT ? VK_ERROR_DEVICE_LOST : wr);

    void* map = nullptr;
    Check(f.vkMapMemory(dev, o.bufMem, 0, VK_WHOLE_SIZE, 0, &map));
    uint32_t pixel = 0;
    std::memcpy(&pixel, static_cast<const uint8_t*>(map) + (kSelfTestSide / 2 * kSelfTestSide + kSelfTestSide / 2) * 4,
                sizeof pixel);
    f.vkUnmapMemory(dev, o.bufMem);
    return pixel;
}

void SelfTestFree(SelfTestObjs& o, Context& c) {
    const rhi::DeviceFns& f = c.fns;
    const VkDevice dev = As<VkDevice>(c.st.vkDevice);
    if (o.fence)   f.vkDestroyFence(dev, o.fence, nullptr);
    if (o.pool)    f.vkDestroyCommandPool(dev, o.pool, nullptr);
    if (o.buf)     f.vkDestroyBuffer(dev, o.buf, nullptr);
    if (o.bufMem)  f.vkFreeMemory(dev, o.bufMem, nullptr);
    if (o.fb)      f.vkDestroyFramebuffer(dev, o.fb, nullptr);
    if (o.pass)    f.vkDestroyRenderPass(dev, o.pass, nullptr);
    if (o.view)    f.vkDestroyImageView(dev, o.view, nullptr);
    if (o.image)   f.vkDestroyImage(dev, o.image, nullptr);
    if (o.imageMem) f.vkFreeMemory(dev, o.imageMem, nullptr);
    std::memset(&o, 0, sizeof o);
}

// The guarded run: the pixel through `pixel`, false when a Vulkan call raised.
bool SelfTestGuarded(SelfTestObjs& o, Context& c, bool drawIt, uint32_t& pixel) {
    __try {
        pixel = SelfTestRender(o, c, drawIt);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The verdict line is the proof that the ImGui Vulkan backend drew on this GPU; true only on PASS.
bool SelfTest() {
    Context& c = g_ctx;
    uint32_t expected = 0;
    const VkFormat format = static_cast<VkFormat>(c.st.vkFormat);
    if (!ExpectedPixel(format, expected)) {
        UE_LOGE("[VK-OVERLAY] selftest FAIL: pixel=0x00000000 expected=0x00000000 (the back buffer's VkFormat %d "
                "has no pixel layout here; a float or HDR format is out of scope)",
                c.st.vkFormat);
        return false;
    }
    SelfTestObjs o{};
    uint32_t pixel = 0;
    const bool ran = SelfTestGuarded(o, c, c.red != Red::SelfTest, pixel);
    SelfTestFree(o, c);
    if (!ran) {
        UE_LOGE("[VK-OVERLAY] selftest FAIL: pixel=0x%08X expected=0x%08X (a Vulkan call failed: result %d)", pixel,
                expected, g_lastResult.load());
        return false;
    }
    const bool pass = pixel == expected;
    if (pass) UE_LOGI("[VK-OVERLAY] selftest PASS: pixel=0x%08X expected=0x%08X", pixel, expected);
    else      UE_LOGE("[VK-OVERLAY] selftest FAIL: pixel=0x%08X expected=0x%08X", pixel, expected);
    ue_wrap::log::Flush();
    return pass;
}

// ---- the ImGui backend and our render pass -------------------------------------------------------------

struct LoaderState {
    rhi::Handle instance;
    const char* missing;
};

PFN_vkVoidFunction LoaderFn(const char* name, void* user) {
    auto* s = static_cast<LoaderState*>(user);
    const auto fn = reinterpret_cast<PFN_vkVoidFunction>(rhi::GetInstanceProcAddr(s->instance, name));
    if (!fn && !s->missing) s->missing = name;
    return fn;
}

// A step that holds the ImGui init info (it owns a vector) and so no __try.
bool BuildBackend() {
    Context& c = g_ctx;
    const VkDevice dev = As<VkDevice>(c.st.vkDevice);

    VkAttachmentDescription att{};
    att.format = static_cast<VkFormat>(c.st.vkFormat);
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    // The load waits for the semaphore the present would have waited on: the external dependency's source
    // stage is the stage our submit's wait semaphore blocks.
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1;
    rpi.pAttachments = &att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    rpi.dependencyCount = 1;
    rpi.pDependencies = &dep;
    Check(c.fns.vkCreateRenderPass(dev, &rpi, nullptr, &c.renderPass));

    static LoaderState s_loader{};
    s_loader = {c.st.vkInstance, nullptr};
    if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_1, &LoaderFn, &s_loader)) {
        UE_LOGE("[VK-OVERLAY] the loader does not know %s, which the ImGui Vulkan backend needs",
                s_loader.missing ? s_loader.missing : "?");
        return false;
    }
    ImGui_ImplVulkan_InitInfo info = {};
    info.ApiVersion = VK_API_VERSION_1_1;  // the RHI creates its instance at 1.1
    info.Instance = As<VkInstance>(c.st.vkInstance);
    info.PhysicalDevice = As<VkPhysicalDevice>(c.st.vkPhysicalDevice);
    info.Device = dev;
    info.QueueFamily = c.st.queueFamily;
    info.Queue = As<VkQueue>(c.st.vkQueue);
    info.DescriptorPoolSize = kPoolSize;  // the backend owns a pool of the right types
    info.MinImageCount = 2;
    info.ImageCount = c.st.imageCount;
    info.PipelineInfoMain.RenderPass = c.renderPass;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.CheckVkResultFn = &Check;
    if (!ImGui_ImplVulkan_Init(&info)) {
        UE_LOGE("[VK-OVERLAY] ImGui_ImplVulkan_Init refused");
        return false;
    }
    return true;
}

// ---- skin-preview textures ---------------------------------------------------------------------------

struct UserTexture {
    ImTextureData* tex;
    bool           retiring;
};
UserTexture g_textures[kMaxTextures];

bool UploadGuarded(ImTextureData* tex) {
    __try {
        NoteUploads(1);
        ImGui_ImplVulkan_UpdateTexture(tex);  // creates the image, uploads, sets TexID; waits on the queue once
        return tex->TexID != ImTextureID_Invalid;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        MarkDown("a skin preview texture could not be uploaded");
        return false;
    }
}

// ---- the ops --------------------------------------------------------------------------------------

// The dev red arm (vk_overlay_red), read once at the bring-up.
Red ReadRedArm() {
    const std::string arm = coop::config::ResolveEnum(::coop::config_registry::rows::vk_overlay_red);
    if (arm == "selftest") return Red::SelfTest;
    if (arm == "present") return Red::Present;
    return Red::None;
}

bool Capture(const FrameTarget& t) {
    std::memset(&g_ctx, 0, sizeof g_ctx);
    char why[256];
    if (!VerifyGuarded(t, g_ctx, why, sizeof why)) {
        Refuse(why);
        return false;
    }
    g_ctx.red = ReadRedArm();
    if (g_ctx.red != Red::None)
        UE_LOGW("[VK-OVERLAY] red arm vk_overlay_red=%s is on: the proof is made to FAIL",
                g_ctx.red == Red::SelfTest ? "selftest" : "present");
    const rhi::ViewportState& st = g_ctx.st;
    UE_LOGI("[VK-OVERLAY] bring-up verified: device=0x%llx queue family=%u index=%u images=%u format=%d extent=%ux%u "
            "hwnd=0x%llx",
            static_cast<unsigned long long>(st.vkDevice), st.queueFamily, st.queueIndex, st.imageCount, st.vkFormat,
            st.internalW, st.internalH, static_cast<unsigned long long>(st.hwnd));
    return true;
}

void Abandon() { std::memset(&g_ctx, 0, sizeof g_ctx); }

bool InitGuarded() {
    __try {
        return BuildBackend();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        UE_LOGE("[VK-OVERLAY] the ImGui Vulkan backend failed to initialise (last failing result %d)",
                g_lastResult.load());
        return false;
    }
}

// The init op is the backend, then the draw selftest: a failure of either unwinds the bring-up and leaves the
// overlay down for the run.
bool InitRenderer(const FrameTarget&) {
    RingReset();
    bool ok = InitGuarded();
    if (ok) {
        UE_LOGI("[VK-OVERLAY] ImGui Vulkan backend up: api 1.1, %u images, descriptor pool %u",
                g_ctx.st.imageCount, kPoolSize);
        ok = SelfTest();
    }
    if (!ok) {
        ShutdownBackend();
        g_down.store(true, std::memory_order_relaxed);
    }
    return ok;
}

void NewFrame() {
    // Reap the previews the panels released: a texture ImGui has finished destroying is unregistered and freed.
    for (UserTexture& u : g_textures) {
        if (!u.tex || !u.retiring || u.tex->Status != ImTextureStatus_Destroyed) continue;
        ImGui::UnregisterUserTexture(u.tex);
        IM_DELETE(u.tex);
        u = {};
    }
    ImGui_ImplVulkan_NewFrame();
}

// The ImGui backend rebuilds the atlas texture itself (RendererHasTextures): nothing to drop here.
void InvalidateDeviceObjects() {}

void* CreateTextureFromImageFile(const wchar_t* path, int* outW, int* outH) {
    if (!path || g_down.load(std::memory_order_relaxed) || !ImGui::GetCurrentContext()) return nullptr;
    UserTexture* slot = nullptr;
    for (UserTexture& u : g_textures)
        if (!u.tex) { slot = &u; break; }
    if (!slot) {
        static bool s_said = false;  // the render thread only
        if (!s_said) {
            s_said = true;
            UE_LOGW("[VK-OVERLAY] a skin preview was refused: all %zu texture slots are in use (the overlay keeps "
                    "running without it)",
                    kMaxTextures);
        }
        return nullptr;
    }
    std::vector<uint8_t> bgra;
    unsigned w = 0, h = 0;
    if (!detail::DecodeImageFileBgra(path, bgra, w, h)) return nullptr;
    ImTextureData* tex = IM_NEW(ImTextureData)();
    tex->Create(ImTextureFormat_RGBA32, static_cast<int>(w), static_cast<int>(h));
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        tex->Pixels[i * 4 + 0] = bgra[i * 4 + 2];
        tex->Pixels[i * 4 + 1] = bgra[i * 4 + 1];
        tex->Pixels[i * 4 + 2] = bgra[i * 4 + 0];
        tex->Pixels[i * 4 + 3] = bgra[i * 4 + 3];
    }
    tex->UseColors = true;
    ImGui::RegisterUserTexture(tex);
    // Uploaded now rather than at the next draw: the caller draws it with the id this returns, this frame.
    if (!UploadGuarded(tex)) {
        ImGui::UnregisterUserTexture(tex);
        IM_DELETE(tex);
        return nullptr;
    }
    *slot = {tex, false};
    if (outW) *outW = static_cast<int>(w);
    if (outH) *outH = static_cast<int>(h);
    // The handle is the ImTextureData itself: its device id changes when a re-init rebuilds the backend, so
    // callers draw through TextureRef and never keep the id.
    return tex;
}

ImTextureRef TextureRef(void* handle) {
    for (const UserTexture& u : g_textures)
        if (u.tex && !u.retiring && u.tex == handle) return u.tex->GetTexRef();
    return ImTextureRef();
}

void DestroyTexture(void* handle) {
    if (!handle) return;
    for (UserTexture& u : g_textures) {
        if (!u.tex || u.retiring || u.tex != handle) continue;
        u.tex->WantDestroyNextFrame = true;  // ImGui destroys it once no frame in flight uses it
        u.retiring = true;
        return;
    }
}

}  // namespace

Context& Ctx() { return g_ctx; }

void Check(VkResult result) {
    if (result >= 0) return;  // VK_SUCCESS, VK_NOT_READY, VK_INCOMPLETE, VK_SUBOPTIMAL_KHR are not errors
    g_lastResult.store(static_cast<int>(result), std::memory_order_relaxed);
    ::RaiseException(kVkFailure, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}

void MarkDown(const char* why) {
    if (g_down.exchange(true)) return;
    UE_LOGE("[VK-OVERLAY] overlay down: %s (last failing Vulkan result %d) -- every later present goes through "
            "untouched",
            why, g_lastResult.load());
    ue_wrap::log::Flush();
    imgui_overlay::OverlayDown();
}

bool IsDown() { return g_down.load(std::memory_order_relaxed); }

bool InitBackend() { return BuildBackend(); }

void ShutdownBackend() {
    // Best effort and guarded: it also unwinds a half-built backend, whose state may be partial.
    __try {
        if (ImGui::GetCurrentContext() && ImGui::GetIO().BackendRendererUserData) ImGui_ImplVulkan_Shutdown();
        if (g_ctx.renderPass) {
            g_ctx.fns.vkDestroyRenderPass(As<VkDevice>(g_ctx.st.vkDevice), g_ctx.renderPass, nullptr);
            g_ctx.renderPass = VK_NULL_HANDLE;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        MarkDown("a fault while shutting the ImGui Vulkan backend down");
    }
}

uint32_t RingIndex() { return g_ctx.st.imageCount ? g_ring % g_ctx.st.imageCount : 0; }
void RingAdvance() { ++g_ring; }
void RingReset() { g_ring = 0; }

void NoteUploads(uint32_t n) { g_uploads += n; }
uint32_t Uploads() { return g_uploads; }

const BackendOps& Ops() {
    static const BackendOps ops = {
        &Capture, &Abandon, &InitRenderer, &NewFrame, &EnsureTarget, &Render, &InvalidateDeviceObjects,
        nullptr, nullptr, &CreateTextureFromImageFile, &DestroyTexture, "Vulkan", &TextureRef};
    return ops;
}

}  // namespace ui::overlay_backend::vk
