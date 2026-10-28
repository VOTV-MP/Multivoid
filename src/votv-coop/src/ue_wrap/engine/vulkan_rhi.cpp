// ue_wrap/engine/vulkan_rhi.cpp -- see ue_wrap/engine/vulkan_rhi.h.

#include "ue_wrap/engine/vulkan_rhi.h"

#include "ue_wrap/core/scan_ledger.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/sig_scan.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace ue_wrap::vulkan_rhi {
namespace {

namespace prof = ue_wrap::profile;

constexpr size_t kMaxNameLen   = 48;
constexpr size_t kCallWindow   = 32;  // bytes from the lea in which the lookup call must start
constexpr size_t kStoreWindow  = 32;  // bytes from the call's end in which the slot store must start
constexpr std::uint32_t kMaxPixelFormat = 128;
constexpr std::uint32_t kMaxImages      = 16;

std::atomic<std::uintptr_t> g_table{0};
std::atomic<bool>           g_tableTried{false};

template <typename T>
T Rd(std::uintptr_t base, size_t off) {
    return *reinterpret_cast<const T*>(base + off);
}

bool PtrOk(std::uintptr_t p) { return p != 0 && (p & 7) == 0; }

// ---- the viewport ---------------------------------------------------------------------------

// The reads themselves; any of them may fault. Plain values only, so the SEH frame below holds no object
// with a destructor.
bool ReadUnguarded(std::uintptr_t vp, std::uintptr_t table, ViewportState& o) {
    o.viewport = vp;
    o.device   = Rd<std::uintptr_t>(vp, prof::kVkViewport_Device);
    o.swapChain = Rd<std::uintptr_t>(vp, prof::kVkViewport_SwapChain);
    o.hwnd     = Rd<std::uintptr_t>(vp, prof::kVkViewport_WindowHandle);
    o.pixelFormat   = Rd<std::int32_t>(vp, prof::kVkViewport_PixelFormat);
    if (!PtrOk(o.device) || !PtrOk(o.swapChain)) return false;
    if (o.pixelFormat < 1 || static_cast<std::uint32_t>(o.pixelFormat) >= kMaxPixelFormat) return false;

    o.vkDevice         = Rd<Handle>(o.device, prof::kVkDevice_VkDevice);
    o.vkPhysicalDevice = Rd<Handle>(o.device, prof::kVkDevice_PhysicalDevice);
    o.rhi              = Rd<std::uintptr_t>(o.device, prof::kVkDevice_Rhi);
    o.presentQueue     = Rd<std::uintptr_t>(o.device, prof::kVkDevice_PresentQueue);
    if (!PtrOk(o.rhi) || !PtrOk(o.presentQueue)) return false;
    o.vkInstance  = Rd<Handle>(o.rhi, prof::kVkRhi_Instance);
    o.vkQueue     = Rd<Handle>(o.presentQueue, prof::kVkQueue_Queue);
    o.queueFamily = Rd<std::uint32_t>(o.presentQueue, prof::kVkQueue_FamilyIndex);
    o.queueIndex  = Rd<std::uint32_t>(o.presentQueue, prof::kVkQueue_QueueIndex);

    o.vkSwapchain   = Rd<Handle>(o.swapChain, prof::kVkSwapChain_VkSwapchain);
    o.vkSurface     = Rd<Handle>(o.swapChain, prof::kVkSwapChain_Surface);
    o.internalW     = Rd<std::uint32_t>(o.swapChain, prof::kVkSwapChain_InternalW);
    o.internalH     = Rd<std::uint32_t>(o.swapChain, prof::kVkSwapChain_InternalH);
    o.imageCount    = Rd<std::uint32_t>(o.swapChain, prof::kVkSwapChain_ImageCount);
    if (o.imageCount < 1 || o.imageCount > kMaxImages) return false;
    if (!o.vkInstance || !o.vkPhysicalDevice || !o.vkDevice || !o.vkQueue || !o.vkSwapchain || !o.vkSurface)
        return false;

    o.vkFormat = Rd<std::int32_t>(
        table, static_cast<size_t>(o.pixelFormat) * prof::kGPixelFormats_Stride + prof::kGPixelFormats_PlatformFormat);
    return true;
}

bool ReadGuarded(std::uintptr_t vp, std::uintptr_t table, ViewportState& o) {
    __try {
        return ReadUnguarded(vp, table, o);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- the loader slots -----------------------------------------------------------------------

// The pattern "00 <name bytes> 00": the name as a NUL-bounded string preceded by a NUL.
bool NamePattern(const char* name, char* out, size_t cap) {
    const size_t n = std::strlen(name);
    if (n == 0 || n > kMaxNameLen || cap < 3 * (n + 2) + 1) return false;
    size_t at = 0;
    at += static_cast<size_t>(std::snprintf(out + at, cap - at, "00"));
    for (size_t i = 0; i < n; ++i)
        at += static_cast<size_t>(std::snprintf(out + at, cap - at, " %02X", static_cast<unsigned char>(name[i])));
    std::snprintf(out + at, cap - at, " 00");
    return true;
}

// The address of the one NUL-bounded copy of `name` in the image, or 0 when there is none or several.
std::uintptr_t OnlyNameInImage(const char* name) {
    char pat[3 * (kMaxNameLen + 2) + 1];
    if (!NamePattern(name, pat, sizeof pat)) return 0;
    std::uintptr_t base = 0;
    size_t size = 0;
    MainModuleRange(base, size);
    if (!base || !size) return 0;
    const std::uintptr_t hit = FindPatternIn(base, size, pat);
    if (!hit) return 0;
    const std::uintptr_t next = hit + 1;
    if (FindPatternIn(next, base + size - next, pat)) return 0;
    return hit + 1;
}

// The address of the one `lea rdx,[rip+rel32]` in .text whose target is `target`, or 0 for none or several.
std::uintptr_t OnlyLeaRdxOf(std::uintptr_t target) {
    std::uintptr_t text = 0;
    size_t size = 0;
    if (!MainTextRange(text, size) || size < 8) return 0;
    const auto* p = reinterpret_cast<const std::uint8_t*>(text);
    std::uintptr_t found = 0;
    for (size_t i = 1; i + 6 < size;) {
        const void* op = std::memchr(p + i, 0x8D, size - 6 - i);
        if (!op) break;
        i = static_cast<size_t>(static_cast<const std::uint8_t*>(op) - p);
        if (p[i - 1] == 0x48 && p[i + 1] == 0x15) {
            std::int32_t rel;
            std::memcpy(&rel, p + i + 2, sizeof rel);
            if (text + (i - 1) + 7 + rel == target) {
                if (found) return 0;
                found = text + (i - 1);
            }
        }
        ++i;
    }
    return found;
}

// The first occurrence of the `n` bytes `lead` that starts within `window` bytes of `from` and ends before
// `limit`; its address, or 0.
std::uintptr_t FirstBytes(std::uintptr_t from, size_t window, const std::uint8_t* lead, size_t n,
                          std::uintptr_t limit) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(from);
    for (size_t i = 0; i < window; ++i) {
        if (from + i + n > limit) return 0;
        if (std::memcmp(p + i, lead, n) == 0) return from + i;
    }
    return 0;
}

using GetInstanceProcFn = ProcAddr (*)(Handle, const char*);
using GetDeviceProcFn   = ProcAddr (*)(Handle, const char*);

GetInstanceProcFn InstanceProcFn() {
    static GetInstanceProcFn fn = [] {
        HMODULE mod = ::GetModuleHandleW(L"vulkan-1.dll");
        return mod ? reinterpret_cast<GetInstanceProcFn>(::GetProcAddress(mod, "vkGetInstanceProcAddr")) : nullptr;
    }();
    return fn;
}

}  // namespace

std::uintptr_t ResolvePresent() {
    std::uintptr_t text = 0;
    size_t size = 0;
    std::uintptr_t fn = 0;
    if (MainTextRange(text, size)) {
        const std::uintptr_t hit = FindPatternIn(text, size, prof::kSigVulkanViewportPresentBody);
        if (hit) fn = FunctionStart(hit);
    }
    scan_ledger::Record(scan_ledger::Scan::VulkanPresent, fn != 0);
    return fn;
}

std::uintptr_t ResolvePixelFormatTable() {
    if (g_tableTried.load(std::memory_order_acquire)) return g_table.load(std::memory_order_acquire);
    std::uintptr_t text = 0;
    size_t size = 0;
    std::uintptr_t table = 0;
    if (MainTextRange(text, size)) {
        const std::uintptr_t hit = FindPatternIn(text, size, prof::kSigVulkanPixelFormatTable);
        if (hit) {
            std::int32_t rel;
            std::memcpy(&rel, reinterpret_cast<const void*>(hit + prof::kVulkanPixelFormatTableRelOff), sizeof rel);
            table = hit + prof::kVulkanPixelFormatTableNextOff + rel;
        }
    }
    scan_ledger::Record(scan_ledger::Scan::VulkanPixelFormats, table != 0);
    g_table.store(table, std::memory_order_release);
    g_tableTried.store(true, std::memory_order_release);
    return table;
}

std::uintptr_t ResolveLoaderSlot(const char* name) {
    if (!name) return 0;
    const std::uintptr_t str = OnlyNameInImage(name);
    if (!str) return 0;
    const std::uintptr_t lea = OnlyLeaRdxOf(str);
    if (!lea) return 0;
    std::uintptr_t text = 0;
    size_t size = 0;
    if (!MainTextRange(text, size)) return 0;
    const std::uintptr_t limit = text + size;
    static const std::uint8_t kCall[2]  = {0xFF, 0x15};
    static const std::uint8_t kStore[3] = {0x48, 0x89, 0x05};
    const std::uintptr_t call = FirstBytes(lea + 7, kCallWindow, kCall, sizeof kCall, limit);
    if (!call) return 0;
    const std::uintptr_t store = FirstBytes(call + 6, kStoreWindow, kStore, sizeof kStore, limit);
    if (!store || store + 7 > limit) return 0;
    std::int32_t rel;
    std::memcpy(&rel, reinterpret_cast<const void*>(store + 3), sizeof rel);
    const std::uintptr_t slot = store + 7 + rel;
    // SwapSlot's compare-exchange is atomic only on an aligned pointer; the loader's table is one, so anything else is a
    // wrong match.
    return (slot & 7) == 0 ? slot : 0;
}

bool ReadViewport(void* viewport, ViewportState& out) {
    std::memset(&out, 0, sizeof out);
    const std::uintptr_t vp = reinterpret_cast<std::uintptr_t>(viewport);
    const std::uintptr_t table = ResolvePixelFormatTable();
    if (!PtrOk(vp) || !table) return false;
    return ReadGuarded(vp, table, out);
}

ProcAddr GetInstanceProcAddr(Handle instance, const char* name) {
    const GetInstanceProcFn fn = InstanceProcFn();
    return fn ? fn(instance, name) : nullptr;
}

ProcAddr GetDeviceProcAddr(Handle instance, Handle device, const char* name) {
    const GetDeviceProcFn fn = reinterpret_cast<GetDeviceProcFn>(GetInstanceProcAddr(instance, "vkGetDeviceProcAddr"));
    return fn ? fn(device, name) : nullptr;
}

namespace {

// The lookups themselves; a wrong device handle faults inside the loader. Plain values only, so the SEH
// frame below holds no object with a destructor.
bool FillUnguarded(Handle instance, Handle device, DeviceFns& out, const char** missing) {
    if (!GetInstanceProcAddr(instance, "vkGetDeviceProcAddr")) {
        *missing = "vkGetDeviceProcAddr";
        return false;
    }
#define UE_WRAP_VK_FILL(name)                                                           \
    out.name = reinterpret_cast<PFN_##name>(GetDeviceProcAddr(instance, device, #name)); \
    if (!out.name) {                                                                    \
        *missing = #name;                                                               \
        return false;                                                                   \
    }
    UE_WRAP_VK_DEVICE_FNS(UE_WRAP_VK_FILL)
#undef UE_WRAP_VK_FILL
    return true;
}

bool FillGuarded(Handle instance, Handle device, DeviceFns& out, const char** missing) {
    __try {
        return FillUnguarded(instance, device, out, missing);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *missing = "a fault inside the loader";
        return false;
    }
}

}  // namespace

bool FillDeviceFns(Handle instance, Handle device, DeviceFns& out, const char** missing) {
    const char* unused = nullptr;
    const char** why = missing ? missing : &unused;
    *why = nullptr;
    std::memset(&out, 0, sizeof out);
    if (!instance || !device) {
        *why = "a null instance or device";
        return false;
    }
    return FillGuarded(instance, device, out, why);
}

ProcAddr LoaderExport(const char* name) {
    HMODULE mod = ::GetModuleHandleW(L"vulkan-1.dll");
    return mod ? reinterpret_cast<ProcAddr>(::GetProcAddress(mod, name)) : nullptr;
}

void* ReadSlot(std::uintptr_t slot) {
    return *reinterpret_cast<void* const volatile*>(slot);
}

bool SwapSlot(std::uintptr_t slot, void* expected, void* ours) {
    return ::InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot), ours, expected) == expected;
}

bool RestoreSlot(std::uintptr_t slot, void* ours, void* saved) {
    return ::InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot), saved, ours) == ours;
}

}  // namespace ue_wrap::vulkan_rhi
