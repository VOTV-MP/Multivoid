// coop/dev/join_prop_loss_drill.cpp -- see coop/dev/join_prop_loss_drill.h.

#include "coop/dev/join_prop_loss_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/drill_chat.h"
#include "join_prop_loss_drill_arms.h"  // the helpers shared with the arms (co-located private header)
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/props/join_membership_sweep.h"
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

enum class Arm : uint8_t { Off, Break, BreakOwn, Rekey, Swing, ThrowAt };

struct ArmRow {
    const char* name;
    Arm arm;
};
constexpr ArmRow kArms[] = {{"break", Arm::Break}, {"breakown", Arm::BreakOwn}, {"rekey", Arm::Rekey},
                           {"swing", Arm::Swing}, {"throwat", Arm::ThrowAt}};

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
constexpr float kPairGapCm = 13.7f;                       // the second can, on world X
constexpr float kPairRangeCm = 3000.f;                    // rekey: the pair nearest the client player
constexpr float kPairMaxCm = 25.f;                        // two cans this close are the pair
constexpr float kBreakDamage = 100000.f;
constexpr uint64_t kFindBudgetMs = 5000;                  // a placed can shows, with its key, within this
constexpr uint64_t kJudgeDelayMs = 3000;                  // the host judges this long after the client's line
constexpr uint64_t kAnswerBudgetMs = 15000;               // the key index answers within this

constexpr const char* kRekeyJudgedLine = "[JPL] rekey judged";
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

}  // namespace detail

using namespace detail;

