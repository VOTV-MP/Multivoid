// coop/world/event_box_authority.cpp -- see coop/world/event_box_authority.h.

#include "coop/world/event_box_authority.h"

#include "coop/net/session.h"
#include "coop/world/spawn_authority.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>

namespace coop::event_box_authority {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

UE_FACT_FN(kRowOverlap, "event_box_authority", L"trigger_box_N_C",
           L"BndEvt__Box_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature", Declared);
constexpr int kTag = 0x45564258;   // 'EVBX'

// The boxes whose walk-in makes props every peer shares, by the name the level gives each.
constexpr const wchar_t* kShared[] = {
    L"TB_event_arirToe",   // the dropped Ariral gun (toeStab, day 24)
};

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_registered = false;

sg::Verdict OnOverlapPre(const sg::Call& call) {
    if (call.tag != kTag || !call.object || !coop::spawn_authority::IsActiveClientSession())
        return sg::Verdict::Run;
    for (const wchar_t* name : kShared) {
        if (!R::NameStartsWith(R::NameOf(call.object), name)) continue;
        static bool s_said = false;
        if (!s_said) {
            s_said = true;
            UE_LOGI("event_box: this client's %ls refused -- the host's box runs for this player's puppet", name);
        }
        return sg::Verdict::Cancel;
    }
    return sg::Verdict::Run;
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    coop::net::Session* const s = g_session.load(std::memory_order_acquire);
    if (g_registered || !s || !s->running()) return;
    if ((g_registered = sg::WatchName(kRowOverlap, kTag, &OnOverlapPre, nullptr)))
        UE_LOGI("event_box: the event boxes' walk-in is watched");
}

void OnDisconnect() { g_session.store(nullptr, std::memory_order_release); }

}  // namespace coop::event_box_authority
