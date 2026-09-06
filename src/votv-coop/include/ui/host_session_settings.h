// ui/host_session_settings.h -- STEP 2 OF HOSTING: who is allowed to join this session.
//
// THE FLOW, and why it is a second window rather than more rows on the first. Browser ->
// "Host game" -> the hosting window (WHICH WORLD, HOW TO CONNECT) -> "Next" -> THIS (WHO MAY
// JOIN) -> "Host". The session's admission settings are therefore all chosen BEFORE the
// session exists.
//
// WHY THEY ARE FIXED FOR THE SESSION'S LIFE. An earlier plan made the lock EDITABLE
// MID-SESSION, which meant the master had to learn about a change after the announce --
// `/v1/heartbeat` carries `players_cur` and `listed` and has never carried `locked`, so it
// needed a new field, a new client setter, and a window where the browser shows a lock the host
// has already removed. Settling it before the announce means the value the master is told is
// the value the session has, for the session's whole life; re-hosting is how you change it, and
// the mid-session plumbing was reverted rather than left dark (RULE 2).

#pragma once

#include "coop/session/session_manager.h"

#include <string>

namespace ui::host_session_settings {

// Hand the hosting window's choices forward and show this screen. Safe from any thread:
// it records the intent and the next main-menu tick performs it (the switcher is driven
// through ProcessEvent).
//
// Everything needed to COMPLETE the host action travels in this call rather than being
// re-derived here -- there is one place that reads the save list and the connection rows,
// and it is the window the player just used.
void Open(const coop::session_manager::SaveChoice& choice, const std::string& serverName,
          int connMode);

bool IsOpen();

// ---- read-only seams, for the self-check to aim at and assert on ----------------------
//
// They exist because the two things this window is FOR had no observer outside the pixels:
// whether the lock row can be clicked at all (it is a hand-built `UImage`, a construct measured
// un-hoverable by Slate, and a control of this kind has already shipped dead twice in this
// tree), and whether pressing it actually MINTS something.
//
// `Back` is here for the same reason it is on the hosting window: with no X on any of these
// screens, Back and ESC are the only ways out, so the one a pointer can reach owes a driven
// measurement rather than an assumption.
void* LockRow();       // the "Password required" row's hit target
void* BackButton();

bool Locked();

// THE LENGTH, NEVER THE VALUE. This is the one secret in the window, and a self-check that
// could read it is a self-check that can log it -- which is how a password ends up in a
// screenshot attached to a bug report. A length proves a password was minted; nothing that
// needs the characters lives outside this module.
int PasswordLength();

// What a freshly MINTED password is expected to measure, so a self-check can assert the mint
// without hard-coding a number the generator is free to change under it. A literal that has
// gone stale fails a working feature, and in the log that is indistinguishable from the defect
// the check was written to catch.
int GeneratedPasswordLength();

// Driven from the main-menu tick observer, beside the browser's and the hosting window's.
void OnMenuTick(void* menu, void* switcher);

}  // namespace ui::host_session_settings
