// coop/dev/hunt_drill.cpp -- see coop/dev/hunt_drill.h.

// The steps (host only; the client joins and takes what is delivered):
//   1. WAIT: slot 1's join is over and its bracket closed. 2. WAIT PUPPET: its puppet is live and posed.
//   3. SPAWN: the arm's creature 600 cm from the host on the line from the puppet (so without the switch the host is nearer), in
//      the state its hunt needs; the Grays arm stands a grayEventController_C and calls its `despawn` 3 s later.
//   4. RUN: each second, the sample (the hunted slot, served_player's slot, both distances, reads answered with a client),
//      vitals refilled; the verdict at 40 s (15 s for the Grays).
// The verdict with hunt_skip_host: the creature hunted slot 1 and its reads were answered with the puppet; a walker also
// closed on it; the Grays delivered a hit to the client. Without it: the first pick is the nearer player at the spawn, and
// when that is the host no read was answered with a client. Both players stand at one spot, so closing shows only the hunt.

#include "coop/dev/hunt_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/creatures/hunt_target.h"
#include "coop/creatures/hunt_target_rule.h"
#include "coop/creatures/owner_entity_sync.h"
#include "coop/creatures/served_player.h"
#include "coop/dev/restore_vitals.h"
#include "coop/dev/spawn_npc.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/player_damage.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_snapshot.h"
#include "coop/session/net_pump.h"
#include "coop/session/teleport_client.h"

#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace coop::dev::hunt_drill {
namespace {

namespace E  = ue_wrap::engine;
namespace EL = coop::element;
namespace HT = coop::hunt_target;
namespace PL = coop::players;
namespace R  = ue_wrap::reflection;
namespace SP = coop::served_player;

struct Arm {
    const char*    name;
    const wchar_t* cls;
    const wchar_t* path;
    bool           mustClose;    // the creature walks to its prey: its distance to the puppet must close
    bool           mustDeliver;  // its attack is a delivered hit: the client must be hit
    bool           adopt;        // the game makes it (an event the rig fires beside this drill): the drill takes the first live one
};
constexpr Arm kArms[] = {
    {"krampus", L"npc_krampus_C", L"/Game/objects/npc/npc_krampus.npc_krampus_C", false, false, false},
    {"follower", L"npc_arirFollower_C", L"/Game/objects/npc/npc_arirFollower.npc_arirFollower_C", true, false, false},
    {"gray", nullptr, nullptr, false, true, false},
    // Not a hunter: the Shadow Figura is each peer's own (coop/creatures/owner_entity_sync). The host makes one and the
    // client judges that it is shown a copy of it.
    {"figura", L"figura_C", L"/Game/objects/figura.figura_C", false, false, false},
};
constexpr uint8_t kFiguraClassId = 1;  // owner_entity_sync's member table

constexpr long long kWatchMs = 40000, kGrayWatchMs = 15000, kBoundMs = 60000, kAdoptBoundMs = 150000, kSampleMs = 1000, kGrayCallMs = 3000;
constexpr float kSpawnCm = 600.f;
// The hunters' stage: open ground outside the base, where a creature that walks has a NavMesh under it. The host goes there
// before the client joins at its place.
constexpr float kStageX = -1692.f, kStageY = 560.f, kStageZ = 6318.f;
bool g_staged = false;
constexpr const wchar_t* kControllerClass = L"grayEventController_C";
constexpr const wchar_t* kControllerPath = L"/Game/objects/misc/grayEventController.grayEventController_C";

std::atomic<coop::net::Session*> g_session{nullptr};

const Arm* ArmOf() {
    static const Arm* s = []() -> const Arm* {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::hunt_drill);
        for (const Arm& a : kArms)
            if (v == a.name) return &a;
        return nullptr;
    }();
    return s;
}

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

enum class Step { Wait, WaitPuppet, Run, Done };
struct State {
    Step      step = Step::Wait;
    long long stepMs = 0, startMs = 0, nextSampleMs = 0;
    void*     creature = nullptr;  // the creature, or the Grays arm's controller
    int32_t   index = -1;
    bool      skip = false;
    uint8_t   expect = 0;
    float     d0Host = 0.f, d0Pup = 0.f, minHost = 1e9f, minPup = 1e9f;
    uint8_t   firstPick = HT::kNoTarget;
    int       samples = 0;
    bool      called = false;
    unsigned long long answered0 = 0;
    uint32_t  delivered0 = 0;
} g;

