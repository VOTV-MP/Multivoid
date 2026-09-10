// ue_wrap/desk/tape_caddy.h -- standalone engine access for the STOLAS tape recorder
// (Awallunit_tapes_C, ONE level-baked instance) and the reel props
// (Aprop_reel_C base + prop_reel_big_C / prop_reel_small_C). Principle-7
// engine-wrapper layer -- NO network logic; coop/tape_caddy_sync (the L7
// slot/corrector lanes) drives the unit through here.
//
// reelBig / reelSmall DOUBLE as slot state (-1.0 = slot empty, >= 0 = reel
// present at that progress); Active is already wire-synced by the ApplianceState
// family (ue_wrap/devices/appliance.cpp); upd() applies mesh visibility +
// SetActorTickEnabled(Active). A reel's own progress is NOT read here: it is part
// of the reel's save record, which coop/props/prop_save_data carries whole
// through the prop's own getData. NOTE: the wallunit's `Active` toggle is NOT
// surfaced here either (RULE 2 -- one owner: the appliance adapter).

#pragma once

#include <cstdint>

namespace ue_wrap::tape_caddy {

// Reel slot indices on the wire + in this API.
inline constexpr int kReelBig   = 0;
inline constexpr int kReelSmall = 1;

// The -1.0 empty-slot sentinel (native encoding).
inline constexpr float kSlotEmpty = -1.0f;

// Resolve Awallunit_tapes_C + Aprop_reel_C classes, field offsets, and upd().
// Idempotent; cheap after first success. Returns false while classes are not
// yet loaded (caller throttles retries). Game thread.
bool EnsureResolved();

// The single placed wallunit instance (cached ptr + InternalIndexOf;
// IsLiveByIndex fast path; GUObjectArray walk only on a cache miss). May
// return nullptr (pre-world / unit not yet spawned). Game thread.
void* Instance();

// Read both reel scalars from the live unit. Returns false if no live unit.
bool ReadReels(float& big, float& small);

// Raw-write one reel scalar (reel = kReelBig/kReelSmall). Does NOT call upd()
// -- pair with CallUpd() when the slot OCCUPANCY changed (mesh visibility);
// a pure progress correction needs no repaint. Returns false if no live unit.
bool WriteReel(int reel, float value);

// Reflected upd() on the live unit (mesh visibility + tick-enable refresh).
bool CallUpd();

// --- reel props (Aprop_reel_C lineage) ---------------------------------------

// True if `cls` is Aprop_reel_C or a subclass (the eject-birth whitelist).
bool IsReelClass(void* cls);

// Session teardown: drop the cached singleton (the next call re-resolves).
void ResetCache();

}  // namespace ue_wrap::tape_caddy
