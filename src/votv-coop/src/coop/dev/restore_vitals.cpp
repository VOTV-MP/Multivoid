#include "coop/dev/restore_vitals.h"

#include "coop/dev/dev_gate.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/vitals.h"

#include <atomic>

namespace coop::dev::restore_vitals {

namespace GT = ue_wrap::game_thread;
namespace V = ue_wrap::vitals;

namespace {

// Cached Session pointer for the broadcast. nullptr-safe at every callsite
// (a press before Session start is logged + no-op'd, NOT crashed).
std::atomic<coop::net::Session*> g_session{nullptr};

// VOTV's vitals top out at 100.0 (% units). Writing >100 doesn't visually
// over-fill the meter -- the BP graph clamps display at 100. Picking the
// exact UI cap rather than something larger so subsequent food consumption
// behaves identically to a player who reached max naturally (no hidden
// over-stored value that would gate hunger draining differently).
// 2026-05-25 NIGHT (user retest +1): writing coffeePower=100 produced a
// screen-shaking post-coffee vanilla effect (the BP keys an effect off
// coffeePower > 0). Restore only tops up food/sleep/health, leaving coffeePower as-is.
constexpr float kMaxVital = 100.0f;

// The GameInstance->save_gameInst->saveSlot resolution + the cached-offset writes
// now live in ue_wrap::vitals (extracted 2026-05-30, P7). This file owns only the
// menu action + the coop broadcast.

}  // namespace

void SetSession(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void ApplyLocally() {
    // Game-thread only (saveSlot writes touch BP state). ue_wrap::vitals caches
    // the resolution after the first call; a press before the save is registered
    // returns false on each field and is logged, not crashed.
    const bool food   = V::Write(V::Field::Food,   kMaxVital);
    const bool sleep  = V::Write(V::Field::Sleep,  kMaxVital);
    const bool health = V::Write(V::Field::Health, kMaxVital);
    if (food && sleep && health) {
        UE_LOGI("restore_vitals: vitals refilled (food/sleep/health = %.1f)", kMaxVital);
    } else {
        UE_LOGW("restore_vitals: refill incomplete (food=%d sleep=%d health=%d) -- "
                "save not registered / still booting?", food, sleep, health);
    }
}

void Restore() {
    // Strict client lockout: a joined client refilling its own vitals is a
    // survival cheat in someone else's game (coop::dev_gate).
    if (!coop::dev_gate::Allowed()) {
        UE_LOGW("restore_vitals: REFUSED -- dev features are disabled while connected as a client");
        return;
    }
    // Local apply runs on the game thread (saveSlot writes touch BP state); the
    // broadcast is wire-thread-safe, so this is safe to call from the menu (render
    // thread). Best-effort: if the Session isn't connected, the local apply still
    // refills the caller's vitals (solo play).
    GT::Post([] { ApplyLocally(); });
    if (auto* s = g_session.load(std::memory_order_acquire)) {
        // No payload: the action is fixed (max-out food/sleep/health on the peer).
        if (!s->SendReliable(coop::net::ReliableKind::RestoreVitals, nullptr, 0))
            UE_LOGW("restore_vitals: broadcast failed (channel busy or not connected)");
    }
    UE_LOGI("restore_vitals: Restore triggered (local apply + peer broadcast)");
}

void SetStaminaLow() {
    // Strict client lockout (same as Restore -- dev verbs are host-only).
    if (!coop::dev_gate::Allowed()) {
        UE_LOGW("restore_vitals: SetStaminaLow REFUSED -- dev features are disabled while connected as a client");
        return;
    }
    // VOTV has no separate "stamina" scalar (verified against the full CXXHeaderDump) -- the player's
    // energy/exhaustion meter IS `sleep`: low sleep -> tired -> isExhausted -> can't sprint (saveSlot.sleep,
    // drained by mainPlayer.sleepDraining). So "set stamina low" sets ONLY sleep=10, leaving food + health
    // alone. LOCAL ONLY -- no broadcast: the v19 vitals DISPLAY stream (PoseSnapshot sleepFrac) already
    // mirrors the low value to peers' nameplates, so this stays a one-peer test. Posted to the game thread
    // (saveSlot writes touch BP state).
    GT::Post([] {
        constexpr float kLow = 10.0f;
        if (V::Write(V::Field::Sleep, kLow))
            UE_LOGI("restore_vitals: stamina (sleep) set LOW (%.0f)", kLow);
        else
            UE_LOGW("restore_vitals: set-stamina-low failed -- save not registered / still booting?");
    });
}

}  // namespace coop::dev::restore_vitals
