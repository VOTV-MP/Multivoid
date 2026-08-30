// bootstrap/refuse_dialog.h -- the modal we show when the mod will NOT run.
//
// Deliberately NOT `ui::boot_warning_dialog`. That one renders from our ImGui
// overlay, and every caller here is a case where the overlay must never be
// installed: a duplicate instance that is standing down, or a boot whose SDK
// health check failed (wrong offsets -> our own drawing hooks are suspect).
// A plain Win32 modal is the only surface guaranteed to exist at that point.
//
// Extracted 2026-08-30 from loader/cppmod_entry.cpp, which had the only copy.
// It grew a second caller (bootstrap/boot.cpp, the health-check refusal) and
// two copies of one concept is RULE 2; `loader::cppmod` was also the wrong
// owner, since boot sits BELOW the loader lane and must not depend on it.

#pragma once

#include <string>

namespace bootstrap {

// Show `body` in a topmost modal titled `title`, on its own detached thread.
//
// Non-blocking BY CONTRACT, for two independent reasons: `start_mod` must
// return promptly (no UE4SS thread may block on a modal), and a boot thread
// that blocks here would hold whatever the caller is holding. The caller must
// therefore treat this as fire-and-forget -- there is no dismissal result,
// because no caller has a decision left to make by the time it is shown.
//
// Callers must have PINNED the module first (`GET_MODULE_HANDLE_EX_FLAG_PIN`,
// taken at the top of `start_mod`): the thread outlives the call, so its code
// must not be unloadable under it.
//
// Logs "refuse dialog up" / "refuse dialog dismissed" and flushes around the
// modal -- that pair is how the headless drills prove the dialog REALLY
// showed; probing for the window from outside proved unreliable.
void ShowRefuseDialog(const std::wstring& title, const std::wstring& body);

}  // namespace bootstrap
