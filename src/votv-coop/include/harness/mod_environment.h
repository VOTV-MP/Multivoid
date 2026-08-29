// harness/mod_environment.h -- census the OTHER mods sharing this process, and
// warn the player when the ones present are known to cost frames.
//
// WHY THIS EXISTS (measured 2026-08-29). A player reported "60 fps, my PC does
// 120" and a whole session went into instrumenting Multivoid for a cost that was
// never ours. The bisect, same save / same DLL / same windowed launch:
//
//     dev rig as found ................................ ~75 fps
//     - DebugMod.pak (Content/Paks/LogicMods) ......... ~89 fps
//     - UE4SS's bundled Lua mods (Mods/mods.txt) ..... ~119 fps
//     Multivoid loaded + hosting + its own paks ...... ~119 fps  (i.e. ~0 cost)
//
// The same machine ran the SAME Multivoid build at a stable 120 through r2modman,
// whose profile simply has no `mods.txt`. So the frames go to UE4SS's own shipped
// Lua mods and to the Blueprint mods `BPModLoaderMod` loads out of `LogicMods` --
// and a player has no way to know that, because nothing in the game or the loader
// says it. This check is that missing sentence.
//
// HONESTY BOUND, and it constrains the wording: what was measured is the COST OF
// THE SET, not a per-mod cost. `BPModLoaderMod` is the loader for every BP pak, so
// its cost and the paks' cost are not separable by the measurement that was run --
// removing Multivoid's own two paks appeared to buy 5 fps while BPModLoader was on
// and bought exactly nothing once it was off. So this module NAMES what is present
// and reports the measured SET figure; it must never attribute a number to an
// individual mod it never measured alone.
//
// Detection is pure file-system reading next to the exe -- no engine calls, no
// reflection -- so it is safe from the boot thread before any world exists.

#pragma once

namespace harness::mod_environment {

// Census the loader environment, log it, and arm the boot warning modal when
// something known-costly is present. Idempotent; safe to call once at boot.
// No-op when `warn.perf_mods` is false.
void Run();

}  // namespace harness::mod_environment
