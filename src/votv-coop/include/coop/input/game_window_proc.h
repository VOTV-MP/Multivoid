// coop/input/game_window_proc.h -- the ONE subclass of the game window's procedure, and the
// fixed order its messages travel. Two modules used to subclass the same window on their own
// schedules: the shutdown handler from the boot pump, and the ImGui overlay from the first
// present. The overlay's hook therefore never existed under an RHI whose present the overlay does
// not wrap (the Vulkan one), so no key reached a native text field, and a window recreated by the
// engine cut whichever subclass sat underneath out of the chain. Now one function owns the
// window, follows it when the engine recreates it, and calls, in this order:
//   1. ui::imgui_overlay::HandleWindowMessage (hotkeys, ImGui input, native text fields);
//   2. coop::shutdown::OnWindowMessage (the user-close signals), then the engine's procedure;
//   3. coop::shutdown::OnWindowMessageForwarded (the end-session veto, once the engine answered).
// The consumers stay where they are; the order is code here, not a registration.

#pragma once

namespace coop::input::game_window_proc {

// Find the game window and subclass it; follow it when the engine destroys and recreates it.
// Called once from harness::Start so the menu is covered from the first frame, then from every frame
// tail of the pump (the play loop posts it at the main menu too). Game thread afterwards: one
// IsWindow call while a window is held, plus one enumeration a second to follow a larger visible
// window that appeared; an enumeration at most every 250 ms while none is.
void Tick();

// The subclassed window, or null before one was found (an HWND, as void* so the header stays
// free of the Windows headers).
void* Hwnd();

}  // namespace coop::input::game_window_proc
