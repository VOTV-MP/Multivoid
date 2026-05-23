// dev/common.h -- shared helpers for the dev tools (freecam, pos_hud, ...).
//
// Two responsibilities:
//   1) MASTER SWITCH. votv-coop.ini's `[dev] enabled=0` forces ALL dev features
//      OFF regardless of their granular switches (freecam=..., posinfo=...).
//      Default (key absent OR =1) lets the granular switches decide. This is the
//      "ship lockdown" lever -- one line in the ini disables every dev tool at
//      once for a shipping build, without rebuilding.
//   2) Dedup the per-module ini-line parser. freecam + pos_hud had identical
//      copies of the same case/space-tolerant reader (RULE 2 violation creeping
//      in). One helper here, both modules call it.
//
// No engine access -- pure file I/O on votv-coop.ini next to the DLL. Safe to
// call from any thread, but in practice every caller runs at Init() time.

#pragma once

namespace dev {

// Returns false ONLY if votv-coop.ini contains `enabled=0` (or `enabled=false`)
// in the [dev] section -- the master kill-switch. Missing key or =1 returns true
// (the default; granular switches decide). Each dev module's Init() should call
// this BEFORE its own IsEnabled() check.
bool MasterEnabled();

// Read a `key=1` / `key=true` style line from votv-coop.ini. Case/space tolerant.
// Returns false if the file is missing, the key is absent, or the key is set
// to 0/false. The shared replacement for freecam::ReadIniEnabled / pos_hud::
// ReadIniEnabled (RULE 2 dedup).
bool IsIniKeyTrue(const char* key);

// True ONLY when the current foreground window belongs to our process. Dev
// hotkey threads (freecam HOME, pos_hud F2) must gate on this -- otherwise
// when the user runs TWO instances of the game on one machine (host + client),
// pressing F2 in the client's window fires the hotkey in BOTH processes (the
// underlying GetAsyncKeyState is GLOBAL, not foreground-only). The fix is
// per-process: only react when our window is focused. Returns true if no
// foreground window query is possible (defensive default = don't break the
// hotkey if the foreground API errors).
bool IsOurWindowForeground();

}  // namespace dev
