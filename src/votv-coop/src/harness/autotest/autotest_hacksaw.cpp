// harness/autotest/autotest_hacksaw.cpp -- TEST-ONLY: a big prop sawn apart, and what every peer sees of it.
//
// One peer (the client, or the host with VOTVCOOP_HACKSAW_WHO=host) takes a hacksaw into its hand by the game's own hold.
// The HOST spawns a desk about 120 cm in front of that peer's body, facing it, filled with two discs; the peer turns its
// camera until its own interaction trace is on it and starts the cut by the saw's use verb, so the saw sets its target
// itself. A client's saw asks the host before its cut completes; the host grants it, and its copy breaks when the
// saw's destroy of the prop arrives. Each peer counts the scrap and the other props within 4 m before the cut and again
// once the prop is gone. The host says `rig: VERDICT hacksaw PASS|FAIL|INVALID`: PASS when the prop is gone on both peers
// and both count the same new scrap (at least one, what the saw spawned) and other props (a container's spill).
// VOTVCOOP_HACKSAW_ARM=refused with the host's row sawn_refuse_all is the refusal arm: PASS when the client is answered,
// its saw ends its cut and the prop stands on both peers. A saw that cut without asking the host (hacksaw_request_off)
// says FAIL once the prop is gone. Every wait is an event; a bound says INVALID. Gated by VOTVCOOP_RUN_HACKSAW; the
// peers' folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"
#include "harness/autotest/peer_signal.h"

#include "coop/config/config.h"
#include "coop/dev/director/aim_fan.h"
#include "coop/dev/drill_hand.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/container_break.h"
#include "coop/props/hacksaw_cut.h"
#include "coop/session/host_world_settle.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"
#include "coop/session/world_load_episode.h"
#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_gather.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_attach.h"   // GetAttachParentActor
#include "ue_wrap/engine/engine_pawn.h"  // DestroyActor

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace PS = harness::autotest::peer_signal;
namespace R = ue_wrap::reflection;
namespace RV = coop::rig_verdict;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

constexpr const char* kTag = "hacksaw";
constexpr int kStepSec = 120;
constexpr float kArmCm = 200.f;      // the saw's trace: mainPlayer's armLength with no weapon row (mainPlayer.offsets.txt:11130)
constexpr float kFixtureAheadCm = 120.f;   // the fixture stands this far in front of the acting peer, on level ground
constexpr float kFixtureClearCm = 60.f;   // the line to the fixture's spot runs this far past it: no wall within the desk's reach
constexpr float kFixtureAboveFloorCm = 20.f;   // its spawn point above the floor the trace found (the body's centre is about 90)
constexpr float kFloorSearchCm = 300.f;   // the floor trace runs this far below the body's centre
constexpr const wchar_t* kFixturePath = L"/Game/objects/prop_container_desk.prop_container_desk_C";
constexpr const wchar_t* kDiscPath = L"/Game/objects/prop_floppyDisc_R.prop_floppyDisc_R_C";
constexpr float kNearCm = 400.f;
constexpr float kCutSec = 0.5f;   // the saw's cut lasts this long once started (its own is the prop's volume times its density)

// A peer that said FAIL or INVALID tells the other to stop, with its reason: the other peer says INVALID at once with it,
// instead of waiting out its own bound for a signal that will not come.
bool Aborted() {
    std::string s;
    return PS::Peek("abort", s);
}

void GiveUp(RV::Outcome outcome, const char* fmt, ...) {
    char reason[384] = {};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(reason, sizeof(reason), fmt, args);
    va_end(args);
    for (char* c = reason; *c; ++c)
        if (*c == '\n' || *c == '\r') *c = ' ';
    RV::Say(kTag, outcome, "%s", reason);
    PS::Mark("abort", reason);
}

// Polls an event every 100 ms; a bound passed says INVALID (PS) and stops the other peer; the other peer's abort ends the
// wait at once and says INVALID with its reason.
template <class Pred>
bool Wait(const char* what, int seconds, Pred pred) {
    bool byPeer = false;
    const bool ok = PS::WaitFor(what, seconds, [&] {
        if (pred()) return true;
        byPeer = Aborted();
        return byPeer;
    });
    if (!ok) {
        PS::Mark("abort", std::string(what) + " not within " + std::to_string(seconds) + "s");
        return false;
    }
    if (!byPeer && !Aborted()) return true;
    std::string why;
    PS::Peek("abort", why);
    RV::Say(kTag, RV::Outcome::Invalid, "the other peer gave up while this one waited for %s: %s", what, why.c_str());
    return false;
}

