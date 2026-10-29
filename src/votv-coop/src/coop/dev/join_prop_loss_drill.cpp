// coop/dev/join_prop_loss_drill.cpp -- see coop/dev/join_prop_loss_drill.h.

#include "coop/dev/join_prop_loss_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/drill_chat.h"
#include "join_prop_loss_drill_arms.h"  // the helpers shared with the arms (co-located private header)
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/remote_prop.h"
#include "coop/session/host_world_settle.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/session/world_load_episode.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/world/world_singleton.h"

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::dev::join_prop_loss_drill {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace DC = coop::dev::drill_chat;
namespace PT = coop::prop_element_tracker;
namespace KI = coop::prop_key_index;
using ue_wrap::FVector;

namespace detail {

struct ArmRow {
    const char* name;
    Arm arm;
};
constexpr ArmRow kArms[] = {{"break", Arm::Break}, {"swing", Arm::Swing}, {"heldbreak", Arm::HeldBreak},
                           {"fire", Arm::Fire},   {"rekey", Arm::Rekey}, {"twin", Arm::Twin},
                           {"stack", Arm::Stack}, {"late", Arm::Late},   {"throwat", Arm::ThrowAt}};

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::join_prop_loss_drill);
        for (const ArmRow& r : kArms)
            if (v == r.name) return r.arm;
        return Arm::Off;
    }();
    return a;
}

const char* ArmName() {
    for (const ArmRow& r : kArms)
        if (r.arm == ArmOf()) return r.name;
    return "off";
}

const char* Role() { return coop::roster::LocalIsHost() ? "host" : "client"; }

constexpr float kAheadCm = 150.f;                         // the placement spot, in front of the host player
constexpr float kLiftCm = 3.f;                            // a can spawned at floor height falls through
constexpr float kPairGapCm = 13.7f;                       // the cans of a pair or a stack, on world X
constexpr float kLateOffsetCm = 60.f;                     // late: the can placed after the bracket, on world Y
constexpr uint64_t kFindBudgetMs = 5000;                  // a placed can shows, with its key, within this
constexpr uint64_t kJudgeDelayMs = 3000;                  // the host judges this long after the client's line

constexpr const char* kClientInconclusiveLine = "[JPL] client inconclusive";

bool g_done = false;

void Finish() {
    g_done = true;
    UE_LOGI("[JPL-DRILL] %s DONE", Role());
}

void Pass() { UE_LOGI("[JPL-DRILL] %s %s PASS", Role(), ArmName()); }

void Fail(const char* fmt, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    UE_LOGW("[JPL-DRILL] %s %s FAIL: %s", Role(), ArmName(), buf);
}

void Inconclusive(const char* fmt, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    UE_LOGW("[JPL-DRILL] %s %s INCONCLUSIVE: %s", Role(), ArmName(), buf);
    // The host waits on the client's lines, so a client that stops says so and the host ends with it.
    if (!coop::roster::LocalIsHost()) DC::Say(kClientInconclusiveLine);
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back(c < 128 ? static_cast<char>(c) : '?');
    return s;
}
std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

