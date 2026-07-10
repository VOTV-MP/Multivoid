// coop/dev/inventory_probe.cpp -- see coop/dev/inventory_probe.h.

#include "coop/dev/inventory_probe.h"

#include "coop/config/config.h"
#include "coop/items/inventory_wire.h"
#include "ue_wrap/inventory.h"
#include "ue_wrap/log.h"

#include <cstdint>
#include <vector>

namespace coop::dev::inventory_probe {

void Tick() {
    static const bool s_on = ::coop::config::IsIniKeyTrue("inventory_probe");
    if (!s_on) return;
    static bool s_done = false;
    if (s_done) return;
    static int s_ticks = 0;
    if (++s_ticks < 600) return;  // ~5 s settle (world up + saveSlot resolvable)

    // 1. Read the local player's current inventory.
    ue_wrap::inventory::PlayerInventory inv0;
    if (!ue_wrap::inventory::ReadAll(inv0)) return;  // saveSlot not up yet -> retry next tick

    // WAIT for a >=2-item state before firing: the CRITICAL bug (wrong TArray element stride) only
    // manifests for N>=2 elements -- a 1-item probe is self-consistent and proves nothing about the
    // stride. So poll until inventory+equipment+hold total >= 2, then fire once. Fall back to firing
    // anyway after ~90 s (so the log is never silent) with a loud note that the stride path was NOT
    // exercised. Pick up 2+ items (or load a save that has them) within ~90 s of world-up.
    const size_t total = inv0.inventory.size() + inv0.equipment.size() + inv0.hold.size();
    const bool timedOut = s_ticks > 10800;  // ~90 s at ~120 Hz
    if (total < 2 && !timedOut) return;     // keep waiting for a multi-element inventory
    s_done = true;
    if (total < 2)
        UE_LOGW("inventory[probe]: firing with only %zu item(s) after ~90s -- the multi-element "
                "STRIDE path is NOT exercised. Pick up 2+ items (or load a save that has them) and "
                "reload to fully test the fix.", total);

    // 2. Wire round-trip (read -> serialize -> deserialize -> serialize).
    const std::vector<uint8_t> blob0 = coop::inventory_wire::Serialize(inv0);
    ue_wrap::inventory::PlayerInventory inv1;
    const bool de = coop::inventory_wire::Deserialize(blob0, inv1);
    const std::vector<uint8_t> blob1 = de ? coop::inventory_wire::Serialize(inv1)
                                          : std::vector<uint8_t>{};
    if (!de) {
        UE_LOGE("inventory[probe]: deserialize FAILED (blob0=%zu bytes) -- ABORT before apply",
                blob0.size());
        return;
    }

    UE_LOGI("inventory[probe]: read local inventory=%zu equip=%zu hold=%zu (blob %zu bytes); "
            "applying it back onto the live saveSlot to crash-test the engine write path...",
            inv0.inventory.size(), inv0.equipment.size(), inv0.hold.size(), blob0.size());

    // 3. THE TEST: write inv1 back into the live saveSlot (engine TArray construction). This
    // overwrites the saveSlot MIRROR arrays only -- the live obj_11 player container is untouched
    // until a save/load, so it is non-destructive to the running game.
    void* saveSlot = ue_wrap::inventory::ResolveSaveSlot();
    if (!saveSlot) { UE_LOGE("inventory[probe]: saveSlot unresolved at apply time"); return; }
    if (!ue_wrap::inventory::ApplyToSaveObject(saveSlot, inv1)) {
        UE_LOGE("inventory[probe]: ApplyToSaveObject FAILED (EngineAlloc unresolved / dead "
                "saveSlot) -- the apply path is NOT yet usable");
        return;
    }

    // 4. Read it back and 5. compare. The whole loop is identity iff the write is faithful.
    ue_wrap::inventory::PlayerInventory inv2;
    const bool re = ue_wrap::inventory::ReadAll(inv2);
    const std::vector<uint8_t> blob2 = re ? coop::inventory_wire::Serialize(inv2)
                                          : std::vector<uint8_t>{};
    const bool pass = re && blob0 == blob1 && blob1 == blob2;
    UE_LOGI("inventory[probe]: APPLY ROUND-TRIP %s -- readback inventory=%zu equip=%zu hold=%zu "
            "(blob0=%zu blob1=%zu blob2=%zu). %s",
            pass ? "PASS" : "FAIL",
            inv2.inventory.size(), inv2.equipment.size(), inv2.hold.size(),
            blob0.size(), blob1.size(), blob2.size(),
            pass ? "Engine write path is structurally sound (no fault, faithful reconstruction). "
                   "Items-usable still needs the multiplayer-join hands-on (materialize path)."
                 : "MISMATCH/READ-FAIL -- the engine write reconstructs the arrays incorrectly "
                   "(it would corrupt a joiner's per-player inventory).");
}

}  // namespace coop::dev::inventory_probe
