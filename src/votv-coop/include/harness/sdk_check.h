// harness/sdk_check.h -- SDK self-check on boot.
//
// Walks every class / UFunction / asset name in `ue_wrap::profile::name::`
// and verifies it resolves against the running VOTV build. Logs a clear
// "X/Y resolved, K critical failures" summary, plus per-failure detail.
//
// Purpose: when VOTV updates and silently renames/removes content, this
// gives an INSTANT report ("update broke npc_zombie_C / takeObj / ...")
// instead of waiting for features to fail mysteriously in hands-on play.
//
// Should be called AFTER gameplay has loaded -- many VOTV BP classes are
// content-cooked and load on first gameplay-level transition, not at the
// menu. The harness boot path calls Run() once gameplay is stable.

#pragma once

namespace harness::sdk_check {

// Run the full check. Logs a one-line summary at INFO level + per-failure
// detail at WARN/ERROR. Idempotent (safe to call multiple times across the
// session; each call re-resolves fresh).
//
// Resolution failures are bucketed by severity:
//   - CRITICAL: mod won't function at all (engine substrate classes)
//   - IMPORTANT: a major feature degrades (NPC suppression, prop sync)
//   - COSMETIC:  visual polish only (font, translucent materials)
//
// Returns the number of CRITICAL+IMPORTANT failures (0 = healthy).
int Run();

}  // namespace harness::sdk_check
