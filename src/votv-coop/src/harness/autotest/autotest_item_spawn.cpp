// harness/autotest/autotest_item_spawn.cpp -- TEST-ONLY: an item whose class this process has not loaded is spawned
// anyway (item_spawn_drill, the host).
//
// The host spawns the items its refunds and hand-overs make through ue_wrap/actors/item_spawn, which loads the item's
// class first. The drill takes each site's own spawn verb (the power upgrade, the coordination tower's fuse, the server
// upgrade, the mining rig's GPU and cooler, the ATV's battery and wheel) and, for each one whose class is NOT in this
// world yet (read from the object index and the whole-array lookup right before the spawn), spawns it in front of the
// host's player. The item must stand, a live actor of that class, and is destroyed at once. The first one lost is
// `rig: VERDICT item-spawn FAIL`, said at once with no later site run. PASS when at least one class was absent and each
// absent one stood; INVALID when the host has no body, or every class the sites name is loaded already.
// The pack's bag and the cover plate's prop are read in the code only (their spawns need a pile and a desk). A last leg
// lists the module bytes the game maps to a class and FAILs on a byte the desk takes that maps to none. Fresh world.
//
// Red arm: item_spawn_no_load=1 turns the class resolver's registry step off, so a class the host has not loaded is not
// loaded and the spawn answers null, as a lookup in the object index alone did before item_spawn; the verdict is FAIL.

#include "harness/autotest.h"   // IsClientRole
#include "harness/autotest/peer_signal.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/player/players_registry.h"
#include "coop/session/rig_verdict.h"

#include "ue_wrap/actors/item_spawn.h"
#include "ue_wrap/actors/miner.h"
#include "ue_wrap/core/class_resolve.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/desk/phys_mods.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/devices/atv_parts.h"
#include "ue_wrap/devices/coord_tower.h"
#include "ue_wrap/devices/generator.h"
#include "ue_wrap/devices/serverbox.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"  // DestroyActor

#include <windows.h>

#include <atomic>
#include <string>

