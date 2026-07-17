// ue_wrap/begin_equipment.h -- give the LOCAL player a New-Game equipment item via the game's OWN
// equip path. Wraps the exact two UFunctions the gamemode's begin-equipment loop chains
// (ExecuteUbergraph_mainGamemode): <prop>.getData(Fstruct_save&) -> mainGamemode_C.AddEquipment(
// Fstruct_save, bool&). Doing it through AddEquipment means canonical placement (the game decides
// inventory vs worn slot) + a FRESH engine-assigned item key -- no hand-built record, no host
// dependence. Principle-7 engine-wrapper layer: pure UFunction thunks, no coop/gameplay state.

#pragma once

#include <string>

namespace ue_wrap::begin_equipment {

// Give the local player one item of `className` (e.g. L"prop_equipment_flashlight_C") via
// getData() -> AddEquipment(). Returns AddEquipment's bool result; false on any reflection/call
// failure (logged). Game thread (ProcessEvent dispatch + actor spawn).
bool GiveFromClass(const std::wstring& className);

}  // namespace ue_wrap::begin_equipment
