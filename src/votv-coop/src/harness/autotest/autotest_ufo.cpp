// harness/autotest/autotest_ufo.cpp -- TEST-ONLY: the ambient UFOs' rolled paths cross from the host to a client's
// copies. A verdict is decided by an event; a bound passing with none is INVALID. Armed by the row ufo_drill on both
// peers (VOTVCOOP_CAN_DIR).
//   1     The host calls the gamemode's own ufo_ball (once, until a ball is listed) and waits for it to stand enrolled
//         (the route the game takes: its spawn is the EX drain's, where the lane sends the path), then runs the Midas
//         UFO, whose take-off is its own UfoEvent on the same ordered lane. The client waits for the Midas run (the
//         fence): the path sent before it has arrived by then or never will, so the client says FAIL there when none
//         came. Past the fence it waits for the path to be applied and compares the copy's with the host's:
//         `rig: VERDICT ufo PASS|FAIL|INVALID`. Red: ufo_path_send_off (host). The boo is not driven: the gamemode's
//         ufo_boo needs day 21 and a night hour (mainGamemode ufo_boo, timeZ), which a fresh game never has and the
//         gamemode has no setter for; its path sync is unmeasured.
//   join  With the rig's --rejoin. The host holds a ball (its tick off, so it still flies at the joiner's ready edge),
//         the client leaves and comes back; at the ready edge the host must have sent the joiner a path, and the
//         rejoined client (VOTVCOOP_UFO_DRILL_REJOIN=1) must apply it and read the host's path off its copy:
//         `rig: VERDICT ufo-join ...`. Red: ufo_path_send_off (host).

#include "harness/autotest.h"
#include "disc_drills.h"      // co-located private headers (src tree, not include/)
#include "gt_wait.h"
#include "rejoin_steps.h"
#include "harness/autotest/peer_signal.h"
#include "harness/session_runtime.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/death_seam.h"
#include "coop/element/registry.h"
#include "coop/session/rig_verdict.h"
#include "coop/world/ufo_sync.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"   // DestroyActor
#include "ue_wrap/world/keyed_objects.h"
#include "ue_wrap/world/world_singleton.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace EL = coop::element;
namespace PS = harness::autotest::peer_signal;
namespace RV = coop::rig_verdict;
namespace US = coop::ufo_sync;
using disc_drill::OnGameThread;

constexpr const char* kTag = "ufo";
constexpr const char* kTagJoin = "ufo-join";
constexpr int kStepSec = 120;
constexpr int kSummonSec = 90;  // INVALID bound: the gamemode's own timer rolls a chance, so it is called until a UFO comes

std::string Arm() { return coop::config::ResolveEnum(coop::config_registry::rows::ufo_drill); }

// A path as the two peers compare it: how many points and the sum of each axis.
struct Sum {
    uint32_t n = 0;
    float x = 0.f, y = 0.f, z = 0.f;
};

Sum SumOf(const std::vector<ue_wrap::FVector>& pts) {
    Sum s;
    s.n = static_cast<uint32_t>(pts.size());
    for (const ue_wrap::FVector& v : pts) {
        s.x += v.X;
        s.y += v.Y;
        s.z += v.Z;
    }
    return s;
}

std::string Text(uint32_t eid, const Sum& s) {
    char b[128];
    std::snprintf(b, sizeof(b), "%u %u %.3f %.3f %.3f", eid, s.n, s.x, s.y, s.z);
    return b;
}

bool Parse(const char* text, uint32_t& eid, Sum& s) {
    return sscanf_s(text, "%u %u %f %f %f", &eid, &s.n, &s.x, &s.y, &s.z) == 5;
}

bool Same(const Sum& a, const Sum& b) {
    return a.n == b.n && std::fabs(a.x - b.x) < 1.f && std::fabs(a.y - b.y) < 1.f && std::fabs(a.z - b.z) < 1.f;
}