bool WaitText(const char* name, std::string& out, int seconds = kStepSec) {
    return Wait(name, seconds, [&] { return PS::Peek(name, out); });
}

// HOST: its player stands and its world has settled, so what the drill cuts is not taken back by the load.
bool HostWorldUp() {
    return Wait("the host's player", 240, [] {
               void* pawn = nullptr;
               OnGameThread([&] { pawn = coop::players::Registry::Get().Local(); });
               return pawn != nullptr;
           }) &&
           Wait("the host's world settled", 120, [] {
               bool settled = false;
               OnGameThread([&] {
                   settled = coop::host_world_settle::Settled() && ue_wrap::object_index::IsSeeded() &&
                             ue_wrap::object_index::Backlog() == 0;
               });
               return settled;
           });
}

// CLIENT: its world is announced, the host's puppet stands here, and the join's reconcile window is over (rig: READY
// joined), whose churn a client's births and destroys sit out.
bool HostPuppetUp() {
    return Wait("the announced world", 480, [] {
               bool ready = false;
               OnGameThread([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
               return ready;
           }) &&
           Wait("the host's puppet", 60, [] {
               bool have = false;
               OnGameThread([&] {
                   coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(0);
                   have = p && p->valid();
               });
               return have;
           }) &&
           Wait("the reconcile window over", kStepSec, [] {
               bool over = false;
               OnGameThread([&] { over = !coop::world_load_episode::InReconcileWindow(); });
               return over;
           });
}

bool HostActs() { return coop::config::ReadEnv("VOTVCOOP_HACKSAW_WHO") == "host"; }

struct Target {
    std::wstring cls, key;
    ue_wrap::FVector at{};
    bool container = false;   // the actor's own pick only
};

bool IsScrap(void* p) { return ue_wrap::prop::GetPropNameString(p).rfind(L"scrap_", 0) == 0; }

int ScrapNear(const ue_wrap::FVector& at) {
    int n = 0;
    OnGameThread([&] {
        for (void* p : R::FindObjectsByClass(L"prop_C")) {
            if (!R::IsLive(p) || !IsScrap(p)) continue;
            ue_wrap::FVector l{};
            if (!ue_wrap::engine::TryGetActorLocation(p, l)) continue;
            const float dx = l.X - at.X, dy = l.Y - at.Y, dz = l.Z - at.Z;
            if (dx * dx + dy * dy + dz * dz <= kNearCm * kNearCm) ++n;
        }
    });
    return n;
}

// Every prop within kNearCm of the target but itself and the scrap: what a container's spill adds. A nearest-walk
// per prop, so a handful of walks.
int OthersNear(const Target& t) {
    int n = 0;
    OnGameThread([&] {
        std::vector<void*> seen;
        for (int i = 0; i < 40; ++i) {
            const ue_wrap::prop::NearestResult r =
                ue_wrap::prop::FindNearest(t.at, false, nullptr, nullptr, {seen.data(), seen.size()});
            if (!r.prop || r.dist > kNearCm) break;
            seen.push_back(r.prop);
            if (r.keyString == t.key || IsScrap(r.prop)) continue;
            ++n;
        }
    });
    return n;
}

bool StillThere(const Target& t) {
    bool there = false;
    OnGameThread([&] {
        for (void* p : R::FindObjectsByClass(t.cls.c_str()))
            if (R::IsLive(p) && ue_wrap::prop::GetInteractableKeyString(p) == t.key) there = true;
    });
    return there;
}

template <class T>
bool WriteField(void* obj, const wchar_t* name, const T& v) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(obj), name);
    if (off < 0) return false;
    *reinterpret_cast<T*>(static_cast<uint8_t*>(obj) + off) = v;
    return true;
}

void* g_saw = nullptr;  // the actor's saw, in its hand, from PrepareSaw to the cut

