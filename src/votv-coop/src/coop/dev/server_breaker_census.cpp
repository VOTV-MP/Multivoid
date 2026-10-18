// coop/dev/server_breaker_census.cpp -- see coop/dev/server_breaker_census.h.

#include "coop/dev/server_breaker_census.h"

#include "coop/config/config.h"
#include "coop/net/session.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/serverbox.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/daynightcycle.h"
#include "ue_wrap/world/game_mode.h"
#include "ue_wrap/world/world_singleton.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <vector>

namespace coop::dev::server_breaker_census {
namespace {

namespace R   = ue_wrap::reflection;
namespace OI  = ue_wrap::object_index;
namespace SG  = ue_wrap::script_gate;
namespace SB  = ue_wrap::serverbox;
namespace DNC = ue_wrap::daynightcycle;
using Clock = std::chrono::steady_clock;

UE_FACT_FN(kRowBreakerTick, "dev.server_breaker_census", L"ticker_serverBreaker_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowBreakerBreakServer, "dev.server_breaker_census", L"serverBox_C", L"breakServer", Declared);
UE_FACT_FN(kRowBreakerFix, "dev.server_breaker_census", L"serverBox_C", L"fix", Declared);
UE_FACT_FN(kRowBreakerBreakType, "dev.server_breaker_census", L"serverBox_C", L"break_type", Declared);

constexpr int kTagTick = 1, kTagBreakServer = 2, kTagFix = 3, kTagBreakType = 4;
constexpr auto kPeriod = std::chrono::seconds(30);

struct Watch {
    const ue_wrap::facts::Function* row;
    int tag;
    const char* label;
};
const Watch kBoxWatches[] = {
    {&kRowBreakerBreakServer, kTagBreakServer, "breakServer"},
    {&kRowBreakerFix,         kTagFix,         "fix"},
    {&kRowBreakerBreakType,   kTagBreakType,   "break_type"},
};

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_registered = false;
bool g_registerFailed = false;
bool g_held = false;
Clock::time_point g_nextLine{};
uint64_t g_ticksSinceLine = 0;

// A box's pending call: the gamemode's count at the pre, kept until its post. Keyed by box and
// tag, so a post another consumer cancelled leaves one stale row that the next pre overwrites.
struct Pending { void* box; int tag; int32_t before; };
std::vector<Pending> g_pending;

const char* Role() {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->role() == coop::net::Role::Host) ? "host" : "client";
}

// The gamemode's brokenServers, -1 while the break group is unresolved.
int32_t BrokenServers() {
    if (!SB::EnsureBreakResolved()) return -1;
    SB::Aggregates a;
    return SB::ReadAggregates(a) ? a.brokenServers : -1;
}

const char* LabelOf(int tag) {
    for (const Watch& w : kBoxWatches)
        if (w.tag == tag) return w.label;
    return "?";
}

SG::Verdict OnTickPre(const SG::Call&) {
    ++g_ticksSinceLine;
    return SG::Verdict::Run;
}

SG::Verdict OnBoxPre(const SG::Call& c) {
    const int32_t before = BrokenServers();
    for (Pending& p : g_pending)
        if (p.box == c.object && p.tag == c.tag) { p.before = before; return SG::Verdict::Run; }
    g_pending.push_back({c.object, c.tag, before});
    return SG::Verdict::Run;
}

// A breakServer, fix or break_type is a rare call (a break rolls every 10-20 minutes, a fix is a
// player's finished repair), so each prints a line with no rate limit.
void OnBoxPost(const SG::Call& c) {
    int32_t before = -1;
    for (Pending& p : g_pending)
        if (p.box == c.object && p.tag == c.tag) { before = p.before; p.box = nullptr; break; }
    g_pending.erase(std::remove_if(g_pending.begin(), g_pending.end(),
                                   [](const Pending& p) { return p.box == nullptr; }),
                    g_pending.end());
    UE_LOGI("server_breaker_census: %s box=%ls brokenServers %d -> %d", LabelOf(c.tag),
            R::ToString(R::NameOf(c.object)).c_str(), before, BrokenServers());
}

void EnsureWatches() {
    if (g_registered || g_registerFailed) return;
    if (!g_held) { SG::Acquire("server_breaker_census"); g_held = true; }
    if (!SG::IsInstalled()) return;
    bool ok = SG::WatchName(kRowBreakerTick, kTagTick, &OnTickPre, nullptr);
    for (const Watch& w : kBoxWatches)
        ok = SG::WatchName(*w.row, w.tag, &OnBoxPre, &OnBoxPost) && ok;
    if (!ok) {
        g_registerFailed = true;
        UE_LOGE("server_breaker_census: the gate refused a watch -- this run is partial");
        return;
    }
    g_registered = true;
    UE_LOGI("server_breaker_census: 4 name watches registered");
}

// Live instances of one exact class, into `out`; the object index lists them, no object-array walk.
void CollectLive(const wchar_t* className, std::vector<void*>& out) {
    void* cls = OI::ClassByName(className);
    if (!cls) return;
    OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t index) {
        if (!obj || (R::SlotFlags(index) & (R::slot_flags::Dying | R::slot_flags::NotYetReadable))) return;
        if (!R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
        static_cast<std::vector<void*>*>(ctx)->push_back(obj);
    }, &out);
}

// The first ticker's interval and tick-enabled flag through the engine's own getters.
void ReadTickerState(void* ticker, float& interval, int& tickOn) {
    static void* s_getInterval = nullptr;
    static void* s_isEnabled = nullptr;
    if (!s_getInterval || !s_isEnabled) {
        if (void* actor = R::FindClass(L"Actor")) {
            if (!s_getInterval) s_getInterval = R::FindFunction(actor, L"GetActorTickInterval");
            if (!s_isEnabled)   s_isEnabled   = R::FindFunction(actor, L"IsActorTickEnabled");
        }
    }
    if (s_getInterval) {
        ue_wrap::ParamFrame pf(s_getInterval);
        if (ue_wrap::Call(ticker, pf)) interval = pf.Get<float>(L"ReturnValue");
    }
    if (s_isEnabled) {
        ue_wrap::ParamFrame pf(s_isEnabled);
        bool on = false;
        if (ue_wrap::Call(ticker, pf) && pf.GetRaw(L"ReturnValue", &on, sizeof(on))) tickOn = on ? 1 : 0;
    }
}

// One line. It reads the index's lists for three classes and the servers array once: a cold path,
// 30 s apart, one allocation burst of a few vectors, nothing per tick.
void PrintLine() {
    void* gm = ue_wrap::world_singleton::Gamemode();
    if (!gm) return;

    std::vector<void*> tickers;
    CollectLive(L"ticker_serverBreaker_C", tickers);
    float interval = -1.f;
    int tickOn = -1;
    if (!tickers.empty()) ReadTickerState(tickers.front(), interval, tickOn);

    static R::InstanceOffset s_difficulty{L"difficulty"};
    const int32_t dOff = s_difficulty.Of(gm);
    const int difficulty = dOff >= 0 ? *(static_cast<const uint8_t*>(gm) + dOff) : -1;

    DNC::EnsureResolved();
    float totalTime = -1.f, dayF = 0.f, scale = 0.f;
    int32_t h = 0, m = 0, day = -1;
    if (!DNC::ReadClock(totalTime, dayF, scale)) totalTime = -1.f;
    if (!DNC::ReadTimeZ(h, m, day)) day = -1;

    std::vector<void*> servers;
    SB::ReadServers(servers);
    int dead = 0, nulls = 0;
    for (void* s : servers) {
        if (!s) ++nulls;
        else if (!R::IsLive(s)) ++dead;
    }

    std::vector<void*> boxes;
    CollectLive(L"serverBox_C", boxes);
    CollectLive(L"serverBox_dish_C", boxes);  // a subclass: GetAllActorsOfClass(serverBox_C) lists it too
    int brokenActual = 0, unlisted = 0;
    const bool breakOk = SB::EnsureBreakResolved();
    for (void* b : boxes) {
        if (breakOk && SB::ReadIsBroken(b)) ++brokenActual;
        if (std::find(servers.begin(), servers.end(), b) == servers.end()) ++unlisted;
    }

    UE_LOGI("server_breaker_census: role=%s tickers=%zu interval=%.1f tickOn=%d gmByte=%d difficulty=%d day=%d "
            "totalTime=%.1f servers=%zu dead=%d null=%d liveBoxes=%zu brokenServers=%d brokenActual=%d "
            "ticks=%llu unlisted=%d",
            Role(), tickers.size(), interval, tickOn, ue_wrap::game_mode::ReadLocal(), difficulty, day,
            totalTime, servers.size(), dead, nulls, boxes.size(), BrokenServers(), brokenActual,
            static_cast<unsigned long long>(g_ticksSinceLine), unlisted);
    g_ticksSinceLine = 0;
}

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::server_breaker_census);
    return s;
}

void Install(coop::net::Session* session) {
    if (!IsEnabled()) return;
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    if (!IsEnabled()) return;
    if (!ue_wrap::game_thread::IsGameThread()) return;
    EnsureWatches();
    if (g_registered) SG::ResolvePendingNames();
    const auto now = Clock::now();
    if (now < g_nextLine) return;
    if (!ue_wrap::world_singleton::Gamemode()) return;  // no world yet: the first line waits for one
    g_nextLine = now + kPeriod;
    PrintLine();
}

}  // namespace coop::dev::server_breaker_census
