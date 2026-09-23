// coop/dev/kerfur_menu_drill.cpp -- see coop/dev/kerfur_menu_drill.h.

#include "coop/dev/kerfur_menu_drill.h"

#include "coop/config/config.h"
#include "coop/creatures/kerfur_form_assembler.h"
#include "coop/dev/spawn_npc.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_snapshot.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <vector>

namespace coop::dev::kerfur_menu_drill {
namespace {

namespace R  = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;
namespace sg = ue_wrap::script_gate;

enum class Step { WaitJoin, WaitNpc, WaitProp, WaitSuccessor, Done, Invalid };

// The classes the two verbs spawn: the NPC's dropProp and the prop's spawnKerfur default to exactly
// these two, so the exact-class index finds each form.
constexpr const wchar_t* kNpcClass  = L"kerfurOmega_C";
constexpr const wchar_t* kPropClass = L"prop_kerfurOmega_C";
constexpr const wchar_t* kMenuEvent = L"actionOptionIndex";
constexpr int     kMenuTag      = 0x4B4D0001;  // 'KM'
constexpr uint8_t kActionTurnOn = 8;           // the prop's event graph calls spawnKerfuro on this action alone

// Game thread only, but for the session pointer the Install fanout stores.
std::atomic<coop::net::Session*> g_session{nullptr};
Step  g_step = Step::WaitJoin;
bool  g_saidArm = false;
bool  g_menuWatched = false;
bool  g_menuLive = false;
std::chrono::steady_clock::time_point g_nextLiveCheck{};
std::vector<void*> g_before;  // every kerfur of either form alive before the spawn
void* g_npc = nullptr;        // the spawned kerfur
void* g_prop = nullptr;       // its prop
int   g_menuEntries = 0;      // the menu event entered on g_prop under this drill's watch
int   g_menuDepth = 0;        // the gate's nesting at that entry

bool IsEnabled_() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::kerfur_menu_drill);
    return s;
}

void Invalid(const char* why) {
    g_step = Step::Invalid;
    UE_LOGW("kerfur_menu_drill: [H] INVALID -- %s", why);
}

// A live, readable instance of exactly `className` that is not in `g_before` and is not `skip`.
void* FindNew(const wchar_t* className, void* skip) {
    void* cls = R::FindClass(className);
    if (!cls) return nullptr;
    struct Ctx { void* skip; void* found; } ctx{skip, nullptr};
    OI::ForEachInstance(cls, [](void* p, void* obj, int32_t index) {
        auto* c = static_cast<Ctx*>(p);
        if (c->found || !obj || obj == c->skip) return;
        if (R::SlotFlags(index) & (R::slot_flags::Dying | R::slot_flags::NotYetReadable)) return;
        if (!R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
        if (std::find(g_before.begin(), g_before.end(), obj) != g_before.end()) return;
        c->found = obj;
    }, &ctx);
    return ctx.found;
}

void RecordBefore() {
    g_before.clear();
    for (const wchar_t* name : {kNpcClass, kPropClass}) {
        void* cls = R::FindClass(name);
        if (!cls) continue;
        OI::ForEachInstance(cls, [](void*, void* obj, int32_t) { if (obj) g_before.push_back(obj); }, nullptr);
    }
}

sg::Verdict OnMenuPre(const sg::Call& c) {
    if (g_prop && c.object == g_prop) {
        ++g_menuEntries;
        g_menuDepth = c.depth;
    }
    return sg::Verdict::Run;
}

// Registered once per process; its liveness is asked at most once a second until it resolves.
void EnsureMenuWatch() {
    if (g_menuLive || g_step == Step::Invalid || !sg::IsInstalled()) return;
    if (!g_menuWatched) {
        g_menuWatched = sg::WatchName(kMenuEvent, kMenuTag, &OnMenuPre, nullptr);
        if (!g_menuWatched) {
            Invalid("the gate refused the watch on the menu event");
            return;
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextLiveCheck) return;
    g_nextLiveCheck = now + std::chrono::seconds(1);
    sg::ResolvePendingNames();
    g_menuLive = sg::NameWatchLive(kMenuEvent, kMenuTag);
    if (!g_menuLive && sg::PendingNameCount() == 0) Invalid("the menu watch is dead in a full gate table");
}

// The function a call on `obj` dispatches to, as the game's own call would reach it.
bool CallVerb(void* obj, const wchar_t* verb) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(obj), verb);
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && ue_wrap::Call(obj, f);
}

