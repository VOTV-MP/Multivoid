// coop/dev/join_prop_loss_drill_bracket.cpp -- the client's steps of the join prop-loss drill's four bracket arms; see
// coop/dev/join_prop_loss_drill_arms.h. Each judges what the join's snapshot bracket did to the cans, and the host then
// reads whether its own cans are still there.
//
//   rekey -- two cans 13.7 cm apart: the client destroys one (A) before the bracket. After it, the other (B) must carry its
//            own key and each key must resolve to its own actor.
//   twin  -- the client destroys its copy of a can (A) and spawns a can of its own at A's place before the bracket. After
//            it, and once the divergence sweep has fired, key A must resolve to that can.
//   stack -- three cans 13.7 cm apart on X: the client destroys the middle one (M) before the bracket. After it, the left
//            and right cans must carry their own keys and key M must resolve to an actor that is neither.
//   late  -- the client says when its bracket closed; the host then places a can, and that can must show with no wait.

#include "join_prop_loss_drill_arms.h"

#include "coop/dev/drill_chat.h"
#include "coop/net/net_clock.h"
#include "coop/props/bracket_position_wait.h"
#include "coop/props/join_membership_sweep.h"
#include "coop/props/prop_key_index.h"
#include "coop/session/net_pump.h"
#include "coop/session/world_load_episode.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::dev::join_prop_loss_drill::detail {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace DC = coop::dev::drill_chat;
namespace KI = coop::prop_key_index;
namespace BW = coop::props::bracket_position_wait;
using ue_wrap::FVector;

constexpr uint64_t kSweepMs = 5000;         // twin: the divergence sweep has fired this long after the join ended
constexpr uint64_t kLateBudgetMs = 10000;   // late: the can shows within this once the host named it

enum class Step : uint8_t { Seek, Spawn, Destroyed, WaitHost, Done };

Step                  g_step = Step::Seek;
uint64_t              g_readyMs = 0;        // the key index is asked from here
uint64_t              g_quietMs = 0;        // the first tick the join was over
std::wstring          g_key1, g_key2, g_key3;   // rekey A, B / twin A / stack M, L, R / late C
ue_wrap::CachedObjRef g_ref2, g_ref3;           // rekey B / twin's can / stack L, R
std::vector<void*>    g_before;
FVector               g_at{};
bool                  g_sawWaiting = false;
bool                  g_sent = false;
uint64_t              g_lateSeenMs = 0;

void Abort(const char* fmt, ...) {
    char buf[240];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Inconclusive("%s", buf);
    g_step = Step::Done;
    Finish();
}

// The client's line that releases the host's judgment; it names the first can the host placed.
void SayJudged(const std::wstring& key) {
    DC::Say(std::string(kClientJudgedLine) + Narrow(key));
    g_step = Step::WaitHost;
}

// ---- finding the cans -------------------------------------------------------------------------------------------

struct Pos {
    std::vector<void*>   cans;
    std::vector<FVector> at;
};

Pos Positions() {
    Pos p;
    Cans(p.cans);
    p.at.resize(p.cans.size());
    for (size_t i = 0; i < p.cans.size(); ++i) E::TryGetActorLocation(p.cans[i], p.at[i]);
    return p;
}

FVector Mid(const FVector& a, const FVector& b) { return {(a.X + b.X) * .5f, (a.Y + b.Y) * .5f, (a.Z + b.Z) * .5f}; }

// The pair of cans within 25 cm of each other whose midpoint is nearest the local player within 3000 cm; A is the one
// with the lower world X.
bool FindPair(void* me, void*& a, void*& b, float& gap) {
    FVector mine{};
    if (!E::TryGetActorLocation(me, mine)) return false;
    const Pos p = Positions();
    float bestMid = kPairRangeCm;
    bool found = false;
    for (size_t i = 0; i < p.cans.size(); ++i) {
        for (size_t j = i + 1; j < p.cans.size(); ++j) {
            const float d = Dist(p.at[i], p.at[j]);
            if (d > kPairMaxCm) continue;
            const float md = Dist(mine, Mid(p.at[i], p.at[j]));
            if (md > bestMid) continue;
            bestMid = md;
            found = true;
            gap = d;
            const bool iLower = p.at[i].X <= p.at[j].X;
            a = iLower ? p.cans[i] : p.cans[j];
            b = iLower ? p.cans[j] : p.cans[i];
        }
    }
    return found;
}

