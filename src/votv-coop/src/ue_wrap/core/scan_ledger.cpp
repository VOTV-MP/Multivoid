// ue_wrap/core/scan_ledger.cpp -- see ue_wrap/core/scan_ledger.h.

#include "ue_wrap/core/scan_ledger.h"

#include <atomic>
#include <cstddef>

namespace ue_wrap::scan_ledger {
namespace {

constexpr const char* kNames[] = {
    "GUObjectArray",
    "FName::ToString",
    "ProcessEvent",
    "FMemory::Realloc (GMalloc)",
    "the script-body gate (GNatives, then the VM loop by code)",
    "SaveGameToSlot",
    "the overlay's draw seam (the present of the RHI the game runs: D3D11, D3D12 or Vulkan)",
    "the overlay's resize bracket (D3D11 Resize or D3D12 ResizeInternal)",
    "AActor::EndPlay (by code)",
    "the Vulkan renderer's Present (FVulkanViewport::Present, by a stretch of its body)",
    "the Vulkan renderer's pixel-format table (GPixelFormats)",
};
static_assert(sizeof kNames / sizeof kNames[0] == static_cast<size_t>(Scan::kCount), "a name per Scan");

std::atomic<uint8_t>  g_state[static_cast<size_t>(Scan::kCount)] = {};
std::atomic<uint32_t> g_version{0};

}  // namespace

const char* Name(Scan scan) { return kNames[static_cast<size_t>(scan)]; }

void Record(Scan scan, bool found) {
    const uint8_t next = static_cast<uint8_t>(found ? State::Found : State::Missed);
    if (g_state[static_cast<size_t>(scan)].exchange(next, std::memory_order_acq_rel) != next)
        g_version.fetch_add(1, std::memory_order_release);
}

State StateOf(Scan scan) {
    return static_cast<State>(g_state[static_cast<size_t>(scan)].load(std::memory_order_acquire));
}

uint32_t Version() { return g_version.load(std::memory_order_acquire); }

}  // namespace ue_wrap::scan_ledger
