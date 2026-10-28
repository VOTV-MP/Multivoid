// coop/dev/container_route_census.cpp -- see coop/dev/container_route_census.h.

#include "coop/dev/container_route_census.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_physics.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace coop::dev::container_route_census {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace ci = ue_wrap::container_inventory;
namespace E  = ue_wrap::engine;

constexpr int kR3 = 2, kR4 = 3, kR5 = 4, kNone = 5;   // route indices
constexpr int kGenTag = 6;
constexpr uint64_t kReportMs = 30000;
constexpr int kLast = 32;        // containers whose last writing route is remembered
constexpr int kCallers = 16;     // distinct R6 caller classes, then "other"
constexpr int kClasses = 8;      // distinct R4 simulated-shared classes
constexpr int kOverlapLines = 16;
constexpr size_t kName = 40;

UE_FACT_FN(kRowR1, "container_route_census", L"uicomp_playerInvContainerSlot_C", L"pressButton", Declared);
UE_FACT_FN(kRowR2, "container_route_census", L"ui_playerInventory_C", L"em_take", Declared);
UE_FACT_FN(kRowR3, "container_route_census", L"prop_container_C", L"playerUsedOn", Declared);
UE_FACT_FN(kRowR4, "container_route_census", L"prop_container_C", L"putObjectIn_overlap", Declared);
UE_FACT_FN(kRowR5, "container_route_census", L"prop_vacuum2_C", L"gather", Declared);
UE_FACT_FN(kRowGen, "container_route_census", L"ui_playerInventory_C", L"gen_player", Declared);

const ue_wrap::facts::Function* const kRows[] = {&kRowR1, &kRowR2, &kRowR3, &kRowR4, &kRowR5, &kRowGen};
const char* const kSeam[] = {"R1 pressButton", "R2 em_take", "R3 playerUsedOn", "R4 putObjectIn_overlap",
                             "R5 gather", "gen_player"};

const std::string& Mode() {
    static const std::string m = coop::config::ResolveString(::coop::config_registry::rows::container_route_census);
    return m;
}

coop::net::Session* g_session = nullptr;
bool g_host = false;
bool g_registered = false;
bool g_said[6] = {};              // each seam's state said once
uint64_t g_nextReport = 0;
int g_quiet = 0;

void* g_fn[5] = {};               // each route's function, as its pre last saw it (for IsBodyActive)
unsigned g_multiSaid = 0;         // the combinations of routes said to be active together
uintptr_t g_r3Bit = 0;            // the scope bit of an R3 body whose player was running (sg::ScopeBitFor)

uint64_t g_entries[5] = {};       // seam entries R1..R5
uint64_t g_unshared[6] = {};      // verbs on an inventory that is no shared world container, by the open route
uint64_t g_cell[kRoutes][2] = {};     // [route][add=0, take=1] verbs on a shared container
uint64_t g_slices[kRoutes][2] = {};
uint64_t g_unattributed = 0;
uint64_t g_gen = 0;

struct Last { uint32_t eid; int8_t route; int8_t verb; };
Last g_last[kLast] = {};
int g_lastNext = 0;

struct CallerCount { char name[kName]; uint32_t n; };
CallerCount g_callers[kCallers] = {};
int g_callerCount = 0;
uint64_t g_callerOther = 0;

uint64_t g_r4Sim = 0;
uint64_t g_r4Overlaps = 0;
int g_r4Lines = 0;
R4Overlap g_lastR4;
char g_r4Classes[kClasses][kName] = {};
int g_r4ClassCount = 0;

const char* Role() { return g_host ? "host" : "client"; }

uint64_t NowMs() { return ::GetTickCount64(); }

void Narrow(const std::wstring& w, char* out, size_t cap) {
    size_t i = 0;
    for (; i + 1 < cap && i < w.size(); ++i) out[i] = w[i] < 128 ? static_cast<char>(w[i]) : '?';
    out[i] = 0;
}