namespace harness::autotest {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace CR = ue_wrap::class_resolve;
namespace OI = ue_wrap::object_index;
namespace PS = peer_signal;
namespace RV = coop::rig_verdict;

constexpr const char* kTag = "item-spawn";
constexpr int kBodySec = 120;
constexpr float kAheadCm = 250.f;
constexpr float kApartCm = 90.f;
constexpr float kBatteryCharge = 50.f;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

void* SpawnGpu(const ue_wrap::FVector& at) { return ue_wrap::miner::SpawnItem(ue_wrap::miner::kItemGpu, at); }
void* SpawnCooler(const ue_wrap::FVector& at) { return ue_wrap::miner::SpawnItem(ue_wrap::miner::kItemFan, at); }
void* SpawnBattery(const ue_wrap::FVector& at) {
    return ue_wrap::atv::SpawnBattery(at, {}, {1.f, 1.f, 1.f}, 1, kBatteryCharge);
}

// The wheel is made by the ATV part verb, which loads the class and resolves; it needs an ATV in the world.
void* SpawnWheel(const ue_wrap::FVector& at) { return ue_wrap::atv_parts::SpawnWheel(at, ue_wrap::atv_parts::Wheel{}); }

// One spawn site: what it is, the class its verb names, the class it needs in the world to be reachable (null: none),
// and the verb itself.
struct Site {
    const char* what;
    const wchar_t* cls;
    const wchar_t* needs;
    void* (*spawn)(const ue_wrap::FVector& at);
};
constexpr Site kSites[] = {
    {"power upgrade", L"prop_transformerUpgrade_C", nullptr, &ue_wrap::generator::SpawnUpgrade},
    {"coordination tower fuse", L"prop_fuse_C", nullptr, &ue_wrap::coord_tower::SpawnFuse},
    {"server upgrade", L"prop_serverUpg_1_C", nullptr, &ue_wrap::serverbox::SpawnUpgradeProp},
    {"mining rig GPU", L"prop_gpu_C", nullptr, &SpawnGpu},
    {"mining rig cooler", L"prop_cooler_C", nullptr, &SpawnCooler},
    {"ATV battery", L"prop_atvcarbattery_C", nullptr, &SpawnBattery},
    {"ATV wheel", L"prop_atvWheel_C", L"ATV_C", &SpawnWheel},
};

// The module bytes the desk and ATV refunds name through the game's own byte-to-class function.
constexpr int kModuleBytes = 64;

struct Result {
    bool noBody = false;
    int tested = 0;
    const Site* lost = nullptr;
    int modulesMapped = 0;      // bytes the game maps to a class
    int modulesUnlisted = 0;    // of them, classes the object index does not list under their name
    int moduleLostByte = -1;    // a byte the desk takes whose class does not read
};

// The module classes as a refund of a module would find them: every byte the desk takes must map to a class.
void MeasureModules(Result& out) {
    if (!ue_wrap::phys_mods::EnsureResolved()) {
        UE_LOGW("item-spawn: modules: the module table does not resolve here -- not measured");
        return;
    }
    for (int b = 1; b < kModuleBytes; ++b) {
        void* const cls = ue_wrap::phys_mods::ClassForByte(static_cast<uint8_t>(b));
        const bool allowed = ue_wrap::phys_mods::IsModuleAllowed(static_cast<uint8_t>(b));
        if (!cls) {
            if (allowed && out.moduleLostByte < 0) out.moduleLostByte = b;
            continue;
        }
        const std::wstring name = R::ToString(R::NameOf(cls));
        const bool listed = OI::ClassByName(name.c_str()) == cls;
        ++out.modulesMapped;
        if (!listed) ++out.modulesUnlisted;
        UE_LOGI("item-spawn: modules: byte %d -> %ls (%s, %s)", b, name.c_str(),
                allowed ? "the desk takes it" : "not the desk's", listed ? "listed" : "NOT listed in the object index");
    }
    UE_LOGI("item-spawn: modules: %d bytes map to a class, %d of them not listed; first desk byte with no class: %d",
            out.modulesMapped, out.modulesUnlisted, out.moduleLostByte);
}

// Every site whose class is absent, in order, until one is lost; each item that stands is destroyed on the spot.
Result RunSites() {
    Result out;
    OnGameThread([&] {
        void* me = coop::players::Registry::Get().Local();
        ue_wrap::FVector at{};
        if (!me || !E::TryGetActorLocation(me, at)) {
            out.noBody = true;
            return;
        }
        const ue_wrap::FVector fwd = E::GetActorForwardVector(me);
        for (const Site& s : kSites) {
            if (s.needs && !OI::ClassByName(s.needs)) {
                UE_LOGI("item-spawn: %s: %ls is not in this world -- the site cannot run here", s.what, s.needs);
                continue;
            }
            if (OI::ClassByName(s.cls) || R::FindClass(s.cls)) {
                UE_LOGI("item-spawn: %s: %ls is loaded here already -- not a test of the load", s.what, s.cls);
                continue;
            }
            const float side = kApartCm * static_cast<float>(out.tested);
            ++out.tested;
            UE_LOGI("item-spawn: %s: %ls is not loaded here; spawning it", s.what, s.cls);
            void* const actor = s.spawn({at.X + fwd.X * kAheadCm - fwd.Y * side, at.Y + fwd.Y * kAheadCm + fwd.X * side,
                                         at.Z + 60.f});
            if (actor && R::IsLive(actor) && R::ClassNameOf(actor) == s.cls) {
                UE_LOGI("item-spawn: %s: the item stands as %ls", s.what, s.cls);
                E::DestroyActor(actor);
                continue;
            }
            UE_LOGW("item-spawn: %s: %ls was not spawned -- the item is lost", s.what, s.cls);
            out.lost = &s;
            return;
        }
        MeasureModules(out);
    });
    return out;
}

void RunHost() {
    const bool red = coop::config::ResolveFlag(coop::config_registry::rows::item_spawn_no_load);
    // The host's own body: HasAnnouncedWorldReady is a CLIENT's announcement to the host and is never set on a host,
    // so a wait on it ran to the rig's cap with no line. The wait says what it is for, and ends INVALID at its bound.
    UE_LOGI("item-spawn: waiting for the host's body, at most %d s", kBodySec);
    if (!PS::WaitFor("the host's body", kBodySec, [] {
            bool up = false;
            OnGameThread([&] {
                ue_wrap::FVector at{};
                void* const me = coop::players::Registry::Get().Local();
                up = me && E::TryGetActorLocation(me, at);
            });
            return up;
        }))
        return;
    if (red) {
        OnGameThread([] { CR::SetRegistryStep(false); });
        UE_LOGW("item-spawn: red arm: the registry step is off, so a class this host has not loaded is not loaded");
    }
    const Result r = RunSites();
    if (r.noBody) {
        RV::Say(kTag, RV::Outcome::Invalid, "the host has no body to spawn in front of");
    } else if (r.lost) {
        RV::Say(kTag, RV::Outcome::Fail, "%s (%ls) was lost: its class was not loaded and the spawn did not load it",
                r.lost->what, r.lost->cls);
    } else if (r.moduleLostByte >= 0) {
        RV::Say(kTag, RV::Outcome::Fail, "module byte %d is one the desk takes and the game maps it to no class",
                r.moduleLostByte);
    } else if (r.tested == 0) {
        RV::Say(kTag, RV::Outcome::Invalid, "every class the sites name is loaded already");
    } else {
        RV::Say(kTag, RV::Outcome::Pass, "%d item class(es) were absent and each stood", r.tested);
    }
    UE_LOGI("item-spawn: DONE on host");
}

}  // namespace

DWORD WINAPI ItemSpawnDrillThread(LPVOID /*arg*/) {
    if (IsClientRole()) return 0;
    if (!PS::Begin(kTag)) return 0;
    RunHost();
    return 0;
}

}  // namespace harness::autotest