// The first live UFO of one of the classes; its element id, or kInvalidId while it is not enrolled yet.
void* FirstLive(std::initializer_list<const wchar_t*> classes, uint32_t& eid) {
    struct Ctx { void* found = nullptr; uint32_t eid = EL::kInvalidId; } ctx;
    for (const wchar_t* cls : classes) {
        void* c = ue_wrap::object_index::ClassByName(cls);
        if (!c) continue;
        ue_wrap::object_index::ForEachInstance(c, [](void* p, void* obj, int32_t) {
            auto& x = *static_cast<Ctx*>(p);
            if (x.found || !obj || !R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
            x.found = obj;
            x.eid = static_cast<uint32_t>(EL::Registry::Get().EidForActor(obj));
        }, &ctx);
        if (ctx.found) break;
    }
    eid = ctx.eid;
    return ctx.found;
}

bool CallGamemode(const wchar_t* fn) {
    void* gm = ue_wrap::world_singleton::Gamemode();
    void* f = gm ? R::FindFunction(R::ClassOf(gm), fn) : nullptr;
    ue_wrap::ParamFrame frame(f);
    return f && frame.valid() && ue_wrap::Call(gm, frame);
}

// The gamemode's own timer function, called while no UFO of the classes is listed (it rolls a chance, and a fresh
// instance is listed from the next pass), then waited on until the UFO stands enrolled (the drain that enrols it has
// run: the spawn and the path went out in the same task). The path is read off the host's UFO. False when the bound
// passed.
bool Summon(const wchar_t* fn, std::initializer_list<const wchar_t*> classes, uint32_t& eid, Sum& sum, void** actorOut) {
    return PS::WaitFor("the gamemode's UFO enrolled", kSummonSec, [&] {
        bool found = false;
        OnGameThread([&] {
            void* a = FirstLive(classes, eid);
            if (!a) {
                CallGamemode(fn);
                return;
            }
            std::vector<ue_wrap::FVector> pts;
            if (eid != EL::kInvalidId && US::DevReadPath(a, pts)) {
                sum = SumOf(pts);
                if (actorOut) *actorOut = a;
                found = true;
            }
        });
        return found;
    });
}

bool RunMidas() {
    bool ran = false;
    OnGameThread([&] {
        void* midas = ue_wrap::keyed_objects::Resolve(L"midasufo");
        ue_wrap::ParamFrame f(midas ? R::FindFunction(R::ClassOf(midas), L"runTrigger") : nullptr);
        if (f.valid()) {
            f.Set<int32_t>(L"index", 0);
            ran = ue_wrap::Call(midas, f);
        }
    });
    return ran;
}

// This peer's copy of the UFO `eid`: its path summary, once the copy reads one.
bool CopySum(uint32_t eid, Sum& out) {
    bool ok = false;
    OnGameThread([&] {
        void* copy = EL::LiveActorOfType(static_cast<EL::ElementId>(eid), EL::ElementType::WorldActor);
        std::vector<ue_wrap::FVector> pts;
        if (copy && US::DevReadPath(copy, pts)) {
            out = SumOf(pts);
            ok = true;
        }
    });
    return ok;
}

// The cause of a world actor's end, as the engine's drain hands it out (the host's and a client's copy alike).
void OnWorldActorEnd(const coop::element::death_seam::Death& d) {
    UE_LOGI("ufo-drill: world actor eid %u ended (mirror=%d, streamed out=%d)", static_cast<unsigned>(d.eid), d.mirror ? 1 : 0,
            d.streamedOut ? 1 : 0);
}

// The copy of the UFO `eid` here. The first time it reads not live after it was, or after the lane applied a path to it, its
// registry row, its actor and the actor's liveness are said once, so a copy that died names how.
bool CopyLive(uint32_t eid) {
    static bool s_wasLive = false, s_said = false;
    bool live = false;
    OnGameThread([&] {
        live = EL::LiveActorOfType(static_cast<EL::ElementId>(eid), EL::ElementType::WorldActor) != nullptr;
        if (live) {
            s_wasLive = true;
        } else if (!s_said && (s_wasLive || US::Read().pathsApplied >= 1)) {
            s_said = true;
            EL::Element* row = EL::Registry::Get().Get(static_cast<EL::ElementId>(eid));
            void* act = row ? row->GetActor() : nullptr;
            UE_LOGI("ufo-drill: copy eid %u is not live (was live=%d): registry row=%d, actor=%p, actor live=%d, paths applied=%u",
                    eid, s_wasLive ? 1 : 0, row ? 1 : 0, act, act && R::IsLive(act) ? 1 : 0, US::Read().pathsApplied);
        }
    });
    return live;
}

// ---- 1 ----

void RunHost() {
    if (!disc_drill::HostWorldUp()) return;
    if (!PS::Await("client_ready", 480)) return;
    if (!WaitGT("the client's slot world-ready", 120, [] { return harness::session_runtime::Session().IsSlotWorldReady(1); })) return;
    uint32_t ballEid = 0;
    Sum ball;
    if (!Summon(L"ufo_ball", {L"ufo_ballfo_C"}, ballEid, ball, nullptr)) return;
    UE_LOGI("ufo-drill: HOST ball eid %u, path %hs", ballEid, Text(ballEid, ball).c_str());
    PS::Mark("ufos", Text(ballEid, ball));
    if (!PS::Await("copies", kStepSec)) return;
    // The fence: the Midas take-off is a UfoEvent after the ball's path, on the same ordered lane.
    if (!RunMidas()) {
        RV::Say(kTag, RV::Outcome::Invalid, "the level's Midas UFO could not be run: no fence for the path");
        return;
    }
    UE_LOGI("ufo-drill: HOST ran the Midas UFO after the path -- the fence");
    std::string done;
    if (!PS::AwaitText("client_done", done, kStepSec)) return;
    if (done[0] != 'P') return;  // the client said its FAIL where it was decided
    US::Counters c;
    OnGameThread([&] { c = US::Read(); });
    RV::Say(kTag, RV::Outcome::Pass, "a ball flew the host's path on the client (the host sent %u path(s))", c.pathsSent);
    UE_LOGI("ufo-drill: DONE on host");
}

void RunClient() {
    if (!disc_drill::HostPuppetUp() || !disc_drill::ClientJoined()) return;
    OnGameThread([] { coop::element::death_seam::Subscribe(EL::ElementType::WorldActor, &OnWorldActorEnd); });
    PS::Mark("client_ready");
    std::string text;
    if (!PS::AwaitText("ufos", text, kStepSec)) return;
    uint32_t ballEid = 0;
    Sum ball;
    if (!Parse(text.c_str(), ballEid, ball)) {
        RV::Say(kTag, RV::Outcome::Invalid, "the host's UFO summary '%s' does not parse", text.c_str());
        return;
    }
    if (!WaitGT("the host's ball copy", 60, [&] { return CopyLive(ballEid); })) return;
    PS::Mark("copies");
    // The fence: the host's Midas run, sent after the path.
    US::Counters c;
    if (!WaitGT("the host's Midas run received", kStepSec, [&] { return US::Read().midasRuns >= 1; })) return;
    OnGameThread([&] { c = US::Read(); });
    if (c.pathsReceived < 1) {
        RV::Say(kTag, RV::Outcome::Fail, "the host's Midas run arrived and the ball's path, sent before it, had not");
        PS::Mark("client_done", "F");
        return;
    }
    if (!WaitGT("the path applied to the copy", 30, [] { return US::Read().pathsApplied >= 1; })) return;
    Sum mine;
    if (!CopySum(ballEid, mine)) {
        RV::Say(kTag, RV::Outcome::Invalid, "the copy's path could not be read");
        return;
    }
    if (!Same(ball, mine)) {
        RV::Say(kTag, RV::Outcome::Fail, "the copy flies another path than the host's: %s vs %s", Text(ballEid, ball).c_str(),
                Text(ballEid, mine).c_str());
        PS::Mark("client_done", "F");
        return;
    }
    PS::Mark("client_done", "P");
    UE_LOGI("ufo-drill: DONE on client");
}

// ---- join ----

void RunJoinHost() {
    if (!disc_drill::HostWorldUp()) return;
    if (!PS::Await("client_ready", 480)) return;
    if (!WaitGT("the client's slot world-ready", 120, [] { return harness::session_runtime::Session().IsSlotWorldReady(1); })) return;
    uint32_t eid = 0;
    Sum ball;
    void* actor = nullptr;
    OnGameThread([] { coop::element::death_seam::Subscribe(EL::ElementType::WorldActor, &OnWorldActorEnd); });
    if (!Summon(L"ufo_ball", {L"ufo_ballfo_C"}, eid, ball, &actor)) return;
    // Held where it is: its tick off, so it has not flown to its end when the next life's ready edge comes.
    OnGameThread([&] { E::SetActorTickEnabled(actor, false); });
    UE_LOGI("ufo-drill: HOST holds ball eid %u, path %hs", eid, Text(eid, ball).c_str());
    PS::Mark("held");
    if (!PS::Await("client_done", kStepSec)) return;
    const uint32_t gen = rejoin::FirstLifeGen();
    PS::Mark("host_ack");
    uint32_t sent0 = 0, sentAtEdge = 0;   // the counter is monotonic, so the delta cannot underflow
    OnGameThread([&] { sent0 = US::Read().pathsSent; });
    if (!rejoin::AwaitNextLifeReady(gen, [&] { sentAtEdge = US::Read().pathsSent - sent0; })) return;
    // The ball itself is asked, not the registry: the last client's leave drained its element, and the joiner's edge enrols it again
    // under a new id, which is the id the joiner's copy carries.
    bool flying = false;
    uint32_t edgeEid = EL::kInvalidId;
    OnGameThread([&] {
        flying = R::IsLive(actor);
        if (flying) edgeEid = static_cast<uint32_t>(EL::Registry::Get().EidForActor(actor));
    });
    UE_LOGI("ufo-drill: HOST the rejoined client's world is up; %u path(s) sent at its edge", sentAtEdge);
    if (!flying) {
        RV::Say(kTagJoin, RV::Outcome::Invalid, "the held ball was gone at the client's ready edge");
        return;
    }
    // Decided at the edge: a UFO still flying and no path told to the joiner cannot change later.
    if (sentAtEdge == 0) {
        RV::Say(kTagJoin, RV::Outcome::Fail, "the host's ball was flying at the client's ready edge and the host sent the joiner no path");
        return;
    }
    PS::Mark("path_sent", Text(edgeEid, ball));
    std::string done;
    if (!PS::AwaitText("rejoin_done", done, kStepSec)) return;
    OnGameThread([&] { E::DestroyActor(actor); });
    if (done[0] == 'P') RV::Say(kTagJoin, RV::Outcome::Pass, "the joiner's ball copy flies the host's path (%u path(s) sent at its edge)", sentAtEdge);
    UE_LOGI("ufo-drill: JOIN DONE on host");
}

void RunJoinClient() {
    if (!disc_drill::HostPuppetUp() || !disc_drill::ClientJoined()) return;
    PS::Mark("client_ready");
    if (!PS::Await("held", kStepSec)) return;
    PS::Mark("client_done");
    PS::Await("host_ack", 60);
    rejoin::LeaveForRejoin("ufo drill: life 1 leaves for the rejoin");
    UE_LOGI("ufo-drill: DONE on client");
}

void RunRejoin() {
    if (!disc_drill::HostPuppetUp()) return;
    OnGameThread([] { coop::element::death_seam::Subscribe(EL::ElementType::WorldActor, &OnWorldActorEnd); });
    std::string text;
    if (!PS::AwaitText("path_sent", text, kStepSec)) return;   // the host's path reached the edge: the joiner's must come
    uint32_t eid = 0;
    Sum host;
    if (!Parse(text.c_str(), eid, host)) {
        PS::Mark("rejoin_done", "F bad text");
        return;
    }
    if (!WaitGT("the joiner's ball copy", 60, [&] { return CopyLive(eid); }) ||
        !WaitGT("the host's path received and applied here", kStepSec, [] { return US::Read().pathsApplied >= 1; })) {
        PS::Mark("rejoin_done", "I");
        return;
    }
    Sum mine;
    const bool read = CopySum(eid, mine);
    const bool same = read && Same(host, mine);
    UE_LOGI("ufo-drill: REJOIN the copy reads %hs, the host's %hs", Text(eid, mine).c_str(), Text(eid, host).c_str());
    if (!same) RV::Say(kTagJoin, RV::Outcome::Fail, "the joiner's ball copy flies another path than the host's (%s vs %s)", Text(eid, mine).c_str(), Text(eid, host).c_str());
    PS::Mark("rejoin_done", same ? "P" : "F");
    UE_LOGI("ufo-drill: REJOIN DONE");
}

}  // namespace

DWORD WINAPI UfoDrillThread(LPVOID /*arg*/) {
    const bool join = Arm() == "join";
    if (!PS::Begin(join ? kTagJoin : kTag)) return 0;
    if (!IsClientRole()) {
        if (join) RunJoinHost();
        else RunHost();
    } else if (!join) {
        RunClient();
    } else if (coop::config::ReadEnv("VOTVCOOP_UFO_DRILL_REJOIN") == "1") {
        RunRejoin();
    } else {
        RunJoinClient();
    }
    return 0;
}

}  // namespace harness::autotest
