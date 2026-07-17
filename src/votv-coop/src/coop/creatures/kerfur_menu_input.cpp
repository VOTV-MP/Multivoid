// coop/kerfur_menu_input.cpp -- see coop/kerfur_menu_input.h.

#include "coop/creatures/kerfur_menu_input.h"

#include "coop/creatures/kerfur_command.h"  // TryRecordMenuCommand (the existing client->host relay)
#include "coop/net/session.h"
#include "ue_wrap/engine/engine.h"       // ReadMainPlayerLookAtActor + ReadMainPlayerRadialSelect
#include "ue_wrap/core/game_thread.h"  // RegisterPreObserver
#include "ue_wrap/actors/kerfur.h"       // IsKerfurActor + ReadKerfurState
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <unordered_map>

namespace coop::kerfur_menu_input {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace P  = ue_wrap::profile;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_installed = false;  // observer-registration latch (the cb stays registered for process life)

// The radial actionIndex -> verb name, for a NON-busy kerfur. The option-list order is
// bytecode-verified (votv-npc-action-menu-RE, kerfurOmega::getActionOptions):
//   [0:turn_off, 1:follow, 2:idle, 3:patrol, 4:fix_servers, 5:get_reports, 6:fix_transformers,
//    7:take_object, 8:pat, 9:equipment].
// We relay only the 6 State verbs (1..6). turn_off is poll-driven (kerfur_convert); take_object /
// pat / equipment are out of v1 scope (held-object resolution / cosmetic montage / per-player UI).
const wchar_t* VerbFromNonBusyIndex(int32_t idx) {
    switch (idx) {
        case 1: return L"follow";
        case 2: return L"idle";
        case 3: return L"patrol";
        case 4: return L"fix_servers";
        case 5: return L"get_reports";
        case 6: return L"fix_transformers";
        default: return nullptr;
    }
}

// CLIENT radial-confirm detection. PRE observer on AmainPlayer_C::InpActEvt_use (PE-visible; the
// SAME seam interactable_sync uses for doors). Fires BEFORE the local useSelectedAction body, so
// actionIndex/releaseEToUse still hold the selection. We do NOT cancel: the local actionName then
// runs transiently on the parked mirror and the host's authoritative State stream re-asserts it (no
// flicker for an accepted verb). Puppets are unpossessed -> never process input -> this fires only
// for the local player.
void OnUseInputPre(void* self, void* /*function*/, void* /*params*/) {
    if (!self) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return;  // CLIENT-only
    bool release = false;
    int32_t idx = -1;
    if (!ue_wrap::engine::ReadMainPlayerRadialSelect(self, release, idx)) return;
    if (!release) return;  // not the radial "release E to use" confirm -> filters the press edge + plain E-use
    void* kerfur = ue_wrap::engine::ReadMainPlayerLookAtActor(self);
    if (!kerfur || !ue_wrap::kerfur::IsKerfurActor(kerfur)) return;  // not aiming at a kerfur
    uint8_t state = 0;
    bool spooky = false;
    uint8_t face = 0;
    if (!ue_wrap::kerfur::ReadKerfurState(kerfur, state, spooky, face)) return;
    // A BUSY kerfur (State 3/4/5 = the fix_servers/get_reports/fix_transformers task) offers only
    // [turn_off, equipment] -> no State verb is reachable, so an index here would mis-map. Skip.
    if (state == 3 || state == 4 || state == 5) return;
    const wchar_t* verb = VerbFromNonBusyIndex(idx);
    if (!verb) return;  // turn_off (poll) / take_object / pat / equipment -> not relayed in v1
    // InpActEvt_use fires on BOTH press + release; releaseEToUse already isolates the confirm, but
    // collapse any same-kerfur repeat within a tap window defensively (game-thread-only map).
    static std::unordered_map<void*, std::chrono::steady_clock::time_point> s_last;
    const auto now = std::chrono::steady_clock::now();
    if (auto it = s_last.find(kerfur);
        it != s_last.end() && now - it->second < std::chrono::milliseconds(250)) {
        return;
    }
    s_last[kerfur] = now;
    // Record via the existing relay: queued here -> kerfur_command::Tick sends a KerfurCommand ->
    // host OnCommandRequest -> ExecuteHostCommand (RunActionName for State verbs / the ownership
    // follow loop). isHost=false: this seam is CLIENT-only (the host's own use is stream-mirrored).
    coop::kerfur_command::TryRecordMenuCommand(kerfur, verb, /*isClient=*/true, /*isHost=*/false);
    UE_LOGI("kerfur_menu_input: client radial verb '%ls' on kerfur=%p (actionIndex=%d State=%u) -> relayed to host",
            verb, kerfur, idx, static_cast<unsigned>(state));
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);  // re-cache every call (reconnect)
    if (g_installed) return;
    void* cls = R::FindClass(P::name::MainPlayerClass);
    if (!cls) return;  // mainPlayer_C not loaded yet -> retry on the next world-gated Install
    void* fn = R::FindFunction(cls, P::name::MainPlayerUseInputEventFn);
    if (!fn) {
        UE_LOGW("kerfur_menu_input: InpActEvt_use UFunction not found -- client kerfur menu verbs cannot be relayed");
        g_installed = true;  // permanent give-up (don't re-walk the class forever)
        return;
    }
    if (!GT::RegisterPreObserver(fn, &OnUseInputPre)) {
        UE_LOGW("kerfur_menu_input: InpActEvt_use PRE observer register failed (table full?)");
        return;  // not latched -> retry next Install
    }
    g_installed = true;
    UE_LOGI("kerfur_menu_input: client radial-confirm observer installed on InpActEvt_use (kerfur menu verb relay)");
}

void OnDisconnect() {
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::kerfur_menu_input
