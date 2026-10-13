// harness/autotest/autotest_seeddrill.cpp -- the ready-edge seed drill
// (VOTVCOOP_RUN_SEED_DRILL=1, host-only). It authors two distinctively-topic'd emails at the
// two instants a ready-edge seed must handle:
//   1. "[seed-drill] solo" -- BEFORE any client connects. It lands in the joiner's
//      transferred save, the seed's delta is 0 for it, and the vacuous adopt keeps the old
//      retry from re-broadcasting it. The client must hold exactly ONE copy: zero wire
//      applies of this topic is the pass, one is the duplicate reproduced.
//   2. "[seed-drill] in-window" -- while a slot is CONNECTED but NOT world-ready, after the
//      host took its world. Only the ready-edge seed can deliver it; the client logging
//      "email_sync: applied email from slot 0" for this topic is the pass.
//
// VOTVCOOP_SEED_DISABLE=1 skips the seeds' capture; the in-window email is still authored (the
// drill waits on the save transfer's instant, not on the capture) and must never arrive: the seed,
// not a leftover retry, delivers it. Grep key: "[SEED-DRILL] authored", once per topic.

#include "harness/autotest.h"

#include "coop/net/session.h"
#include "coop/save/save_transfer.h"
#include "coop/world/email_sync.h"
#include "harness/session_runtime.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/world/email.h"

#include <atomic>

namespace harness::autotest {
namespace {

namespace GT = ue_wrap::game_thread;

// The email lane's own predicate lives on the game thread; this thread asks it there.
bool EmailSettled() {
    return GT::RunAndWait([](std::atomic<int>& a) { a.store(coop::email_sync::Settled() ? 1 : 2); }) == 1;
}

// Game thread. False only when the game cannot be asked yet (NotNow): the caller asks again.
bool AuthorNow(const wchar_t* topic) {
    ue_wrap::email::Row r;
    r.username = 0;
    r.topic = topic;
    r.text = L"seeds-arc drill row (autotest_seeddrill)";
    const ue_wrap::RowApply a = ue_wrap::email::AddEmail(r);
    if (a.result == ue_wrap::RowApplyResult::NotNow) return false;
    if (a.result == ue_wrap::RowApplyResult::Applied)
        UE_LOGI("[SEED-DRILL] authored %ls", topic);
    else
        UE_LOGW("[SEED-DRILL] AddEmail FAILED for %ls -- %s", topic, a.why);
    return true;
}

// Authors the in-window email for the first slot inside its load window past the host's taking of its
// world; the check and the add are one game-thread task. The ready edge sets the slot's ready flag and
// runs its seed in one game-thread step (event_feed's ClientWorldReady), so a slot found not ready here
// is not seeded until this task has added the row: the row cannot land after the seed and ride the
// live broadcast instead.
bool AuthorInWindow(coop::net::Session& s) {
    return GT::RunAndWait([&s](std::atomic<int>& a) {
               for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
                   if (!s.HasPeerConn(slot) || s.IsSlotWorldReady(slot) ||
                       !coop::save_transfer::WorldTakenFor(slot))
                       continue;
                   a.store(AuthorNow(L"[seed-drill] in-window") ? 1 : 2);
                   return;
               }
               a.store(2);
           }) == 1;
}

}  // namespace

DWORD WINAPI SeedDrillThread(LPVOID /*arg*/) {
    if (IsClientRole()) {
        UE_LOGI("[SEED-DRILL] client role -- host-only drill, exiting");
        return 0;
    }
    auto& s = harness::session_runtime::Session();

    // Phase 1: the SOLO email -- must land before any client connects. Authored the moment the
    // session runs and the email lane has settled against its array, the state a row added to this
    // world needs; the loop bounds below are failure bounds, never a wait. If a peer already
    // connected, skip: the solo case needs the pre-connect save state.
    for (int i = 0; i < 600 && !s.running(); ++i) ::Sleep(100);
    if (!s.running()) {
        UE_LOGW("[SEED-DRILL] the session never ran -- drill not run");
        return 0;
    }
    bool settled = false;
    for (int i = 0; i < 1200 && s.running() && !(settled = EmailSettled()); ++i) ::Sleep(100);
    if (!settled) {
        UE_LOGW(s.running() ? "[SEED-DRILL] the email lane never settled -- drill not run"
                            : "[SEED-DRILL] the session stopped -- drill not run");
        return 0;
    }
    bool anyPeer = false;
    for (int i = 1; i < coop::net::kMaxPeers; ++i)
        if (s.IsSlotWorldReady(i)) anyPeer = true;
    if (!anyPeer) {
        GT::Post([] {
            if (!AuthorNow(L"[seed-drill] solo"))
                UE_LOGW("[SEED-DRILL] the game could not take the solo email yet -- solo phase not run");
        });
    } else {
        UE_LOGW("[SEED-DRILL] a peer is already world-ready -- solo phase skipped");
    }

    // Phase 2: the IN-WINDOW email -- authored for a slot that is CONNECTED, not yet world-ready (the
    // joiner's load window), and whose world the host has taken: a row authored before that instant
    // would ride the SAVE and leave the loss case unexercised, so the instant itself is the state
    // waited on. Author once. The loop bound is a failure bound.
    UE_LOGI("[SEED-DRILL] waiting for a connected-but-not-ready slot past its world capture...");
    for (int waited = 0; waited < 3000; ++waited) {  // up to 5 min
        if (AuthorInWindow(s)) return 0;
        ::Sleep(100);
    }
    UE_LOGW("[SEED-DRILL] no load window observed -- in-window phase not run");
    return 0;
}

}  // namespace harness::autotest
