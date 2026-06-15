// coop/dev/heap_probe.h -- RULE-1 raw-heap leak attribution (dev-only).
//
// The companion to coop/dev/leak_probe (which censuses live UObjects). When the
// UObject census is FLAT but committed RAM still climbs, the leak is RAW HEAP
// (FString/FText/std::* containers) -- this tool names the OUR-MODULE call site
// responsible.
//
// Mechanism: MinHook-detour the CRT malloc/free/realloc/calloc. Our DLL links
// the STATIC CRT (/MT), so those functions are baked into votv-coop.dll and no
// other module calls them -- the detour therefore fires for EXACTLY our-module
// allocations (the engine has its own allocator). All our C++ heap
// (std::string/vector/unordered_map + operator new) bottoms out in this malloc.
// Each allocation's call chain inside our module is captured (a few frames -- the
// static CRT's operator-new/malloc wrappers are also in-module, so the first
// frames are CRT plumbing and the deeper ones the real caller); free/realloc
// decrement. Every ~4s a [heap_probe] block logs our total live CRT bytes + the
// top sites by live bytes, each as a chain of module-relative RVAs -- resolve via
// build/votv-coop/Release/votv-coop.map (the first non-CRT RVA in a chain is the
// owning call site).
//
// Discriminator:
//   our live climbs in lockstep with RSS  -> WE leak; the top growing RVA names it.
//   our live flat while RSS climbs         -> engine-side (GMalloc) leak; the CRT
//                                             probe has done its job (ruled us out)
//                                             -> escalate to a GMalloc vtable hook.
//
// Self-gated by the [dev] ini key `heap_probe=1` (read once). Installs its hooks
// lazily on the first armed Tick and reports on a ~4s throttle thereafter. Adds
// a stack-walk to every process-wide CRT malloc while armed -> OFF for real play.

#pragma once

namespace coop::dev::heap_probe {

// Drive from a per-tick context (mirrors leak_probe::Tick). First armed call
// installs the CRT detours; subsequent calls emit a throttled growth report.
void Tick();

}  // namespace coop::dev::heap_probe
