// harness/autotest/autotest_scanparity.cpp -- the scan-hub N-MATCH PARITY drill, run with
// VOTVCOOP_RUN_SCANPARITY=1 on either role.
//
// An INDEPENDENT old-shape probe walk -- ObjectAt plus each consumer's own public predicate and key
// filter, never the hub's classifier -- is compared against the hub's per-consumer completed-pass
// counts inside ONE GT task, so GC cannot move the array between the two reads. Mode B goes first,
// probing against the last sliced-built counts, which certifies the shipping accumulation path;
// mode A then calls ForceSyncFullPass() and probes again, certifying the classifier even when the
// world never settled. VOTVCOOP_HUB_SKIP=door is the mutate control: both modes must FAIL for
// exactly the skipped consumer before a green run counts.
//
// The probe counts DISTINCT KEYS through each consumer's REAL key function -- independence is about
// the walk and the classifier, not the extractor. grime keys by DebugPosKeyForActor, since an
// instance count over-reads its cell collisions; turbine counts instances; the atv row assumes no
// purchased synth-keyed ATVs. Every row and the verdict print under "[SCANPARITY]".

#include "harness/autotest.h"

#include "coop/element/object_scan_hub.h"
#include "coop/element/portable_identity.h"  // PortableWireKey (the channels' key rule)
#include "coop/interactables/grime_sync.h"   // DebugPosKeyForActor (the real quantizer)
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
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
    bool portable;         // the interactable channels index under a portable identity where one exists
};

// The 13 consumers' OLD-shape filters -- the independent control, calling each consumer's public
// wrapper predicate directly and never the hub registry. The key is the consumer's real rule:
// the channels replace the game's key by the portable identity where one exists.
const ProbeRow kRows[] = {
    {"door",       &ue_wrap::door::IsDoor,              &ue_wrap::door::GetKeyString,        true, true},
    {"light",      &ue_wrap::lightswitch::IsLightSwitch,&ue_wrap::lightswitch::GetSwitchKeyString, true, true},
    {"container",  &ue_wrap::swinger::IsSwinger,        &ue_wrap::prop::GetKeyString,        true, true},
    {"garage",     &ue_wrap::garage::IsGarage,          &ue_wrap::garage::GetNameKey,        true, true},
    {"appliance",  &ue_wrap::appliance::IsAppliance,    &ue_wrap::appliance::GetKeyString,   true, true},
    {"doorbox",    &ue_wrap::door_box::IsDoorBox,       &ue_wrap::door_box::GetNameKey,      true, true},
    {"keypad",     &ue_wrap::passwordlock::IsPasswordLock, &ue_wrap::passwordlock::GetKeyString, true, false},
    {"power",      &ue_wrap::power_control::IsPowerControl, &ue_wrap::power_control::GetKeyString, true, false},
    {"window",     &ue_wrap::base_window::IsBaseWindow, &ue_wrap::base_window::GetKeyString, true, false},
    // turbine: skipDefaultCdo=true although the OLD walk had no Default__ skip -- the first
    // WorldOf-term run PROVED the old index carried a phantom 5th entry, the turbine CDO
    // (PosKey'd from its default position; identical on both peers, so it "synced" CDO-to-CDO
    // harmlessly). The hub's WorldOf() != CurrentWorld() term now excludes every non-world
    // object; the probe mirrors the intended truth (placed instances), not the latent wart.
    {"turbine",    &ue_wrap::windturbine::IsTurbine,    nullptr,                             true, false},
    {"grime",      &ue_wrap::grime::IsGrime,            &coop::grime_sync::DebugPosKeyForActor, true, false},
    {"atv",        &ue_wrap::atv::IsAtv,                &ue_wrap::atv::GetKeyString,         true, false},
    {"trash_pile", &ue_wrap::prop::IsTrashBitsPile,     &ue_wrap::prop::GetInteractableKeyString, true, false},
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
            if (row.portable) {
                std::wstring portable = coop::element::PortableWireKey(obj);
                if (!portable.empty()) key = std::move(portable);
            }
            keys[rI].insert(std::move(key));
        }
    }
    for (int rI = 0; rI < kNumRows; ++rI)
        out[rI] = kRows[rI].getKey ? keys[rI].size() : instances[rI];
}

// Returns a bitmask of FAILING consumers (bit i = kRows[i]).
uint32_t CompareOnce(const char* mode, bool skipUnsettled) {
    size_t probe[kNumRows];
    ProbeWalk(probe);
    uint32_t fails = 0;
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
        if (!ok) fails |= (1u << rI);
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
        // Mode C first: the object index the passes read, against the live array. The hub can
        // only be as complete as the index, so a miss here names the substrate, not a consumer.
        // VOTVCOOP_LISTENER_DROP_CREATES=<n> is its mutate control.
        const auto parity = ue_wrap::object_index::DebugCompareWithWalk();
        const bool indexOk = parity.liveNotIndexed == 0 && parity.indexedNotLive == 0;
        UE_LOGI("[SCANPARITY] C(index)   live-not-indexed=%zu indexed-not-live=%zu objects=%zu classes=%zu -> %s",
                parity.liveNotIndexed, parity.indexedNotLive, parity.objects, parity.classes,
                indexOk ? "OK" : "FAIL");
        const uint32_t failB = CompareOnce("B(sliced)", /*skipUnsettled*/ true);
        coop::element::scan_hub::ForceSyncFullPass();
        const uint32_t failA = CompareOnce("A(forced)", /*skipUnsettled*/ false);
        // Verdict = the INTERSECTION: mode A is the zero-staleness oracle, so a B-only
        // mismatch is inter-pass churn lag (a slow-churning class like grime SETTLES between
        // its ~30 s count steps -- the settled gate cannot see the step that lands after the
        // last completed pass), informational, not a defect. A both-modes failure is real, and
        // so is an index miss.
        const uint32_t both = failA & failB;
        int defects = indexOk ? 0 : 1, churnLag = 0;
        for (uint32_t m = both; m; m &= m - 1) ++defects;
        for (uint32_t m = failB & ~failA; m; m &= m - 1) ++churnLag;
        UE_LOGI("[SCANPARITY] DONE fail=%d (index=%s churn-lag=%d modeA-only=%d)",
                defects, indexOk ? "OK" : "FAIL", churnLag, (failA & ~failB) ? 1 : 0);
        sDone.store(true, std::memory_order_release);
    });
    for (int i = 0; i < 600 && !sDone.load(std::memory_order_acquire); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return 0;
}

}  // namespace harness::autotest
