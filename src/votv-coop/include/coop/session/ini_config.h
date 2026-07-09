// coop/ini_config.h -- shared helpers for reading votv-coop.ini.
//
// Three responsibilities:
//   1) MASTER SWITCH. `[dev] enabled=0` forces all dev features OFF regardless
//      of their granular switches. Default (key absent OR =1) lets the granular
//      switches decide. One-line "ship lockdown" lever without rebuilding.
//   2) Generic key reader. Case/space tolerant `key=1`/`key=true` parser shared
//      by every subsystem that gates on the ini.
//   3) Foreground-window predicate. Hotkey threads must gate on this so a
//      same-machine host+client pair doesn't fire both instances when the user
//      presses a hotkey in one window (GetAsyncKeyState is process-global).
//
// No engine access -- pure file I/O on votv-coop.ini next to the DLL. Safe to
// call from any thread, but in practice every caller runs at Init() time.

#pragma once

namespace coop::ini_config {

// Returns false ONLY if votv-coop.ini contains `enabled=0` (or `enabled=false`)
// in the [dev] section -- the master kill-switch. Missing key or =1 returns true.
bool MasterEnabled();

// Read a `key=1` / `key=true` style line from votv-coop.ini. Case/space tolerant.
// Returns false if the file is missing, the key is absent, or the key is set
// to 0/false.
bool IsIniKeyTrue(const char* key);

// True ONLY when the current foreground window belongs to our process. Hotkey
// threads gate on this so a same-box host+client doesn't double-fire. Returns
// true if no foreground window query is possible (defensive default).
bool IsOurWindowForeground();

// Companion to IsOurWindowForeground for the SAME hotkey pollers: true while OUR
// overlay is capturing typed text (a text field is focused -- the chat input, a
// rebind box...). Every global-key poller (voice PTT/whisper/mute, freecam WASD,
// the spawn-menu Q) must ALSO gate on !IsOverlayCapturingText() so a keystroke
// meant for the text field never ALSO fires a game/voice bind. Born 2026-07-09:
// pressing T to chat then G activated voice, because the mic thread's global
// GetAsyncKeyState(G) poll is independent of ImGui eating the 'g' as text.
// Published once per frame by the overlay (io.WantTextInput || chat open); the
// backing flag is atomic, so any thread may read it. Defaults false (keys live)
// until the overlay first publishes.
void SetOverlayCapturingText(bool capturing);
bool IsOverlayCapturingText();

}  // namespace coop::ini_config