void* ParamPtr(const sg::Call& c, R::ParamOffset& param) {
    const int32_t off = c.function ? param.Of(c.function) : -1;
    if (off < 0 || !c.locals) return nullptr;
    void* p = nullptr;
    std::memcpy(&p, c.locals + off, sizeof(p));
    return p;
}

// R3 counts only the put, the BP's own branch: the body's player is running (input_run). An input_run that does not
// resolve reads as not running, so it is said once.
bool Runs(void* player) {
    if (!player || !R::IsLive(player)) return false;
    bool run = false;
    if (!E::ReadInputRun(player, run)) {
        static bool sSaid = false;
        if (!sSaid) {
            sSaid = true;
            UE_LOGW("[CI-CENSUS] R3: the player's input_run does not resolve -- R3 is not counted");
        }
        return false;
    }
    return run;
}

bool PlayerRuns(const sg::Call& c) {
    static R::ParamOffset sPlayer(L"player");
    return Runs(ParamPtr(c, sPlayer));
}

// Is `route`'s own body running on this thread's chain of watched bodies? The gate's RAII answer, which unwinds on a
// Cancel and on a fault; the function is the one the route's pre last saw. An R3 body is the put only when its pre
// marked the scope: the body's own player was running, the same player the entry count reads.
bool Active(int route) {
    if (!g_fn[route] || !sg::IsBodyActive(g_fn[route])) return false;
    return route != kR3 || (sg::ScopeBits(g_fn[route]) & g_r3Bit) != 0;
}

// The route a verb runs in, innermost by the BP's nesting: R1 when its body is active (em_take's hover branch calls
// pressButton, so R1 is the inner one); else the one active route. Two or more of the others active at once is said
// once per combination, by name, and counted as the first in R1..R5 order. -1: none.
int ActiveRoute() {
    if (Active(0)) return 0;
    int first = -1;
    unsigned mask = 0;
    for (int r = 1; r < 5; ++r) {
        if (!Active(r)) continue;
        if (first < 0) first = r;
        mask |= 1u << r;
    }
    if ((mask & (mask - 1)) != 0 && !(g_multiSaid & mask)) {
        g_multiSaid |= mask;
        char names[16] = "";
        for (int r = 1; r < 5; ++r)
            if (mask & (1u << r)) std::snprintf(names + std::strlen(names), sizeof(names) - std::strlen(names), "R%d ", r + 1);
        UE_LOGW("[CI-CENSUS] routes active together: %s-- counted as R%d", names, first + 1);
    }
    return first;
}

void NoteOverlap(const sg::Call& c) {
    static R::ParamOffset sActor(L"actor"), sComp(L"component");
    ++g_r4Overlaps;
    void* actor = ParamPtr(c, sActor);
    void* comp = ParamPtr(c, sComp);
    // The overlap arrives from a delayed continuation: either may be gone by the time it runs.
    if (actor && !R::IsLive(actor)) actor = nullptr;
    if (comp && !R::IsLive(comp)) comp = nullptr;
    const bool sim = comp && E::IsComponentSimulatingPhysics(comp);
    const bool shared = actor && coop::element::Registry::Get().EidForActor(actor) != coop::element::kInvalidId;
    if (shared) {   // read by the container intent drill's throw arms, whatever the row's value
        auto& reg = coop::element::Registry::Get();
        g_lastR4 = R4Overlap{g_lastR4.seq + 1, reg.EidForActor(c.object), reg.EidForActor(actor), sim, UnixMs(), actor};
    }
    char cls[kName] = "?";
    if (actor) Narrow(R::ClassNameOf(actor), cls, sizeof(cls));
    if (IsDrill() && g_r4Lines < kOverlapLines) {
        ++g_r4Lines;
        UE_LOGI("[CI-CENSUS] %s R4 overlap actor=%s simulating=%d shared=%d", Role(), cls, sim ? 1 : 0, shared ? 1 : 0);
        ue_wrap::FVector at{};
        if (shared && actor && E::TryGetActorLocation(actor, at))
            UE_LOGI("[CI-CENSUS] r4 %s prop=(%.1f,%.1f,%.1f) t=%llu", Role(), at.X, at.Y, at.Z,
                    static_cast<unsigned long long>(UnixMs()));
    }
    if (!sim || !shared) return;
    ++g_r4Sim;
    for (int i = 0; i < g_r4ClassCount; ++i)
        if (std::strcmp(g_r4Classes[i], cls) == 0) return;
    if (g_r4ClassCount < kClasses) {
        std::snprintf(g_r4Classes[g_r4ClassCount++], kName, "%s", cls);
        UE_LOGI("[CI-CENSUS] %s R4: a client-side simulating shared prop of class %s entered a container", Role(), cls);
    }
}

