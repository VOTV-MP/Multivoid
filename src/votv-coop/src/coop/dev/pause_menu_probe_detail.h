// coop/dev/pause_menu_probe_detail.h -- what the pause-menu probe's files share. A private header of
// coop/dev/pause_menu_probe*.cpp (src tree, not include/).

#pragma once

#include <cstdint>
#include <vector>

namespace coop::dev::pause_menu_probe::detail {

// One frame-time sample set per condition.
inline constexpr int kFrames = 300;

// A widget's identity: its address and the serial of its object-array slot, which a rebuilt widget at a
// recycled address would not share.
struct Ident {
    void*   p      = nullptr;
    int32_t idx    = -1;
    int32_t serial = 0;
};
Ident IdentOf(void* widget);

// The widget tree under `root` to `maxDepth`, one "pmprobe <tag>" line per widget (class, name, visibility,
// slot class); a nested user widget is a leaf. Every widget reached is appended to `ids` when given.
void DumpTree(const char* tag, void* root, int maxDepth, std::vector<Ident>* ids);

// The rows measurement (M3), inside the open menu. Game thread. The pointers it holds live under the scroll
// box it added to `screen`, which Teardown takes out again.
namespace rows {

enum class Step : uint8_t { Running, Done, Failed };

// Adds the scroll box to `screen`. False, with `why`, when a widget cannot be made.
bool Begin(void* menu, void* screen, const char** why);

// One pump tick of M3: 12 rows then 100 rows (build time, 60 rebinds), then the settings row class. Done
// leaves the 100 rows in place.
Step Advance(const char** why);

// The scroll box, and its scrollable travel (0 until the rows are laid out).
void* Scroll();

// Removes everything Begin added. Idempotent.
void Teardown();

}  // namespace rows

}  // namespace coop::dev::pause_menu_probe::detail
