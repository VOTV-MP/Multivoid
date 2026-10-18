// ue_wrap/devices/laptop_widget.h -- the laptop interface widget's tab switcher: the process's one ui_laptop_C
// (the base laptop and every portable PC render this one widget), the active tab index it shows, the
// number of tabs, and a write of the index and nothing else. Principle-7 engine-wrapper layer: no network,
// no coop state.
//
// The write is the engine's UWidgetSwitcher::SetActiveWidgetIndex on the widget's `switcher`. It is not the
// Blueprint's switcherSwitched(Index), which also runs createRC (it binds the viewer's keys to a remote
// drone or kerfur) or leaveRC: a peer that only shows a tab must not run either. Resolved lazily from the
// live objects: the gamemode's `laptop` field and the widget's `switcher` field by name, kept per class
// object. Game thread only.

#pragma once

namespace ue_wrap::laptop_widget {

// The widget this world holds (mainGamemode_C::laptop, built once at the gamemode's BeginPlay), or null
// before that, after the world is gone, or when the field does not resolve. Live or null; no walk.
void* Widget();

// The switcher's active tab index. False when the widget has no switcher, the engine call did not run or
// no tab is active (the switcher answers -1); `index` is left untouched then.
bool ReadTab(void* widget, int& index);

// The number of tabs (the switcher's child count). False when the switcher or the call did not resolve;
// `n` is left untouched then.
bool TabCount(void* widget, int& n);

// Show tab `index`: SetActiveWidgetIndex on the widget's switcher, nothing else. `index` is the caller's to
// bound against TabCount. False when the switcher did not resolve, `index` is negative or the call did not run.
bool WriteTab(void* widget, int index);

}  // namespace ue_wrap::laptop_widget
