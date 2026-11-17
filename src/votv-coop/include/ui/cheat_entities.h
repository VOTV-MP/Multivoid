// ui/cheat_entities.h -- the Entities pane's entries a developer tool adds. The product pane (ui/cheat_entities_pane)
// lists the creatures the host can spawn and names no drill module; a dev tool that wants a button there registers it
// from its own install, while its dev row is on.

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace ui::cheat_entities {

// Add "Spawn <label> in front of me" to the Entities pane, which runs `spawn` on click. The label is shown as given (a
// developer entry is not translated). Idempotent for the same label. Game thread; the pane reads it on the render thread.
void AddDevEntry(const char* label, void (*spawn)());

namespace detail {
// The entries added so far, copied out for the pane. Any thread.
std::vector<std::pair<std::string, void (*)()>> DevEntries();
}  // namespace detail

}  // namespace ui::cheat_entities
