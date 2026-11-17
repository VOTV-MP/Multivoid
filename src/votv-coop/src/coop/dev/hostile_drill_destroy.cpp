// coop/dev/hostile_drill_destroy.cpp -- see coop/dev/hostile_drill_destroy.h.

#include "coop/dev/hostile_drill_destroy.h"

#include "coop/element/identity_create.h"
#include "coop/element/identity_destroy.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/remote_prop.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <chrono>
#include <cstdint>

namespace coop::dev::hostile_drill::destroy {
namespace {

using Clock = std::chrono::steady_clock;
namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;
namespace E = ue_wrap::engine;

// The victim's element id: the top of the peer range, clear of the walk arms' sentinels (65528 and 65500..65515).
constexpr coop::element::ElementId kVictimEid = 65526u;
constexpr float                    kLiftCm    = 300.f;
constexpr auto                     kFailBound = std::chrono::seconds(180);  // a failure bound, never a wait

// ---- client (sender) ----
bool g_sent = false;

bool OwnJoinOver() {
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

// ---- host (judge) ----
enum class Step { WaitArm, Judge, Done };
Step                 g_step = Step::WaitArm;
int                  g_slot = -1;
ue_wrap::CachedObjRef g_victim;
Clock::time_point    g_armAt{};

// The victim leaves the world and the registry whatever the verdict was; a live one is destroyed here, so the run
// leaves no stray actor behind.
void RemoveVictim() {
    coop::element::RetireMirror(kVictimEid);
    void* a = g_victim.Alive() ? g_victim.Raw() : nullptr;
    if (a) E::DestroyActor(a);
    g_victim.Reset();
}

void Done(const char* marker, Arm a, const char* why) {
    UE_LOGI("[HOSTILE-DRILL] %s %s -- %s", ArmName(a), marker, why);
    RemoveVictim();
    g_step = Step::Done;
}

void ArmHost(coop::net::Session* s, Arm a) {
    for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers) && g_slot < 0; ++slot)
        if (s->IsSlotWorldReady(slot)) g_slot = slot;
    if (g_slot < 0) return;
    if (coop::element::Registry::Get().Get(kVictimEid)) {
        UE_LOGW("[HOSTILE-DRILL] %s INCONCLUSIVE: the victim eid was in use at arm", ArmName(a));
        g_step = Step::Done;
        return;
    }
    ue_wrap::FVector at{};
    void* pawn = coop::players::Registry::Get().Local();
    if (!pawn || !E::TryGetActorLocation(pawn, at)) return;  // no body yet; wait
    void* cls = R::FindClass(P::name::ActorClassName);
    void* victim = cls ? E::SpawnActor(cls, {at.X, at.Y, at.Z + kLiftCm}) : nullptr;
    if (!victim) {
        UE_LOGW("[HOSTILE-DRILL] %s INCONCLUSIVE: the victim actor was not spawned", ArmName(a));
        g_step = Step::Done;
        return;
    }
    g_victim.Set(victim);
    if (!coop::element::CreateOrAdoptWorldActorMirror(kVictimEid, victim, L"Actor", g_slot)) {
        UE_LOGW("[HOSTILE-DRILL] %s INCONCLUSIVE: the victim element was not bound", ArmName(a));
        RemoveVictim();
        g_step = Step::Done;
        return;
    }
    coop::remote_prop::dev::WatchWireDestroys(g_slot, kVictimEid, coop::element::kInvalidId);
    g_armAt = Clock::now();
    g_step = Step::Judge;
    UE_LOGI("[HOSTILE-DRILL] %s armed on slot %d (victim eid %u, a world-actor element)", ArmName(a), g_slot,
            static_cast<unsigned>(kVictimEid));
}

void JudgeHost(coop::net::Session* s, Arm a) {
    if (!s->HasPeerConn(g_slot)) { Done("INCONCLUSIVE", a, "the judged slot left before the verdict"); return; }
    // The count and the receiver's effect are one call, so once the message is counted the victim's fate is settled.
    if (coop::remote_prop::dev::WireDestroysNaming(kVictimEid) == 0) {
        if (Clock::now() - g_armAt > kFailBound) Done("INCONCLUSIVE", a, "the PropDestroy never reached the host");
        return;
    }
    if (!g_victim.Alive())
        Done("FAIL", a, "a PropDestroy naming a non-prop element destroyed that element's actor");
    else
        Done("PASS", a, "a PropDestroy naming a non-prop element left that element's actor alive");
}

}  // namespace

const char* ArmName(Arm a) { return a == Arm::BadDestroyEid ? "bad_destroy_eid" : "off"; }

void TickClient(coop::net::Session* s, Arm a) {
    if (g_sent || !OwnJoinOver()) return;
    g_sent = true;
    coop::net::PropDestroyPayload d{};
    d.elementId = kVictimEid;
    if (!s->SendPropDestroy(d)) {
        UE_LOGW("[HOSTILE-DRILL] %s INCONCLUSIVE: the PropDestroy was not sent", ArmName(a));
        return;
    }
    UE_LOGI("[HOSTILE-DRILL] %s sent a PropDestroy naming eid %u", ArmName(a), static_cast<unsigned>(kVictimEid));
}

void TickHostJudge(coop::net::Session* s, Arm a) {
    if (g_step == Step::WaitArm)    ArmHost(s, a);
    else if (g_step == Step::Judge) JudgeHost(s, a);
}

void OnDisconnect() {
    g_sent = false;
    g_step = Step::WaitArm;
    g_slot = -1;
    g_victim.Reset();
    g_armAt = {};
}

}  // namespace coop::dev::hostile_drill::destroy
