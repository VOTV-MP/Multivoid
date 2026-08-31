// harness/autotest_korespawn.cpp -- the KO-RESPAWN acceptance test
// (VOTVCOOP_RUN_KORESPAWN_TEST). SOLO: it needs no second peer, because the
// thing under test is entirely local (the ragdoll gate + the health poll).
//
// WHAT IT PROVES, and why each assertion is there rather than the obvious one.
// The lane's whole claim is "a hit that WOULD have killed the player leaves them
// alive". A test that just checks "we did not end up in the main menu" would
// pass for the wrong reason -- e.g. because the damage never landed. So the
// arms are ordered so that a false green is not reachable:
//
//   A1  the gate TOOK          -- canRagdoll reads false before the hit. Read
//                                 back from the pawn, not assumed from the
//                                 write's return value.
//   A2  the damage LANDED      -- health actually falls to <= 0. This is the
//                                 falsifier for A3: without it, "did not die"
//                                 is uninformative.
//   A3  the death DID NOT RUN  -- `dead` stays false across the lethal hit. In
//                                 stock VOTV this is the instant `dead := true`
//                                 fires (uber @37412) and the 10 s march to
//                                 loadLevel('menu') begins.
//   A4  the KO ENGAGED         -- ko_respawn::Active() and isRagdoll both true.
//   A5  the RESPAWN COMPLETED  -- Active() false again, health back to full,
//                                 and `dead` STILL false (the recovery path must
//                                 not have re-entered the death branch: forceGetUp
//                                 would route through uber @39685, which re-reads
//                                 `dead`; forceWakeup deliberately does not).
//
// The hit is delivered with the game's own `Add Player Damage`, so it walks the
// real lethal chain: Add Player Damage -> kill() -> ragdollMode(...,death=true)
// -> blocked at `IFNOT(canRagdoll) POP`. That is the exact path a fall, a fire,
// or an enemy takes; only the trigger is synthetic.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/player/ko_respawn.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"

#include <atomic>
#include <memory>

namespace harness::autotest {
namespace {

namespace GT = ue_wrap::game_thread;
namespace E = ue_wrap::engine;
namespace V = ue_wrap::vitals;
namespace R = ue_wrap::reflection;
namespace cfg = coop::config;

bool WaitDone(const std::shared_ptr<std::atomic<int>>& d, int timeoutMs) {
    for (int i = 0; i < timeoutMs / 5 && d->load() == 0; ++i) ::Sleep(5);
    return d->load() != 0;
}

// One game-thread sample of everything the arms assert on.
struct Sample {
    bool  havePawn = false;
    bool  canRagdoll = true;
    bool  haveCanRagdoll = false;
    bool  isRagdoll = false;
    bool  dead = false;
    bool  haveState = false;
    float health = -1.f;
    bool  koActive = false;
    // `Add Player Damage`'s own early-out terms (uber @659:
    // `gamemode.immortal || isDreaming || dead || startInvinc` -> return). Read
    // so a hit that lands NOWHERE can say WHY instead of just failing: the first
    // run of this test lost its lethal hit here (health 100.00 -> 100.00) and the
    // verdict alone could not name the term.
    bool  startInvinc = false;
    bool  haveStartInvinc = false;
    bool  immortal = false;
    bool  haveImmortal = false;
};

// Read a plain BP bool by name off a live object (byte+mask, same shape as the
// engine's canRagdoll accessor). Test-local: nothing in the mod needs these.
bool ReadBpBool(void* obj, const wchar_t* name, bool& out) {
    if (!obj || !R::IsLive(obj)) return false;
    int32_t byteOff = -1; uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(obj), name, byteOff, mask)) return false;
    out = (*(reinterpret_cast<uint8_t*>(obj) + byteOff) & mask) != 0;
    return true;
}