namespace {

bool Has(const std::vector<void*>& set, void* a) {
    for (void* p : set)
        if (p == a) return true;
    return false;
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

// ---- host ----------------------------------------------------------------------------------------------------

enum class HStep : uint8_t { Ready, FindA, FindB, Wait, Judge, Done };

struct Placed {
    std::wstring key;
    uint32_t eid = 0;
};

HStep g_hstep = HStep::Ready;
FVector g_spot{};
std::vector<void*> g_before;
Placed g_a, g_b;
uint64_t g_hstepMs = 0;
std::wstring g_brokeKey;
uint64_t g_judgeAtMs = 0;

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

// The first can not in g_before whose key has been written and whose element the tracker has registered; false while
// there is none, and the drill ends when none shows within the budget. Waiting for the element orders the pair's ids:
// B is placed only after A holds one, so A is the lower and arrives first in the snapshot bracket.
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

void Place(const FVector& at, HStep next, uint64_t now) {
    Cans(g_before);
    if (!SpawnCan(at)) {
        Inconclusive("the gamemode's spawnPropThroughGamemode did not run");
        g_hstep = HStep::Done;
        Finish();
        return;
    }
    g_hstepMs = now;
    g_hstep = next;
}

// The verdict, then the host's line that releases the client; an empty `failWhy` is a pass.
void HostJudgeAndFinish(const std::string& failWhy) {
    if (failWhy.empty()) Pass();
    else Fail("%s", failWhy.c_str());
    DC::Say(kHostJudgedLine);
    g_hstep = HStep::Done;
    Finish();
}

void HostTick() {
    if (g_hstep == HStep::Done) return;
    const uint64_t now = coop::net::NowMs();
    switch (g_hstep) {
    case HStep::Ready:
        if (!coop::host_world_settle::Settled() || !LocalPlayer() || !ComputeSpot()) return;
        UE_LOGI("[JPL-DRILL] host places at (%.1f,%.1f,%.1f)", g_spot.X, g_spot.Y, g_spot.Z);
        Place(g_spot, HStep::FindA, now);
        return;
    case HStep::FindA:
        if (!FindNew(g_a, now)) return;
        if (ArmOf() != Arm::Rekey) {
            g_hstep = HStep::Wait;
            return;
        }
        Place({g_spot.X + kPairGapCm, g_spot.Y, g_spot.Z}, HStep::FindB, now);
        return;
    case HStep::FindB:
        if (!FindNew(g_b, now)) return;
        UE_LOGI("[JPL-DRILL] host pair A=%ls eid=%u B=%ls eid=%u (A first in the bracket: %d)", g_a.key.c_str(),
                g_a.eid, g_b.key.c_str(), g_b.eid, g_a.eid < g_b.eid ? 1 : 0);
        g_hstep = HStep::Wait;
        return;
    case HStep::Wait: {
        if (DC::SeenHas(kClientInconclusiveLine)) {
            Inconclusive("the client ended inconclusive");
            Finish();
            return;
        }
        if (ArmOf() == Arm::Rekey) {
            if (!DC::SeenHas(kRekeyJudgedLine)) return;
            if (!KI::CanAnswer()) return;
            const bool aLive = KeyLive(g_a.key), bLive = KeyLive(g_b.key);
            UE_LOGI("[JPL-DRILL] host rekey A live=%d B live=%d", aLive ? 1 : 0, bLive ? 1 : 0);
            if (!aLive) HostJudgeAndFinish("host lost " + Narrow(g_a.key));
            else if (!bLive) HostJudgeAndFinish("host lost " + Narrow(g_b.key));
            else HostJudgeAndFinish("");
            return;
        }
        std::string rest;
        // throwat reports a can that stood too: the host reads either line and measures.
        if (!DC::SeenRestOf(kBrokeLine, &rest) && !(IsThrowAt() && DC::SeenRestOf(kStoodLine, &rest))) return;
        g_brokeKey = Widen(rest);
        g_judgeAtMs = now + kJudgeDelayMs;
        g_hstepMs = now;
        UE_LOGI("[JPL-DRILL] host heard the client broke %ls (the placed can: %d)", g_brokeKey.c_str(),
                g_brokeKey == g_a.key ? 1 : 0);
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
        if (IsThrowAt()) HostJudgeAndFinish("");   // a measure: the line above is the host's reading
        else if (ArmOf() == Arm::Break) HostJudgeAndFinish(live ? std::string() : "host lost " + Narrow(g_brokeKey));
        else HostJudgeAndFinish(live ? "host kept " + Narrow(g_brokeKey) : std::string());
        return;
    }
    case HStep::Done:
        return;
    }
}

// ---- client --------------------------------------------------------------------------------------------------

enum class CStep : uint8_t { Seek, Broke, Observe, Destroyed, WaitHost, Done };

CStep g_cstep = CStep::Seek;
uint64_t g_readyMs = 0;
ue_wrap::CachedObjRef g_target;     // break: the can broken. rekey: B's actor
std::string g_targetKey;
std::wstring g_keyA, g_keyB;
bool g_pairSeen = false;

// impactDamage on the can's physicsImpact component, damage 100000, the hit zero; `causer` is the hitting actor.
bool CallImpactDamage(void* can, void* causer) {
    static int32_t off = -2;
    if (off == -2) {
        void* cls = R::FindClass(L"prop_C");
        if (!cls) return false;
        off = R::FindPropertyOffset(cls, L"physicsImpact");
    }
    if (off < 0) return false;
    void* comp = *reinterpret_cast<void**>(static_cast<uint8_t*>(can) + off);
    if (!comp || !R::IsLive(comp)) return false;
    void* fn = R::FindFunction(R::ClassOf(comp), L"impactDamage");
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid()) return false;
    f.Set<float>(L"damage", kBreakDamage);
    f.Set<void*>(L"actor", causer);
    return ue_wrap::Call(comp, f);
}

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
                Inconclusive("no beer can with a wire mirror within %.0f cm of the player after %llu ms", kMirrorRangeCm,
                             static_cast<unsigned long long>(kMirrorBudgetMs));
                g_cstep = CStep::Done;
                Finish();
            }
            return;
        }
        const std::wstring key = ue_wrap::prop::GetKeyString(can);
        g_targetKey = Narrow(key);
        UE_LOGI("[JPL-DRILL] client breaks %ls eid=%u", key.c_str(),
                static_cast<unsigned>(coop::remote_prop::ResolveMirrorEidByActor(can, true)));
        g_target.Set(can);
        void* causer = ArmOf() == Arm::BreakOwn ? me : nullptr;
        if (!CallImpactDamage(can, causer)) {
            Inconclusive("impactDamage did not run on the can's physicsImpact component");
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
        if (ArmOf() == Arm::Break) {
            if (live) Pass();
            else Fail("the client's own physics destroyed the mirror");
        } else {
            if (!live) Pass();
            else Fail("the client's own player's hit left the mirror standing");
        }
        DC::Say(std::string(kBrokeLine) + g_targetKey);
        g_cstep = CStep::WaitHost;
        return;
    }
    default:
        return;
    }
}

// The pair of cans within 25 cm of each other whose midpoint is nearest the local player within 3000 cm; A is the one
// with the lower world X. False when there is none.
bool FindPair(void* me, void*& a, void*& b, float& gap) {
    FVector mine{};
    if (!E::TryGetActorLocation(me, mine)) return false;
    std::vector<void*> cans;
    Cans(cans);
    std::vector<FVector> pos(cans.size());
    for (size_t i = 0; i < cans.size(); ++i) E::TryGetActorLocation(cans[i], pos[i]);
    float bestMid = kPairRangeCm;
    bool found = false;
    for (size_t i = 0; i < cans.size(); ++i) {
        for (size_t j = i + 1; j < cans.size(); ++j) {
            const float d = Dist(pos[i], pos[j]);
            if (d > kPairMaxCm) continue;
            const FVector mid{(pos[i].X + pos[j].X) * .5f, (pos[i].Y + pos[j].Y) * .5f, (pos[i].Z + pos[j].Z) * .5f};
            const float md = Dist(mine, mid);
            if (md > bestMid) continue;
            bestMid = md;
            found = true;
            gap = d;
            const bool iLower = pos[i].X <= pos[j].X;
            a = iLower ? cans[i] : cans[j];
            b = iLower ? cans[j] : cans[i];
        }
    }
    return found;
}

