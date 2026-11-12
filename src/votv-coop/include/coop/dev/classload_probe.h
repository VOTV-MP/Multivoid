// coop/dev/classload_probe.h -- [dev] what it takes to load, on demand, a class this client has not loaded.
//
// The receive-side class lookups find an already-loaded UClass, so a host's spawn of a class this
// client never touched is dropped. This probe measures the three facts a loader through the engine's
// asset registry would stand on, for five classes asserted absent first:
//   M1 the registry is reachable by reflection and, asked by asset name, returns the object path;
//   M2 the cost on the game thread of loading that path, cold and then warm;
//   M3 whether the object index finds the class at once after the load.
// It reads the registry's own function lists too, so a name that does not resolve is read from the log.
//
// Armed by classload_probe=1 (VOTVCOOP_CLASSLOAD_PROBE). Runs ONCE, on a client, the first frame its
// world is settled; the loads are synchronous and may hitch that frame. Game thread.
#pragma once

namespace coop::dev::classload_probe {

// The frame tail's entry (harness::pump::TickFrameTail): one latched flag read when off. Game thread.
void Tick();

}  // namespace coop::dev::classload_probe
