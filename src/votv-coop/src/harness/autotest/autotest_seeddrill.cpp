// harness/autotest/autotest_seeddrill.cpp -- the ready-edge seed drill
// (VOTVCOOP_RUN_SEED_DRILL=1, host-only). It authors two distinctively-topic'd emails at the
// two instants a ready-edge seed must handle:
//   1. "[seed-drill] solo" -- BEFORE any client connects. It lands in the joiner's
//      transferred save, the seed's delta is 0 for it, and the vacuous adopt keeps the old
//      retry from re-broadcasting it. The client must hold exactly ONE copy: zero wire
//      applies of this topic is the pass, one is the duplicate reproduced.
//   2. "[seed-drill] in-window" -- while a slot is CONNECTED but NOT world-ready, in the
//      joiner's load window, after its save snapshot was taken. It is not in the save, so
//      only the ready-edge seed can deliver it; the client logging "email_sync: applied
//      email from slot 0" for this topic is the pass.
//
// VOTVCOOP_SEED_DISABLE=1 skips CaptureJoinSnapshot in email_sync and signal_sync, so the
// in-window email never arrives -- which is what shows the seed, not a leftover retry, is
// the delivery mechanism. Grep keys: "[SEED-DRILL] authored [seed-drill] solo" and "... in-window".

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
                // The authoring must land AFTER the OnRequest snapshot: the save serialize fires
                // within ~2-5 s of the connect and the load window is 30-60 s long, so a 3 s wait
                // races ahead of the request and the email rides the SAVE instead, leaving the loss
                // case unexercised. 12 s is safely post-snapshot and still deep in the window. The
                // log ORDER is the per-run proof: "[SEED-DRILL] authored ... in-window" must follow
                // the host's "captured ... at blob instant" line.
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