void To(Step s) { g.step = s; g.stepMs = NowMs(); }

void Verdict(bool pass, const char* arm, const char* fmt, ...) {
    char buf[360];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (pass) UE_LOGI("[HUNT] VERDICT %s PASS %s", arm, buf);
    else      UE_LOGW("[HUNT] VERDICT %s FAIL %s", arm, buf);
    UE_LOGI("hunt_drill: DONE %s", arm);
}

void* LivePuppet() {
    coop::RemotePlayer* rp = PL::Registry::Get().Puppet(1);
    void* puppet = (rp && rp->valid() && rp->HasPose()) ? rp->GetActor() : nullptr;
    return (puppet && R::IsLive(puppet)) ? puppet : nullptr;
}

float Distance(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// 600 cm from the host along the line from the puppet to the host, so the host is the nearer without the switch.
bool PlaceAt(void* puppet, ue_wrap::FVector& at, float& dHost, float& dPup) {
    void* host = PL::Registry::Get().Local();
    ue_wrap::FVector hp{}, pp{};
    ue_wrap::FRotator yaw{};
    if (!host || !puppet || !E::TryGetActorLocation(host, hp) || !E::TryGetActorLocation(puppet, pp) ||
        !E::TryGetActorRotation(puppet, yaw))
        return false;
    float dx = hp.X - pp.X, dy = hp.Y - pp.Y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 5.f) {
        dx = std::cos(yaw.Yaw * 0.01745329252f);
        dy = std::sin(yaw.Yaw * 0.01745329252f);
    } else {
        dx /= len;
        dy /= len;
    }
    at = ue_wrap::FVector{hp.X + dx * kSpawnCm, hp.Y + dy * kSpawnCm, hp.Z + 30.f};
    dHost = Distance(at, hp);
    dPup = Distance(at, pp);
    return true;
}

void* LoadClassAndSpawn(const wchar_t* cls, const wchar_t* path, const ue_wrap::FVector& at) {
    if (!ue_wrap::object_index::ClassByName(cls)) ue_wrap::asset_load::LoadObjectByPath(path);
    return coop::dev::spawn_npc::SpawnAt(cls, at);
}

void* LiveInstanceOf(const wchar_t* cls) {
    for (void* obj : R::FindObjectsByClass(cls))
        if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) return obj;
    return nullptr;
}

// Takes the creature: spawns it (or, for an arm the game makes, takes the first live one, and keeps waiting when there is none).
void Spawn(const Arm& arm, void* puppet) {
    ue_wrap::FVector at{};
    const bool gray = arm.cls == nullptr;
    void* actor = nullptr;
    if (arm.adopt) {
        actor = LiveInstanceOf(arm.cls);
        void* host = PL::Registry::Get().Local();
        ue_wrap::FVector hp{}, pp{};
        if (!actor) return;  // not made yet
        if (!host || !E::TryGetActorLocation(actor, at) || !E::TryGetActorLocation(host, hp) || !E::TryGetActorLocation(puppet, pp)) return;
        g.d0Host = Distance(at, hp);
        g.d0Pup = Distance(at, pp);
    } else if (!PlaceAt(puppet, at, g.d0Host, g.d0Pup)) {
        Verdict(false, arm.name, "reason=the host's or the puppet's pose could not be read");
        To(Step::Done);
        return;
    }
    g.skip = HT::SkipHost();
    g.expect = g.skip ? 1 : (g.d0Host <= g.d0Pup ? 0 : 1);
    if (!arm.adopt) actor = gray ? LoadClassAndSpawn(kControllerClass, kControllerPath, at) : LoadClassAndSpawn(arm.cls, arm.path, at);
    if (!actor) {
        Verdict(false, arm.name, "reason=the %s was not made", gray ? "grayEventController" : "creature");
        To(Step::Done);
        return;
    }
    if (!gray) {
        const EL::ElementId eid = EL::Registry::Get().EidForActor(actor);
        if (eid == EL::kInvalidId) {
            Verdict(false, arm.name, "reason=the creature was not enrolled as an Npc element");
            To(Step::Done);
            return;
        }
    }
    g.creature = actor;
    g.index = R::InternalIndexOf(actor);
    g.startMs = g.nextSampleMs = NowMs();
    g.answered0 = SP::AnsweredCount();
    g.delivered0 = coop::player_damage::DeliveredCount();
    UE_LOGI("[HUNT] %s %s %p at (%.0f, %.0f, %.0f): %.0f cm from the host, %.0f cm from the puppet; skip=%d expect slot %u",
            arm.adopt ? "took" : "spawned", arm.name, actor, at.X, at.Y, at.Z, g.d0Host, g.d0Pup, g.skip ? 1 : 0, static_cast<unsigned>(g.expect));
    To(Step::Run);
}