void RekeyAbort(const char* why) {
    Inconclusive("%s", why);
    g_cstep = CStep::Done;
    Finish();
}

void ClientRekeyJudge(uint64_t now) {
    if (!g_readyMs) g_readyMs = now;
    if (!KI::CanAnswer()) {
        if (now - g_readyMs >= kAnswerBudgetMs) RekeyAbort("the key index did not answer");
        return;
    }
    void* b = g_target.Alive() ? g_target.Get() : nullptr;
    const bool bLive = b && R::IsLive(b);
    const std::wstring bKey = bLive ? ue_wrap::prop::GetKeyString(b) : std::wstring();
    const bool c1 = bLive && bKey == g_keyB;
    UE_LOGI("[JPL-DRILL] client (1) B live=%d carries %ls (own key %ls) -> %d", bLive ? 1 : 0, bKey.c_str(),
            g_keyB.c_str(), c1 ? 1 : 0);
    void* aNow = KI::Find(g_keyA);
    const bool aLive = aNow && R::IsLive(aNow);
    const bool c2 = aLive && aNow != b;
    UE_LOGI("[JPL-DRILL] client (2) key A resolves to %p live=%d not B's=%d -> %d", aNow, aLive ? 1 : 0,
            aNow != b ? 1 : 0, c2 ? 1 : 0);
    void* bNow = KI::Find(g_keyB);
    const bool c3 = bNow && bNow == b;
    UE_LOGI("[JPL-DRILL] client (3) key B resolves to %p, B's actor %p -> %d", bNow, b, c3 ? 1 : 0);
    if (!c1) Fail("(1) B's actor does not carry key B");
    else if (!c2) Fail("(2) key A does not resolve to a live actor other than B's");
    else if (!c3) Fail("(3) key B does not resolve to B's actor");
    else Pass();
    DC::Say(kRekeyJudgedLine);
    g_cstep = CStep::WaitHost;
}

void ClientTick() {
    const uint64_t now = coop::net::NowMs();
    if (IsMeasureArm()) {   // swing and throwat run their own steps (join_prop_loss_drill_arms.cpp)
        MeasureClientTick(now);
        return;
    }
    if (g_cstep == CStep::Done) return;
    if (g_cstep == CStep::WaitHost) {
        if (DC::SeenHas(kHostJudgedLine)) {
            g_cstep = CStep::Done;
            Finish();
        }
        return;
    }
    if (ArmOf() != Arm::Rekey) {
        ClientBreakTick(now);
        return;
    }
    if (!JoinedAndQuiet()) return;
    if (g_cstep == CStep::Seek) {
        RekeyAbort(g_pairSeen ? "the bracket began before the destroy" : "no pair of cans was found before the bracket");
        return;
    }
    if (g_cstep == CStep::Destroyed) ClientRekeyJudge(now);
}

}  // namespace

void Tick(coop::net::Session*) {
    if (ArmOf() == Arm::Off || g_done) return;
    if (coop::roster::LocalIsHost()) HostTick();
    else ClientTick();
}

void TickLoad(coop::net::Session*) {
    if (ArmOf() != Arm::Rekey || g_done || g_cstep != CStep::Seek || coop::roster::LocalIsHost()) return;
    if (coop::join_membership_sweep::IsClaimTrackingActive()) {
        RekeyAbort("the bracket began before the destroy");
        return;
    }
    if (!coop::net_pump::IsWorldSettled() || ue_wrap::object_index::Backlog() != 0) return;
    void* me = LocalPlayer();
    void *a = nullptr, *b = nullptr;
    float gap = 0.f;
    if (!me || !FindPair(me, a, b, gap)) return;
    g_pairSeen = true;
    g_keyA = ue_wrap::prop::GetKeyString(a);
    g_keyB = ue_wrap::prop::GetKeyString(b);
    UE_LOGI("[JPL-DRILL] client pair A=%ls B=%ls d=%.1f", g_keyA.c_str(), g_keyB.c_str(), gap);
    g_target.Set(b);
    UE_LOGI("[JPL-DRILL] client windows at the destroy: episode=%d reconcile=%d",
            coop::world_load_episode::InEpisode() ? 1 : 0, coop::world_load_episode::InReconcileWindow() ? 1 : 0);
    if (!E::DestroyActor(a)) {
        RekeyAbort("K2_DestroyActor did not run on A");
        return;
    }
    UE_LOGI("[JPL-DRILL] client destroyed A before the bracket");
    g_cstep = CStep::Destroyed;
}

void OnDisconnect() {
    g_done = false;
    g_hstep = HStep::Ready;
    g_before.clear();
    g_a = Placed{};
    g_b = Placed{};
    g_hstepMs = 0;
    g_brokeKey.clear();
    g_judgeAtMs = 0;
    g_cstep = CStep::Seek;
    g_readyMs = 0;
    g_target.Reset();
    g_targetKey.clear();
    g_keyA.clear();
    g_keyB.clear();
    g_pairSeen = false;
    MeasureOnDisconnect();
}

}  // namespace coop::dev::join_prop_loss_drill
