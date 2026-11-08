// harness/autotest/autotest_fill_needs.cpp -- TEST-ONLY: the local player neither hungers nor tires while a drill runs.
//
// A drill that is not about food, sleep or fainting must not have its player faint halfway: the game's food and sleep
// ticker ragdolls a player whose save slot's `food` or `sleep` reaches 0. With VOTVCOOP_FILL_NEEDS=1 every peer fills
// both (0..100) to 100 once its world is up, and again every 20 s for as long as the game runs. Gated by
// VOTVCOOP_FILL_NEEDS; the rig's launcher sets it on every drill that does not clear it.

#include "harness/autotest.h"
#include "disc_drills.h"   // co-located private header (src tree, not include/)

#include "coop/player/players_registry.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/world/world_singleton.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace harness::autotest {
namespace {

namespace R = ue_wrap::reflection;
using disc_drill::OnGameThread;

// The gamemode's save slot's `food` and `sleep` to 100. False while the world, its gamemode or the slot is not there.
bool Fill() {
    bool done = false;
    OnGameThread([&] {
        if (!coop::players::Registry::Get().Local()) return;
        void* gm = ue_wrap::world_singleton::Gamemode();
        const int32_t slotOff = gm ? R::FindPropertyOffset(R::ClassOf(gm), L"saveSlot") : -1;
        void* slot = nullptr;
        if (slotOff >= 0) std::memcpy(&slot, static_cast<const uint8_t*>(gm) + slotOff, sizeof(slot));
        if (!slot || !R::IsLive(slot)) return;
        const int32_t food = R::FindPropertyOffset(R::ClassOf(slot), L"food");
        const int32_t sleep = R::FindPropertyOffset(R::ClassOf(slot), L"sleep");
        if (food < 0 || sleep < 0) return;
        const float full = 100.f;
        std::memcpy(static_cast<uint8_t*>(slot) + food, &full, sizeof(full));
        std::memcpy(static_cast<uint8_t*>(slot) + sleep, &full, sizeof(full));
        done = true;
    });
    return done;
}

}  // namespace

DWORD WINAPI FillNeedsThread(LPVOID /*arg*/) {
    bool said = false;
    for (;;) {
        const bool ok = Fill();
        if (ok && !said) {
            said = true;
            UE_LOGI("fill-needs: the local player's food and sleep are kept at 100 while the drill runs");
        }
        ::Sleep(ok ? 20000 : 2000);
    }
}

}  // namespace harness::autotest
