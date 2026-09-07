// coop/dev/spawn_menu_unlock.h -- DEV: use the prop-spawn menu (Q) in STORY mode.
//
// Gameplay/dev layer (principle 7). The prop-spawn menu is a sandbox tool, restricted two ways:
// the Q key is bound to the spawnmenu action only in sandbox, and the game's own open block is
// gated on lib_C::isBuoyant, which outside flight answers hasWeapon -- a flag the gamemode sets
// from the mode itself. So there is no enum check on the open, but it is gamemode-gated all the
// same. This feature fights neither: when ENABLED it watches the Q key itself and calls
// ue_wrap::spawn_menu::Open, which reproduces a Q press's effects on the game's own widget
// without driving that block. No asset edit (RULE 3) and no GameMode flip, which would reach
// unrelated systems.

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
