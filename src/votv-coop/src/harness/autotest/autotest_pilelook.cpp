// harness/autotest/autotest_pilelook.cpp -- the pile-look census (VOTVCOOP_RUN_PILELOOK=1): each
// peer, once the join has settled, writes one line per chip pile it can name by eid -- the visible
// mesh's world rotation, and on a client whether the pile is a mirror this peer SPAWNED or its own
// save-loaded actor bound to the host's eid. The driver joins the two logs by eid. A pile's init()
// rolls a random mesh roll on every construction, a save load included, so the question the census
// answers is which of the two kinds takes the host's roll. The same walk lists every garbage clump
// (a trash entity's other resting form: one loaded from the save, or thrown and at rest on a box)
// with its eid and position, and counts the clumps that have no eid at all. Nobody moves; nothing
// is written.

#include "harness/autotest.h"

#include "coop/element/element.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/remote_prop.h"
#include "coop/props/trash_mirror.h"
#include "ue_wrap/actors/chip_pile.h"
#include "ue_wrap/actors/prop.h"              // IsChipPile
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <atomic>
#include <cstdint>
#include <memory>

namespace harness::autotest {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;

template <class Fn>
int RunGT(Fn&& body) {
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([done, body]() mutable { body(*done); });
    while (done->load() == 0) ::Sleep(5);
    return done->load();
}

}  // namespace

void RunPileLookScenario() {
    const bool client = IsClientRole();
    const uint8_t otherSlot = client ? 0 : 1;
    UE_LOGI("pilelook: %s -- waiting for the other peer's puppet, then 60 s for the join to settle",
            client ? "CLIENT" : "HOST");
    for (int waited = 0; waited < 180; ++waited) {
        const int r = RunGT([otherSlot](std::atomic<int>& d) {
            coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(otherSlot);
            d.store(rp && rp->valid() ? 1 : 2);
        });
        if (r == 1) break;
        ::Sleep(1000);
    }
    ::Sleep(60000);   // the snapshot burst, the async load tail and the quiescence re-bind

    RunGT([client](std::atomic<int>& d) {
        int named = 0, unnamed = 0, clumps = 0, clumpsUnnamed = 0;
        // Every pile class, the variants included: a walk of the object array with the pile test,
        // where a lookup by one class name would count actorChipPile_C alone.
        const int32_t n = R::NumObjects();
        for (int32_t i = 0; i < n; ++i) {
            void* o = R::ObjectAt(i);
            if (!o || !R::IsLive(o)) continue;
            const bool isClump = ue_wrap::prop::IsGarbageClump(o);
            if (!isClump && !ue_wrap::prop::IsChipPile(o)) continue;
            if (R::NameStartsWith(R::NameOf(o), L"Default__")) continue;
            const coop::element::ElementId eid =
                client ? coop::remote_prop::ResolveMirrorEidByActor(o)
                       : coop::prop_element_tracker::GetPropElementIdForActor(o);
            if (isClump) {
                ue_wrap::FVector at{};
                const bool none = (eid == coop::element::kInvalidId || eid == 0);
                const char* kind = !client ? "host" : (coop::trash_mirror::WeMade(o) ? "made" : "save");
                if (E::TryGetActorLocation(o, at))
                    UE_LOGI("pilelook: CLUMP eid=%u kind=%s cls='%ls' pos=(%.1f,%.1f,%.1f)",
                            none ? 0u : static_cast<unsigned>(eid), kind, R::ClassNameOf(o).c_str(), at.X, at.Y, at.Z);
                else   // no numbers: the judge reads a row's coordinates as a place
                    UE_LOGI("pilelook: CLUMP eid=%u kind=%s cls='%ls' pos=(unread)",
                            none ? 0u : static_cast<unsigned>(eid), kind, R::ClassNameOf(o).c_str());
                ++clumps;
                if (none) ++clumpsUnnamed;
                continue;
            }
            if (eid == coop::element::kInvalidId || eid == 0) { ++unnamed; continue; }
            const ue_wrap::FRotator m = ue_wrap::chip_pile::VisibleMeshWorldRotation(o);
            const ue_wrap::FRotator a = E::GetActorRotation(o);
            const char* kind = !client ? "host" : (coop::trash_mirror::WeMade(o) ? "made" : "save");
            ue_wrap::chip_pile::Look look{};
            ue_wrap::chip_pile::ReadLook(o, look);
            UE_LOGI("pilelook: PILE eid=%u kind=%s mesh=(%.1f,%.1f,%.1f) actor=(%.1f,%.1f,%.1f) scale=(%.3f,%.3f,%.3f)",
                    static_cast<unsigned>(eid), kind, m.Pitch, m.Yaw, m.Roll, a.Pitch, a.Yaw, a.Roll,
                    look.relScale.X, look.relScale.Y, look.relScale.Z);
            ++named;
        }
        UE_LOGI("pilelook: DONE named=%d unnamed=%d clumps=%d clumpsUnnamed=%d", named, unnamed, clumps,
                clumpsUnnamed);
        d.store(1);
    });
}

DWORD WINAPI PileLookThread(LPVOID /*arg*/) {
    RunPileLookScenario();
    return 0;
}

}  // namespace harness::autotest