// The saw in this peer's hand: the game's own hold (the hold slot written, then updateHold), tried each second until the
// held actor is a hacksaw; a client then waits for its hand lane to announce it, so the host judges its cut against that
// hand. The drill measures the judge, not the residual of a cut made before its announce.
void* TakeSaw(bool client) {
    void* saw = nullptr;
    const bool held = Wait("the saw in hand", 60, [&] {
        OnGameThread([&] {
            void* me = coop::players::Registry::Get().Local();
            ue_wrap::engine::MainPlayerGrabState gs{};
            void* h = me && ue_wrap::engine::ReadMainPlayerGrabState(me, gs) ? gs.holdingActor : nullptr;
            if (h && R::IsLive(h) && R::ClassNameOf(h) == L"prop_hacksaw_C") saw = h;
            else coop::dev::drill_hand::TakeIntoHandNow(L"hacksaw", L"prop_hacksaw_C");
        });
        return saw != nullptr;
    });
    if (!held) return nullptr;
    if (client && !Wait("the hand lane's announce of the saw", 60, [] {
            bool a = false;
            OnGameThread([&] { a = coop::hand_item::OwnAnnounced(L"prop_hacksaw_C"); });
            return a;
        }))
        return nullptr;
    return saw;
}

// A float of the saw's, or -1 when it does not read. Game thread.
float SawFloat(const wchar_t* name) {
    const int32_t off = g_saw && R::IsLive(g_saw) ? R::FindPropertyOffset(R::ClassOf(g_saw), name) : -1;
    return off >= 0 ? *reinterpret_cast<const float*>(static_cast<const uint8_t*>(g_saw) + off) : -1.f;
}

// SawFloat from the drill thread.
float SawFloatNow(const wchar_t* name) {
    float v = -1.f;
    OnGameThread([&] { v = SawFloat(name); });
    return v;
}

// The actor's cut, part one: the saw in hand. Nothing is written into it here: its target is set by its own trace, and its
// duration by its own start of the cut, which a drill shortens once that start has run (BeginCut).
bool PrepareSaw(bool client) {
    void* saw = TakeSaw(client);
    if (!saw) return false;
    OnGameThread([&] {
        g_saw = saw;
    });
    return true;
}

// The live prop of the target, by class and key. Game thread.
void* FindProp(const Target& t) {
    for (void* p : R::FindObjectsByClass(t.cls.c_str()))
        if (R::IsLive(p) && ue_wrap::prop::GetInteractableKeyString(p) == t.key) return p;
    return nullptr;
}

// The nearest point of the prop's colliding bounds to `from` (the box clamped), and its distance. False without bounds.
bool NearestSurface(void* prop, const ue_wrap::FVector& from, ue_wrap::FVector& nearest, float& dist) {
    ue_wrap::FVector o{}, e{};
    if (!ue_wrap::engine::GetActorBounds(prop, true, o, e)) return false;
    auto clampTo = [](float v, float c, float h) { return v < c - h ? c - h : v > c + h ? c + h : v; };
    nearest = {clampTo(from.X, o.X, e.X), clampTo(from.Y, o.Y, e.Y), clampTo(from.Z, o.Z, e.Z)};
    const float dx = nearest.X - from.X, dy = nearest.Y - from.Y, dz = nearest.Z - from.Z;
    dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    return true;
}

// The inventory's own addObject, once the container's inventory has run its init (before that its arrays are not built).
bool PutIn(void* container, void* item) {
    if (!ue_wrap::container_inventory::InventoryInitialized(container)) return false;
    void* inv = ue_wrap::container_inventory::InventoryOf(container);
    void* fn = inv ? R::FindDispatchFunctionCached(R::ClassOf(inv), L"addObject") : nullptr;
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid() || !f.Set<void*>(L"actor", item) || !f.Set<int32_t>(L"insertIndex1", -1) || !ue_wrap::Call(inv, f))
        return false;
    return f.Get<bool>(L"return");
}

// The body of the peer that acts, on the game thread: the host's own player, or the client's puppet once it stands (spawned
// and given its first pose, before which its position is the spawn placeholder).
bool ActingBody(void*& body, ue_wrap::FVector& at) {
    body = nullptr;
    if (HostActs()) body = coop::players::Registry::Get().Local();
    else if (coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(1))
        body = rp->valid() && rp->HasPose() ? rp->GetActor() : nullptr;
    return body && ue_wrap::engine::TryGetActorLocation(body, at);
}

