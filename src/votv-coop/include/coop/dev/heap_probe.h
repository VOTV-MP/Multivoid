// coop/dev/heap_probe.h -- raw-heap leak attribution (dev only).
//
// The companion to coop/dev/leak_probe, which censuses live UObjects: when that census is FLAT and
// committed RAM still climbs, the leak is RAW HEAP -- FString, FText, std:: containers. Live bytes
// climbing in lockstep with RSS means WE leak, and the top growing site names it; flat live bytes
// under a climbing RSS rule us out and point at the engine's GMalloc.
//
// It MinHook-detours the CRT malloc, free, realloc and calloc. Our DLL links the static CRT, so
// those are baked into main.dll and no other module calls them: the detour fires for exactly our
// allocations. Each one's in-module call chain is captured, and every ~4 s a [heap_probe] block
// logs our live CRT bytes and the top sites, as chains of module-relative RVAs to resolve against
// build/votv-coop/Release/main.map -- the first non-CRT RVA in a chain is the owning call site.
// Gated by the [dev] ini key `heap_probe=1`, read once; the hooks install on the first armed Tick,
// and the stack walk they add to every CRT malloc keeps this OFF for play.

#pragma once

namespace coop::dev::heap_probe {

// Drive from a per-tick context (mirrors leak_probe::Tick). First armed call
// installs the CRT detours; subsequent calls emit a throttled growth report.
void Tick();

}  // namespace coop::dev::heap_probe