float Dist(const FVector& a, const FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void* LocalPlayer() {
    void* p = coop::players::Registry::Get().Local();
    return p && R::IsLive(p) ? p : nullptr;
}

void CollectOne(void* ctx, void* obj, int32_t) {
    if (R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
    static_cast<std::vector<void*>*>(ctx)->push_back(obj);
}

// Every live can of the world, the class default left out.
void Cans(std::vector<void*>& out) {
    out.clear();
    void* cls = R::FindClass(kCanClass);
    if (cls) ue_wrap::object_index::ForEachInstance(cls, &CollectOne, &out);
}

// The client's join is over and the bracket's windows are down: the seam broadcasts a destroy from here on.
bool JoinedAndQuiet() {
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle &&
           !coop::world_load_episode::InEpisode() && !coop::world_load_episode::InReconcileWindow();
}

// The can nearest the local player within the range whose wire mirror resolves; null when none.
void* NearestMirror(void* me) {
    FVector mine{};
    if (!E::TryGetActorLocation(me, mine)) return nullptr;
    std::vector<void*> cans;
    Cans(cans);
    void* best = nullptr;
    float bestD = kMirrorRangeCm;
    for (void* a : cans) {
        FVector at{};
        if (!E::TryGetActorLocation(a, at)) continue;
        const float d = Dist(mine, at);
        if (d > bestD) continue;
        if (coop::remote_prop::ResolveMirrorEidByActor(a, true) == coop::element::kInvalidId) continue;
        best = a;
        bestD = d;
    }
    return best;
}

bool IsMeasureArm() { return ArmOf() == Arm::Swing || ArmOf() == Arm::ThrowAt; }
bool IsThrowAt() { return ArmOf() == Arm::ThrowAt; }
bool IsBracketArm() {
    const Arm a = ArmOf();
    return a == Arm::Rekey || a == Arm::Twin || a == Arm::Stack || a == Arm::Late;
}

bool KeyLive(const std::wstring& key) {
    void* a = KI::Find(key);
    return a && R::IsLive(a);
}

// The gamemode's own spawn verb with the can's row; it returns no actor.
bool SpawnCan(const FVector& at) {
    void* gm = ue_wrap::world_singleton::Gamemode();
    void* fn = gm ? R::FindFunction(R::ClassOf(gm), L"spawnPropThroughGamemode") : nullptr;
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid()) return false;
    const R::FName row = ue_wrap::fname_utils::StringToFName(kCanRow);
    ue_wrap::FTransform xform{};
    xform.TX = at.X;
    xform.TY = at.Y;
    xform.TZ = at.Z;
    return f.SetRaw(L"prop", &row, sizeof(row)) && f.SetRaw(L"InputPin", &xform, sizeof(xform)) &&
           f.Set<int32_t>(L"amount", 1) && ue_wrap::Call(gm, f);
}

namespace {

// The can's physicsImpact component, or null.
void* PhysicsImpactOf(void* can) {
    static int32_t off = -2;
    if (off == -2) {
        void* cls = R::FindClass(L"prop_C");
        if (!cls) return nullptr;
        off = R::FindPropertyOffset(cls, L"physicsImpact");
    }
    if (off < 0) return nullptr;
    void* comp = *reinterpret_cast<void**>(static_cast<uint8_t*>(can) + off);
    return comp && R::IsLive(comp) ? comp : nullptr;
}

}  // namespace

// impactDamage on the can's physicsImpact component, damage 100000, the hit zero; `causer` is the hitting actor.
bool CallImpactDamage(void* can, void* causer) {
    void* comp = PhysicsImpactOf(can);
    void* fn = comp ? R::FindFunction(R::ClassOf(comp), L"impactDamage") : nullptr;
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid()) return false;
    f.Set<float>(L"damage", kBreakDamage);
    f.Set<void*>(L"actor", causer);
    return ue_wrap::Call(comp, f);
}

// fireDamage(damage) on the same component: its health goes down by the damage and it ends in the owner's destroy.
bool CallFireDamage(void* can) {
    void* comp = PhysicsImpactOf(can);
    void* fn = comp ? R::FindFunction(R::ClassOf(comp), L"fireDamage") : nullptr;
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid()) return false;
    f.Set<float>(L"damage", kBreakDamage);
    return ue_wrap::Call(comp, f);
}

bool EndOnHostJudged() {
    if (!DC::SeenHas(kHostJudgedLine)) return false;
    Finish();
    return true;
}

}  // namespace detail

using namespace detail;

