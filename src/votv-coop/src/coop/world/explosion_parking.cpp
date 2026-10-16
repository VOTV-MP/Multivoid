// coop/world/explosion_parking.cpp -- see coop/world/explosion_parking.h.

#include "coop/world/explosion_parking.h"

#include "coop/net/session.h"
#include "coop/world/spawn_authority.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdint>

namespace coop::explosion_parking {
namespace {

namespace SG = ue_wrap::script_gate;

std::atomic<coop::net::Session*> g_session{nullptr};

// The four verbs, on any class: melee and guns call these names too, so the callback decides by the caller.
UE_FACT_FN(kRowVerbAddDamage, "explosion", nullptr, L"addDamage", AnyClass);
UE_FACT_FN(kRowVerbExploded, "explosion", nullptr, L"exploded", AnyClass);
UE_FACT_FN(kRowVerbReached, "explosion", nullptr, L"reachedByExplosion", AnyClass);
UE_FACT_FN(kRowVerbIgnite, "explosion", nullptr, L"ignite", AnyClass);
UE_FACT_FN(kRowRuneCheck, "explosion", L"explosion_C", L"runeCheck", Declared);
UE_FACT_FN(kRowNarrowBeginPlay, "explosion", L"explosion_C", L"ReceiveBeginPlay", Declared);

constexpr int kVerbs = 4;
constexpr int kTagVerb0 = 0x45585630;  // 'EXV0'..'EXV3'
constexpr int kTagRune = 0x45585243;   // 'EXRC'
constexpr int kTagNarrow = 0x45584E52; // 'EXNR'

SG::Verdict OnVerbPre(const SG::Call& c);
SG::Verdict OnRunePre(const SG::Call& c);
void        OnNarrowPost(const SG::Call& c);

struct Hook {
    const ue_wrap::facts::Function* row;
    int                             tag;
    SG::PreFn                       pre;
    SG::PostFn                      post;
    const char*                     name;
};
constexpr int kHooks = 6;
const Hook kTable[kHooks] = {
    {&kRowVerbAddDamage, kTagVerb0 + 0, &OnVerbPre, nullptr, "addDamage"},
    {&kRowVerbExploded, kTagVerb0 + 1, &OnVerbPre, nullptr, "exploded"},
    {&kRowVerbReached, kTagVerb0 + 2, &OnVerbPre, nullptr, "reachedByExplosion"},
    {&kRowVerbIgnite, kTagVerb0 + 3, &OnVerbPre, nullptr, "ignite"},
    {&kRowRuneCheck, kTagRune, &OnRunePre, nullptr, "runeCheck"},
    {&kRowNarrowBeginPlay, kTagNarrow, nullptr, &OnNarrowPost, "ReceiveBeginPlay"},
};

constexpr auto kRetry = std::chrono::seconds(1);
constexpr int  kMaxTries = 30;
using Clock = std::chrono::steady_clock;

bool              g_done[kHooks] = {};
bool              g_dead[kHooks] = {};
int               g_tries[kHooks] = {};
Clock::time_point g_nextTry{};
bool              g_liveLogged = false;

Counters g_counters{};

SG::Verdict OnVerbPre(const SG::Call& c) {
    const int verb = c.tag - kTagVerb0;
    if (verb < 0 || verb >= kVerbs || !coop::spawn_authority::IsActiveClientSession()) return SG::Verdict::Run;
    ++g_counters.verbCalls[verb];
    return SG::Verdict::Run;
}

SG::Verdict OnRunePre(const SG::Call&) { return SG::Verdict::Run; }

void OnNarrowPost(const SG::Call&) {}

}  // namespace

void Install(coop::net::Session* s) {
    g_session.store(s, std::memory_order_release);
    if (g_liveLogged) return;
    bool pending = false, dead = false;
    if (Clock::now() >= g_nextTry) {
        g_nextTry = Clock::now() + kRetry;
        for (int i = 0; i < kHooks; ++i) {
            if (g_done[i] || g_dead[i]) continue;
            if (SG::WatchName(*kTable[i].row, kTable[i].tag, kTable[i].pre, kTable[i].post)) {
                g_done[i] = true;
            } else if (++g_tries[i] >= kMaxTries) {
                g_dead[i] = true;
                UE_LOGE("[EXPLOSION] the %s watch was refused %d times; a client's explosion copy keeps that part "
                        "of its write", kTable[i].name, kMaxTries);
            }
        }
    }
    for (int i = 0; i < kHooks; ++i)
        if (g_dead[i]) dead = true;
        else if (!g_done[i]) pending = true;
    if (pending || dead) return;
    SG::ResolvePendingNames();
    if (Live()) {
        g_liveLogged = true;
        UE_LOGI("[EXPLOSION] the explosion copy's parking watches are live");
    }
}

bool Live() {
    for (int i = 0; i < kHooks; ++i)
        if (!g_done[i] || !SG::NameWatchLive(*kTable[i].row, kTable[i].tag)) return false;
    return true;
}

void OnDisconnect() {
    const Counters c = GetCounters();
    if (c.verbCalls[0] || c.verbCalls[1] || c.verbCalls[2] || c.verbCalls[3])
        UE_LOGI("[EXPLOSION] parking end: verbCalls=%u/%u/%u/%u refused=%u/%u/%u/%u runeRefused=%u narrowed=%u",
                c.verbCalls[0], c.verbCalls[1], c.verbCalls[2], c.verbCalls[3], c.verbRefused[0], c.verbRefused[1],
                c.verbRefused[2], c.verbRefused[3], c.runeRefused, c.narrowed);
    g_counters = Counters{};
}

Counters GetCounters() { return g_counters; }

}  // namespace coop::explosion_parking