// The Grays' call: the controller's own `despawn`, through the engine's dispatch (so the gate's watch sees it with no caller frame
// and judges from the controller, which stands where the creature arms' creatures stand).
void CallDespawn(void* controller) {
    void* fn = R::FindFunction(R::ClassOf(controller), L"despawn");
    if (!fn) {
        UE_LOGW("[HUNT] the controller has no despawn function");
        return;
    }
    ue_wrap::ParamFrame f(fn);
    if (f.valid()) ue_wrap::Call(controller, f);
    UE_LOGI("[HUNT] called the controller's despawn");
}

void RunTick(const Arm& arm) {
    const long long now = NowMs();
    const bool gray = arm.cls == nullptr;
    if (!g.creature || !R::IsLiveByIndex(g.creature, g.index)) {
        Verdict(false, arm.name, "reason=the %s ended before the verdict", gray ? "controller" : "creature");
        To(Step::Done);
        return;
    }
    if (gray && !g.called && now - g.startMs >= kGrayCallMs) {
        g.called = true;
        CallDespawn(g.creature);
    }
    if (now < g.nextSampleMs) return;
    g.nextSampleMs = now + kSampleMs;
    ++g.samples;
    coop::dev::restore_vitals::Restore();  // the creature also bites the host, which stands beside the client
    void* puppet = LivePuppet();
    void* host = PL::Registry::Get().Local();
    ue_wrap::FVector cp{}, hp{}, pp{};
    const bool read = puppet && host && E::TryGetActorLocation(g.creature, cp) && E::TryGetActorLocation(host, hp) &&
                      E::TryGetActorLocation(puppet, pp);
    const float dHost = read ? Distance(cp, hp) : -1.f, dPup = read ? Distance(cp, pp) : -1.f;
    if (read) {
        if (dHost < g.minHost) g.minHost = dHost;
        if (dPup < g.minPup) g.minPup = dPup;
    }
    const uint8_t picked = gray ? HT::kNoTarget : HT::TargetOf(g.creature);
    if (picked != HT::kNoTarget && g.firstPick == HT::kNoTarget) g.firstPick = picked;
    const unsigned long long answered = SP::AnsweredCount() - g.answered0;
    const uint32_t delivered = coop::player_damage::DeliveredCount() - g.delivered0;
    UE_LOGI("[HUNT] sample %s t=%llds slot=%d served=%u dHost=%.0f dPuppet=%.0f answered=%llu delivered=%u at(c/h/p)=(%.0f,%.0f)"
            "/(%.0f,%.0f)/(%.0f,%.0f)", arm.name, (now - g.startMs) / 1000, picked == HT::kNoTarget ? -1 : static_cast<int>(picked),
            static_cast<unsigned>(gray ? 0 : SP::ServedSlot(g.creature)), dHost, dPup, answered, delivered, cp.X, cp.Y, hp.X, hp.Y,
            pp.X, pp.Y);

    if (now - g.startMs < (gray ? kGrayWatchMs : kWatchMs)) return;
    // The verdict.
    const unsigned served = gray ? 0u : static_cast<unsigned>(SP::ServedSlot(g.creature));
    bool pass = true;
    std::string why;
    auto fail = [&](const char* r) { pass = false; why += r; why += "; "; };
    if (!gray) {
        if (g.firstPick != g.expect) fail("the first pick was not the expected slot");
        if (g.expect == 1 && answered == 0) fail("no read of the player was answered with the puppet");
        if (g.expect == 0 && (answered != 0 || served != 0)) fail("a read was answered with a client though the host was nearer");
        if (g.expect == 1 && arm.mustClose && !(g.minPup <= g.d0Pup - 150.f || g.minPup <= 300.f)) fail("it did not close on the puppet");
    }
    if (g.expect == 1 && arm.mustDeliver && delivered == 0) fail("no hit was delivered to the client");
    if (g.expect == 0 && gray && delivered != 0) fail("a hit was delivered to the client though the host was nearer");
    Verdict(pass, arm.name,
            "skip=%d expect=%u firstPick=%d served=%u answered=%llu delivered=%u d0Host=%.0f d0Puppet=%.0f minHost=%.0f minPuppet=%.0f %s",
            g.skip ? 1 : 0, static_cast<unsigned>(g.expect), g.firstPick == HT::kNoTarget ? -1 : static_cast<int>(g.firstPick),
            served, answered, delivered, g.d0Host, g.d0Pup, g.minHost, g.minPup, why.c_str());
    To(Step::Done);
}

