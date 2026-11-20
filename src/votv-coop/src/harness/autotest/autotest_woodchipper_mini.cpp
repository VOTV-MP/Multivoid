// harness/autotest/autotest_woodchipper_mini.cpp -- TEST-ONLY: the mini arm of the woodchipper drill (woodchipper_drill=mini),
// verdict 'woodchipper-mini': one pour from a mini can in the client's hand, a subclass of the plain can, judged on the host's own
// events. The drill's readings and its pour are autotest_woodchipper.cpp's (woodchipper_drill_arms.h).

#include "harness/autotest/peer_signal.h"
#include "woodchipper_drill_arms.h"  // co-located private header

#include "coop/player/players_registry.h"
#include "coop/session/rig_verdict.h"

#include "ue_wrap/actors/item_spawn.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace harness::autotest::woodchipper_drill {
namespace {

namespace E  = ue_wrap::engine;
namespace PS = harness::autotest::peer_signal;
namespace RV = coop::rig_verdict;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

}  // namespace

// A mini can in the world before any hand takes one, as a real mini can in a hand always came from a world the host holds. The
// judge asks the hand's class by a class this host has loaded, and a host never loads a client's class name (class_resolve
// LookupOnly); the forced hold loads it on the client alone. The object index lists the class at its next drain, not in the
// spawning tick: the wait is on that listing.
const char* SpawnMiniCan() {
    bool spawned = false;
    OnGameThread([&] {
        void* me = coop::players::Registry::Get().Local();
        ue_wrap::FVector at{};
        if (!me || !E::TryGetActorLocation(me, at)) return;
        const ue_wrap::FVector fwd = E::GetActorForwardVector(me);
        spawned = ue_wrap::item_spawn::Spawn(kMiniClass, {at.X - fwd.X * 150.f, at.Y - fwd.Y * 150.f, at.Z + 60.f}) != nullptr;
    });
    if (!spawned) return "the spawn failed: no local player, or the mini can's class did not load";
    const bool listed = PS::WaitFor("the host lists the mini can's class", 10, [] {
        bool in = false;
        OnGameThread([&] { in = ue_wrap::object_index::ClassByName(kMiniClass) != nullptr; });
        return in;
    }, false);
    return listed ? nullptr : "the can was spawned, and the object index did not list its class within 10 s";
}

// PASS: the host's fuel rose by what the client's copy gained. FAIL: the host refused the pour for the can (its refusal count
// rose), or took another amount. INVALID: the client's copy gained nothing, or neither came in kCopySec.
void HostMini(const std::wstring& key) {
    const float before = FuelNow(key);
    const unsigned long long refusedBefore = RefusedForCan();
    PS::Mark("gm_go", CopyText(key));
    std::string poured;
    if (!PS::AwaitText("gm_poured", poured, kSignalSec)) return;
    const size_t at = poured.find("gained=");
    const float gained = at == std::string::npos ? -1.f : static_cast<float>(std::strtod(poured.c_str() + at + std::strlen("gained="), nullptr));
    if (!(gained > 1.f)) {
        RV::Say(kTagMini, RV::Outcome::Invalid, "the client's copy gained %.2f from the mini can: nothing for the host to judge",
                static_cast<double>(gained));
        return;
    }
    PS::WaitFor("the host judges the mini can's pour", kCopySec,
                [&] { return FuelNow(key) > before + kFuelTol || RefusedForCan() > refusedBefore; }, false);
    const float after = FuelNow(key);
    const unsigned long long refusals = RefusedForCan() - refusedBefore;
    char what[200];
    std::snprintf(what, sizeof(what), "the client's copy gained %.2f; the host's fuel %.2f -> %.2f, refused for the can %llu times",
                  static_cast<double>(gained), static_cast<double>(before), static_cast<double>(after), refusals);
    const bool refused = refusals > 0;
    const bool took = std::fabs(after - (before + gained)) <= kFuelTol;
    const bool moved = after > before + kFuelTol;
    UE_LOGI("woodchipper: ARM gm %s -- %s", refused ? "FAIL" : took ? "PASS" : moved ? "FAIL" : "INVALID", what);
    if (refused) RV::Say(kTagMini, RV::Outcome::Fail, "the host refused a pour from a mini can in the hand: %s", what);
    else if (took) RV::Say(kTagMini, RV::Outcome::Pass, "%s", what);
    else if (moved) RV::Say(kTagMini, RV::Outcome::Fail, "the host took another amount: %s", what);
    else RV::Say(kTagMini, RV::Outcome::Invalid, "the host judged nothing in %d s: %s", kCopySec, what);
}

// The pour of the mini can in the hand, once the host says it has taken its baseline.
void ClientMini(const std::wstring& key) {
    std::string go;
    if (!PS::AwaitText("gm_go", go, kSignalSec)) return;
    const float gained = PourHeldCan(key);
    UE_LOGI("woodchipper: CLIENT poured the mini can: the copy gained %.2f", static_cast<double>(gained));
    PS::Mark("gm_poured", "gained=" + std::to_string(gained));
}

}  // namespace harness::autotest::woodchipper_drill