// Each route's function is kept as it is entered, for IsBodyActive; the entry is counted. The scope itself is the gate's:
// it unwinds on a Cancel (the act's seats cancel R2 and R3) and on a fault, which no counter here could.
sg::Verdict ScopePre(const sg::Call& c) {
    const int route = c.tag - 1;
    if (route < 0 || route > 4) return sg::Verdict::Run;
    g_fn[route] = c.function;
    if (route == kR3) {   // only the put is an R3 entry, and only its scope is marked as the put
        if (!PlayerRuns(c)) return sg::Verdict::Run;
        sg::MarkScope(c, g_r3Bit);
    }
    ++g_entries[route];
    if (route == kR4) NoteOverlap(c);
    return sg::Verdict::Run;
}


void GenPost(const sg::Call&) { ++g_gen; }

void RememberWriter(uint32_t eid, int route, int verb) {
    for (Last& l : g_last)
        if (l.eid == eid && l.route >= 0) { l.route = static_cast<int8_t>(route); l.verb = static_cast<int8_t>(verb); return; }
    g_last[g_lastNext] = Last{eid, static_cast<int8_t>(route), static_cast<int8_t>(verb)};
    g_lastNext = (g_lastNext + 1) % kLast;
}

// An R6 verb: the caller's class counted, and on the first of a class the stack's outermost script function said.
void NoteR6Caller(const sg::Call& c) {
    char cls[kName] = "(no script caller)";
    if (c.callerObject && R::IsLive(c.callerObject)) Narrow(R::ClassNameOf(c.callerObject), cls, sizeof(cls));
    for (int i = 0; i < g_callerCount; ++i)
        if (std::strcmp(g_callers[i].name, cls) == 0) { ++g_callers[i].n; return; }
    if (g_callerCount >= kCallers) { ++g_callerOther; return; }
    CallerCount& e = g_callers[g_callerCount++];
    std::snprintf(e.name, kName, "%s", cls);
    e.n = 1;
    char outer[kName] = "?";
    if (void* fn = sg::OutermostFunction(c)) Narrow(R::ToString(R::NameOf(fn)), outer, sizeof(outer));
    char caller[kName] = "?";
    if (c.callerFunction) Narrow(R::ToString(R::NameOf(c.callerFunction)), caller, sizeof(caller));
    UE_LOGI("[CI-CENSUS] R6 first caller=%s fn=%s outermost=%s", cls, caller, outer);
}

void RegisterWatches() {
    g_r3Bit = sg::ScopeBitFor(&g_fn);
    if (!g_r3Bit) UE_LOGE("[CI-CENSUS] no scope bit for R3 -- an R3 put is never attributed");
    sg::PreFn pre[5] = {&ScopePre, &ScopePre, &ScopePre, &ScopePre, &ScopePre};
    bool all = true;
    for (int i = 0; i < 5; ++i) {
        const bool ok = sg::WatchName(*kRows[i], i + 1, pre[i], nullptr);
        if (!ok) { all = false; UE_LOGE("[CI-CENSUS] seam %s: the gate REFUSED the watch -- no scope for that route", kSeam[i]); }
    }
    if (!sg::WatchName(kRowGen, kGenTag, nullptr, &GenPost)) {
        all = false;
        UE_LOGE("[CI-CENSUS] seam gen_player: the gate REFUSED the watch -- the screen's rebuild is not counted");
    }
    g_registered = true;
    if (!all) UE_LOGW("[CI-CENSUS] not every seam took its watch");
}