bool IsFigura(const Arm& arm) { return std::strcmp(arm.name, "figura") == 0; }

// The Figura arm. The host makes a Figura beside the players once the client's puppet is live; the client, from its own
// world-ready edge, waits for a copy of it and says the verdict.
void FiguraTick(const Arm& arm, coop::net::Session& s) {
    const long long now = NowMs();
    if (g.step == Step::Wait) {
        if (s.role() == coop::net::Role::Host ? (s.IsSlotWorldReady(1) && coop::prop_snapshot::IsBracketClosed(1))
                                              : coop::net_pump::HasAnnouncedWorldReady())
            To(Step::WaitPuppet);
        return;
    }
    if (s.role() != coop::net::Role::Host) {
        const size_t copies = coop::owner_entity_sync::MirrorCount(kFiguraClassId);
        if (copies > 0) {
            Verdict(true, "figura", "the host's Figura is shown here as %zu parked copy(ies), %lld ms after this join", copies,
                    now - g.stepMs);
            To(Step::Done);
        } else if (now - g.stepMs >= kAdoptBoundMs) {
            Verdict(false, "figura", "reason=no copy of the host's Figura was shown here in %lld s", kAdoptBoundMs / 1000);
            To(Step::Done);
        }
        return;
    }
    void* puppet = LivePuppet();
    ue_wrap::FVector at{};
    if (!puppet || !PlaceAt(puppet, at, g.d0Host, g.d0Pup)) {
        if (now - g.stepMs >= kBoundMs) {
            Verdict(false, "figura", "reason=slot 1's puppet never became live");
            To(Step::Done);
        }
        return;
    }
    void* figura = LoadClassAndSpawn(arm.cls, arm.path, at);
    UE_LOGI("[HUNT] the host made a Figura %p at (%.0f, %.0f, %.0f); the client says the verdict", figura, at.X, at.Y, at.Z);
    if (!figura) Verdict(false, "figura", "reason=the Figura was not made");
    To(Step::Done);
}

}  // namespace


void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    const Arm* arm = ArmOf();
    if (!arm) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || g.step == Step::Done) return;
    if (IsFigura(*arm)) return FiguraTick(*arm, *s);
    if (s->role() != coop::net::Role::Host) return;
    if (!g_staged && PL::Registry::Get().Local()) {
        g_staged = true;
        const bool moved = coop::teleport_client::ApplyLocally({kStageX, kStageY, kStageZ, 0.f, 0.f, 0.f});
        UE_LOGI("[HUNT] the host goes to the drill's stage at (%.0f, %.0f, %.0f): teleport %s", kStageX, kStageY, kStageZ,
                moved ? "dispatched" : "NOT dispatched");
    }
    if (g.step == Step::Wait) {
        if (s->IsSlotWorldReady(1) && coop::prop_snapshot::IsBracketClosed(1)) To(Step::WaitPuppet);
        return;
    }
    if (g.step == Step::WaitPuppet) {
        if (void* puppet = LivePuppet()) {
            Spawn(*arm, puppet);
            if (g.step == Step::WaitPuppet && NowMs() - g.stepMs >= kAdoptBoundMs) {
                Verdict(false, arm->name, "reason=the game made no %ls in %lld s", arm->cls ? arm->cls : L"creature", kAdoptBoundMs / 1000);
                To(Step::Done);
            }
        } else if (NowMs() - g.stepMs >= kBoundMs) {
            Verdict(false, arm->name, "reason=slot 1's puppet never became live");
            To(Step::Done);
        }
        return;
    }
    RunTick(*arm);
}

void OnDisconnect() { g = State{}; }

}  // namespace coop::dev::hunt_drill
