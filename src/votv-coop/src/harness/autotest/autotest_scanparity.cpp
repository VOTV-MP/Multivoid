// harness/autotest/autotest_scanparity.cpp -- the R-2 scan-hub N-MATCH PARITY drill
// (VOTVCOOP_RUN_SCANPARITY=1, either role; design acceptance (i) in
// votv-shared-scan-hub-R2-DESIGN-2026-08-23.md).
//
// An INDEPENDENT old-shape probe walk (ObjectAt + each consumer's own public predicate +
// key filter -- never the hub's classifier) is compared against the hub's per-consumer
// completed-pass counts, INSIDE ONE GT task so GC cannot move the array between the two
// reads (GC runs on the GT; a same-task pair is atomic):
//   mode B (first): probe vs the LAST SLICED-BUILT counts -- certifies the shipping
//     accumulation path; at a settled anchor staleness is provably nil.
//   mode A (second): ForceSyncFullPass() then probe -- certifies the classifier with zero
//     staleness even if the world was not settled.
// MUTATE control: run once with VOTVCOOP_HUB_SKIP=door -- mode A/B must FAIL for exactly
// the skipped consumer before any green run counts (the instrument can see a miss).
//
// Count semantics: the probe counts DISTINCT KEYS using each consumer's REAL key fn (the
// independence requirement is about the WALK/classifier, not the key extractor -- production
// key fns are shared). grime uses DebugPosKeyForActor (the first RED run measured 2 real
// cell collisions among 1,023 decals, so an instance count over-reads). turbine still counts
// instances (5 turbines; a collision there would be a map-design change). The atv compare
// assumes no PURCHASED (synth-keyed) ATVs in the run (the parked smoke scenario).
//
// Grep keys: "[SCANPARITY]" per-consumer rows + "[SCANPARITY] DONE fail=<n>".

#include "harness/autotest.h"

#include "coop/element/object_scan_hub.h"
#include "coop/interactables/grime_sync.h"   // DebugPosKeyForActor (the real quantizer)
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/swinger.h"
#include "ue_wrap/devices/appliance.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/devices/base_window.h"
#include "ue_wrap/devices/door.h"
#include "ue_wrap/devices/door_box.h"
#include "ue_wrap/devices/garage.h"
#include "ue_wrap/devices/grime.h"
#include "ue_wrap/devices/lightswitch.h"
#include "ue_wrap/devices/passwordlock.h"
#include "ue_wrap/devices/power_control.h"
#include "ue_wrap/devices/windturbine.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <unordered_set>

namespace harness::autotest {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;

struct ProbeRow {
    const char* name;
    bool (*isInstance)(void* obj);
    // Key extractor; nullptr = count live instances (position-keyed consumers).
    std::wstring (*getKey)(void* actor);
    bool skipDefaultCdo;   // the consumer's old walk had a Default__ skip (turbine did not)
};

// The 13 consumers' OLD-shape filters, verbatim (the independent control -- calls the
// public wrapper predicates directly, never the hub registry).
const ProbeRow kRows[] = {
    {"door",       &ue_wrap::door::IsDoor,              &ue_wrap::door::GetKeyString,        true},
    {"light",      &ue_wrap::lightswitch::IsLightSwitch,&ue_wrap::lightswitch::GetSwitchKeyString, true},
    {"container",  &ue_wrap::swinger::IsSwinger,        &ue_wrap::prop::GetKeyString,        true},
    {"garage",     &ue_wrap::garage::IsGarage,          &ue_wrap::garage::GetNameKey,        true},
    {"appliance",  &ue_wrap::appliance::IsAppliance,    &ue_wrap::appliance::GetKeyString,   true},
    {"doorbox",    &ue_wrap::door_box::IsDoorBox,       &ue_wrap::door_box::GetNameKey,      true},
    {"keypad",     &ue_wrap::passwordlock::IsPasswordLock, &ue_wrap::passwordlock::GetKeyString, true},
    {"power",      &ue_wrap::power_control::IsPowerControl, &ue_wrap::power_control::GetKeyString, true},
    {"window",     &ue_wrap::base_window::IsBaseWindow, &ue_wrap::base_window::GetKeyString, true},
    {"turbine",    &ue_wrap::windturbine::IsTurbine,    nullptr,                             false},
    {"grime",      &ue_wrap::grime::IsGrime,            &coop::grime_sync::DebugPosKeyForActor, true},
    {"atv",        &ue_wrap::atv::IsAtv,                &ue_wrap::atv::GetKeyString,         true},
    {"trash_pile", &ue_wrap::prop::IsTrashBitsPile,     &ue_wrap::prop::GetInteractableKeyString, true},
};
constexpr int kNumRows = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));