namespace {

bool Has(const std::vector<void*>& set, void* a) {
    for (void* p : set)
        if (p == a) return true;
    return false;
}

// ---- host ----------------------------------------------------------------------------------------------------

enum class HStep : uint8_t { Ready, Find, WaitBracket, Wait, Judge, Done };

struct Placed {
    std::wstring key;
    uint32_t eid = 0;
};

HStep g_hstep = HStep::Ready;
FVector g_spot{};
std::vector<void*> g_before;
std::vector<FVector> g_plan;        // where each can goes, in order
std::vector<Placed> g_placed;       // the cans placed so far, in order
uint64_t g_hstepMs = 0;
std::wstring g_brokeKey;
uint64_t g_judgeAtMs = 0;
bool g_swingerSeen = false;

// swing: a connected swing is read from the client's puppet, which has an eye only once a pose reached it (the test
// hit_intent.cpp OnIntent makes). Said once; the client's swings wait for the line.
void SaySwingerSeen() {
    if (g_swingerSeen || ArmOf() != Arm::Swing) return;
    coop::RemotePlayer& rp = coop::puppet_drive::Puppet(1);
    if (!rp.valid() || !rp.HasPose()) return;
    g_swingerSeen = true;
    DC::Say(kHostSeesLine);
    UE_LOGI("[JPL-DRILL] host sees the swinger");
}

// The host player's ground point 150 cm ahead, lifted 3 cm. False with the reason printed when there is none.
bool ComputeSpot() {
    void* me = LocalPlayer();
    FVector at{};
    if (!me || !E::TryGetActorLocation(me, at)) return false;
    const FVector fwd = E::GetActorForwardVector(me);
    const float x = at.X + fwd.X * kAheadCm, y = at.Y + fwd.Y * kAheadCm;
    ue_wrap::trace::Hit hit;
    if (!ue_wrap::trace::LineHitStatDyn(me, {x, y, at.Z + 50.f}, {x, y, at.Z - 300.f}, &hit) || !hit.blocked) {
        Inconclusive("no ground under the placement spot (%.0f,%.0f) near z=%.0f", x, y, at.Z);
        g_hstep = HStep::Done;
        Finish();
        return false;
    }
    g_spot = {hit.point.X, hit.point.Y, hit.point.Z + kLiftCm};
    return true;
}

// The cans of the arm, as offsets from the spot; late places its can after the client's bracket line instead.
void BuildPlan() {
    g_plan.clear();
    g_plan.push_back({0.f, 0.f, 0.f});
    switch (ArmOf()) {
    case Arm::Rekey: g_plan.push_back({kPairGapCm, 0.f, 0.f}); break;
    case Arm::Stack:   // the middle can first, then the left, then the right
        g_plan.push_back({-kPairGapCm, 0.f, 0.f});
        g_plan.push_back({kPairGapCm, 0.f, 0.f});
        break;
    case Arm::Late: g_plan[0] = {0.f, kLateOffsetCm, 0.f}; break;
    default: break;
    }
}

FVector PlanAt(size_t i) {
    return {g_spot.X + g_plan[i].X, g_spot.Y + g_plan[i].Y, g_spot.Z + g_plan[i].Z};
}

// The first can not in g_before whose key has been written and whose element the tracker has registered; false while
// there is none, and the drill ends when none shows within the budget. Waiting for the element orders the cans' ids:
// the next is placed only after this one holds one, so an earlier can arrives first in the snapshot bracket.
bool FindNew(Placed& out, uint64_t now) {
    std::vector<void*> live;
    Cans(live);
    for (void* a : live) {
        if (Has(g_before, a)) continue;
        const std::wstring key = ue_wrap::prop::GetKeyString(a);
        if (key.empty() || key == L"None") continue;
        const uint32_t eid = static_cast<uint32_t>(PT::GetPropElementIdForActor(a));
        if (eid == static_cast<uint32_t>(coop::element::kInvalidId)) continue;
        FVector at{};
        E::TryGetActorLocation(a, at);
        out.key = key;
        out.eid = eid;
        UE_LOGI("[JPL-DRILL] host placed %ls eid=%u at (%.1f,%.1f,%.1f)", key.c_str(), out.eid, at.X, at.Y, at.Z);
        return true;
    }
    if (now - g_hstepMs >= kFindBudgetMs) {
        Inconclusive("the placed can did not show with a key within %llu ms", static_cast<unsigned long long>(kFindBudgetMs));
        g_hstep = HStep::Done;
        Finish();
    }
    return false;
}

void Place(const FVector& at, uint64_t now) {
    Cans(g_before);
    if (!SpawnCan(at)) {
        Inconclusive("the gamemode's spawnPropThroughGamemode did not run");
        g_hstep = HStep::Done;
        Finish();
        return;
    }
    g_hstepMs = now;
    g_hstep = HStep::Find;
}

// The verdict, then the host's line that releases the client; an empty `failWhy` is a pass.
void HostJudgeAndFinish(const std::string& failWhy) {
    if (failWhy.empty()) Pass();
    else Fail("%s", failWhy.c_str());
    DC::Say(kHostJudgedLine);
    g_hstep = HStep::Done;
    Finish();
}

// Every can placed has been found: the arm's lines, then the wait for the client.
void PlacedAll() {
    const Placed& p0 = g_placed[0];
    if (ArmOf() == Arm::Rekey)
        UE_LOGI("[JPL-DRILL] host pair A=%ls eid=%u B=%ls eid=%u (A first in the bracket: %d)", p0.key.c_str(), p0.eid,
                g_placed[1].key.c_str(), g_placed[1].eid, p0.eid < g_placed[1].eid ? 1 : 0);
    if (ArmOf() == Arm::Stack) {
        const Placed &l = g_placed[1], &r = g_placed[2];
        UE_LOGI("[JPL-DRILL] host stack M=%ls eid=%u L=%ls eid=%u R=%ls eid=%u (M is the lowest: %d)", p0.key.c_str(),
                p0.eid, l.key.c_str(), l.eid, r.key.c_str(), r.eid, p0.eid < l.eid && p0.eid < r.eid ? 1 : 0);
    }
    if (ArmOf() == Arm::Late) DC::Say(std::string(kLateCanLine) + Narrow(p0.key));
    g_hstep = HStep::Wait;
}

// A bracket arm's verdict: the client named the first can it judged, and every can the host placed must still live.
void HostJudgeBracket(const std::string& named) {
    if (named != Narrow(g_placed[0].key)) {
        Inconclusive("the client judged %s, not the host's first can %ls", named.c_str(), g_placed[0].key.c_str());
        DC::Say(kHostJudgedLine);
        g_hstep = HStep::Done;
        Finish();
        return;
    }
    std::string lost;
    for (const Placed& p : g_placed) {
        const bool live = KeyLive(p.key);
        UE_LOGI("[JPL-DRILL] host can %ls live=%d", p.key.c_str(), live ? 1 : 0);
        if (!live && lost.empty()) lost = "host lost " + Narrow(p.key);
    }
    HostJudgeAndFinish(lost);
}

void HostTick() {
    if (g_hstep == HStep::Done) return;
    const uint64_t now = coop::net::NowMs();
    switch (g_hstep) {
    case HStep::Ready:
        if (!coop::host_world_settle::Settled() || !LocalPlayer() || !ComputeSpot()) return;
        BuildPlan();
        UE_LOGI("[JPL-DRILL] host places at (%.1f,%.1f,%.1f)", g_spot.X, g_spot.Y, g_spot.Z);
        if (ArmOf() == Arm::Late) {
            g_hstep = HStep::WaitBracket;
            return;
        }
        Place(PlanAt(0), now);
        return;
    case HStep::WaitBracket:
        if (DC::SeenHas(kClientInconclusiveLine)) {
            Inconclusive("the client ended inconclusive");
            Finish();
            g_hstep = HStep::Done;
            return;
        }
        if (!DC::SeenHas(kBracketDoneLine)) return;
        UE_LOGI("[JPL-DRILL] host heard the client's bracket close; places can C");
        Place(PlanAt(0), now);
        return;
    case HStep::Find: {
        Placed p;
        if (!FindNew(p, now)) return;
        g_placed.push_back(p);
        if (g_placed.size() < g_plan.size()) Place(PlanAt(g_placed.size()), now);
        else PlacedAll();
        return;
    }
    case HStep::Wait: {
        SaySwingerSeen();
        if (DC::SeenHas(kClientInconclusiveLine)) {
            Inconclusive("the client ended inconclusive");
            Finish();
            g_hstep = HStep::Done;
            return;
        }
        std::string rest;
        if (IsBracketArm()) {
            if (!DC::SeenRestOf(kClientJudgedLine, &rest) || !KI::CanAnswer()) return;
            HostJudgeBracket(rest);
            return;
        }
        // throwat reports a can that stood too: the host reads either line and measures.
        if (!DC::SeenRestOf(kBrokeLine, &rest) && !(IsThrowAt() && DC::SeenRestOf(kStoodLine, &rest))) return;
        g_brokeKey = Widen(rest);
        g_judgeAtMs = now + kJudgeDelayMs;
        g_hstepMs = now;
        UE_LOGI("[JPL-DRILL] host heard the client broke %ls (the placed can: %d)", g_brokeKey.c_str(),
                g_brokeKey == g_placed[0].key ? 1 : 0);
        g_hstep = HStep::Judge;
        return;
    }
    case HStep::Judge: {
        if (now < g_judgeAtMs) return;
        if (!KI::CanAnswer()) {
            if (now - g_judgeAtMs >= kAnswerBudgetMs) {
                Inconclusive("the key index did not answer within %llu ms", static_cast<unsigned long long>(kAnswerBudgetMs));
                g_hstep = HStep::Done;
                Finish();
            }
            return;
        }
        const bool live = KeyLive(g_brokeKey);
        UE_LOGI("[JPL-DRILL] host can %ls live=%d", g_brokeKey.c_str(), live ? 1 : 0);
        const Arm arm = ArmOf();
        if (IsThrowAt()) HostJudgeAndFinish("");   // a measure: the line above is the host's reading
        else if (arm == Arm::Break || arm == Arm::Fire)   // a call the client's gate cancels: the can stays
            HostJudgeAndFinish(live ? std::string() : "host lost " + Narrow(g_brokeKey));
        else   // swing and heldbreak: the break is the client's to make and the host's to take
            HostJudgeAndFinish(live ? "host kept " + Narrow(g_brokeKey) : std::string());
        return;
    }
    case HStep::Done:
        return;
    }
}

// ---- client: break and fire ----------------------------------------------------------------------------------

enum class CStep : uint8_t { Seek, Observe, WaitHost, Done };

CStep g_cstep = CStep::Seek;
uint64_t g_readyMs = 0;
ue_wrap::CachedObjRef g_target;
std::string g_targetKey;

void ClientBreakTick(uint64_t now) {
    switch (g_cstep) {
    case CStep::Seek: {
        if (!JoinedAndQuiet()) return;
        void* me = LocalPlayer();
        if (!me) return;
        if (!g_readyMs) g_readyMs = now;
        void* can = NearestMirror(me);
        if (!can) {
            if (now - g_readyMs >= kMirrorBudgetMs) {
                Inconclusive("no beer can with a wire mirror within %.0f cm of the player after %llu ms",
                             static_cast<double>(kMirrorRangeCm), static_cast<unsigned long long>(kMirrorBudgetMs));
                g_cstep = CStep::Done;
                Finish();
            }
            return;
        }
        const std::wstring key = ue_wrap::prop::GetKeyString(can);
        g_targetKey = Narrow(key);
        UE_LOGI("[JPL-DRILL] client %s %ls eid=%u", ArmOf() == Arm::Fire ? "fires" : "breaks", key.c_str(),
                static_cast<unsigned>(coop::remote_prop::ResolveMirrorEidByActor(can, true)));
        g_target.Set(can);
        const bool ran = ArmOf() == Arm::Fire ? CallFireDamage(can) : CallImpactDamage(can, nullptr);
        if (!ran) {
            Inconclusive("%s did not run on the can's physicsImpact component",
                         ArmOf() == Arm::Fire ? "fireDamage" : "impactDamage");
            g_cstep = CStep::Done;
            Finish();
            return;
        }
        g_cstep = CStep::Observe;
        return;
    }
    case CStep::Observe: {
        const bool live = g_target.Alive() && R::IsLive(g_target.Get());
        UE_LOGI("[JPL-DRILL] client copy of %s live=%d", g_targetKey.c_str(), live ? 1 : 0);
        if (live) Pass();
        else Fail("the client's own %s destroyed the mirror", ArmOf() == Arm::Fire ? "fire damage" : "physics");
        DC::Say(std::string(kBrokeLine) + g_targetKey);
        g_cstep = CStep::WaitHost;
        return;
    }
    case CStep::WaitHost:
        if (EndOnHostJudged()) g_cstep = CStep::Done;
        return;
    case CStep::Done:
        return;
    }
}

void ClientTick() {
    const uint64_t now = coop::net::NowMs();
    switch (ArmOf()) {
    case Arm::Swing:
    case Arm::ThrowAt:   // run their own steps (join_prop_loss_drill_arms.cpp)
        MeasureClientTick(now);
        return;
    case Arm::HeldBreak:
        HeldClientTick(now);
        return;
    case Arm::Rekey:
    case Arm::Twin:
    case Arm::Stack:
    case Arm::Late:
        BracketClientTick(now);
        return;
    case Arm::Break:
    case Arm::Fire:
        ClientBreakTick(now);
        return;
    case Arm::Off:
        return;
    }
}

}  // namespace

void Tick(coop::net::Session*) {
    if (ArmOf() == Arm::Off || g_done) return;
    if (coop::roster::LocalIsHost()) HostTick();
    else ClientTick();
}

void TickLoad(coop::net::Session*) {
    if (!IsBracketArm() || g_done || coop::roster::LocalIsHost()) return;
    BracketLoadTick();
}

void OnDisconnect() {
    g_done = false;
    g_hstep = HStep::Ready;
    g_before.clear();
    g_plan.clear();
    g_placed.clear();
    g_hstepMs = 0;
    g_brokeKey.clear();
    g_judgeAtMs = 0;
    g_swingerSeen = false;
    g_cstep = CStep::Seek;
    g_readyMs = 0;
    g_target.Reset();
    g_targetKey.clear();
    MeasureOnDisconnect();
    HeldOnDisconnect();
    BracketOnDisconnect();
}

}  // namespace coop::dev::join_prop_loss_drill
