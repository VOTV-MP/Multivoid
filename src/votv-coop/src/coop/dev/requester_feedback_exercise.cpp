// coop/dev/requester_feedback_exercise.cpp -- see coop/dev/requester_feedback_exercise.h.

#include "coop/dev/requester_feedback_exercise.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/requester_feedback/requester_feedback.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/world/votv_lib.h"

namespace coop::dev::requester_feedback_exercise {
namespace {

constexpr uint8_t kRequesterSlot = 1;
constexpr uint8_t kHintType = 1;
constexpr const wchar_t* kHintText = L"Requester feedback exercise";

bool g_done = false;

bool Enabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::requester_feedback_exercise);
    return s;
}

}  // namespace

void Install(coop::net::Session* session) {
    if (g_done || !Enabled() || !session || !session->running() || session->role() != coop::net::Role::Host) return;
    if (!session->IsSlotWorldReady(kRequesterSlot) || !coop::requester_feedback::Ready()) return;
    void* local = coop::players::Registry::Get().Local();
    if (!local) return;
    g_done = true;
    const uint64_t before = coop::requester_feedback::CountsNow().hintsSent;
    bool raised = false;
    {
        coop::requester_feedback::Scope scope(kRequesterSlot, 0);
        raised = ue_wrap::votv_lib::AddHint(kHintText, kHintType, local);
    }
    const uint64_t routed = coop::requester_feedback::CountsNow().hintsSent - before;
    UE_LOGI("[RF-EXERCISE] host raised its hint inside slot %u's scope: routed=%llu (call %s)",
            static_cast<unsigned>(kRequesterSlot), routed, raised ? "ran" : "did not run");
}

void OnDisconnect() { g_done = false; }

}  // namespace coop::dev::requester_feedback_exercise