// Part two, on the HOST: the sawable fixture, a desk (the heavy, wooden container the saw takes), is spawned on level
// ground about kFixtureAheadCm in front of the acting peer's body, facing it, and filled with two discs so a cut has a spill
// to compare. The acting peer is the client's puppet, or the host's own player with VOTVCOOP_HACKSAW_WHO=host. The host
// waits for that body to stand; the spot is the floor a trace down from the point in front finds, and a wall between the
// body and the spot says INVALID with what the traces hit. The drill then waits for the container's inventory to be up and
// its key to read; no walk, no teleport: nothing stands between them.
bool SpawnFixture(Target& t) {
    if (!Wait("the acting peer's body with a pose", 240, [] {
            bool up = false;
            OnGameThread([&] {
                void* b = nullptr;
                ue_wrap::FVector l{};
                up = ActingBody(b, l);
            });
            return up;
        }))
        return false;
    void* fixture = nullptr;
    ue_wrap::FVector at{}, me{};
    float yaw = 0.f;
    const char* why = "";
    OnGameThread([&] {
        void* body = nullptr;
        if (!ActingBody(body, me)) {
            why = "the acting peer's body went away";
            return;
        }
        const ue_wrap::FVector f = ue_wrap::engine::GetActorForwardVector(body);
        const uint8_t types[2] = {0, 1};   // WorldStatic, WorldDynamic: the set LineBlockedStatDyn traces
        ue_wrap::trace::Hit wall, floor;
        const float reach = kFixtureAheadCm + kFixtureClearCm;
        const ue_wrap::FVector past = {me.X + f.X * reach, me.Y + f.Y * reach, me.Z};
        const ue_wrap::FVector above = {me.X + f.X * kFixtureAheadCm, me.Y + f.Y * kFixtureAheadCm, me.Z};
        const bool wallRan = ue_wrap::trace::LineHitForObjects(body, me, past, types, 2, nullptr, 0, &wall);
        const bool floorRan = ue_wrap::trace::LineHitForObjects(body, above, {above.X, above.Y, above.Z - kFloorSearchCm}, types, 2,
                                                                nullptr, 0, &floor);
        UE_LOGI("hacksaw: HOST traces for the fixture from the body at %.0f,%.0f,%.0f facing %.2f,%.2f: wall ran=%d blocked=%d "
                "by %p (%ls); floor ran=%d blocked=%d at z %.0f", me.X, me.Y, me.Z, f.X, f.Y, wallRan ? 1 : 0, wall.blocked ? 1 : 0,
                wall.actor, wall.actor ? R::ClassNameOf(wall.actor).c_str() : L"nothing", floorRan ? 1 : 0, floor.blocked ? 1 : 0,
                floor.point.Z);
        if (!wallRan || !floorRan) { why = "a trace did not run"; return; }
        if (wall.blocked) { why = "a wall stands between the body and the fixture's spot"; return; }
        if (!floor.blocked) { why = "no floor under the fixture's spot"; return; }
        at = {above.X, above.Y, floor.point.Z + kFixtureAboveFloorCm};
        yaw = std::atan2(-f.Y, -f.X) * 57.29578f;   // facing the body
        void* cls = ue_wrap::asset_load::LoadObjectByPath(kFixturePath);
        if (!cls) { why = "the desk's class did not load"; return; }
        fixture = ue_wrap::engine::SpawnActor(cls, at);
        if (!fixture) { why = "SpawnActor returned null"; return; }
        ue_wrap::engine::SetActorRotation(fixture, ue_wrap::FRotator{0.f, yaw, 0.f});
    });
    if (!fixture) {
        UE_LOGW("hacksaw: HOST could not spawn the fixture: %s (body at %.0f,%.0f,%.0f, spot %.0f,%.0f,%.0f)", why, me.X, me.Y,
                me.Z, at.X, at.Y, at.Z);
        GiveUp(RV::Outcome::Invalid, "the fixture could not be spawned in front of the %s: %s", HostActs() ? "host" : "client", why);
        return false;
    }
    int put = 0;
    if (!Wait("the fixture's inventory up and filled", 60, [&] {
            OnGameThread([&] {
                for (; put < 2 && fixture && R::IsLive(fixture) && ue_wrap::container_inventory::InventoryInitialized(fixture); ++put) {
                    ue_wrap::FVector p{};
                    ue_wrap::engine::TryGetActorLocation(fixture, p);
                    void* cls = ue_wrap::asset_load::LoadObjectByPath(kDiscPath);
                    void* disc = cls ? ue_wrap::engine::SpawnActor(cls, {p.X, p.Y, p.Z + 60.f + 20.f * static_cast<float>(put)}) : nullptr;
                    const bool in = disc && PutIn(fixture, disc);
                    if (disc) ue_wrap::engine::DestroyActor(disc);
                    if (!in) break;
                }
            });
            return put == 2;
        }))
        return false;
    std::wstring key;
    if (!Wait("the fixture's key", 60, [&] {
            OnGameThread([&] { key = ue_wrap::prop::GetInteractableKeyString(fixture); });
            return !key.empty() && key != L"None";
        }))
        return false;
    OnGameThread([&] {
        ue_wrap::engine::TryGetActorLocation(fixture, t.at);
        t.cls = R::ClassNameOf(fixture);
        t.key = key;
        t.container = true;
    });
    UE_LOGI("hacksaw: HOST spawns the fixture '%ls' key '%ls' at %.0f,%.0f,%.0f (it settled at %.0f,%.0f,%.0f), %.0f cm in front "
            "of the %s, facing it (yaw %.0f)", t.cls.c_str(), t.key.c_str(), at.X, at.Y, at.Z, t.at.X, t.at.Y, t.at.Z,
            kFixtureAheadCm, HostActs() ? "host" : "client", yaw);
    return true;
}

