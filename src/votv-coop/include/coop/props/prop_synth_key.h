// coop/props/prop_synth_key.h -- mint synthetic FName Key strings on non-Aprop_C keyed
// interactables (the chipPile, clump and trashBitsPile families).
//
// An Aprop_C blueprint's construction script auto-mints a NewGuid Key when ResetKey is true or
// Key is None. The three non-Aprop_C classes here do NOT auto-mint -- a probe reads clump.GetKey
// as FName(NAME_None) at fresh spawn -- so without a synthetic Key the "None" guard in the Init
// POST broadcast path would silently drop every chipPile, clump and trashBits morph.
//
// Four callers, none with a session or role dependency: the Init-POST grab observer, the
// container extract, the trash-collect held path and the host census's duplicate re-key.
// Keep this file under 200 lines: it is a single-concern utility.
//
// Synthetic format: `cs_<process-low32>_<monotonic-counter>`, a per-peer namespace plus a
// monotonic counter, so two peers minting concurrently cannot collide on identity lookup.

#pragma once

#include <string>

namespace coop::prop_synth_key {

// Returns the (possibly newly minted) Key string for `self`. An already non-None GetKey comes back
// unchanged. For a non-Aprop_C keyed interactable it mints a synthetic Key, calls setKey and
// returns it. For an Aprop_C-derived actor with a None Key it returns "None" unchanged, so the
// caller skips and the construction script mints on its own pass -- unless `mintForAprop` is set.
//
// `mintForAprop` forces the mint for Aprop_C lineage too, which the trash-pile collect needs:
// trashBitsPile::playerTryToCollect spawns an Aprop_C item AND auto-grabs it in the SAME frame,
// before the construction script has minted a NewGuid Key, so the held actor reads "None" and would
// never broadcast or match a held pose. The item's own setKey accepts our synthetic Key exactly as
// the clump and chip classes do. The default of false leaves the normal Init-POST broadcast path
// trusting the construction script.
//
// Game thread only -- calls ProcessEvent through ue_wrap::Call.
std::wstring EnsureKeyForBroadcast(void* self, const std::wstring& currentKey,
                                   bool mintForAprop = false);

// KEY-UNIQUENESS AUTHORITY mint: force-mint a fresh unique Key onto `self` REGARDLESS of its
// current Key. The host census calls it when a second live actor is found carrying an
// already-indexed Key, which the game's own save data produces -- it ships clone families
// sharing one GUID, and the identity layer assumes uniqueness. Format `rk_<64-bit-random-hex>`,
// 19 characters, inside the 31-character wire key field; random rather than the cs_ counter
// format, so a key PERSISTED into a save can never collide with a future boot's mints.
//
// Resolves setKey by climbing from the actual class to the nearest ancestor that declares it,
// since FindFunction matches the exact owner and does not climb the superstruct chain itself:
// actorChipPile and prop_garbageClump declare their own, trashBitsPile takes actor_save's, and
// the prop lineage takes Aprop_C's. Returns the CONFIRMED re-read key on success, or an empty
// string on any failure, where the caller keeps the old key and logs. Game thread only.
std::wstring MintFreshKeyForDuplicate(void* self);

}  // namespace coop::prop_synth_key
