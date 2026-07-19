// harness/autotest_playerdmg.cpp -- the Inc3-WIRE PlayerDamage relay e2e
// (VOTVCOOP_RUN_PLAYERDMG_TEST): the HOST sends synthetic PlayerDamage to slot
// 1 and observes its puppet flash via the round-trip streamed health drop; the
// CLIENT is passive. Extracted verbatim from harness/autotest_vitals.cpp
// (2026-07-19 s27 dissolve); interfaces + per-routine docs in harness/autotest.h.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/player/player_damage.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"

#include <atomic>
#include <memory>
#include <string>

namespace harness::autotest {
namespace {

namespace GT = ue_wrap::game_thread;
namespace cfg = coop::config;

// Bounded spin-wait on a game-thread task's completion flag. Returns true if
// the task signalled (set the flag non-zero), false if it never completed
// within timeoutMs -- which would mean the posted task faulted (the SEH
// firewall ate the AV and the flag was never set). A bound is mandatory:
// driving ragdollMode on the local player COULD fault and an unbounded wait
// would hang the whole smoke.
bool WaitDone(const std::shared_ptr<std::atomic<int>>& d, int timeoutMs) {
    for (int i = 0; i < timeoutMs / 5 && d->load() == 0; ++i) ::Sleep(5);
    return d->load() != 0;
}

// Read whether the slot-1 puppet has a LIVE ragdoll display body whose mesh is
// physically simulating (2026-06-01 xray-actor rework: the visible flop is a

// ===================== Inc3-WIRE relay e2e (PlayerDamage) =====================
// Proves the WHOLE host->owner damage relay with NO real enemy. The HOST both DRIVES
// and OBSERVES: it sends synthetic PlayerDamage to slot 1 (player_damage::
// DebugForceHitPuppet) -> the CLIENT receives it (event_feed) and runs Add Player
// Damage on its OWN possessed player (player_damage::OnWireDamage) -> the client's
// saveSlot.health drops -> Inc1 streams the lower fraction -> the host's slot-1 puppet
// SetVitals detects the drop and flashes (Inc3). The CLIENT is passive (receive +
// apply automatically). Gated by env VOTVCOOP_RUN_PLAYERDMG_TEST=1.

void DrivePlayerDamageOnHost() {
    UE_LOGI("playerdmg_test[host]: driver+observer armed -- send synthetic PlayerDamage to slot 1, "
            "confirm its puppet flashes via the round-trip streamed health drop");

    // Wait for the slot-1 puppet AND the client's Player Element (so SendPlayerDamage
    // can stamp targetElementId). Both resolve a few seconds after the client connects.
    bool ready = false;
    for (int attempt = 0; attempt < 60 && !ready; ++attempt) {
        auto done = std::make_shared<std::atomic<int>>(0);
        auto ok = std::make_shared<int>(0);
        GT::Post([done, ok] {
            auto& reg = coop::players::Registry::Get();
            if (reg.Puppet(1) != nullptr && reg.GetPlayerElement(1) != nullptr) *ok = 1;
            done->store(1);
        });
        WaitDone(done, 8000);
        ready = (*ok != 0);
        if (!ready) ::Sleep(1000);
    }
    if (!ready) {
        UE_LOGW("playerdmg_test[host]: VERDICT INCONCLUSIVE -- slot-1 puppet / Player Element never "
                "resolved (no client connected?)");
        UE_LOGI("playerdmg_test[host]: DONE");
        return;
    }
    UE_LOGI("playerdmg_test[host]: slot-1 ready -- interleaving PlayerDamage hits with flash observation");

    // INTERLEAVE send + observe: the hurt-flash is a transient 500 ms window, and the
    // host both drives AND observes, so we must poll WHILE the flash is open. Each
    // iteration sends ONE hit, then polls the puppet's streamed GetHealth() (DIAGNOSTIC
    // -- proves the round-trip drop actually streamed) + IsHurtFlashing across the
    // round-trip + flash window. Bigger hits (18 dmg) so the drop is unmistakable even
    // against any passive health regen; 4 hits x 18 = 72 from ~100 -> ~28 (no death).
    // Breaks on the first observed flash.
    bool sawFlash = false;
    float minHealthSeen = 1.f;
    int hits = 0;
    // 50 dmg/hit, max 2 hits (100 worst-case if fully dealt -> >=0; we break on the
    // first observed drop, so health stays high). A big hit tells absorption (armor
    // fully ate the earlier 18) from a genuine host/client no-op.
    for (int i = 0; i < 2 && !sawFlash && minHealthSeen > 0.4f; ++i) {
        {
            auto done = std::make_shared<std::atomic<int>>(0);
            GT::Post([done] {
                const bool sent = coop::player_damage::DebugForceHitPuppet(1, 50.f);
                UE_LOGI("playerdmg_test[host]: DebugForceHitPuppet(slot=1, dmg=50) sent=%d", sent ? 1 : 0);
                done->store(1);
            });
            WaitDone(done, 8000);
            ++hits;
        }
        // Poll the streamed health + flash across the round-trip + flash window (~900 ms).
        for (int p = 0; p < 6 && !sawFlash; ++p) {
            ::Sleep(150);
            auto done = std::make_shared<std::atomic<int>>(0);
            auto flashing = std::make_shared<int>(0);
            auto health = std::make_shared<float>(-1.f);
            GT::Post([done, flashing, health] {
                coop::RemotePlayer& rp = coop::puppet_drive::Puppet(1);
                if (rp.valid()) {
                    *health = rp.GetHealth();
                    if (rp.IsHurtFlashing()) *flashing = 1;
                }
                done->store(1);
            });
            WaitDone(done, 8000);
            if (*health >= 0.f && *health < minHealthSeen) minHealthSeen = *health;
            if (*flashing) sawFlash = true;
        }
        UE_LOGI("playerdmg_test[host]: after hit %d -- puppet streamed health=%.3f, flashing=%d",
                hits, minHealthSeen, sawFlash ? 1 : 0);
    }
    UE_LOGI("playerdmg_test[host]: sent %d hit(s); minStreamedHealth=%.3f sawFlash=%d",
            hits, minHealthSeen, sawFlash ? 1 : 0);

    if (sawFlash)
        UE_LOGI("playerdmg_test[host]: VERDICT Inc3-WIRE relay PASS -- a host-sent PlayerDamage made the "
                "client apply damage to its OWN player; the streamed health drop flashed the host's slot-1 "
                "puppet (full reliable host->owner relay, no real enemy)");
    else
        UE_LOGW("playerdmg_test[host]: VERDICT FAIL -- sent PlayerDamage but never saw the slot-1 puppet "
                "flash. Check the CLIENT log for 'player_damage: applied' (apply) + a health-frac stream.");
    UE_LOGI("playerdmg_test[host]: DONE");
}

// CLIENT side: passive. It receives PlayerDamage + applies it via the event_feed ->
// player_damage::OnWireDamage path with no driving needed; just stay connected.
void IdlePlayerDamageOnClient() {
    UE_LOGI("playerdmg_test[client]: connected -- idling; will receive + apply host PlayerDamage automatically");
}

}  // namespace

void RunAutonomousPlayerDamageTest() {
    const std::string roleEnv = cfg::ReadEnv("VOTVCOOP_NET_ROLE");
    const bool isHost = (roleEnv != "client");
    if (isHost) {
        DrivePlayerDamageOnHost();
    } else {
        IdlePlayerDamageOnClient();
    }
}

DWORD WINAPI PlayerDamageTestThread(LPVOID) {
    RunAutonomousPlayerDamageTest();
    return 0;
}

}  // namespace harness::autotest