// One parameterless bool function, read as the engine answers it: 1 or 0, -1 when it did not run, 2 when its answer did not
// read. Game thread.
int BoolCall(void* obj, const wchar_t* fnName) {
    void* fn = obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), fnName) : nullptr;
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid() || !ue_wrap::Call(obj, f)) return -1;
    bool v = false;
    for (const wchar_t* name : {L"ReturnValue", L"return"})
        if (f.GetRaw(name, &v, static_cast<int32_t>(sizeof(v)))) return v ? 1 : 0;
    return 2;
}

// The saw's start-of-cut gates as the Blueprint applies them to the prop it took (prop_hacksaw.cpp, the label after its
// target is set: simulating physics on the prop's StaticMesh, propData.heavy, processMat, isNotSawable), and whether its
// tick is on afterwards: a refused gate leaves the target set and the tick off. Said once, at the start. Game thread.
void LogStartGates(void* prop) {
    void* mesh = ue_wrap::prop::GetStaticMesh(prop);
    const int sim = mesh ? (ue_wrap::engine::IsComponentSimulatingPhysics(mesh) ? 1 : 0) : -1;
    ue_wrap::prop_gather::PropDataFlags pd{};
    const bool pdRead = ue_wrap::prop_gather::ReadPropData(prop, pd);
    UE_LOGI("hacksaw: the saw's start gates on the client's copy %p: StaticMesh %p simulating=%d (the BP needs 1), heavy=%d "
            "(read=%d, needs 1), processMat=%d (needs 1), isNotSawable=%d (needs 0); the saw's tick enabled afterwards=%d",
            prop, mesh, sim, pdRead && pd.heavy ? 1 : 0, pdRead ? 1 : 0, BoolCall(g_saw, L"processMat"),
            BoolCall(prop, L"isNotSawable"), BoolCall(g_saw, L"IsActorTickEnabled"));
}

// Part three: the cut starts as the player's does. The camera is turned through the director's aim fan until the player's
// own interaction trace (hitResult) is on the prop, and the saw's own use verb (playerHandUse_LMB, which the left click
// calls) then reads that trace, runs the saw's gates and sets its own target and tick. Nothing is written into the saw.
bool BeginCut(const Target& t) {
    coop::director::AimFan fan;
    bool started = false, failed = false;
    const char* why = "";
    int polls = 0;
    if (!Wait("the saw's own trace takes the prop", kStepSec, [&] {
            OnGameThread([&] {
                void* me = coop::players::Registry::Get().Local();
                void* prop = FindProp(t);
                ue_wrap::FVector at{};
                float d = 0;
                // The camera is turned toward the prop's nearest surface point: the trace is only as long as the saw's arm.
                if (!me || !prop || !g_saw || !NearestSurface(prop, ue_wrap::engine::GetCameraLocation(), at, d)) {
                    failed = true;
                    why = "the player, the prop or the saw is gone";
                    return;
                }
                // The trace's actor is the prop or a part attached to it.
                void* hit = ue_wrap::engine::ReadMainPlayerHitActor(me);
                void* hitParent = nullptr;
                if (hit && hit != prop) ue_wrap::engine::GetAttachParentActor(hit, hitParent);
                if (polls++ % 30 == 0)
                    UE_LOGI("hacksaw: aim pose %d: the player's trace hits %p (%ls), the prop is %p, the surface point is %.0f cm from "
                            "the camera", fan.Poses(), hit, hit ? R::ClassNameOf(hit).c_str() : L"nothing", prop, d);
                const auto st = fan.Tick(me, at, hit == prop || (hit && hitParent == prop));
                if (st == coop::director::AimFan::State::Failed) {
                    failed = true;
                    why = "the player's trace never took the prop through the whole aim fan";
                    return;
                }
                if (st != coop::director::AimFan::State::Aimed) return;
                void* fn = R::FindDispatchFunctionCached(R::ClassOf(g_saw), L"playerHandUse_LMB");
                ue_wrap::ParamFrame f(fn);
                started = fn && f.valid() && f.Set<void*>(L"player", me) && ue_wrap::Call(g_saw, f);
                failed = !started;
                why = "the saw's playerHandUse_LMB did not run";
                if (!started) return;
                // The saw's start of a cut, once its gates pass, sets `time` from the prop's volume and material (the
                // Blueprint, at the label after its gates): a drill would not wait those seconds out, so the duration is
                // shortened after it.
                const float own = SawFloat(L"time");
                WriteField<float>(g_saw, L"time", kCutSec);
                UE_LOGI("hacksaw: the saw started its cut: its time after the start was %.1f s (shortened to %.1f), sawingProp %p, "
                        "complete %.2f", own, kCutSec, prop, SawFloat(L"complete"));
                LogStartGates(prop);
            });
            return started || failed;
        }))
        return false;
    if (failed) {
        GiveUp(RV::Outcome::Invalid, "the saw's use did not start on '%ls' (%s)", t.cls.c_str(), why);
        return false;
    }
    return true;
}

