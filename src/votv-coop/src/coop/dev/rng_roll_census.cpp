// coop/dev/rng_roll_census.cpp -- see header + docs/COOP_RNG_AUTHORITY.md "T1 PRE-REGISTRATION".

#include "coop/dev/rng_roll_census.h"

#include "coop/element/registry.h"          // EidForActor (wire-explained check, channel e)
#include "coop/net/session.h"
#include "coop/player/hand_item.h"          // IsHandAxisActor (channel e exclusion; GT-only)
#include "coop/props/world_load_episode.h"  // InEpisode (JOIN-EPISODE tag; atomic-read safe)
#include "coop/config/config.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/ufunction_hook.h"
#include "ue_wrap/log.h"
#include "ue_wrap/call.h"
#include "ue_wrap/reflection.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::dev::rng_roll_census {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;

// ---- gating -----------------------------------------------------------------------------------
coop::net::Session* SessionPtr();
std::atomic<coop::net::Session*> g_session{nullptr};

// ---- channel taxonomy ---------------------------------------------------------------------------
enum Native : uint8_t {
    kSetTimerDelegate = 0,
    kSetTimer,
    kDelay,
    kRetrigDelay,
    kDelayFrames,
    kTickInterval,
    kPassThrough,   // channel (a): BeginDeferred spawn npc_sync passed through untouched
    kQuitGame,      // channel (d)
    kNativeCount,
};
const char* const kNativeName[kNativeCount] = {
    "SetTimerDelegate", "SetTimer", "Delay", "RetrigDelay", "DelayFrames",
    "TickInterval", "SPAWN-PASSTHROUGH", "QUITGAME",
};

// ---- per-owner-class stats (mutex-guarded; callbacks fire on anim workers too) -------------------
struct ClassStats {
    std::wstring name;                    // resolved once at first sight (GNames read)
    uint64_t cnt[kNativeCount]     = {};  // total records
    uint64_t coop[kNativeCount]    = {};  // tagged COOP-ORIGIN (our own dispatch)
    uint64_t episode[kNativeCount] = {};  // tagged JOIN-EPISODE
    uint64_t host[kNativeCount]    = {};  // records seen while this process is the HOST
};
std::mutex g_mu;
std::unordered_map<void*, ClassStats> g_stats;  // key = owner UClass*
constexpr size_t kMaxClasses = 1024;            // probe backstop
std::atomic<uint64_t> g_totalNotes{0};
std::atomic<uint64_t> g_droppedNotes{0};        // cap-hit / unresolvable-owner records

// ---- resolved natives -----------------------------------------------------------------------------
// SEAM CORRECTION (measured, first smoke 2026-07-10 12:36): the driver natives are EX_CallMath-
// invoked from BP ubergraphs -- the PE-detour interceptor table NEVER sees them (zero records in a
// 150 s window that produced 973 PE-visible BeginDeferred records; the world-load BeginPlay arms --
// beehive K2_SetTimer, ufoDropper K2_SetTimerDelegate -- were the positive control and they were
// SILENT). Channels (b)+(d) therefore ride the ufunction_hook FUNC-PATCH seam ("the STANDARD seam
// for every dispatch our ProcessEvent detour cannot see"). Attribution comes free: the post
// callback's sourceObject = FFrame::Object = the CALLING BP actor (the re-arming ticker /
// mainGamemode itself) -- better than the param owner, no FFrame stepping needed.
struct Target {
    const wchar_t* cls;
    const wchar_t* fn;
    ue_wrap::ufunction_hook::PostNativeCallback cb;
    bool required;    // false => resolve is best-effort (DelayFrames may not exist in 4.27)
    bool registered = false;
};
bool g_installed = false;

// ---- census (c)/(e) state (GT-only) --------------------------------------------------------------
void* g_tickerBaseCls = nullptr;   // ticker_base_C
void* g_actorCls      = nullptr;   // AActor
void* g_isTickEnabledFn = nullptr; // AActor::IsActorTickEnabled
// Per-UClass* classify memo: 0=not a BP actor, 1=BP actor, 2=BP actor + ticker_base descendant.
std::unordered_map<void*, uint8_t> g_classKind;
std::unordered_map<void*, void*> g_prevActors;  // actor -> class (previous (e) snapshot)
bool g_prevValid = false;
long long g_lastCensusMs = 0;
long long g_enabledAtMs  = 0;
constexpr long long kFirstCensusMs  = 90 * 1000;
constexpr long long kCensusPeriodMs = 600 * 1000;

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

