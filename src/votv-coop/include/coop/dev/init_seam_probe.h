// coop/dev/init_seam_probe.h -- [dev] what a script-gate watch on every Init body would see, beside
// the ProcessEvent observers prop_lifecycle keeps on each keyed class's own Init. For each Init body
// that runs on a keyed interactable it records the body's owning class, the caller's function (none
// when ProcessEvent dispatched it, the route those observers see), whether it runs inside another
// Init of the same object (a super call's body, which returns before its caller's has finished) and
// whether the object's key is still unset when the body returns. Counts are logged every 30 s and at
// each world change, the first 40 calls in full. Armed by init_seam_probe=1; it holds the
// gate for the process so a load outside a session is seen too; read-only otherwise. Game thread.
#pragma once

namespace coop::dev::init_seam_probe {

// The frame tail's entry (harness::pump::TickFrameTail): one latched flag read when off.
void Tick();

}  // namespace coop::dev::init_seam_probe