// Whether the saw took a target, as its own gates decide (heavy, simulating, a material it takes).
bool SawTookTarget() {
    bool took = false;
    OnGameThread([&] {
        const int32_t off = g_saw && R::IsLive(g_saw) ? R::FindPropertyOffset(R::ClassOf(g_saw), L"sawingProp") : -1;
        took = off >= 0 && *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(g_saw) + off) != nullptr;
    });
    return took;
}

// The cut as a player makes it: aimed at, started by the use verb.
bool StartCut(const Target& t) {
    if (!BeginCut(t)) return false;
    if (!SawTookTarget()) {
        GiveUp(RV::Outcome::Invalid, "the saw's own gates took no target on '%ls'", t.cls.c_str());
        return false;
    }
    return true;
}

std::string Encode(const Target& t) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%.1f %.1f %.1f %ls %ls", t.at.X, t.at.Y, t.at.Z, t.cls.c_str(), t.key.c_str());
    return buf;
}

bool Decode(const std::string& s, Target& t) {
    std::istringstream in(s);
    std::string cls, key;
    if (!(in >> t.at.X >> t.at.Y >> t.at.Z >> cls >> key)) return false;
    t.cls.assign(cls.begin(), cls.end());
    t.key.assign(key.begin(), key.end());
    return true;
}

struct Reading {
    int scrap = 0, others = 0;
};

std::string Show(const Reading& r) { return std::to_string(r.scrap) + " " + std::to_string(r.others); }

bool Parse(const std::string& s, Reading& r) {
    std::istringstream in(s);
    return static_cast<bool>(in >> r.scrap >> r.others);
}

// The refused arm (VOTVCOOP_HACKSAW_ARM=refused, with the host's row sawn_refuse_all): the host refuses the client's cut.
bool RefusedArm() { return coop::config::ReadEnv("VOTVCOOP_HACKSAW_ARM") == "refused"; }

// The saw's `amount`: the scrap its completion spawned, read after the cut. -1 when it does not read.
int SawAmount() {
    int n = -1;
    OnGameThread([&] {
        const int32_t off = g_saw && R::IsLive(g_saw) ? R::FindPropertyOffset(R::ClassOf(g_saw), L"amount") : -1;
        if (off >= 0) n = *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(g_saw) + off);
    });
    return n;
}

// Whether the saw ended its cut: its completeSaw clears `sawingProp`.
bool SawEnded() {
    bool ended = false;
    OnGameThread([&] {
        const int32_t off = g_saw && R::IsLive(g_saw) ? R::FindPropertyOffset(R::ClassOf(g_saw), L"sawingProp") : -1;
        ended = off >= 0 && *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(g_saw) + off) == nullptr;
    });
    return ended;
}

// The prop is gone on this peer, then what stands near it: `want`, when given, is the reading the host took, which a
// client waits for (the host's spill reaches it by the prop lanes). `spawned`, when given, is the scrap the client's saw
// spawned, which the host waits for: its births reach the host by the player-birth path after the prop's break.
bool Watch(const char* me, const Target& t, int scrapBefore, int othersBefore, const Reading* want, int spawned, Reading& out) {
    if (!Wait("the prop gone", kStepSec, [&] { return !StillThere(t); })) return false;
    if (!Wait("the scrap and the spill settled", kStepSec, [&] {
            out.scrap = ScrapNear(t.at) - scrapBefore;
            out.others = OthersNear(t) - othersBefore;
            if (want) return out.scrap == want->scrap && out.others == want->others;
            return spawned < 0 || out.scrap >= spawned;
        }))
        return false;
    UE_LOGI("hacksaw: %hs the prop is gone, new within %.0f m: scrap %d, other props %d", me, kNearCm / 100.f, out.scrap,
            out.others);
    return true;
}

