// ue_wrap/devices/atv_condition.h -- engine access for the ATV's CONDITION state: the four
// tires' presence/durability/dirt/fixes/types, the spare trio, the body dirt scalar, fuel and
// health -- plus the game's own per-facet reducers (updTires / updDirt / updSpareTire /
// updHealth) as callable verbs.
//
// Principle-7 engine-wrapper layer: NO network/coop state, no authority decisions. The coop
// half (coop/interactables/atv_condition_sync) decides WHAT to write and WHEN a verb fires;
// this file only knows the class layout and the dispatch.
//
// Layout facts (all [V], ATV.md 17.7/17.17 + the SDK dump the tire probe quotes):
//   tires            TArray<bool>   (1-byte elems; CDO Num=4 by construction)
//   tiresDurability  TArray<float>
//   tiresDirt        TArray<float>
//   tiresFixes       TArray<int32>  (countdown; -1 legally reachable -- ejectWheel writes
//                                    fixes-1 uncapped and putTire copies it back)
//   tiresTypes       TArray<uint8>  (zero runtime writers in ATV_C bytecode; reducer input)
//   dirt / fuel / health / spareTire_durability / spareTire_dirt  FloatProperty
//   spareTire_fixes  IntProperty;  hasSpareTire  BoolProperty
//   skipTireUpdate   BoolProperty  (zero writers, no CDO override => permanently FALSE; read
//                                   anyway -- a TRUE means some future external writer exists
//                                   and the caller must defer its verbs, not no-op silently)
//
// The write primitives are WRITE-IN-PLACE: bounded to min(Num, 4) elements, never touching an
// FScriptArray's Data/Num/Max (no allocation from our side, ever). The tire PROBE
// (coop/dev/atv_tire_probe) deliberately keeps its own independent reads --
// [[lesson-an-instrument-that-shares-the-defect-cancels-it]].

#pragma once

#include <cstdint>

namespace ue_wrap::atv_condition {

// One peer's view of the condition state, in wire order. Plain data; the coop layer moves it
// to/from AtvStatePayload verbatim.
struct Snapshot {
    float   dur[4]  = {};
    float   dirt[4] = {};
    float   bodyDirt = 0.f;
    float   spareDur = 0.f;
    float   spareDirt = 0.f;
    float   fuel = 0.f;
    float   health = 0.f;
    uint8_t mask = 0;        // bit i = tires[i]
    bool    hasSpare = false;
    int8_t  spareFixes = 0;
    int8_t  fixes[4] = {};
    uint8_t types[4] = {};
};

// The game's per-facet reducers, resolvable as verbs. All BlueprintCallable [V] ATV.md CFG row.
enum class Verb : uint8_t { UpdTires, UpdDirt, UpdSpareTire, UpdHealth };

// Resolve ATV_C's 13 property offsets + the 4 verb UFunctions. Idempotent, one attempt per
// process (the class is resident for the game's lifetime). Returns true iff EVERYTHING
// resolved -- the caller treats a partial resolve as "this build cannot run the lane" and the
// producer then ships tiresValid=0 rather than a zeroed block.
bool Resolve();
bool Resolved();

// Read the live condition into `out`. False (and `out` untouched beyond zero-init) when
// unresolved, `atv` is null, or ANY array head is unreadable -- the all-or-nothing contract
// behind the wire's tiresValid bit: a half-read snapshot must never be mistaken for state.
// int32 fixes are clamped to int8 range on read (field census: values live in {-1..3}).
bool Read(void* atv, Snapshot& out);

// Write the ACCUMULATORS (dur/dirt/bodyDirt/spare scalars/fuel/health/fixes/types) from `s`.
// Never touches tires[] or hasSpareTire -- presence is a separate, authority-gated write.
// Bounded min(Num,4) per array; false when unresolved/null.
bool WriteAccumulators(void* atv, const Snapshot& s);

// Write PRESENCE: tires[i] from mask bits (min(Num,4)) and hasSpareTire. The caller gates this
// on packet authorship (host-only); this function only performs it.
bool WritePresence(void* atv, uint8_t mask, bool hasSpare);

// skipTireUpdate, read at apply time. False return = unresolved/null (treat as flag TRUE and
// defer -- fail toward not dispatching verbs).
bool ReadSkipTireUpdate(void* atv, bool& flagOut);

// Dispatch one of the game's reducers on `atv` via ProcessEvent. Game thread only. False when
// unresolved/null (counted by the caller; never silently absorbed).
bool CallVerb(void* atv, Verb v);

}  // namespace ue_wrap::atv_condition
