// ui/server_browser_surface.h -- WHICH server browser this session uses, decided once.
//
// There are two of them. The NATIVE one (`ui/server_browser_native`) is a UMG screen built into
// the game's own menu switcher, and it is the permanent default. The ImGui one
// (`ui/server_browser`) is an overlay surface and STAYS as the fallback, an explicit RULE-2
// exception. `browser_native` in `multivoid.ini` chooses between them and needs a restart.
//
// WHY THIS FILE EXISTS. The choice had five independent deciders the moment the default
// flipped: the MULTIPLAYER button, and four recovery paths in `harness/session_runtime.cpp`
// that reopen the browser after a join or host fails. Every one of them named

#pragma once

namespace ui::server_browser_surface {

// True when the NATIVE screen is the browser for this run. Resolved once, on first call:
// `browser_native` is restart-scoped, and a per-call resolve would let two call sites
// disagree mid-session about which surface is "the" browser.
bool UseNative();

// Open whichever browser this session uses.
void Open();

// True while EITHER browser is showing. The question every caller actually has is "is a
// browser up" -- asking only about the incumbent is what let a second click on MULTIPLAYER
// re-issue Open() on an already-visible native screen.
bool IsOpen();

}  // namespace ui::server_browser_surface