// The client's saw cut, after the host counted: the saw asks the host (an event), and the host grants it, so the prop goes
// on this copy by the saw's own completion; or the host refuses it and the saw ends its cut with the prop whole.
bool ClientCuts(const Target& t) {
    const uint32_t asked = coop::props::container_break::AskedCount();
    const uint32_t refused = coop::props::container_break::RefusedAnswerCount();
    if (!StartCut(t)) return false;
    int polls = 0;
    if (!Wait("the saw asks the host, or the prop goes, or the saw ends its cut", kStepSec, [&] {
            if (polls++ % 50 == 0) {
                uint32_t calls = 0, cancels = 0;
                int tickOn = -1;
                OnGameThread([&] {
                    calls = coop::props::hacksaw_cut::TickCalls();
                    cancels = coop::props::hacksaw_cut::TickCancels();
                    tickOn = BoolCall(g_saw, L"IsActorTickEnabled");
                });
                UE_LOGI("hacksaw: the saw's cut: complete %.2f of its time %.1f s, sawingProp %s, tick enabled=%d, the PRE on its "
                        "ReceiveTick: %u calls, %u cancelled, asked %u", SawFloatNow(L"complete"), SawFloatNow(L"time"),
                        SawEnded() ? "cleared" : "set", tickOn, calls, cancels, coop::props::container_break::AskedCount() - asked);
            }
            return coop::props::container_break::AskedCount() != asked || !StillThere(t) || SawEnded();
        }))
        return false;
    if (coop::props::container_break::AskedCount() == asked) {
        if (StillThere(t))
            GiveUp(RV::Outcome::Invalid, "the saw ended its cut of '%ls' before it asked the host (the player's trace left the "
                   "prop, or the saw was released)", t.cls.c_str());
        else
            GiveUp(RV::Outcome::Fail, "the client's saw cut '%ls' on its copy and asked the host nothing", t.cls.c_str());
        return false;
    }
    if (!Wait("the host's answer", kStepSec,
              [&] { return coop::props::container_break::RefusedAnswerCount() != refused || !StillThere(t); }))
        return false;
    const bool refusedNow = coop::props::container_break::RefusedAnswerCount() != refused;
    if (refusedNow && !RefusedArm()) {
        GiveUp(RV::Outcome::Fail, "the host refused an honest cut of '%ls'", t.cls.c_str());
        return false;
    }
    if (!refusedNow && RefusedArm()) {
        GiveUp(RV::Outcome::Fail, "the host granted the cut of '%ls' that the arm has it refuse", t.cls.c_str());
        return false;
    }
    return true;
}