coop::net::Session* SessionPtr() { return g_session.load(std::memory_order_acquire); }

bool IsHostRole() {
    auto* s = SessionPtr();
    return s && s->role() == coop::net::Role::Host;
}

// The one record sink. Any thread: mutex + GNames read on first sight only; no engine dispatch.
void Note(void* ownerObj, Native which) {
    g_totalNotes.fetch_add(1, std::memory_order_relaxed);
    void* cls = nullptr;
    if (which == kPassThrough) {
        cls = ownerObj;  // channel (a) passes the UClass* about to spawn directly
    } else if (ownerObj) {
        cls = R::ClassOf(ownerObj);  // drivers + QuitGame pass the calling INSTANCE (FFrame::Object)
    }
    if (!cls) { g_droppedNotes.fetch_add(1, std::memory_order_relaxed); return; }
    const bool coopOrigin = R::InCoopDispatch();
    const bool inEpisode  = coop::world_load_episode::InEpisode();
    const bool hostRole   = IsHostRole();
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_stats.find(cls);
    if (it == g_stats.end()) {
        if (g_stats.size() >= kMaxClasses) { g_droppedNotes.fetch_add(1, std::memory_order_relaxed); return; }
        it = g_stats.emplace(cls, ClassStats{}).first;
        it->second.name = R::ToString(R::NameOf(cls));
        UE_LOGI("[RNG-CENSUS] first-sight class='%ls' native=%s role=%s coopOrigin=%d episode=%d",
                it->second.name.c_str(), kNativeName[which], hostRole ? "HOST" : "CLIENT",
                coopOrigin ? 1 : 0, inEpisode ? 1 : 0);
    }
    ClassStats& cs = it->second;
    ++cs.cnt[which];
    if (coopOrigin) ++cs.coop[which];
    if (inEpisode)  ++cs.episode[which];
    if (hostRole)   ++cs.host[which];
}

// ---- channel (b) Func-patch post observers: srcObj = the calling BP actor -----------------------
#define MAKE_DRIVER_POST(fn_name, which)                                        \
void fn_name(void* /*context*/, void* srcObj, void* /*result*/) {               \
    if (!IsEnabled()) return;                                                   \
    Note(srcObj, which);                                                        \
}
MAKE_DRIVER_POST(OnSetTimerDelegate, kSetTimerDelegate)
MAKE_DRIVER_POST(OnSetTimer,         kSetTimer)
MAKE_DRIVER_POST(OnDelay,            kDelay)
MAKE_DRIVER_POST(OnRetrigDelay,      kRetrigDelay)
MAKE_DRIVER_POST(OnDelayFrames,      kDelayFrames)
MAKE_DRIVER_POST(OnSetActorTickInterval, kTickInterval)
#undef MAKE_DRIVER_POST
// ---- channel (d) --------------------------------------------------------------------------------
void OnQuitGame(void* /*context*/, void* srcObj, void* /*result*/) {
    if (!IsEnabled()) return;
    // LOUD: the calculateAreaError->QuitGame consequence guard. Post-observe only (read-only probe;
    // the quit has already been issued -- the log is the evidence). srcObj = the CALLER (e.g. the
    // mainGamemode instance whose ubergraph rolled it).
    UE_LOGW("[RNG-CENSUS][QUITGAME] QuitGame dispatched! caller='%ls' role=%s coopOrigin=%d episode=%d "
            "(in an idle smoke with no user quit this means a peer-local roll/fault ended the game)",
            srcObj ? R::ToString(R::NameOf(srcObj)).c_str() : L"<null>",
            IsHostRole() ? "HOST" : "CLIENT", R::InCoopDispatch() ? 1 : 0,
            coop::world_load_episode::InEpisode() ? 1 : 0);
    Note(srcObj, kQuitGame);
}

