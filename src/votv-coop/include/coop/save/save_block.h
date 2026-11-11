// coop/save/save_block.h -- client-side world-save block. During a coop session the HOST's save is
// the single canonical one; CLIENTS must not write the world save -- their pre-coop save is left
// UNTOUCHED, and coop-only mirror state (phantom props and NPCs) is never serialized into a
// client's slot. The write block below is decided when a save fires: shut for a client in a
// running session and for as long as the mirror world that session built is still what is loaded,
// open otherwise -- so a player who has left plays single-player again and saves, and a host's save
// is always written. The cycle block is set on the mirror's gamemode and dies with that world.
// The WRITE block is a gate on the engine's save write (ue_wrap/engine/save_write_hook, the
// native every save path funnels through): it cancels a write of a world file, the container
// (saveSlot_C) or one of its companion saves; the keybinds and settings saves never reach it.
// The CYCLE block holds gamemode.disableSave true, which saveSlot_C::save tests at its head and
// returns on, before the world gather (saveObjects) and the write funnel. The gamemode's own
// triggers funnel through save(), and no bytecode in mainGamemode ever writes disableSave; a few
// callers elsewhere reach the engine's save write without it, which is what the write block is
// for.

#pragma once

namespace coop::net { class Session; }

namespace coop::save_block {

// Register the write-block's gate and make sure the engine hook is installed. Idempotent; safe to
// call every tick from the install pump (subsystems::Install). In every role: the hook is
// one per process and the gate decides per save, so a host's save passes and is reported to
// whoever listens for it.
void Install(coop::net::Session* session);

// Part 3 driver: hold gamemode.disableSave=true on the CLIENT of a running session (no-op on
// the host / with no session running). Cheap steady state (one liveness check + one masked-bit
// read); the gamemode re-walk after a world change is throttled to 2 s. Call
// per gameplay pump tick.
void Tick(coop::net::Session* session);

}  // namespace coop::save_block