// The run of three cans all within 30 cm of each other whose centre is nearest the local player within 3000 cm; the
// middle one by world X, then the lower, then the higher.
bool FindStack(void* me, void*& m, void*& l, void*& r) {
    FVector mine{};
    if (!E::TryGetActorLocation(me, mine)) return false;
    const Pos p = Positions();
    const size_t n = p.cans.size();
    float best = kPairRangeCm;
    bool found = false;
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = i + 1; j < n; ++j) {
            if (Dist(p.at[i], p.at[j]) > kStackMaxCm) continue;
            for (size_t k = j + 1; k < n; ++k) {
                if (Dist(p.at[i], p.at[k]) > kStackMaxCm || Dist(p.at[j], p.at[k]) > kStackMaxCm) continue;
                const FVector c{(p.at[i].X + p.at[j].X + p.at[k].X) / 3.f, (p.at[i].Y + p.at[j].Y + p.at[k].Y) / 3.f,
                                (p.at[i].Z + p.at[j].Z + p.at[k].Z) / 3.f};
                const float d = Dist(mine, c);
                if (d > best) continue;
                best = d;
                found = true;
                size_t idx[3] = {i, j, k};
                std::sort(idx, idx + 3, [&](size_t x, size_t y) { return p.at[x].X < p.at[y].X; });
                l = p.cans[idx[0]];
                m = p.cans[idx[1]];
                r = p.cans[idx[2]];
            }
        }
    }
    return found;
}

// The can nearest the local player within 3000 cm that has no other can within 25 cm.
void* FindLone(void* me) {
    FVector mine{};
    if (!E::TryGetActorLocation(me, mine)) return nullptr;
    const Pos p = Positions();
    void* best = nullptr;
    float bestD = kPairRangeCm;
    for (size_t i = 0; i < p.cans.size(); ++i) {
        const float d = Dist(mine, p.at[i]);
        if (d > bestD) continue;
        bool alone = true;
        for (size_t j = 0; j < p.cans.size() && alone; ++j)
            if (j != i && Dist(p.at[i], p.at[j]) <= kPairMaxCm) alone = false;
        if (!alone) continue;
        best = p.cans[i];
        bestD = d;
    }
    return best;
}

// ---- before the bracket -----------------------------------------------------------------------------------------

void DestroyBeforeBracket(void* victim, const char* which) {
    UE_LOGI("[JPL-DRILL] client windows at the destroy: episode=%d reconcile=%d",
            coop::world_load_episode::InEpisode() ? 1 : 0, coop::world_load_episode::InReconcileWindow() ? 1 : 0);
    if (!E::DestroyActor(victim)) return Abort("K2_DestroyActor did not run on %s", which);
    UE_LOGI("[JPL-DRILL] client destroyed %s before the bracket", which);
}

void SeekRekey(void* me) {
    void *a = nullptr, *b = nullptr;
    float gap = 0.f;
    if (!FindPair(me, a, b, gap)) return;
    g_key1 = ue_wrap::prop::GetKeyString(a);
    g_key2 = ue_wrap::prop::GetKeyString(b);
    UE_LOGI("[JPL-DRILL] client pair A=%ls B=%ls d=%.1f", g_key1.c_str(), g_key2.c_str(), gap);
    g_ref2.Set(b);
    DestroyBeforeBracket(a, "A");
    if (g_step != Step::Done) g_step = Step::Destroyed;
}

void SeekStack(void* me) {
    void *m = nullptr, *l = nullptr, *r = nullptr;
    if (!FindStack(me, m, l, r)) return;
    g_key1 = ue_wrap::prop::GetKeyString(m);
    g_key2 = ue_wrap::prop::GetKeyString(l);
    g_key3 = ue_wrap::prop::GetKeyString(r);
    UE_LOGI("[JPL-DRILL] client stack M=%ls L=%ls R=%ls", g_key1.c_str(), g_key2.c_str(), g_key3.c_str());
    g_ref2.Set(l);
    g_ref3.Set(r);
    DestroyBeforeBracket(m, "M");
    if (g_step != Step::Done) g_step = Step::Destroyed;
}

void SeekTwin(void* me) {
    void* a = FindLone(me);
    if (!a) return;
    g_key1 = ue_wrap::prop::GetKeyString(a);
    E::TryGetActorLocation(a, g_at);
    Cans(g_before);   // A is in it: the twin is the can that is not
    UE_LOGI("[JPL-DRILL] client twin A=%ls at (%.1f,%.1f,%.1f)", g_key1.c_str(), g_at.X, g_at.Y, g_at.Z);
    DestroyBeforeBracket(a, "A");
    if (g_step == Step::Done) return;
    if (!SpawnCan(g_at)) return Abort("the gamemode's spawnPropThroughGamemode did not run on the client");
    g_step = Step::Spawn;
}