// ---- censuses (c)+(e): one GUObjectArray walk feeding both ---------------------------------------
void RunCensuses() {
    const bool inEpisode = coop::world_load_episode::InEpisode();
    // Lazy class resolves (retry each census until found; ticker_base loads with the world).
    if (!g_actorCls)      g_actorCls      = R::FindClass(L"Actor");
    if (!g_tickerBaseCls) g_tickerBaseCls = R::FindClass(L"ticker_base_C");
    if (!g_isTickEnabledFn && g_actorCls)
        g_isTickEnabledFn = R::FindFunction(g_actorCls, L"IsActorTickEnabled");

    const int32_t n = R::NumObjects();
    std::unordered_map<void*, void*> cur;   // actor -> class
    cur.reserve(4096);
    std::vector<void*> tickers;
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        void* cls = R::ClassOf(obj);
        if (!cls) continue;
        auto kit = g_classKind.find(cls);
        if (kit == g_classKind.end()) {
            uint8_t kind = 0;
            const std::wstring cn = R::ToString(R::NameOf(cls));
            const bool bpClass = cn.size() > 2 && cn.compare(cn.size() - 2, 2, L"_C") == 0;
            if (bpClass && g_actorCls && R::IsDescendantOfAny(cls, &g_actorCls, 1)) {
                kind = 1;
                if (g_tickerBaseCls && R::IsDescendantOfAny(cls, &g_tickerBaseCls, 1)) kind = 2;
            }
            kit = g_classKind.emplace(cls, kind).first;
        }
        if (kit->second == 0) continue;
        if (!R::IsLive(obj)) continue;
        const std::wstring nm = R::ToString(R::NameOf(obj));
        if (nm.rfind(L"Default__", 0) == 0) continue;
        cur.emplace(obj, cls);
        if (kit->second == 2) tickers.push_back(obj);
    }

    // (c) ticker census: per-class instance counts + tick-enabled reads (<= a few dozen dispatches).
    {
        std::unordered_map<void*, std::pair<int, int>> byCls;  // class -> {instances, tickEnabled}
        for (void* t : tickers) {
            auto& e = byCls[cur[t]];
            ++e.first;
            if (g_isTickEnabledFn) {
                ue_wrap::ParamFrame pf(g_isTickEnabledFn);
                bool enabled = false;
                if (ue_wrap::Call(t, pf) && pf.GetRaw(L"ReturnValue", &enabled, sizeof(enabled)) && enabled)
                    ++e.second;
            }
        }
        for (const auto& [cls, e] : byCls) {
            UE_LOGI("[RNG-CENSUS][TICKERS] class='%ls' instances=%d tickEnabled=%d role=%s episode=%d",
                    R::ToString(R::NameOf(cls)).c_str(), e.first, e.second,
                    IsHostRole() ? "HOST" : "CLIENT", inEpisode ? 1 : 0);
        }
        if (byCls.empty())
            UE_LOGI("[RNG-CENSUS][TICKERS] zero live ticker_base_C descendants (base %s)",
                    g_tickerBaseCls ? "resolved" : "UNRESOLVED");
    }

    // (e) per-actor set diff vs the previous snapshot: unexplained residue per class.
    if (g_prevValid) {
        std::unordered_map<void*, std::pair<int, int>> newByCls;  // class -> {new, unexplained}
        for (const auto& [actor, cls] : cur) {
            if (g_prevActors.count(actor)) continue;  // survived from last snapshot
            auto& e = newByCls[cls];
            ++e.first;
            const bool wireExplained =
                coop::element::Registry::Get().EidForActor(actor) != coop::element::kInvalidId;
            if (!wireExplained && !coop::hand_item::IsHandAxisActor(actor) && !inEpisode)
                ++e.second;
        }
        int loggedRows = 0;
        for (const auto& [cls, e] : newByCls) {
            if (e.second == 0) continue;  // fully explained (or episode-tagged) -> counters only
            UE_LOGW("[RNG-CENSUS][RESIDUE] class='%ls' newActors=%d UNEXPLAINED=%d role=%s "
                    "(no Registry eid, not hand-axis, outside the join episode)",
                    R::ToString(R::NameOf(cls)).c_str(), e.first, e.second,
                    IsHostRole() ? "HOST" : "CLIENT");
            if (++loggedRows >= 32) { UE_LOGW("[RNG-CENSUS][RESIDUE] ...capped at 32 rows"); break; }
        }
        if (loggedRows == 0)
            UE_LOGI("[RNG-CENSUS][RESIDUE] zero unexplained new actors this window (bpActors=%zu)",
                    cur.size());
    }
    g_prevActors = std::move(cur);
    g_prevValid = true;

    // (b)/(d)/(a) counter dump (top rows by count).
    {
        std::lock_guard<std::mutex> lk(g_mu);
        UE_LOGI("[RNG-CENSUS][DUMP] classes=%zu notes=%llu dropped=%llu role=%s",
                g_stats.size(),
                static_cast<unsigned long long>(g_totalNotes.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_droppedNotes.load(std::memory_order_relaxed)),
                IsHostRole() ? "HOST" : "CLIENT");
        for (const auto& [cls, cs] : g_stats) {
            for (uint8_t w = 0; w < kNativeCount; ++w) {
                if (!cs.cnt[w]) continue;
                UE_LOGI("[RNG-CENSUS][DUMP]   '%ls' %s: n=%llu coop=%llu episode=%llu host=%llu",
                        cs.name.c_str(), kNativeName[w],
                        static_cast<unsigned long long>(cs.cnt[w]),
                        static_cast<unsigned long long>(cs.coop[w]),
                        static_cast<unsigned long long>(cs.episode[w]),
                        static_cast<unsigned long long>(cs.host[w]));
            }
        }
    }
}

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::IsIniKeyTrue("rng_roll_census");
    return s;
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!IsEnabled() || g_installed) return;
    // Engine natives resolve at boot (no BP load needed); throttle the resolve walk anyway
    // (ambient_spawner_suppress shape).
    static uint32_t sResolveN = 0;
    if ((sResolveN++ % 125) != 0) return;

    static Target targets[] = {
        {L"KismetSystemLibrary", L"K2_SetTimerDelegate",  &OnSetTimerDelegate,     true},
        {L"KismetSystemLibrary", L"K2_SetTimer",          &OnSetTimer,             true},
        {L"KismetSystemLibrary", L"Delay",                &OnDelay,                true},
        {L"KismetSystemLibrary", L"RetriggerableDelay",   &OnRetrigDelay,          true},
        {L"KismetSystemLibrary", L"DelayFrames",          &OnDelayFrames,          false},
        {L"Actor",               L"SetActorTickInterval", &OnSetActorTickInterval, true},
        {L"KismetSystemLibrary", L"QuitGame",             &OnQuitGame,             true},
    };
    int requiredTotal = 0, requiredDone = 0;
    for (const auto& t : targets)
        if (t.required) ++requiredTotal;
    for (auto& t : targets) {
        if (t.registered) { if (t.required) ++requiredDone; continue; }
        void* cls = R::FindClass(t.cls);
        if (!cls) continue;
        void* fn = R::FindFunction(cls, t.fn);
        if (!fn) {
            if (!t.required) { t.registered = true; continue; }  // best-effort target absent: skip forever
            UE_LOGW("[RNG-CENSUS] required native %ls::%ls not found -- retrying", t.cls, t.fn);
            continue;
        }
        // FUNC-PATCH, not the PE interceptor table: these natives are EX_CallMath-invoked (measured
        // -- see the seam-correction note above), invisible to ProcessEvent.
        if (!ue_wrap::ufunction_hook::InstallPostHook(fn, t.cb)) {
            UE_LOGE("[RNG-CENSUS] InstallPostHook failed for %ls::%ls (table full?)", t.cls, t.fn);
            continue;
        }
        t.registered = true;
        if (t.required) ++requiredDone;
        UE_LOGI("[RNG-CENSUS] Func-patch observer installed: %ls::%ls", t.cls, t.fn);
    }
    if (requiredDone == requiredTotal) {
        g_installed = true;
        g_enabledAtMs = NowMs();
        UE_LOGI("[RNG-CENSUS] all %d required driver interceptors live (probe v9; census every %llds, "
                "first at +%llds)",
                requiredTotal, static_cast<long long>(kCensusPeriodMs / 1000),
                static_cast<long long>(kFirstCensusMs / 1000));
    }
}

void NotePassThrough(void* actorClass, bool /*isHostRole*/) {
    if (!IsEnabled() || !actorClass) return;
    Note(actorClass, kPassThrough);
}

void Tick() {
    if (!IsEnabled() || !g_installed) return;
    if (!GT::IsGameThread()) return;
    const long long now = NowMs();
    if (g_lastCensusMs == 0) {
        if (now - g_enabledAtMs < kFirstCensusMs) return;
    } else if (now - g_lastCensusMs < kCensusPeriodMs) {
        return;
    }
    g_lastCensusMs = now;
    RunCensuses();
}

}  // namespace coop::dev::rng_roll_census