// The host spawns the fixture in front of the acting peer; each peer counts what stands near it once its copy is here; the
// actor saws once the other peer has counted.
void RunClient() {
    if (!HostPuppetUp()) return;
    PS::Mark("client_ready");
    Target t;
    std::string text;
    if (!HostActs() && !PrepareSaw(true)) return;
    if (!WaitText("picked", text) || !Decode(text, t)) return;
    if (!Wait("the fixture's copy binds here", kStepSec, [&] {
            bool here = false;
            OnGameThread([&] { here = FindProp(t) != nullptr; });
            return here;
        }))
        return;
    OnGameThread([&] {
        void* copy = FindProp(t);
        ue_wrap::FVector c{};
        const bool read = copy && ue_wrap::engine::TryGetActorLocation(copy, c);
        UE_LOGI("hacksaw: CLIENT's copy of '%ls' key '%ls' is %p at %.0f,%.0f,%.0f (read=%d); the host's is at %.0f,%.0f,%.0f, "
                "%.0f cm apart", t.cls.c_str(), t.key.c_str(), copy, c.X, c.Y, c.Z, read ? 1 : 0, t.at.X, t.at.Y, t.at.Z,
                std::sqrt((c.X - t.at.X) * (c.X - t.at.X) + (c.Y - t.at.Y) * (c.Y - t.at.Y) + (c.Z - t.at.Z) * (c.Z - t.at.Z)));
    });
    const int scrapBefore = ScrapNear(t.at), othersBefore = OthersNear(t);
    PS::Mark("client_counted");
    if (!HostActs()) {
        if (!Wait("the host counted", kStepSec, [&] { return PS::Peek("host_counted", text); })) return;
        if (!ClientCuts(t)) return;
        if (RefusedArm()) {
            // The refusal is the event: the prop is whole here, and the saw's own end of the cut ran.
            if (!StillThere(t)) {
                GiveUp(RV::Outcome::Fail, "the cut the host refused took '%ls' on the client's copy", t.cls.c_str());
                return;
            }
            if (!SawEnded()) {
                GiveUp(RV::Outcome::Fail, "the saw did not end its cut after the host's refusal of '%ls'", t.cls.c_str());
                return;
            }
            PS::Mark("client_refused_ok");
            Wait("the host's verdict", kStepSec, [&] { return PS::Peek("host_done", text); });
            UE_LOGI("hacksaw: DONE on client (refused arm)");
            return;
        }
        PS::Mark("client_spawned", std::to_string(SawAmount()));
    }
    std::string theirs;
    if (!WaitText("host_result", theirs, kStepSec)) return;
    Reading want, mine;
    if (!Parse(theirs, want)) {
        GiveUp(RV::Outcome::Invalid, "the host's reading did not parse");
        return;
    }
    if (!Watch("CLIENT", t, scrapBefore, othersBefore, &want, -1, mine)) return;
    PS::Mark("client_result", Show(mine));
    Wait("the host's verdict", kStepSec, [&] { return PS::Peek("host_done", text); });
    UE_LOGI("hacksaw: DONE on client");
}

void RunHost() {
    if (!HostWorldUp() || !Wait("the client", 480, [] { std::string s; return PS::Peek("client_ready", s); })) return;
    Target t;
    std::string text;
    if (HostActs() && !PrepareSaw(false)) return;
    if (!SpawnFixture(t)) return;
    PS::Mark("picked", Encode(t));
    const int scrapBefore = ScrapNear(t.at), othersBefore = OthersNear(t);
    PS::Mark("host_counted");
    if (HostActs()) {
        if (!Wait("the client counted", kStepSec, [&] { std::string s; return PS::Peek("client_counted", s); })) return;
        if (!StartCut(t)) return;
    } else if (RefusedArm()) {
        // The client reports the refusal it was answered and the saw's end of the cut; the prop stands on this copy too.
        if (!WaitText("client_refused_ok", text, kStepSec)) return;
        const bool whole = StillThere(t);
        RV::Say(kTag, whole ? RV::Outcome::Pass : RV::Outcome::Fail,
                "the host refused the client's cut of '%ls': the saw ended its cut and the prop is %s on the host",
                t.cls.c_str(), whole ? "whole" : "gone");
        PS::Mark("host_done");
        UE_LOGI("hacksaw: DONE on host (refused arm)");
        return;
    }
    int spawned = -1;
    if (!HostActs()) {
        if (!WaitText("client_spawned", text, kStepSec)) return;
        spawned = std::atoi(text.c_str());
    }
    Reading mine;
    if (!Watch("HOST", t, scrapBefore, othersBefore, nullptr, spawned, mine)) return;
    PS::Mark("host_result", Show(mine));
    std::string theirs;
    if (!WaitText("client_result", theirs)) return;
    Reading cl;
    if (!Parse(theirs, cl)) {
        GiveUp(RV::Outcome::Invalid, "the client's reading did not parse");
        return;
    }
    const bool pass = mine.scrap >= 1 && mine.scrap == cl.scrap && mine.others == cl.others &&
                      (spawned < 0 || mine.scrap == spawned);
    RV::Say(kTag, pass ? RV::Outcome::Pass : RV::Outcome::Fail,
            "the %s sawed '%ls': the prop is gone on both peers; new near it on the host: scrap %d (the saw spawned %d), "
            "other props %d; on the client: scrap %d, other props %d",
            HostActs() ? "host" : "client", t.cls.c_str(), mine.scrap, spawned, mine.others, cl.scrap, cl.others);
    PS::Mark("host_done");
    UE_LOGI("hacksaw: DONE on host");
}

}  // namespace

DWORD WINAPI HacksawThread(LPVOID /*arg*/) {
    if (!PS::Begin(kTag)) return 0;
    if (!IsClientRole()) RunHost();
    else RunClient();
    return 0;
}

}  // namespace harness::autotest
