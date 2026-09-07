// coop/dev/native_ui_probe.h -- the measurements a native (UMG) server browser
// needs before it can be built: whether each UMG UFunction it needs resolves on
// its OWNING class (FindFunction has no super-walk, so AddChild is UPanelWidget's
// and resolving ScrollBox alone buys nothing), whether a style donor's unreflected
// FSlateResourceHandle is populated -- InjectCanvasButton memcpys a whole
// FButtonStyle, and a live handle would alias a refcount with no AddRef -- which
// donors exist at all, the layout of UButton::OnClicked read off the game's own
// bound button, and whether a hand-wired, never-Initialize()d UUserWidget renders
// inside a UWidgetSwitcher.
//
// It samples at MENU time, not at boot: every donor is a live-menu widget, so at
// boot they all read null, and that null cannot be told apart from "present and
// empty". The probe rides the same Tick anchor the button inject does and stamps
// which menu instance it sampled. Frames presented per world kind, the other half
// of the question, is counted separately in coop/dev/worldless_frames.h.

#pragma once

namespace coop::dev::native_ui_probe {

// Arm off the config rows and register the ui_menu_C::Tick post-observer (bounded
// retry -- the menu BP may not be loaded when this runs at boot). Idempotent; a no-op
// when disarmed. Call from the harness dev-init block.
//
// TWO GATES, ON PURPOSE. `[dev] native_ui_probe=1` arms the reads and touches
// nothing. `[dev] native_ui_probe_write=1` additionally arms the write rung, which
// AddChilds into the live shipped `switcher_widgets` and moves ActiveWidgetIndex.
// It reads the prior index back and restores it only if the index is still ours --
// the player may have navigated -- removes the throwaway, and is bounded by a
// deadline rather than by anything the player must do: at our index ESC is a no-op
// and a throwaway has no button_back, so a probe that outlived its deadline could
// strand the player in a menu with no way out.
void Init();
}  // namespace coop::dev::native_ui_probe
