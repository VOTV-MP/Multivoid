// harness/screenshot.h -- in-mod screenshot hotkey (dev/testing aid).
//
// Captures the game window to a PNG via Windows GDI (PrintWindow), with NO
// in-game toast and NO focus theft -- unlike the engine's HighResShot, which
// pops a "screenshot saved" notification that distracts a human tester. Saves
// to a coop-screenshots/ folder next to the mod DLL (the game's Win64 dir).
//
// This is tooling, not engine-wrap or coop state (principle 7): it touches only
// Win32/GDI, never the engine.

#pragma once

namespace harness::screenshot {

// Start a background thread that watches for the F12 key and, on each press,
// saves a screenshot. Idempotent. Runs for the process lifetime.
void StartHotkeyWatcher();

}  // namespace harness::screenshot
