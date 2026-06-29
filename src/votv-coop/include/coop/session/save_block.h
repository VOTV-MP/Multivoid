// coop/save_block.h -- client-side world-save block (host-only persistence).
//
// PR-FOUNDATION-2 (save-game safety) increment B, part 1: the HARD guarantee.
//
// Policy (user decision 2026-05-30, host-only persistence): during a coop
// session the HOST's save is the single canonical one; CLIENTS must not write
// the world save -- their pre-coop save is left UNTOUCHED, and coop-only mirror
// state (phantom props/NPCs) must never be serialized into a client's slot.
//
// Mechanism: a native MinHook detour on UGameplayStatics::SaveGameToSlot -- the
// single physical write chokepoint EVERY save path funnels through. We install
// it ONLY in the client process and cancel writes whose USaveGame object is the
// world-save container (saveSlot_C), letting the harmless meta/settings save
// (save_main_C: keybinds/achievements/store) through. This is the root-cause
// fix (RULE 1): one hook, total coverage, no per-trigger allowlist that could
// miss a path. The BP funnel saveSlot_C:saveToSlot is NOT hookable by our
// ProcessEvent interceptor (BP->BP dispatch goes through ProcessInternal); the
// engine-native write fn is. See research/findings/votv-save-path-RE-2026-05-30.md.
//
// Part 2 (the pause-menu "Save Game" button grey-out, the honest UX) is a
// separate increment -- this part is the under-the-hood guarantee that covers
// autosave/sleep/forced/direct-trigger saves the button grey-out cannot.
#pragma once

namespace coop::net { class Session; }

namespace coop::save_block {

// Install the SaveGameToSlot write-block. Idempotent; safe to call every tick
// from the install pump (net_pump::InstallObservers). NO-OP on the host (its
// save path stays byte-for-byte untouched -- the canonical save is the host's).
// On the client, resolves saveSlot_C + the SaveGameToSlot AOB and installs the
// detour once; retries on subsequent calls until saveSlot_C is loaded.
void Install(coop::net::Session* session);

}  // namespace coop::save_block