// Each seam's state, said once it settles: a dead watch is named, not left to read as zero.
void SaySeams() {
    for (int i = 0; i < 6; ++i) {
        if (g_said[i]) continue;
        const int tag = i == 5 ? kGenTag : i + 1;
        if (sg::NameWatchLive(*kRows[i], tag)) {
            g_said[i] = true;
            UE_LOGI("[CI-CENSUS] seam %s watch LIVE", kSeam[i]);
        } else if (sg::NameWatchSettled(*kRows[i], tag)) {
            g_said[i] = true;
            UE_LOGE("[CI-CENSUS] seam %s watch DEAD -- that route is never counted", kSeam[i]);
        }
    }
}

void Reset() {
    std::memset(g_fn, 0, sizeof(g_fn));
    g_multiSaid = 0;
    std::memset(g_entries, 0, sizeof(g_entries));
    std::memset(g_unshared, 0, sizeof(g_unshared));
    std::memset(g_cell, 0, sizeof(g_cell));
    std::memset(g_slices, 0, sizeof(g_slices));
    std::memset(g_last, 0, sizeof(g_last));
    for (Last& l : g_last) l.route = -1;
    std::memset(g_callers, 0, sizeof(g_callers));
    std::memset(g_r4Classes, 0, sizeof(g_r4Classes));
    g_unattributed = g_gen = 0;
    g_lastNext = g_callerCount = g_r4ClassCount = g_r4Lines = 0;
    g_callerOther = g_r4Sim = g_r4Overlaps = 0;
    g_lastR4 = R4Overlap{};
    for (bool& b : g_said) b = false;
    g_registered = false;
    g_nextReport = 0;
}

}  // namespace

uint64_t UnixMs() {
    FILETIME ft;
    ::GetSystemTimeAsFileTime(&ft);
    const uint64_t t100ns = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return t100ns / 10000ull - 11644473600000ull;
}

bool IsEnabled() { return !Mode().empty(); }
bool IsDrill() { return Mode() == "drill"; }

Quiet::Quiet() { ++g_quiet; }
Quiet::~Quiet() { --g_quiet; }


void OnVerb(const sg::Call& c, bool take) {
    if (!IsEnabled() || g_quiet > 0) return;
    const int open = ActiveRoute();
    void* inv = c.object;
    void* owner = inv ? ci::OwnerOf(inv) : nullptr;
    const uint32_t eid = (owner && ci::IsWorldInventory(inv))
        ? static_cast<uint32_t>(coop::element::Registry::Get().EidForActor(owner))
        : static_cast<uint32_t>(coop::element::kInvalidId);
    const int route = open < 0 ? kNone : open;
    if (eid == static_cast<uint32_t>(coop::element::kInvalidId)) {
        if (open >= 0) ++g_unshared[open];
    } else {
        ++g_cell[route][take ? 1 : 0];
        RememberWriter(eid, route, take ? 1 : 0);
        if (route == kNone) NoteR6Caller(c);
    }
}

void NoteSliceSent(uint32_t eid) {
    if (!IsEnabled()) return;
    for (Last& l : g_last) {
        if (l.eid != eid || l.route < 0) continue;
        ++g_slices[l.route][l.verb];
        l.route = -1;
        return;
    }
    ++g_unattributed;
}