bool CallMenuTurnOn(void* prop) {
    void* player = coop::players::Registry::Get().Local();
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(prop), kMenuEvent);
    if (!player || !fn) return false;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    // The hit and the looked-at component stay zeroed: the prop's graph reads the action alone.
    if (!f.Set<void*>(L"player", player) || !f.Set<uint8_t>(L"action", kActionTurnOn)) return false;
    return ue_wrap::Call(prop, f);
}

void TickHost(coop::net::Session* s) {
    switch (g_step) {
    case Step::WaitJoin: {
        if (!g_menuLive) return;
        int slot = -1;
        for (int i = 1; i < static_cast<int>(coop::players::kMaxPeers) && slot < 0; ++i)
            if (s->IsSlotWorldReady(i) && coop::prop_snapshot::IsBracketClosed(i)) slot = i;
        if (slot < 0) return;
        RecordBefore();
        UE_LOGI("kerfur_menu_drill: [H] slot %d's join is over and the menu watch is live; spawning a kerfur "
                "(%zu of either form already alive)", slot, g_before.size());
        coop::dev::spawn_npc::SpawnKerfurOmega();
        g_step = Step::WaitNpc;
        return;
    }
    case Step::WaitNpc: {
        g_npc = FindNew(kNpcClass, nullptr);
        if (!g_npc) return;
        UE_LOGI("kerfur_menu_drill: [H] kerfur %p is up; turning it off by calling dropKerfurProp", g_npc);
        if (!CallVerb(g_npc, L"dropKerfurProp")) { Invalid("the dropKerfurProp call did not dispatch"); return; }
        g_step = Step::WaitProp;
        return;
    }
    case Step::WaitProp: {
        g_prop = FindNew(kPropClass, nullptr);
        if (!g_prop) return;
        UE_LOGI("kerfur_menu_drill: [H] prop %p is down; turning it on through %ls, action %u", g_prop, kMenuEvent,
                static_cast<unsigned>(kActionTurnOn));
        if (!CallMenuTurnOn(g_prop)) { Invalid("the menu event call did not dispatch"); return; }
        if (g_menuEntries == 0) { Invalid("the menu event ran outside the gate: no nesting to test"); return; }
        g_step = Step::WaitSuccessor;
        return;
    }
    case Step::WaitSuccessor: {
        void* npc = FindNew(kNpcClass, g_npc);
        if (!npc) return;
        UE_LOGI("kerfur_menu_drill: [H] kerfur %p is back up; the menu event was entered %d time(s), itself at "
                "gate depth %d, so spawnKerfuro ran one watched body deeper", npc, g_menuEntries, g_menuDepth);
        coop::kerfur_form_assembler::LogSummary("kerfur_menu_drill");
        UE_LOGI("kerfur_menu_drill: [H] done -- the order gate above counts both conversions");
        g_step = Step::Done;
        return;
    }
    default: return;
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    if (!IsEnabled_()) return;
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    if (!IsEnabled_()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != coop::net::Role::Host) return;
    if (!g_saidArm) {
        g_saidArm = true;
        UE_LOGI("kerfur_menu_drill: [H] armed -- waiting for a client's join to end and the menu watch to go live");
    }
    EnsureMenuWatch();
    TickHost(s);
}

void OnDisconnect() {
    g_step = Step::WaitJoin;
    g_saidArm = false;
    g_before.clear();
    g_npc = g_prop = nullptr;
    g_menuEntries = 0;
    g_menuDepth = 0;
}

}  // namespace coop::dev::kerfur_menu_drill