// The client's own can at A's place shows with a key of its own; the bracket must not begin before it does.
void SpawnTwin() {
    std::vector<void*> live;
    Cans(live);
    for (void* a : live) {
        if (std::find(g_before.begin(), g_before.end(), a) != g_before.end()) continue;
        const std::wstring key = ue_wrap::prop::GetKeyString(a);
        if (key.empty() || key == L"None") continue;
        FVector at{};
        E::TryGetActorLocation(a, at);
        g_ref2.Set(a);
        UE_LOGI("[JPL-DRILL] client twin can %ls at (%.1f,%.1f,%.1f), A's key %ls", key.c_str(), at.X, at.Y, at.Z,
                g_key1.c_str());
        g_step = Step::Destroyed;
        return;
    }
}

// ---- after the bracket ------------------------------------------------------------------------------------------

void JudgeRekey() {
    void* b = g_ref2.Alive() ? g_ref2.Get() : nullptr;
    const bool bLive = b && R::IsLive(b);
    const std::wstring bKey = bLive ? ue_wrap::prop::GetKeyString(b) : std::wstring();
    const bool c1 = bLive && bKey == g_key2;
    UE_LOGI("[JPL-DRILL] client (1) B live=%d carries %ls (own key %ls) -> %d", bLive ? 1 : 0, bKey.c_str(),
            g_key2.c_str(), c1 ? 1 : 0);
    void* aNow = KI::Find(g_key1);
    const bool aLive = aNow && R::IsLive(aNow);
    const bool c2 = aLive && aNow != b;
    UE_LOGI("[JPL-DRILL] client (2) key A resolves to %p live=%d not B's=%d -> %d", aNow, aLive ? 1 : 0,
            aNow != b ? 1 : 0, c2 ? 1 : 0);
    void* bNow = KI::Find(g_key2);
    const bool c3 = bNow && bNow == b;
    UE_LOGI("[JPL-DRILL] client (3) key B resolves to %p, B's actor %p -> %d", bNow, b, c3 ? 1 : 0);
    if (!c1) Fail("(1) B's actor does not carry key B");
    else if (!c2) Fail("(2) key A does not resolve to a live actor other than B's");
    else if (!c3) Fail("(3) key B does not resolve to B's actor");
    else Pass();
}

void JudgeTwin() {
    void* twin = g_ref2.Alive() ? g_ref2.Get() : nullptr;
    const bool live = twin && R::IsLive(twin);
    void* aNow = KI::Find(g_key1);
    const bool c1 = live && aNow == twin;
    UE_LOGI("[JPL-DRILL] client (1) key A resolves to %p, the twin is %p live=%d -> %d", aNow, twin, live ? 1 : 0,
            c1 ? 1 : 0);
    if (!live) Fail("the twin is gone");
    else if (!c1) Fail("key A does not resolve to the twin's actor");
    else Pass();
}

bool KeyHeldBy(ue_wrap::CachedObjRef& ref, const std::wstring& key, const char* name) {
    void* a = ref.Alive() ? ref.Get() : nullptr;
    const bool live = a && R::IsLive(a);
    const std::wstring carried = live ? ue_wrap::prop::GetKeyString(a) : std::wstring();
    const bool ok = live && carried == key;
    UE_LOGI("[JPL-DRILL] client %s live=%d carries %ls (own key %ls) -> %d", name, live ? 1 : 0, carried.c_str(),
            key.c_str(), ok ? 1 : 0);
    return ok;
}

void JudgeStack() {
    const bool c1 = KeyHeldBy(g_ref2, g_key2, "L");
    const bool c2 = KeyHeldBy(g_ref3, g_key3, "R");
    void* mNow = KI::Find(g_key1);
    void* l = g_ref2.Alive() ? g_ref2.Get() : nullptr;
    void* r = g_ref3.Alive() ? g_ref3.Get() : nullptr;
    const bool c3 = mNow && R::IsLive(mNow) && mNow != l && mNow != r;
    UE_LOGI("[JPL-DRILL] client key M resolves to %p, L's actor %p, R's actor %p -> %d", mNow, l, r, c3 ? 1 : 0);
    if (!c1) Fail("L's actor does not carry key L");
    else if (!c2) Fail("R's actor does not carry key R");
    else if (!c3) Fail("key M does not resolve to a live actor other than L's and R's");
    else Pass();
}

