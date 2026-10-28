// coop/config/config_registry_rows_dev_port.inc -- the [dev] rows of the version-port tooling (the fingerprint
// capture and census red arm, the dependency dump, the version gate's arms), included by
// config_registry_rows_dev.inc where they stood, so each keeps its place in every expansion of the list.
// The entry forms are config_registry_rows.inc's.

CFG_FLAG(fingerprint_capture, "fingerprint_capture", "dev", false, "VOTVCOOP_FINGERPRINT_CAPTURE",
    "Port tool: once gameplay is loaded, write every function of every loaded Blueprint class with its "
    "canonical bytecode text and fingerprint to multivoid-fingerprints-capture.txt beside the exe: the "
    "mod's half of the comparison with the text rendered from the cooked assets, and the native half of a "
    "build's reference table (docs/bytecode-fingerprint.md). About a second of work at world load; off by default.")
CFG_FLAG(dependency_dump, "dependency_dump", "dev", false, "VOTVCOOP_DEPENDENCY_DUMP",
    "Refactor proof: at the readiness milestones 'joined' (client) and 'peer-world-ready' (host), write every "
    "script_gate watch, ufunction_hook patch and game_thread observer and interceptor, sorted and counted, to "
    "multivoid-dependency-dump-<milestone>.txt beside the exe. Two builds' dumps diffed equal prove a change left "
    "the registered set as it was. Off by default.")
CFG_STRING(fingerprint_census_red, "fingerprint_census_red", "dev", "", "VOTVCOOP_FINGERPRINT_CENSUS_RED", false,
    "The fingerprint census's red arm: a row key ('class.function', lowercase) whose reference is falsified, so the "
    "census must report it 'body changed' and print 'fingerprint-census: RED ARM <row> reported body changed', or "
    "'RED ARM FAILED'. Empty: no red arm.")
CFG_STRING(game_build_force, "game_build_force", "dev", "", "VOTVCOOP_GAME_BUILD_FORCE", false,
    "The version gate's drill arm: run the mod as if the game were another build -- a row id of "
    "ue_wrap/facts/game_builds.inc (a row's id), or 'unknown' for a build no row names -- so the "
    "popup, the report, the stand-down's wording and the Join's refusal can be shown on a healthy install. "
    "Changes what the mod believes, never the game. Empty: the build as read.")
CFG_STRING(scan_ledger_red, "scan_ledger_red", "dev", "", "VOTVCOOP_SCAN_LEDGER_RED", false,
    "The scan ledger's red arm: a scan row's name (GUObjectArray, FNameToString, ProcessEvent, FMemoryRealloc, "
    "ScriptGate, SaveGameToSlot, OverlayDraw, OverlayResize, ActorEndPlay, VulkanPresent, VulkanPixelFormats) the compat report reads as missed, so "
    "it must name what that miss takes away and read DEGRADED; it prints 'scan-ledger: RED ARM <row> reported "
    "missed', or 'RED ARM FAILED'. The report only: the scan itself is untouched. Empty: no red arm.")
CFG_FLAG(game_build_dialog_continue, "game_build_dialog_continue", "dev", false,
    "VOTVCOOP_GAME_BUILD_DIALOG_CONTINUE",
    "The version popup's drill arm: the first frame it draws answers 'Continue anyway' through the button's own "
    "handler, so a run proves the draw and the remembered answer without a click, and a forced-build run is not "
    "held at a modal that takes the input. Off by default.")
