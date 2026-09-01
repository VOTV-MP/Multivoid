// ui/host_session_settings.h -- STEP 2 OF HOSTING: who is allowed to join this session.
//
// THE FLOW, and why it is a second window rather than more rows on the first.
// Browser -> "Host game" -> the hosting window (WHICH WORLD, HOW TO CONNECT) -> "Next" ->
// THIS (WHO MAY JOIN) -> "Host". The user asked for exactly that shape on 2026-08-31:
// "когда он выбрал сейв или new game, далее юзеру покажем еще одно окно, где уже будет
// настройка сессии, пароль замок и тд ... и только после этого он может уже хост кнопку
// нажать."
//
// WHAT THIS REPLACED, and the replacement is the better design. The first plan made the
// lock EDITABLE MID-SESSION, which meant the master had to learn about a change after the
// announce -- `/v1/heartbeat` carries `players_cur` and `listed` and has never carried
// `locked` (`master.rs:518-535`), so it needed a new field, a new client setter, and a
// window where the browser shows a lock the host has already removed. The user cut the
// requirement instead: "может раз создает хост сессию и всё, с этим и живёт? А надо пусть
// пересоздает." Settling it BEFORE the announce means the value the master is told is the
// value the session has, for the session's whole life. The plumbing that had been built
// for the mid-session version was reverted rather than left dark (RULE 2).
//
// THE LOCK GENERATES. Choosing "Password" mints one immediately and shows it -- "если жмет
// на замок то пароль сразу появляется сгенерированный". It stays editable, so a host who
// wants something they can say out loud can replace it; the generated value is a safe
// DEFAULT, not the mechanism. (What makes a chosen, memorable password safe is binding the
// proof to the host's identity so it can never become an offline oracle -- see the design
// pass in the scratchpad qf thread. That is the NEXT commit; this one is the surface.)
//
// STATUS, updated 2026-09-01: the ADMISSION half IS built. A joining client proves it knows
// the password and the host refuses it if not (`coop/net/peer_admission.cpp`, proto 149), so
// `locked` is a gate now rather than the badge this paragraph used to warn it was. The
// caveat that replaces it is narrower and lives on the password hint: a proof is only safe
// to send to a host the joiner has BOUND to an identity it was given in advance, so on
// DIRECT and LAN a friend needs the host's `gen:` line as well as the address.
//
// SECOND QUESTION, added 2026-09-01 (user): this screen now also collects whether the
// session is LISTED, which until then was a hardcoded `false` for "hide" -- i.e. no choice
// at all on the native surface, while the ImGui fallback had one. See `kVis` in the .cpp for
// why the rows state the truth on all three connection modes but only DIRECT can move them.
//
// Game thread only, like every native screen.

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

void Close();
bool IsOpen();

// ---- read-only seams, for the self-check to aim at and assert on ----------------------
//
// They exist because the two things this window is FOR had no observer outside the pixels:
// whether the lock row can be clicked at all (it is a hand-built `UImage`, the construct
// measured un-hoverable by Slate on 2026-08-29 -- a control of this kind has already
// shipped dead twice in this tree), and whether pressing it actually MINTS something.
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

// What a freshly MINTED password is expected to measure, so a self-check can assert the
// mint without hard-coding a number that the generator is free to change under it. The
// LOCK phase used to say `len >= 8`; the user shortened the mint to six on 2026-09-01 and
// that literal would have failed a working feature -- a test asserting a stale constant is
// indistinguishable, in the log, from the defect it was written to catch.
int GeneratedPasswordLength();

// Driven from the main-menu tick observer, beside the browser's and the hosting window's.
void OnMenuTick(void* menu, void* switcher);

}  // namespace ui::host_session_settings
