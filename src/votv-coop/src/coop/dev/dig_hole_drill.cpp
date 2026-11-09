// coop/dev/dig_hole_drill.cpp -- see coop/dev/dig_hole_drill.h.

#include "coop/dev/dig_hole_drill.h"

#include "coop/dev/drill_hand.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/active_drive.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"
#include "coop/world/dug_hole_sync.h"

#include "ue_wrap/actors/dig_tool.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <cstdint>

namespace coop::dev::dig_hole_drill {
namespace {

namespace DT = ue_wrap::dig_tool;
namespace E = ue_wrap::engine;
using coop::rig_verdict::Outcome;

constexpr const char* kTag = "dig-drill";
constexpr float    kBesideUU = 150.f;      // the holes' offset from the client's body, inside a dig's reach
constexpr float    kFloorUU = 85.f;        // a standing body's capsule centre above its feet
constexpr float    kSpotUU = 60.f;         // a hole stands at its spot: it never moves
constexpr float    kFindUU = 400.f;        // the client looks for the host's hole within the dig's reach
constexpr uint64_t kSpawnAfterMs = 6000;   // host: after the client's world-ready
constexpr uint64_t kBodyWaitMs = 30000;    // host: the client's body readable by then
constexpr uint64_t kToolAfterMs = 3000;    // client: after its own world-ready
constexpr uint64_t kStepMs = 4000;         // client: between its steps, so the hand state and each answer arrive
constexpr uint64_t kHostJudgeMs = 40000;   // host: after its spawn (or, empty, after the client's world-ready)
constexpr uint64_t kGiveUpMs = 60000;      // client: the host's hole never came by then

enum class HStep { Wait, Spawned, Done };
enum class CStep { Wait, Armed, Found, Dug, Ended, Done };

HStep    g_h = HStep::Wait;
CStep    g_c = CStep::Wait;
uint64_t g_atMs = 0;
uint64_t g_readyMs = 0;
ue_wrap::FVector g_hostHole{};   // where the host's own hole stands
ue_wrap::FVector g_body{};       // host: the client's body when the drill began
ue_wrap::FVector g_mine{};       // client: where its own dig went
void*    g_copy = nullptr;       // client: its copy of the host's hole
int32_t  g_copyIdx = -1;

void TickHost(coop::net::Session& session, uint64_t now, bool empty) {
    if (g_h == HStep::Wait) {
        if (!session.IsSlotWorldReady(1)) return;
        if (!g_readyMs) g_readyMs = now;
        if (now - g_readyMs < kSpawnAfterMs) return;
        coop::RemotePlayer* puppet = coop::players::Registry::Get().Puppet(1);
        if (!puppet || !puppet->valid() || !E::TryGetActorLocation(puppet->GetActor(), g_body)) {
            if (now - g_readyMs < kBodyWaitMs) return;
            g_h = HStep::Done;
            coop::rig_verdict::Say(kTag, Outcome::Invalid, "host: the client's body could not be read");
            return;
        }
        if (!empty) {
            g_hostHole = {g_body.X + kBesideUU, g_body.Y, g_body.Z - kFloorUU};
            if (!DT::SpawnDugHole(g_hostHole, ue_wrap::FRotator{}, 0)) {
                g_h = HStep::Done;
                coop::rig_verdict::Say(kTag, Outcome::Invalid, "host: the hole could not be spawned");
                return;
            }
            UE_LOGI("[DIG-DRILL] host dug a hole at (%.0f,%.0f,%.0f) beside the client", g_hostHole.X, g_hostHole.Y,
                    g_hostHole.Z);
        }
        g_h = HStep::Spawned;
        g_atMs = now;
        return;
    }
    if (g_h != HStep::Spawned || now - g_atMs < kHostJudgeMs) return;
    g_h = HStep::Done;
    const coop::dug_hole_sync::Counters c = coop::dug_hole_sync::Read();
    const ue_wrap::FVector theirs{g_body.X - kBesideUU, g_body.Y, g_body.Z - kFloorUU};
    const int atTheirs = DT::CountDugHolesNear(theirs, kSpotUU * 2.f, nullptr);
    if (empty) {
        UE_LOGI("[DIG-DRILL] host reads: made=%llu denied=%llu holes at the client's spot=%d",
                static_cast<unsigned long long>(c.made), static_cast<unsigned long long>(c.denied), atTheirs);
        if (c.made == 0 && c.denied >= 1 && atTheirs == 0)
            coop::rig_verdict::Say(kTag, Outcome::Pass, "host: the empty-handed hole was refused: no hole stands for it");
        else
            coop::rig_verdict::Say(kTag, Outcome::Fail, "host: made=%llu denied=%llu holes=%d after an empty-handed dig",
                                   static_cast<unsigned long long>(c.made), static_cast<unsigned long long>(c.denied),
                                   atTheirs);
        return;
    }
    const int atMine = DT::CountDugHolesNear(g_hostHole, kSpotUU, nullptr);
    UE_LOGI("[DIG-DRILL] host reads: made=%llu ended=%llu sent=%llu holes at its spot=%d at the client's=%d",
            static_cast<unsigned long long>(c.made), static_cast<unsigned long long>(c.endedFor),
            static_cast<unsigned long long>(c.sent), atMine, atTheirs);
    if (c.made == 1 && c.endedFor == 1 && atMine == 0 && atTheirs == 1)
        coop::rig_verdict::Say(kTag, Outcome::Pass,
                               "host: the client saw the host's hole and ended it, and its own hole is the host's");
    else
        coop::rig_verdict::Say(kTag, Outcome::Fail, "host: made=%llu ended=%llu own-spot=%d client-spot=%d",
                               static_cast<unsigned long long>(c.made), static_cast<unsigned long long>(c.endedFor),
                               atMine, atTheirs);
}

void Fail(const char* what, int n) {
    g_c = CStep::Done;
    coop::rig_verdict::Say(kTag, Outcome::Fail, "client: %s (%d)", what, n);
}

void TickClient(uint64_t now, bool empty) {
    void* local = coop::players::Registry::Get().Local();
    ue_wrap::FVector me{};
    if (!local || !E::TryGetActorLocation(local, me)) return;
    switch (g_c) {
    case CStep::Wait:
        if (!coop::net_pump::HasAnnouncedWorldReady()) return;
        if (!g_readyMs) g_readyMs = now;
        if (now - g_readyMs < kToolAfterMs) return;
        if (!empty && !coop::dev::drill_hand::TakeIntoHandNow(L"shovel", L"prop_shovel_C")) {
            g_c = CStep::Done;
            coop::rig_verdict::Say(kTag, Outcome::Invalid, "client: a shovel could not be taken into the hand");
            return;
        }
        g_c = empty ? CStep::Found : CStep::Armed;
        g_atMs = now;
        return;
    case CStep::Armed: {
        void* hole = nullptr;
        const int n = DT::CountDugHolesNear(me, kFindUU, &hole);
        if (!n) {
            if (now - g_atMs >= kGiveUpMs) Fail("the host's hole never came within reach", 0);
            return;
        }
        if (n != 1) return Fail("not one copy of the host's hole", n);
        g_copy = hole;
        g_copyIdx = ue_wrap::reflection::InternalIndexOf(hole);
        UE_LOGI("[DIG-DRILL] client sees the host's hole");
        g_c = CStep::Found;
        g_atMs = now;
        return;
    }
    case CStep::Found:
        if (now - g_atMs < kStepMs) return;
        g_mine = {me.X - kBesideUU, me.Y, me.Z - kFloorUU};
        DT::SpawnDugHole(g_mine, ue_wrap::FRotator{}, 0);  // as useShovel does: its begin play is the client's gate
        UE_LOGI("[DIG-DRILL] client dug its own hole at (%.0f,%.0f,%.0f)", g_mine.X, g_mine.Y, g_mine.Z);
        g_c = CStep::Dug;
        g_atMs = now;
        return;
    case CStep::Dug: {
        if (now - g_atMs < kStepMs * 2) return;
        const int n = DT::CountDugHolesNear(g_mine, kSpotUU * 2.f, nullptr);
        UE_LOGI("[DIG-DRILL] client reads %d hole(s) at its own spot", n);
        if (n != (empty ? 0 : 1)) return Fail(empty ? "a refused hole stands here" : "not one hole at its own spot", n);
        if (empty) {
            g_c = CStep::Done;
            return;
        }
        if (!ue_wrap::reflection::IsLiveByIndex(g_copy, g_copyIdx)) return Fail("the host's hole ended early", 0);
        DT::DestroyHole(g_copy);  // as a buried item, a planted seed or a dirt ball ends it
        UE_LOGI("[DIG-DRILL] client ended its copy of the host's hole");
        g_c = CStep::Ended;
        g_atMs = now;
        return;
    }
    case CStep::Ended:
        if (now - g_atMs < kStepMs) return;
        g_c = CStep::Done;
        if (DT::CountDugHolesNear(g_mine, kSpotUU * 2.f, nullptr) != 1) Fail("its own hole is gone", 0);
        return;
    case CStep::Done:
        return;
    }
}

}  // namespace

void Tick(coop::net::Session& session, bool empty) {
    if (!session.running() || (g_h == HStep::Done && g_c == CStep::Done)) return;
    const uint64_t now = coop::active_drive::NowMs();
    if (session.role() == coop::net::Role::Host) TickHost(session, now, empty);
    else TickClient(now, empty);
}

}  // namespace coop::dev::dig_hole_drill
