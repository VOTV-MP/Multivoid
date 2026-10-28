// ue_wrap/core/scan_ledger.h -- what the mod finds in the game's code by searching it, and whether it was
// found: one row per REQUIREMENT, not per signature -- the overlay needs the draw seam of the RHI the game runs,
// so the three present signatures are one row (the exe holds all three RHIs; one fires). The AOB signatures
// of sdk_profile.h, the script loop's inline one and its code walk, and AActor::EndPlay's code walk are all here.
//
// Each site records its outcome once, where it decides today; nothing here searches, and nothing here says
// what a miss costs -- that is the report writer's (harness/compat_boot), so the engine layer holds no
// feature policy (principle 7). The report lists every row: found, missed, or not tried yet, so a miss is
// never only a log line (docs/versioning.md, "Which game is running"). Not every row is tried in every session:
// the save hook is armed with a multiplayer session's subsystems, so a solo world never tries it.
//
// Any thread, lock-free and log-free: the loader's call records too (actor_end_play and script_gate patch
// there), before the boot thread exists.

#pragma once

#include <cstdint>

namespace ue_wrap::scan_ledger {

enum class Scan : uint8_t {
    GUObjectArray,   // reflection::Resolve
    FNameToString,   // reflection::Resolve
    ProcessEvent,    // reflection::Resolve
    FMemoryRealloc,  // ResolveEngineHeap
    ScriptGate,      // script_gate::Patch: GNatives' dispatch site, then the VM loop found by code from it
    SaveGameToSlot,  // save_to_slot_hook::Install
    OverlayDraw,     // imgui_overlay::Init: any of the three present seams resolved; missed again at the first
                     // world milestone when none has fired
    OverlayResize,   // imgui_overlay::Init: D3D11 Resize or D3D12 ResizeInternal
    ActorEndPlay,    // actor_end_play::Patch, by code
    VulkanPresent,   // vulkan_rhi::ResolvePresent, at Init on every run: FVulkanViewport::Present, by a stretch of its body
    VulkanPixelFormats,  // vulkan_rhi::ResolvePixelFormatTable, at Init on every run: GPixelFormats, by the
                         // swapchain constructor's read of it
    kCount
};

enum class State : uint8_t { NotTried, Found, Missed };

// What is searched for, for the report: "GUObjectArray", "the overlay's draw seam (...)".
const char* Name(Scan scan);

// The site's outcome. A later record replaces an earlier one.
void Record(Scan scan, bool found);

State StateOf(Scan scan);

// Moves whenever a record changes a row's state: the report's writer rewrites its section when it moved.
uint32_t Version();

}  // namespace ue_wrap::scan_ledger
