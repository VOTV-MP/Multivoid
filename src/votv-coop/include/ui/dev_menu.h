// ui/dev_menu.h -- the in-game ImGui menu content (nested categories).
//
// One ImGui window: a left nav tree (Category > SubCategory) + a right content
// pane of the selected subcategory's controls. STRICT nested categorization
// (a category per concept, never a flat list). The menu is visible to ALL
// players (the overlay hosts it); DEV-flagged categories/subs/items are hidden
// unless the dev switch ([dev] devkeys) is on. Each feature module exposes a plain
// function the menu calls -- no gameplay/network logic lives here (principle 7).
//
// Rendered every frame by ui::imgui_overlay while the menu is visible.

#pragma once

namespace ui::dev_menu {

// Read the dev switch ([dev] devkeys) ONCE at boot. Call from harness boot (game
// thread) -- NOT from Render (which runs on the render thread; disk I/O there
// would stall a frame).
void Init();

// Build the menu window (called inside the overlay's ImGui frame, render thread).
void Render();

// The dev switch state ([dev] devkeys AND master enabled, latched by Init()) AND something to show:
// host authority or a local grant from the host. Lock-free (atomic loads and a boot-set bool). This is
// the menu's own visibility switch, NOT an authority gate: a client holding a local grant is true
// here too, so a host-only action asks coop::dev_gate::Allowed().
bool DevMode();

// Open the menu on one pane, named as the tree names it ("World", "Rules"). For the autonomous
// harness, which cannot click: the next Render selects it, if this peer may see it. Any thread.
void RequestSelect(const char* category, const char* sub);

// Whether the pane the render thread last DREW is this one, named as the tree names it in English. The
// readiness a drill waits on after RequestSelect: true once a frame drew that pane, its text looked up.
// Any thread.
bool DrawnSelection(const char* category, const char* sub);

}  // namespace ui::dev_menu
