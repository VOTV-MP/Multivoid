// ue_wrap/vitals.h -- local-player vitals scalar accessor (engine substrate).
//
// Resolves the canonical live vitals store -- UmainGameInstance_C::save_gameInst, a UsaveSlot_C*
// and exactly ONE per machine -- and reads and writes the player's vital scalars by reflected
// field offset. Blueprint-cooked offsets shift across recooks, so every offset is resolved by
// NAME and never hardcoded. Game thread only (UObject lookups and blueprint-state writes). No
// network, coop or quantization logic (principle 7): the callers in coop/ own the wire encoding.
//
// CAUTION: there is exactly ONE saveSlot per machine, and every remote-player puppet AND the
// local player resolve to it. This accessor only ever touches the LOCAL player's vitals. NEVER
// use it to store a remote puppet's display health -- that would corrupt the local player's
// persisted health. Puppet display health lives on coop::RemotePlayer.
#pragma once

namespace ue_wrap::vitals {

// Vital scalars on UsaveSlot_C (offsets per CXXHeaderDump/saveSlot.hpp, resolved
// by name at runtime). `health` is current HP; `MaxHealth` is per-peer (upgrades
// / story can raise it) so health-bar wire encoding must normalize health/max.
enum class Field {
    Health,     // saveSlot.health    (current HP)
    MaxHealth,  // saveSlot.maxHealth (per-peer cap)
    Food,
    Sleep,
};

// Read the LOCAL player's vital `f` into *out. Returns false (leaving *out
// untouched) if the store isn't resolvable yet (still booting / save not
// registered) or the field offset can't be found. Game-thread only; O(1) after
// the one-time resolution cache fills.
bool Read(Field f, float* out);

// Write `v` to the LOCAL player's vital `f`. Returns false if unresolved.
// Game-thread only.
bool Write(Field f, float v);

}  // namespace ue_wrap::vitals