void StepJudge(uint64_t now) {
    if (!JoinedAndQuiet()) return;
    if (g_step == Step::Seek) {
        const char* what = ArmOf() == Arm::Stack ? "stack of cans" : ArmOf() == Arm::Twin ? "lone can" : "pair of cans";
        return Abort("no %s was found before the bracket", what);
    }
    if (g_step == Step::Spawn) return Abort("the twin did not show with a key before the bracket");
    if (!g_quietMs) g_quietMs = now;
    if (ArmOf() == Arm::Twin && now - g_quietMs < kSweepMs) return;
    if (!g_readyMs) g_readyMs = now;
    if (!KI::CanAnswer()) {
        if (now - g_readyMs >= kAnswerBudgetMs) Abort("the key index did not answer");
        return;
    }
    switch (ArmOf()) {
    case Arm::Rekey: JudgeRekey(); break;
    case Arm::Twin:  JudgeTwin(); break;
    default:         JudgeStack(); break;
    }
    SayJudged(g_key1);
}

// ---- late ---------------------------------------------------------------------------------------------------------

// The client says when its bracket closed, then watches for the can the host places on that line.
void StepLate(uint64_t now) {
    if (!g_sent) {
        g_sawWaiting = g_sawWaiting || BW::IsWaiting();
        if (!g_sawWaiting) {
            if (JoinedAndQuiet()) Abort("the bracket was never seen waiting");
            return;
        }
        if (BW::IsWaiting()) return;
        UE_LOGI("[JPL-DRILL] client the bracket closed (%zu waiting)", BW::WaitingCount());
        DC::Say(kBracketDoneLine);
        g_sent = true;
        return;
    }
    std::string rest;
    if (!DC::SeenRestOf(kLateCanLine, &rest)) return;
    if (!g_lateSeenMs) {
        g_lateSeenMs = now;
        g_key1.assign(rest.begin(), rest.end());
    }
    void* c = KI::CanAnswer() ? KI::Find(g_key1) : nullptr;
    const bool live = c && R::IsLive(c);
    if (!live) {
        if (now - g_lateSeenMs < kLateBudgetMs) return;
        UE_LOGI("[JPL-DRILL] client late can %ls live=0 waiting=%zu", g_key1.c_str(), BW::WaitingCount());
        Fail("the late can did not show live within %llu ms", static_cast<unsigned long long>(kLateBudgetMs));
        return SayJudged(g_key1);
    }
    const size_t waiting = BW::WaitingCount();
    UE_LOGI("[JPL-DRILL] client late can %ls live=1 waiting=%zu", g_key1.c_str(), waiting);
    if (waiting != 0) Fail("a spawn after the bracket waited (%zu waiting)", waiting);
    else Pass();
    SayJudged(g_key1);
}

}  // namespace

void BracketLoadTick() {
    if (g_step == Step::Done) return;
    if (ArmOf() == Arm::Late) {
        g_sawWaiting = g_sawWaiting || BW::IsWaiting();
        return;
    }
    if (g_step != Step::Seek && g_step != Step::Spawn) return;
    if (coop::join_membership_sweep::IsClaimTrackingActive()) {
        Abort(g_step == Step::Spawn ? "the bracket began before the twin showed" : "the bracket began before the destroy");
        return;
    }
    if (g_step == Step::Spawn) return SpawnTwin();
    if (!coop::net_pump::IsWorldSettled() || ue_wrap::object_index::Backlog() != 0) return;
    void* me = LocalPlayer();
    if (!me) return;
    switch (ArmOf()) {
    case Arm::Rekey: SeekRekey(me); break;
    case Arm::Twin:  SeekTwin(me); break;
    default:         SeekStack(me); break;
    }
}

void BracketClientTick(uint64_t now) {
    switch (g_step) {
    case Step::Done:
        return;
    case Step::WaitHost:
        if (EndOnHostJudged()) g_step = Step::Done;
        return;
    default:
        break;
    }
    if (ArmOf() == Arm::Late) return StepLate(now);
    StepJudge(now);
}

void BracketOnDisconnect() {
    g_step = Step::Seek;
    g_readyMs = g_quietMs = g_lateSeenMs = 0;
    g_key1.clear();
    g_key2.clear();
    g_key3.clear();
    g_ref2.Reset();
    g_ref3.Reset();
    g_before.clear();
    g_at = FVector{};
    g_sawWaiting = g_sent = false;
}

}  // namespace coop::dev::join_prop_loss_drill::detail
