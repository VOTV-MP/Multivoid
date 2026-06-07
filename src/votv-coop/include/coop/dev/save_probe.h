// coop/dev/save_probe.h -- TEST-ONLY native-save-browser verification probe.
//
// Proves ue_wrap::save_browser works against the running VOTV build BEFORE the ImGui
// Host-Game save picker is layered on top: a few seconds after boot, drive
// EnumerateSaves at the menu and log the harvested save list + metadata (so we can
// confirm the native loadSlots harvest resolves headless). If VOTVCOOP_TEST_SAVE_CREATE
// is set, also CreateNamedSave a story save then re-enumerate (proves create+persist +
// that the new save appears in the list).
//
// Gated by env VOTVCOOP_TEST_SAVE_ENUM=1; never ships on (RULE 3 dev-only). No-op
// otherwise.
#pragma once

namespace coop::dev::save_probe {
void Init();
}  // namespace coop::dev::save_probe