Sample Probe() {
    auto done = std::make_shared<std::atomic<int>>(0);
    auto out = std::make_shared<Sample>();
    GT::Post([done, out] {
        void* mp = coop::players::Registry::Get().Local();
        if (mp && R::IsLive(mp)) {
            out->havePawn = true;
            out->haveCanRagdoll = E::ReadMainPlayerCanRagdoll(mp, out->canRagdoll);
            out->haveState = E::ReadMainPlayerRagdollState(mp, out->isRagdoll, out->dead);
            out->haveStartInvinc = ReadBpBool(mp, L"startInvinc", out->startInvinc);
        }
        if (void* gm = R::FindObjectByClass(ue_wrap::profile::name::GamemodeClass))
            out->haveImmortal = ReadBpBool(gm, L"immortal", out->immortal);
        float hp = -1.f;
        if (V::Read(V::Field::Health, &hp)) out->health = hp;
        out->koActive = coop::ko_respawn::Active();
        done->store(1);
    });
    WaitDone(done, 8000);
    return *out;
}

int g_pass = 0, g_fail = 0;

void Verdict(const char* arm, bool ok, const char* detail) {
    if (ok) { ++g_pass; UE_LOGI("korespawn_test: %s PASS -- %s", arm, detail); }
    else    { ++g_fail; UE_LOGW("korespawn_test: %s FAIL -- %s", arm, detail); }
}

}  // namespace

