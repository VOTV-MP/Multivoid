// coop/dev/spawn_menu_unlock.h -- DEV: use the prop-spawn menu (Q) in STORY mode.
//
// Gameplay/dev layer (principle 7). VOTV's prop-spawn menu (the "Q" menu) is a
// sandbox-mode tool. By the BP code its OPEN path is NOT gamemode-gated (see
// research/findings/votv-spawnmenu-storymode-RE-2026-06-14.md); the restriction
// to sandbox lives in the input MAPPING (the Q->"spawnmenu" action binding), not
// in any enum check on the open. So this dev feature does not fight any gate or
// edit any asset (RULE 3) and does not flip GameMode (which drives unrelated
// systems): when ENABLED it watches the Q key itself and invokes the game's own
// spawnmenu-open UFunction (ue_wrap::spawn_menu::Open), reproducing a real Q press.
//
// DEV feature contract:
//   - OFF by default (boot force-on only via votv-coop.ini [dev] spawn_menu_unlock=1).
//   - CLIENT-LOCKED via coop::dev_gate (a joined client can never enable it / its
//     key watcher auto-disables the instant this peer becomes a client).
//   - Gated so it never affects normal play: nothing happens unless the toggle is
//     on; the menu's own activeInterface/isBuoyant guards still apply.

#pragma once

namespace coop::dev::spawn_menu_unlock {

// Boot hook (called from harness Start). No-op unless [dev] enabled!=0 AND
// [dev] spawn_menu_unlock=1, in which case it force-enables at boot. The F1 menu
// (Content > Props) toggles it interactively under [dev] devkeys regardless.
void Init();

// Enable/disable the Q-key watcher. Enabling is REFUSED (logged, no-op) on a
// client (coop::dev_gate). Idempotent. The key-watcher thread is started lazily
// on the first enable; while running it re-checks dev_gate every poll and
// auto-disables if this peer becomes a client.
void SetEnabled(bool on);
bool IsEnabled();

// One-shot: open the prop-spawn menu RIGHT NOW (the F1 menu "open now" button).
// Refused on a client. Posts the engine call to the game thread.
void OpenNow();

}  // namespace coop::dev::spawn_menu_unlock
