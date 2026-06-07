// harness/harness_diag.h -- autonomous-scenario diagnostic dumps.
//
// Extracted from harness.cpp (2026-06-06, modular file-size audit: the
// DriveHostBootIfPending host-with-save orchestration pushed harness.cpp past the
// 800-LOC soft cap). These are pure reflection-based logging helpers used by the
// autonomous TimelineThread scenarios (paramdump / skin / show / the intro widget
// sampling) -- NO g_session / coop-session-lifecycle coupling, so they lift cleanly out.
// Game thread only (they read engine state via reflection). NOT shipped behaviour --
// diagnostics for the autonomous test scenarios.

#pragma once

namespace harness::diag {

// Log a UFunction's parameter frame (names/offsets/sizes/flags) to verify FProperty
// offsets against the known signature before relying on them for marshaling.
void DumpParams(const wchar_t* className, const wchar_t* funcName);

// Log NumObjects + the current world name -- a "where are we" marker.
void Report(const char* label);

// Log an actor's default subobjects (its components).
void DumpComponents(const char* label, void* actor);

// Log every live UUserWidget instance (name + class); flag intro/gate candidates and
// dump their UFunctions (used to find the OMEGA Proceed handler).
void DumpLiveWidgets();

}  // namespace harness::diag