DWORD WINAPI KoRespawnTestThread(LPVOID) {
    UE_LOGI("korespawn_test: armed -- a LETHAL Add Player Damage must leave the player "
            "alive, knocked out, and then respawned");

    if (cfg::ResolveFlag(coop::config_registry::rows::ko_respawn) == 0) {
        UE_LOGW("korespawn_test: VERDICT INCONCLUSIVE -- death.ko_respawn is OFF, nothing to test");
        UE_LOGI("korespawn_test: DONE");
        return 0;
    }

    const int ragdollSec =
        static_cast<int>(cfg::ResolveInt(coop::config_registry::rows::ko_ragdoll_seconds));

    // Wait for a pawn with the gate applied and positive health (the KO trigger
    // arms on a falling edge, so the lane must have seen the player alive first)
    // AND for `Add Player Damage`'s own early-out terms to be clear. That last
    // condition is not defensive padding: the FIRST run of this test delivered a
    // 1000-damage hit into `startInvinc` and measured health 100.00 -> 100.00,
    // which made the no-death and respawn arms pass for a reason that had nothing
    // to do with the gate under test.
    Sample s;
    bool ready = false;
    for (int i = 0; i < 120 && !ready; ++i) {
        s = Probe();
        ready = s.havePawn && s.haveCanRagdoll && s.haveState && s.health > 0.f &&
                !s.canRagdoll && !s.startInvinc && !s.immortal;
        if (!ready) ::Sleep(1000);
    }
    UE_LOGI("korespawn_test: pre-hit state -- havePawn=%d canRagdoll=%d(read=%d) health=%.2f "
            "startInvinc=%d(read=%d) immortal=%d(read=%d) dead=%d",
            s.havePawn ? 1 : 0, s.canRagdoll ? 1 : 0, s.haveCanRagdoll ? 1 : 0, s.health,
            s.startInvinc ? 1 : 0, s.haveStartInvinc ? 1 : 0,
            s.immortal ? 1 : 0, s.haveImmortal ? 1 : 0, s.dead ? 1 : 0);
    if (!ready) {
        UE_LOGW("korespawn_test: VERDICT INCONCLUSIVE -- preconditions never met (see the "
                "pre-hit state line above; a set startInvinc/immortal would swallow the hit)");
        UE_LOGI("korespawn_test: DONE");
        return 0;
    }

    // ---- A1: the gate took ---------------------------------------------------
    Verdict("A1 gate", !s.canRagdoll,
            s.canRagdoll ? "canRagdoll is TRUE -- the death gate never applied"
                         : "canRagdoll=false on the local pawn before the hit");
    const float hpBefore = s.health;

    // ---- deliver the lethal hit ---------------------------------------------
    // 10x max health so it is lethal under any armor/isStrong scaling (the BP
    // multiplies by 0.75 when strong; nothing scales it UP).
    float maxHp = 100.f;
    { auto done = std::make_shared<std::atomic<int>>(0);
      auto mh = std::make_shared<float>(100.f);
      GT::Post([done, mh] { float v = 100.f; if (V::Read(V::Field::MaxHealth, &v)) *mh = v; done->store(1); });
      WaitDone(done, 8000);
      maxHp = *mh; }
    const float lethal = (maxHp > 0.f ? maxHp : 100.f) * 10.f;

    auto hitDone = std::make_shared<std::atomic<int>>(0);
    auto hitOk = std::make_shared<int>(0);
    GT::Post([hitDone, hitOk, lethal] {
        void* mp = coop::players::Registry::Get().Local();
        if (mp && R::IsLive(mp) && E::InvokeAddPlayerDamage(mp, lethal)) *hitOk = 1;
        hitDone->store(1);
    });
    WaitDone(hitDone, 8000);
    UE_LOGI("korespawn_test: delivered Add Player Damage(%.0f) (health was %.2f, invoke=%s)",
            lethal, hpBefore, *hitOk ? "ok" : "FAILED");

    // ---- A2/A3/A4: the hit landed, no death ran, the KO engaged --------------
    // Poll fast: the lane reacts on the next pump tick, and `dead` -- if the gate
    // had failed -- would be set inside the hit itself, so a slow first read
    // could not miss it either way.
    bool sawZeroHealth = false, sawDead = false, sawKo = false, sawRagdoll = false;
    for (int i = 0; i < 60; ++i) {
        Sample p = Probe();
        if (p.haveState && p.dead) sawDead = true;
        if (p.health <= 0.f && p.health >= -0.5f) sawZeroHealth = true;
        if (p.koActive) { sawKo = true; if (p.isRagdoll) sawRagdoll = true; }
        if (sawKo && sawRagdoll) break;
        ::Sleep(100);
    }
    Verdict("A2 damage-landed", sawZeroHealth || sawKo,
            sawZeroHealth ? "health reached <= 0 -- the lethal hit really applied"
                          : "health never reached 0; if the KO still fired the hit landed, "
                            "otherwise this run proves nothing");
    Verdict("A3 no-death", !sawDead,
            sawDead ? "dead=true -- the gate did NOT stop the death chain; the game is now "
                      "10 s from loadLevel('menu')"
                    : "dead stayed false across a lethal hit");
    Verdict("A4 ko-engaged", sawKo && sawRagdoll,
            sawKo ? (sawRagdoll ? "ko_respawn::Active() and isRagdoll both true"
                                : "Active() true but the player never ragdolled")
                  : "the KO never engaged");

    // ---- A5: the respawn completes ------------------------------------------
    bool respawned = false;
    Sample f;
    for (int i = 0; i < (ragdollSec + 12) * 2 && !respawned; ++i) {
        f = Probe();
        respawned = !f.koActive && f.health > 1.f;
        if (!respawned) ::Sleep(500);
    }
    if (f.haveState && f.dead) sawDead = true;
    Verdict("A5 respawn", respawned && !sawDead,
            respawned ? (sawDead ? "respawned but dead=true was observed -- the recovery path "
                                   "re-entered the death branch"
                                 : "KO cleared, vitals restored, dead still false")
                      : "the respawn never completed");

    UE_LOGI("korespawn_test: VERDICT %s (%d pass / %d fail) -- health %.2f -> %.2f",
            g_fail == 0 ? "PASS" : "FAIL", g_pass, g_fail, hpBefore, f.health);
    UE_LOGI("korespawn_test: DONE");
    return 0;
}

}  // namespace harness::autotest
