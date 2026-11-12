// coop/player/leave_in_event.cpp -- see coop/player/leave_in_event.h.
//
// Bytecode ground truth (ui_menu_C on 0.9.0n): the Main Menu button's entry in ExecuteUbergraph_ui_menu runs
// `lib_C::getEvent`, returns on true, and else runs `lib_C::loadLevel(menu)`; the Exit button's entry runs the same
// test, and else shows the exit confirmation. enterPause asks it from its own function, to grey out Save and skip
// the pause; no other frame of the menu asks it.

#include "coop/player/leave_in_event.h"

#include "coop/net/session.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

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
void* g_menuCls = nullptr;                    // game thread: the class g_menuGraph was read from
void* g_libCls = nullptr;
bool g_watching = false;
uint32_t g_let = 0;                           // game thread: presses let through, for the log

sg::Verdict OnGetEventPre(const sg::Call& c) {
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

// This world's menu graph and getEvent's out parameter, read again when either class is a new object.
void Refresh() {
    void* menu = OI::ClassByName(L"ui_menu_C");
    if (menu != g_menuCls) {
        g_menuCls = menu;
        g_menuGraph.store(menu ? R::FindFunction(menu, L"ExecuteUbergraph_ui_menu") : nullptr,
                          std::memory_order_release);
    }
    void* lib = OI::ClassByName(L"lib_C");
    if (lib != g_libCls) {
        g_libCls = lib;
        void* fn = lib ? R::FindFunction(lib, L"getEvent") : nullptr;
        g_offIsEventActive.store(fn ? R::FindParamOffset(fn, L"isEventActive") : -1, std::memory_order_release);
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    static ULONGLONG s_next = 0;
    const ULONGLONG now = ::GetTickCount64();
    if (now < s_next) return;
    s_next = now + 1000;
    Refresh();
    if (g_watching || !sg::IsInstalled()) return;
    if (sg::WatchName(kRowGetEvent, kTag, &OnGetEventPre, nullptr)) {
        g_watching = true;
        UE_LOGI("leave_in_event: watching lib_C::getEvent -- in a session the pause menu's Main Menu and Exit work "
                "while an event runs");
    }
}

}  // namespace coop::player::leave_in_event
