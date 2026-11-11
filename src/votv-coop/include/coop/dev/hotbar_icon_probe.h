// coop/dev/hotbar_icon_probe.h -- dev-only, read-only observability for the hotbar's
// icons at world load (ini hotbar_icon_probe=1 / env VOTVCOOP_HOTBAR_ICON_PROBE, off by default).
//
// It prints the bar's four inputs on change -- the slot widgets, the prop-icon name table, the
// texture array and the count of slots naming an item -- and, per slot, which texture is bound. A
// slot that was never touched, one drawn with no icon, and one drawn correctly are three
// different words rather than one. Alongside that it watches `uicomp_FLI_itemSlot_C::upd` at the
// script gate and prints the same inputs at the instant the GAME draws a slot, which is the
// only way to see inside a world load: the load is one long blocking call, so no tick of ours
// gets a sample while it runs. docs/ui.md has what this measured.
//
// Read-only by construction: field reads only, and never GetDynamicMaterial -- creating a
// material instance would destroy the evidence that no rebuild has touched a slot.

#pragma once

namespace coop::dev::hotbar_icon_probe {

// Poll the hotbar's readiness and its per-slot icon binding; log on change (no-op unless
// the flag is set). Game thread.
void Tick();

}  // namespace coop::dev::hotbar_icon_probe