void ReportNow(const char* why) {
    if (!IsEnabled()) return;
    UE_LOGI("[CI-CENSUS] %s report (%s)", Role(), why ? why : "");
    for (int r = 0; r < kRoutes; ++r)
        for (int v = 0; v < 2; ++v)
            if (g_cell[r][v] || g_slices[r][v])
                UE_LOGI("[CI-CENSUS] %s route=R%d verb=%s n=%llu slices=%llu", Role(), r + 1, v ? "take" : "add",
                        static_cast<unsigned long long>(g_cell[r][v]), static_cast<unsigned long long>(g_slices[r][v]));
    for (int r = 0; r < 5; ++r)
        if (g_entries[r] || g_unshared[r])
            UE_LOGI("[CI-CENSUS] %s scope R%d entries=%llu verbs-on-no-shared-container=%llu", Role(), r + 1,
                    static_cast<unsigned long long>(g_entries[r]), static_cast<unsigned long long>(g_unshared[r]));
    if (g_entries[kR4]) {
        char list[kClasses * (kName + 1)] = "";
        size_t used = 0;
        for (int i = 0; i < g_r4ClassCount; ++i)
            used += static_cast<size_t>(std::snprintf(list + used, sizeof(list) - used, "%s%s", i ? "," : "", g_r4Classes[i]));
        UE_LOGI("[CI-CENSUS] %s R4 simulated-shared n=%llu classes=%s", Role(), static_cast<unsigned long long>(g_r4Sim),
                g_r4ClassCount ? list : "-");
    }
    if (g_callerCount) {
        char line[(kCallers + 1) * (kName + 16)] = "";
        size_t used = 0;
        for (int i = 0; i < g_callerCount; ++i)
            used += static_cast<size_t>(std::snprintf(line + used, sizeof(line) - used, "%s%s=%u", i ? "," : "",
                                                      g_callers[i].name, g_callers[i].n));
        if (g_callerOther)
            std::snprintf(line + used, sizeof(line) - used, ",other=%llu", static_cast<unsigned long long>(g_callerOther));
        UE_LOGI("[CI-CENSUS] R6 callers: %s", line);
    }
    if (g_unattributed) UE_LOGI("[CI-CENSUS] %s slices with no counted writer=%llu", Role(),
                                static_cast<unsigned long long>(g_unattributed));
}

void Tick(coop::net::Session* s) {
    if (!IsEnabled() || !s || !s->running()) return;
    g_session = s;
    g_host = s->role() == coop::net::Role::Host;
    if (!g_registered) {
        RegisterWatches();
        g_nextReport = NowMs() + kReportMs;
        UE_LOGI("[CI-CENSUS] armed on the %s (%s)", Role(), Mode().c_str());
        if (Mode() != "1" && !IsDrill())
            UE_LOGW("[CI-CENSUS] unknown value '%s': the census runs alone", Mode().c_str());
    }
    SaySeams();
    if (NowMs() >= g_nextReport) {
        g_nextReport = NowMs() + kReportMs;
        ReportNow("30 s");
    }
}

void OnDisconnect() {
    if (!IsEnabled()) return;
    ReportNow("session end");
    Reset();
}

uint64_t RouteEntries(int route) { return route >= 0 && route < 5 ? g_entries[route] : 0; }
uint64_t R4Overlaps() { return g_r4Overlaps; }
R4Overlap LastR4Overlap() { return g_lastR4; }
uint64_t GenCalls() { return g_gen; }
uint64_t RouteVerbs(int route, bool take) {
    return route >= 0 && route < kRoutes ? g_cell[route][take ? 1 : 0] : 0;
}

Seams SeamsState(const char** dead) {
    if (!g_registered) return Seams::Pending;
    bool pending = false;
    for (int i = 0; i < 6; ++i) {
        const int tag = i == 5 ? kGenTag : i + 1;
        if (sg::NameWatchLive(*kRows[i], tag)) continue;
        if (!sg::NameWatchSettled(*kRows[i], tag)) { pending = true; continue; }
        if (dead) *dead = kSeam[i];
        return Seams::Dead;
    }
    return pending ? Seams::Pending : Seams::Live;
}

}  // namespace coop::dev::container_route_census
