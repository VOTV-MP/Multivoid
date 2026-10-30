// coop/dev/pause_menu_probe_detail.h -- what the pause-menu probe's files share. A private header of
// coop/dev/pause_menu_probe*.cpp (src tree, not include/).

#pragma once

#include <cstdint>
#include <vector>

namespace coop::dev::pause_menu_probe::detail {

// One frame-time sample set per condition.
inline constexpr int kFrames = 300;

// The buttons of one row, in the rows measurement and in the bound-delegate measurement that walks them.
inline constexpr int kButtons = 4;

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

// The two-peer reading (M2): a window opened at Begin, one "pmprobe m2" line per whole second up to `maxSeconds`
// (wire packets in and out since the last, the world's paused state, and at the first second the hotkeys' state),
// closed by End with the window's rates. `who` names the peer's role in the lines.
namespace wire {
void   Begin();
double Elapsed();
void   Pump(const char* who, int maxSeconds);
void   End(const char* who);
}  // namespace wire

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

// The rows present, and the widgets of one: its four buttons (0..3) and its slider.
int   Count();
void* ButtonAt(int row, int button);
void* SliderAt(int row);

// Removes everything Begin added. Idempotent.
void Teardown();

}  // namespace rows

// The bound delegate (M5): a button's OnClicked and a slider's OnValueChanged bound to a function that a
// registered interceptor swallows, and the visibility of enterPause and resume. Game thread.
namespace bind {

enum class Step : uint8_t { Waiting, Done, Failed };

// Registers the observers, the gate watches and the candidate counters, and starts the counting window.
// False, with `why`, when a function the measurement stands on is not there.
bool Start(const char** why);

// The dispatch counters of enterPause and resume since the last report, logged and zeroed. `via` names how
// the cycle opened and closed the menu.
void ReportDispatch(int cycle, const char* via);

// Once the counting window is over and the rows exist: pick the silent candidates, bind every row, press
// each, log, restore. Done when everything is restored.
Step Advance(const char** why);

// Restores every delegate it changed and drops its interceptors, observers and watches. Idempotent.
void Release();

}  // namespace bind

}  // namespace coop::dev::pause_menu_probe::detail
