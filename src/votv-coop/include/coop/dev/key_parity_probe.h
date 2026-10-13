// coop/dev/key_parity_probe.h -- [dev] the prop key index against the live world. Every 2 s (and on the
// tick a world changes) a walk of the object array takes the live keyed props of the current world as they
// are and counts, for coop/props/prop_key_index (the lookup column through Find, the index column through
// Resolve, with Pending apart and its entries checked the other way), for the tracker's tracked keys, and
// for a shadow fed only by script-gate POST watches on init, setKey, getKey and loadData, the props each
// lacks or holds under another key, naming the ones that persist, with a verdict per report. Once per
// world it also checks AnotherHolder on one live key with a single holder: asked to leave that holder out
// it must Miss, asked to leave nobody out it must Hit with that holder.
// Armed by key_parity_probe=1; it holds the gate for the process so a load outside a session is seen too;
// read-only otherwise (it assigns slot serials). Game thread.
#pragma once

namespace coop::dev::key_parity_probe {

// The frame tail's entry (harness::pump::TickFrameTail): one latched flag read when off.
void Tick();

}  // namespace coop::dev::key_parity_probe
