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
constexpr int kStack = 16;       // open scopes, nesting included
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

uint8_t g_stack[kStack] = {};     // route ids of the open scopes, innermost last
int g_depth = 0;
uint64_t g_underflow = 0;
uint64_t g_overflow = 0;

uint64_t g_entries[5] = {};       // seam entries R1..R5
uint64_t g_unshared[6] = {};      // verbs on an inventory that is no shared world container, by the open route
uint64_t g_cell[kRoutes][2] = {};     // [route][add=0, take=1] verbs on a shared container
uint64_t g_slices[kRoutes][2] = {};
uint64_t g_unattributed = 0;
uint64_t g_slicesSent = 0;
uint64_t g_gen = 0;
uint64_t g_hostSlice[coop::net::kMaxPeers][2] = {};   // [slot][accepted=0, refused=1]

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
char g_r4Classes[kClasses][kName] = {};
int g_r4ClassCount = 0;

const char* Role() { return g_host ? "host" : "client"; }

uint64_t NowMs() { return ::GetTickCount64(); }

void Narrow(const std::wstring& w, char* out, size_t cap) {
    size_t i = 0;
    for (; i + 1 < cap && i < w.size(); ++i) out[i] = w[i] < 128 ? static_cast<char>(w[i]) : '?';
    out[i] = 0;
}

int Innermost() { return g_depth > 0 ? g_stack[g_depth - 1] : -1; }

void Push(int route) {
    if (g_depth >= kStack) { ++g_overflow; return; }
    g_stack[g_depth++] = static_cast<uint8_t>(route);
}

// Removes the innermost open scope of `route`. False when none is open.
bool Pop(int route) {
    for (int i = g_depth - 1; i >= 0; --i) {
        if (g_stack[i] != route) continue;
        for (int j = i; j + 1 < g_depth; ++j) g_stack[j] = g_stack[j + 1];
        --g_depth;
        return true;
    }
    return false;
}

void* ParamPtr(const sg::Call& c, R::ParamOffset& param) {
    const int32_t off = c.function ? param.Of(c.function) : -1;
    if (off < 0 || !c.locals) return nullptr;
    void* p = nullptr;
    std::memcpy(&p, c.locals + off, sizeof(p));
    return p;
}

// R3 counts only the put, the BP's own branch: the player's input_run.
bool PlayerRuns(const sg::Call& c) {
    static R::ParamOffset sPlayer(L"player");
    static void* sCls = nullptr;
    static int32_t sOff = -1;
    static uint8_t sMask = 0;
    void* player = ParamPtr(c, sPlayer);
    if (!player || !R::IsLive(player)) return false;
    void* cls = R::ClassOf(player);
    if (cls != sCls) {
        sCls = cls;
        sOff = -1;
        R::FindBoolProperty(cls, L"input_run", sOff, sMask);
        if (sOff < 0) UE_LOGW("[CI-CENSUS] R3: the player's input_run does not resolve -- R3 is not counted");
    }
    return sOff >= 0 && (*(static_cast<const uint8_t*>(player) + sOff) & sMask) != 0;
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

sg::Verdict ScopePre(const sg::Call& c) {
    const int route = c.tag - 1;
    if (route < 0 || route > 4) return sg::Verdict::Run;
    if (route == kR3 && !PlayerRuns(c)) return sg::Verdict::Run;   // only the put is an R3 entry
    ++g_entries[route];
    if (route == kR4) NoteOverlap(c);
    Push(route);
    return sg::Verdict::Run;
}

void ScopePost(const sg::Call& c) {
    const int route = c.tag - 1;
    if (route < 0 || route > 4) return;
    // R3 pushes only for the put, so its post may find nothing to close.
    if (!Pop(route) && route != kR3) ++g_underflow;
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
    sg::PreFn pre[5] = {&ScopePre, &ScopePre, &ScopePre, &ScopePre, &ScopePre};
    bool all = true;
    for (int i = 0; i < 5; ++i) {
        const bool ok = sg::WatchName(*kRows[i], i + 1, pre[i], &ScopePost);
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
    g_depth = 0;
    g_underflow = g_overflow = 0;
    std::memset(g_entries, 0, sizeof(g_entries));
    std::memset(g_unshared, 0, sizeof(g_unshared));
    std::memset(g_cell, 0, sizeof(g_cell));
    std::memset(g_slices, 0, sizeof(g_slices));
    std::memset(g_hostSlice, 0, sizeof(g_hostSlice));
    std::memset(g_last, 0, sizeof(g_last));
    for (Last& l : g_last) l.route = -1;
    std::memset(g_callers, 0, sizeof(g_callers));
    std::memset(g_r4Classes, 0, sizeof(g_r4Classes));
    g_unattributed = g_slicesSent = g_gen = 0;
    g_lastNext = g_callerCount = g_r4ClassCount = g_r4Lines = 0;
    g_callerOther = g_r4Sim = g_r4Overlaps = 0;
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
bool IsDrill() { return Mode() == "drill" || Mode() == "drill-before"; }
bool IsBeforeOnly() { return Mode() == "drill-before"; }

Quiet::Quiet() { ++g_quiet; }
Quiet::~Quiet() { --g_quiet; }


void OnVerb(const sg::Call& c, bool take) {
    if (!IsEnabled() || g_quiet > 0) return;
    const int open = Innermost();
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
    ++g_slicesSent;
    for (Last& l : g_last) {
        if (l.eid != eid || l.route < 0) continue;
        ++g_slices[l.route][l.verb];
        l.route = -1;
        return;
    }
    ++g_unattributed;
}

void NoteSliceJudged(uint8_t senderSlot, bool accepted) {
    if (!IsEnabled() || senderSlot >= coop::net::kMaxPeers) return;
    ++g_hostSlice[senderSlot][accepted ? 0 : 1];
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
    if (g_host)
        for (int s = 0; s < coop::net::kMaxPeers; ++s)
            if (g_hostSlice[s][0] || g_hostSlice[s][1])
                UE_LOGI("[CI-CENSUS] host slices from slot %d: accepted=%llu refused=%llu", s,
                        static_cast<unsigned long long>(g_hostSlice[s][0]), static_cast<unsigned long long>(g_hostSlice[s][1]));
    for (int r = 0; r < 5; ++r) {
        int depth = 0;
        for (int i = 0; i < g_depth; ++i) depth += g_stack[i] == r ? 1 : 0;
        if (depth) UE_LOGW("[CI-CENSUS] scope leak route=R%d depth=%d", r + 1, depth);
    }
    if (g_underflow || g_overflow)
        UE_LOGW("[CI-CENSUS] scope bookkeeping: post without a pre=%llu, nesting past %d=%llu",
                static_cast<unsigned long long>(g_underflow), kStack, static_cast<unsigned long long>(g_overflow));
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

uint64_t SlicesSent() { return g_slicesSent; }
uint64_t RouteEntries(int route) { return route >= 0 && route < 5 ? g_entries[route] : 0; }
uint64_t R4Overlaps() { return g_r4Overlaps; }
uint64_t GenCalls() { return g_gen; }
uint64_t HostSlices(int slot, bool accepted) {
    return slot >= 0 && slot < coop::net::kMaxPeers ? g_hostSlice[slot][accepted ? 0 : 1] : 0;
}
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
