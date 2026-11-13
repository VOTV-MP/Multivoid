// coop/player/leave_in_event.cpp -- see coop/player/leave_in_event.h.
//
// Bytecode ground truth (ui_menu_C on 0.9.0n): the Main Menu button's entry in ExecuteUbergraph_ui_menu runs
// `lib_C::getEvent`, returns on true, and else runs `lib_C::loadLevel(menu)`; the Exit button's entry runs the same
// test, and else shows the exit confirmation. enterPause asks it from its own function, to grey out Save and skip
// the pause; no other frame of the menu asks it.

#include "coop/player/leave_in_event.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"

#include "ue_wrap/core/class_reask.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>

namespace coop::player::leave_in_event {
namespace {

namespace R  = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;
namespace sg = ue_wrap::script_gate;

// A name watch: lib_C is a function library whose class and functions are new objects with each level.
UE_FACT_FN(kRowGetEvent, "leave_in_event", L"lib_C", L"getEvent", Declared);
constexpr int kTag = 0x4C455654;  // 'LEVT'

std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<void*> g_menuGraph{nullptr};      // ui_menu_C's ExecuteUbergraph_ui_menu, this world's
std::atomic<int32_t> g_offIsEventActive{-1};  // getEvent's out parameter

// A class this watch reads from, and the listing it was found under: a class is a new object with each level, and
// one unloaded and loaded again answers a new listing id even at the same address (object_index.h).
struct BoundClass {
    void* cls = nullptr;
    uint64_t listing = 0;
    ue_wrap::class_reask::Latch ask;
};
BoundClass g_menuCls;  // game thread: the class g_menuGraph was read from
BoundClass g_libCls;   // game thread: the class g_offIsEventActive was read from
bool g_watching = false;
uint32_t g_let = 0;                           // game thread: presses let through, for the log

sg::Verdict OnGetEventPre(const sg::Call& c) {
    // Drill red (leave_in_event_red, off by default): the game's answer stands, as before this watch.
    static const bool kRed = coop::config::ResolveFlag(coop::config_registry::rows::leave_in_event_red);
    if (kRed) return sg::Verdict::Run;
    void* graph = g_menuGraph.load(std::memory_order_acquire);
    if (!graph || c.callerFunction != graph) return sg::Verdict::Run;
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return sg::Verdict::Run;
    if (ue_wrap::world_identity::CurrentWorldKind() != ue_wrap::world_identity::WorldKind::Gameplay)
        return sg::Verdict::Run;
    const int32_t off = g_offIsEventActive.load(std::memory_order_acquire);
    uint8_t* out = off >= 0 ? sg::OutParamPtr(c, off) : nullptr;
    if (!out) return sg::Verdict::Run;
    *out = 0;
    ++g_let;
    UE_LOGI("leave_in_event: the pause menu's Main Menu or Exit asked whether an event runs -- answered no, so the "
            "player can leave the session (%u so far)", g_let);
    return sg::Verdict::Cancel;
}

// The class `name`, looked up only while this holder has none: when the class set moves and through the load window
// after it (ue_wrap/core/class_reask), never every tick once it is bound. True when `b` holds the class; an unloaded
// class is dropped (false) so the next call asks again.
bool Bind(BoundClass& b, const wchar_t* name) {
    if (b.cls && OI::ClassListingId(b.cls) != b.listing) {
        b.cls = nullptr;
        b.ask.Reset();
    }
    if (b.cls) return true;
    if (!b.ask.Due()) return false;
    void* cls = OI::ClassByName(name);
    if (!cls) return false;
    b.cls = cls;
    b.listing = OI::ClassListingId(cls);
    return true;
}

// This world's menu graph and getEvent's out parameter. Steady state, with both classes bound, is two listing-id
// hash lookups; a new world's classes bind again at the class set's move. A class that binds without its function or
// parameter leaves the watch inert for that world; each of the two misses is said once a process.
void Refresh() {
    static bool sMenuMissSaid = false, sLibMissSaid = false;
    const uint64_t menuBefore = g_menuCls.listing;
    if (Bind(g_menuCls, L"ui_menu_C")) {
        if (g_menuCls.listing != menuBefore) {
            void* graph = R::FindFunction(g_menuCls.cls, L"ExecuteUbergraph_ui_menu");
            g_menuGraph.store(graph, std::memory_order_release);
            if (!graph && !sMenuMissSaid) {
                sMenuMissSaid = true;
                UE_LOGW("leave_in_event: ui_menu_C has no ExecuteUbergraph_ui_menu here -- the pause menu's buttons wait for an event");
            }
        }
    } else if (menuBefore != 0) {
        g_menuGraph.store(nullptr, std::memory_order_release);
        g_menuCls.listing = 0;
    }
    const uint64_t libBefore = g_libCls.listing;
    if (Bind(g_libCls, L"lib_C")) {
        if (g_libCls.listing != libBefore) {
            void* fn = R::FindFunction(g_libCls.cls, L"getEvent");
            const int32_t off = fn ? R::FindParamOffset(fn, L"isEventActive") : -1;
            g_offIsEventActive.store(off, std::memory_order_release);
            if (off < 0 && !sLibMissSaid) {
                sLibMissSaid = true;
                UE_LOGW("leave_in_event: lib_C::getEvent or its isEventActive parameter is not here -- the watch is inert");
            }
        }
    } else if (libBefore != 0) {
        g_offIsEventActive.store(-1, std::memory_order_release);
        g_libCls.listing = 0;
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    Refresh();
    if (g_watching || !sg::IsInstalled()) return;
    if (sg::WatchName(kRowGetEvent, kTag, &OnGetEventPre, nullptr)) {
        g_watching = true;
        UE_LOGI("leave_in_event: watching lib_C::getEvent -- in a session the pause menu's Main Menu and Exit work "
                "while an event runs");
    }
}

}  // namespace coop::player::leave_in_event
