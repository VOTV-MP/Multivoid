// coop/save/save_block.h -- client-side world-save block. During a coop session the HOST's save is
// the single canonical one; CLIENTS must not write the world save -- their pre-coop save is left
// UNTOUCHED, and coop-only mirror state (phantom props and NPCs) is never serialized into a
// client's slot.
//
// The WRITE block is a MinHook detour on UGameplayStatics::SaveGameToSlot, the one physical
// chokepoint every save path funnels through: it cancels writes whose USaveGame object is the
// world-save container (saveSlot_C) and lets the harmless meta save (save_main_C) through. The BP
// funnel saveSlot_C::saveToSlot is out of reach of our ProcessEvent interceptor -- BP-to-BP
// dispatch goes through ProcessInternal -- but the engine-native write function is reachable.
//
// The CYCLE block holds gamemode.disableSave true, which saveSlot_C::save tests at its head and
// returns on, before the world gather (saveObjects) and the write funnel. Every gamemode trigger --
// autosave, sleep, menu, quicksave -- funnels through save(), and no bytecode in mainGamemode ever
// writes disableSave.

#pragma once

namespace coop::net { class Session; }

namespace coop::save_block {

// Install the SaveGameToSlot write-block. Idempotent; safe to call every tick
// from the install pump (net_pump::InstallObservers). NO-OP on the host (its
// save path stays byte-for-byte untouched -- the canonical save is the host's).
// On the client, resolves saveSlot_C + the SaveGameToSlot AOB and installs the
// detour once; retries on subsequent calls until saveSlot_C is loaded.
void Install(coop::net::Session* session);

// Part 3 driver: hold gamemode.disableSave=true on the CLIENT (no-op on the
// host / no session). Cheap steady state (one liveness check + one masked-bit
// read); the gamemode re-walk after a world change is throttled to 2 s. Call
// per gameplay pump tick.
void Tick(coop::net::Session* session);

}  // namespace coop::save_block
