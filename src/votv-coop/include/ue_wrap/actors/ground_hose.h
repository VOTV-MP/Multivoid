// ue_wrap/actors/ground_hose.h -- a laid garden hose (groundHose_C), its faucet and its sprinkler.
//
// A hose coil (prop_hose_C) used on a faucet spawns a groundHose_C at the faucet's connector and ties the two together
// (faucet.hose, hose.faucet). Until it is finished the hose ticks, laying a point wherever this peer's own player walks;
// the coil used on a sprinkler (connectHose) or anywhere else finishes it: tick off, the spline meshes built from
// `points` (gen), collision on, the faucet refreshed. A tied sprinkler (hose.sprinkler, sprinkler.hose) sprays and waters
// the plants round it while the hose's faucet runs; grabbing it, or its action, calls the hose's detachSprinkler.
// A hose's save record (getData) is the actor_save_C base, `vectors[0]` = the points and `names[0]` = [faucet key,
// sprinkler key]; loadData takes them back and processKeys ties the hose to the two keys' actors. Game thread.

#pragma once

#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/types.h"

#include <string>
#include <vector>

namespace ue_wrap::ground_hose {

// groundHose_C, once something has loaded it (the coil's class names it). Null before.
void* HoseClass();

// Every live hose of this world.
void ListHoses(std::vector<void*>& out);

// Laying: its tick still runs, adding points as this peer's player walks. A finished hose's tick is off.
bool IsLaying(void* hose);

// The hose's Aactor_save_C::Key as it stands, without minting one. "" or "None" when it has none.
std::wstring KeyOf(void* hose);

// The two ends it is tied to, or null.
void* FaucetOf(void* hose);
void* SprinklerOf(void* hose);

// The hose's own record (getData, which fills the two keys from the tied actors). An end not tied now is "None": getData
// would keep the key of an end cut since.
bool Capture(void* hose, save_record::SaveRecord& out);

// The faucet whose Key is `key`, or null.
void* FindFaucet(const std::wstring& key);

// A bare hose at `loc`: not tied to a faucet, so it does not start laying.
void* Spawn(const FVector& loc);

// Hand `r` to the hose (loadData), tie it to `faucet` and `sprinkler` both ways (either may be null; a sprinkler tied
// before and not now is detached by the hose's own detachSprinkler), then finish it as the coil does.
bool Apply(void* hose, const save_record::SaveRecord& r, void* faucet, void* sprinkler);

// The hose's own detachSprinkler.
bool CallDetachSprinkler(void* hose);

// The record's two keys (names[0]); false when the record does not carry them.
bool RecordKeys(const save_record::SaveRecord& r, std::wstring& faucetKey, std::wstring& sprinklerKey);

}  // namespace ue_wrap::ground_hose