// One full independent walk; fills per-row distinct-key (or instance) counts.
void ProbeWalk(size_t out[kNumRows]) {
    std::unordered_set<std::wstring> keys[kNumRows];
    size_t instances[kNumRows] = {};
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        for (int rI = 0; rI < kNumRows; ++rI) {
            const ProbeRow& row = kRows[rI];
            if (!row.isInstance(obj)) continue;
            if (row.skipDefaultCdo &&
                R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
            if (!R::IsLive(obj)) continue;
            if (!row.getKey) { ++instances[rI]; continue; }
            std::wstring key = row.getKey(obj);
            if (key.empty() || key == L"None") continue;
            keys[rI].insert(std::move(key));
        }
    }
    for (int rI = 0; rI < kNumRows; ++rI)
        out[rI] = kRows[rI].getKey ? keys[rI].size() : instances[rI];
}

int CompareOnce(const char* mode, bool skipUnsettled) {
    size_t probe[kNumRows];
    ProbeWalk(probe);
    int fails = 0;
    for (int rI = 0; rI < kNumRows; ++rI) {
        // Mode B only certifies SETTLED consumers: a churning class (grime while the world
        // breathes) is <=1 pass stale BY DESIGN (first GREEN run: grime 1020 vs probe 1021,
        // one decal born after the last completed pass); mode A certifies it with zero
        // staleness instead.
        if (skipUnsettled && !coop::element::scan_hub::DebugConsumerSettled(kRows[rI].name)) {
            UE_LOGI("[SCANPARITY] %s %-10s SKIP (unsettled/churning -- certified by mode A)",
                    mode, kRows[rI].name);
            continue;
        }
        const size_t hub = coop::element::scan_hub::DebugConsumerCount(kRows[rI].name);
        const bool ok = (hub != SIZE_MAX) && (hub == probe[rI]);
        if (!ok) ++fails;
        UE_LOGI("[SCANPARITY] %s %-10s probe=%zu hub=%s%zu -> %s", mode, kRows[rI].name,
                probe[rI], hub == SIZE_MAX ? "(none) " : "", hub == SIZE_MAX ? 0 : hub,
                ok ? "OK" : "FAIL");
    }
    return fails;
}

}  // namespace

DWORD WINAPI ScanParityThread(LPVOID /*arg*/) {
    // Settle anchor: the computed bound is settleScans(15) x 2 s + pass duration ~ 32 s
    // post-churn; 75 s from boot comfortably clears world load + streaming + settle on a
    // parked smoke. Mode B first (the sliced-built indexes), then mode A (ForceSync).
    std::this_thread::sleep_for(std::chrono::seconds(75));
    static std::atomic<bool> sDone{false};
    GT::Post([] {
        const int failB = CompareOnce("B(sliced)", /*skipUnsettled*/ true);
        coop::element::scan_hub::ForceSyncFullPass();
        const int failA = CompareOnce("A(forced)", /*skipUnsettled*/ false);
        UE_LOGI("[SCANPARITY] DONE fail=%d (modeB=%d modeA=%d)", failA + failB, failB, failA);
        sDone.store(true, std::memory_order_release);
    });
    for (int i = 0; i < 600 && !sDone.load(std::memory_order_acquire); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return 0;
}

}  // namespace harness::autotest
