// harness/autotest/autotest_seeddrill.cpp -- the seeds-arc RED/GREEN drill
// (VOTVCOOP_RUN_SEED_DRILL=1, host-only).
//
// Authors two distinctively-topic'd emails at the two instants the ready-edge
// seed design must handle (votv-signal-email-ready-seeds-DESIGN-2026-08-23 par.3):
//   1. "[seed-drill] solo"      -- BEFORE any client connects. It lands in the
//      joiner's transferred save; the seed's delta is 0 for it and the
//      vacuous-adopt keeps the old retry from re-broadcasting it. The client
//      must hold exactly ONE copy (zero wire applies of this topic = PASS;
//      one wire apply = the pre-existing duplicate reproduced).
//   2. "[seed-drill] in-window" -- while a slot is CONNECTED but NOT world-ready
//      (the joiner's load window, after its save snapshot was taken). Not in the
//      save; only the ready-edge seed can deliver it. Client log
//      "email_sync: applied email from slot 0" for this topic = PASS.
// Mutate control: VOTVCOOP_SEED_DISABLE=1 skips the lanes' CaptureJoinSnapshot
// (wired in email_sync/signal_sync) -- the in-window email then never arrives
// (RED), proving the seed, not a leftover retry, is the delivery mechanism.
//
// Grep keys: "[SEED-DRILL] authored solo", "[SEED-DRILL] authored in-window",
// and email_sync's own applied/seed lines.

#include "harness/autotest.h"

#include "coop/net/session.h"
#include "harness/session_runtime.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/world/email.h"

#include <atomic>

namespace harness::autotest {
namespace {

namespace GT = ue_wrap::game_thread;

void AuthorDrillEmail(const wchar_t* topic) {
    GT::Post([topic] {
        ue_wrap::email::Row r;
        r.username = 0;
        r.topic = topic;
        r.text = L"seeds-arc drill row (autotest_seeddrill)";
        if (ue_wrap::email::AddEmail(r))
            UE_LOGI("[SEED-DRILL] authored %ls", topic);
        else
            UE_LOGW("[SEED-DRILL] AddEmail FAILED for %ls", topic);
    });
}

}  // namespace

DWORD WINAPI SeedDrillThread(LPVOID /*arg*/) {
    if (IsClientRole()) {
        UE_LOGI("[SEED-DRILL] client role -- host-only drill, exiting");
        return 0;
    }
    auto& s = harness::session_runtime::Session();

    // Phase 1: the SOLO email -- must land before any client connects (the smoke
    // launches the client ~20 s after host boot; author as soon as the session
    // runs and the world can take it). If a peer already connected, skip: the
    // solo case needs the pre-connect save state.
    for (int i = 0; i < 600 && !s.running(); ++i) ::Sleep(100);
    ::Sleep(8000);  // world-up settle (the email array must be live)
    bool anyPeer = false;
    for (int i = 1; i < coop::net::kMaxPeers; ++i)
        if (s.IsSlotWorldReady(i)) anyPeer = true;
    if (!anyPeer) {
        AuthorDrillEmail(L"[seed-drill] solo");
    } else {
        UE_LOGW("[SEED-DRILL] a peer is already world-ready -- solo phase skipped");
    }

    // Phase 2: the IN-WINDOW email -- wait for a slot that is CONNECTED but not
    // yet world-ready (the joiner's load window, which starts AFTER its save
    // snapshot is captured at the transfer OnRequest). Author once.
    UE_LOGI("[SEED-DRILL] waiting for a connected-but-not-ready slot (the load window)...");
    for (int waited = 0; waited < 3000; ++waited) {  // up to 5 min
        for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
            if (s.HasPeerConn(slot) && !s.IsSlotWorldReady(slot)) {
                // The authoring must land AFTER the OnRequest snapshot (the save
                // serialize fires within ~2-5 s of the connect; the load window is
                // 30-60 s long). A 3 s wait raced AHEAD of the request in the first
                // run (authored :46, snapshot :47 -> the email rode the SAVE and the
                // loss case never exercised) -- 12 s is safely post-snapshot while
                // still deep in the window. The log ORDER is the per-run proof:
                // "[SEED-DRILL] authored ... in-window" must follow the host's
                // "captured ... at blob instant" line.
                ::Sleep(12000);
                if (s.IsSlotWorldReady(slot)) {
                    UE_LOGW("[SEED-DRILL] slot %d raced to ready during the wait -- "
                            "in-window phase NOT run (inconclusive, re-run)", slot);
                    return 0;
                }
                AuthorDrillEmail(L"[seed-drill] in-window");
                return 0;
            }
        }
        ::Sleep(100);
    }
    UE_LOGW("[SEED-DRILL] no load window observed -- in-window phase not run");
    return 0;
}

}  // namespace harness::autotest
