// The puppet-damage hazard probe (VOTVCOOP_RUN_DMGHAZARD_TEST): fire both of mainPlayer_C's damage
// entries at the host's own slot-1 puppet and diff the HOST's saveSlot.health. Interfaces and
// per-routine docs in harness/autotest.h.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/actors/vitals.h"

#include <atomic>
#include <memory>
#include <string>

namespace harness::autotest {
namespace {

namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace cfg = coop::config;

// Bounded spin-wait on a game-thread task's completion flag: true if the task signalled, false if
// it never completed within timeoutMs -- which means the posted task faulted and the SEH firewall
// ate the access violation before the flag was set. The bound is mandatory: driving damage on the
// local player could fault, and an unbounded wait would hang the whole test.
bool WaitDone(const std::shared_ptr<std::atomic<int>>& d, int timeoutMs) {
    for (int i = 0; i < timeoutMs / 5 && d->load() == 0; ++i) ::Sleep(5);
    return d->load() != 0;
}

// ===================== puppet-damage hazard PROBE =====================
// mainPlayer_C carries no per-actor health: health lives on UsaveSlot_C, reached through
// GameInstance->save_gameInst, one store per machine (ue_wrap/actors/vitals.h resolves the field by
// name). So a damage entry invoked on an UNPOSSESSED host-side puppet -- a second mainPlayer_C with
// GetController()==null -- can drain the HOST'S OWN health, and only a runtime measurement settles
// it, because the `addDamage` skipSetting guard and the subtraction itself live in BP bytecode.
//   HOST: read own saveSlot.health, invoke a damage entry on the slot-1 puppet, re-read. A DROP
//     means the hazard is real and native damage on a puppet has to be INTERCEPTED rather than
//     merely relayed -- coop/player/player_damage.h installs exactly that for the three impact
//     entries. No drop, and a LOCAL control (the same call on the host's own player) separates a BP
//     early-out from a call that never landed. Host health is restored after; the hit is 5 of ~100.
//   CLIENT: connects, so the slot-1 puppet exists for the host. Host-only verdict.

// Invoke AmainPlayer_C::"Add Player Damage"(Damage) on `target`; true iff the UFunction resolved
// and the call dispatched. A raw ParamFrame, so the probe can aim at a puppet. Game-thread only.
bool InvokeAddPlayerDamage(void* target, float damage) {
    if (!target || !R::IsLive(target)) return false;
    void* cls = R::FindClass(L"mainPlayer_C");
    void* fn = cls ? R::FindFunction(cls, L"Add Player Damage") : nullptr;
    if (!fn) { UE_LOGW("dmghazard: 'Add Player Damage' UFunction did not resolve"); return false; }
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<float>(L"Damage", damage);  // damageLocation/fullBody/blood/Source stay zero-init
    return ue_wrap::Call(target, f);
}

// Invoke AmainPlayer_C::addDamage(Actor, Damage, Hit, impact, skipSetting=false) on `target` -- the
// hit-actor-keyed entry the native enemy/physics-impact path forwards to (impactDamageCPP, the
// npc_zombie attack-sphere overlap). skipSetting=false means "do write the health value". Hit
// (FHitResult) and impact (FVector) stay zero-init, since the frame is zeroed; a well-formed BP
// null-checks them, and the SEH firewall plus the bounded WaitDone contain any fault. Game-thread
// only.
bool InvokeAddDamage(void* target, void* sourceActor, float damage) {
    if (!target || !R::IsLive(target)) return false;
    void* cls = R::FindClass(L"mainPlayer_C");
    void* fn = cls ? R::FindFunction(cls, L"addDamage") : nullptr;
    if (!fn) { UE_LOGW("dmghazard: 'addDamage' UFunction did not resolve"); return false; }
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<void*>(L"Actor", sourceActor);  // damage source/instigator
    f.Set<float>(L"Damage", damage);
    f.Set<bool>(L"skipSetting", false);   // false => DO write the health value
    return ue_wrap::Call(target, f);
}

// GT-posted wrappers: invoke, log the dispatch, bounded-wait for completion. `who` is a string
// literal, so capturing it by pointer is safe.
void InvokeAddPlayerDamageGT(void* target, float damage, const char* who) {
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([target, damage, who, done] {
        const bool ok = InvokeAddPlayerDamage(target, damage);
        UE_LOGI("dmghazard[host]: Add Player Damage(%.0f) on %s dispatched=%d", damage, who, ok ? 1 : 0);
        done->store(1);
    });
    WaitDone(done, 8000);
}

void InvokeAddDamageGT(void* target, void* sourceActor, float damage, const char* who) {
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([target, sourceActor, damage, who, done] {
        const bool ok = InvokeAddDamage(target, sourceActor, damage);
        UE_LOGI("dmghazard[host]: addDamage(%.0f, skipSetting=false) on %s dispatched=%d", damage, who, ok ? 1 : 0);
        done->store(1);
    });
    WaitDone(done, 8000);
}

// Read the host's own saveSlot.health on the game thread (-1 if unresolved).
float ReadHostHealthGT() {
    auto done = std::make_shared<std::atomic<int>>(0);
    auto h = std::make_shared<float>(-1.f);
    GT::Post([done, h] { float v = -1.f; if (ue_wrap::vitals::Read(ue_wrap::vitals::Field::Health, &v)) *h = v; done->store(1); });
    WaitDone(done, 8000);
    return *h;
}

// Restore host saveSlot.health (undo the probe's deliberate damage).
void RestoreHostHealth(float v) {
    if (v < 0.f) return;
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([v, done] { ue_wrap::vitals::Write(ue_wrap::vitals::Field::Health, v);
        UE_LOGI("dmghazard[host]: restored host saveSlot.health=%.2f", v); done->store(1); });
    WaitDone(done, 8000);
}

void ProbeDamageHazardOnHost() {
    UE_LOGI("dmghazard[host]: probe armed -- fire BOTH damage entries (Add Player Damage + "
            "addDamage) on the slot-1 puppet, diff host saveSlot.health");

    // Wait for BOTH the slot-1 puppet (client connected) AND the host saveSlot.
    auto puppet = std::make_shared<void*>(nullptr);
    auto before = std::make_shared<float>(-1.f);
    // 180 s: the window has to outlast a cold client boot and connect that only starts after the
    // host's own boot armed this probe.
    for (int attempt = 0; attempt < 180 && (!*puppet || *before < 0.f); ++attempt) {
        auto done = std::make_shared<std::atomic<int>>(0);
        GT::Post([puppet, before, done] {
            void* p = coop::puppet_drive::Puppet(1).GetActor();
            if (p && R::IsLive(p)) *puppet = p;
            float v = -1.f; if (ue_wrap::vitals::Read(ue_wrap::vitals::Field::Health, &v)) *before = v;
            done->store(1);
        });
        WaitDone(done, 8000);
        if (!*puppet || *before < 0.f) ::Sleep(1000);
    }
    if (!*puppet) { UE_LOGW("dmghazard[host]: VERDICT INCONCLUSIVE -- slot-1 puppet never resolved (no client connected?)"); UE_LOGI("dmghazard[host]: DONE"); return; }
    if (*before < 0.f) { UE_LOGW("dmghazard[host]: VERDICT INCONCLUSIVE -- host saveSlot.health never resolved"); UE_LOGI("dmghazard[host]: DONE"); return; }
    UE_LOGI("dmghazard[host]: slot-1 puppet resolved; host saveSlot.health BEFORE=%.2f", *before);

    // --- Test 1: high-level entry "Add Player Damage" on the PUPPET ---
    const float b1 = ReadHostHealthGT();
    InvokeAddPlayerDamageGT(*puppet, 5.f, "PUPPET");
    ::Sleep(300);
    const float a1 = ReadHostHealthGT();
    const float d1 = (b1 >= 0.f && a1 >= 0.f) ? (b1 - a1) : 0.f;
    UE_LOGI("dmghazard[host]: 'Add Player Damage' on puppet: host health %.2f -> %.2f (delta=%.2f)", b1, a1, d1);

    // --- Test 2: hit-actor entry "addDamage" on the PUPPET (the native-enemy-shaped call) ---
    const float b2 = ReadHostHealthGT();
    InvokeAddDamageGT(*puppet, *puppet, 5.f, "PUPPET");
    ::Sleep(300);
    const float a2 = ReadHostHealthGT();
    const float d2 = (b2 >= 0.f && a2 >= 0.f) ? (b2 - a2) : 0.f;
    UE_LOGI("dmghazard[host]: 'addDamage' on puppet: host health %.2f -> %.2f (delta=%.2f)", b2, a2, d2);

    if (d1 > 0.01f || d2 > 0.01f) {
        RestoreHostHealth(*before);
        UE_LOGW("dmghazard[host]: VERDICT = SHARED-SAVESLOT CORRUPTION CONFIRMED -- invoking %s on a "
                "host-side UNPOSSESSED puppet drained the HOST'S OWN saveSlot.health (Add Player Damage "
                "delta=%.2f, addDamage delta=%.2f). Health is the per-machine shared saveSlot, so "
                "native damage on a puppet has to be intercepted and routed to the owner as a "
                "reliable PlayerDamage event, never run against the host's own store "
                "(host health restored to %.2f).",
                (d1 > 0.01f ? "Add Player Damage" : "addDamage"), d1, d2, *before);
        UE_LOGI("dmghazard[host]: DONE");
        return;
    }

    // Neither puppet entry dropped host health -> control: the SAME Add Player Damage on the host's
    // OWN possessed player. That separates "both entries early-out on the unpossessed puppet" (the
    // writes work, guarded by possession, so they are safe) from "the calls never landed" (a param
    // or resolution bug, which makes the verdict inconclusive).
    UE_LOGI("dmghazard[host]: neither puppet entry changed host health -- running a LOCAL control "
            "(Add Player Damage on the host's OWN player) to tell early-out from a non-landing call");
    auto local = std::make_shared<void*>(nullptr);
    const float bc = [&] {
        auto done = std::make_shared<std::atomic<int>>(0);
        auto h = std::make_shared<float>(-1.f);
        GT::Post([local, h, done] {
            void* mp = coop::players::Registry::Get().Local();
            if (mp && R::IsLive(mp)) *local = mp;
            float v = -1.f; if (ue_wrap::vitals::Read(ue_wrap::vitals::Field::Health, &v)) *h = v;
            done->store(1);
        });
        WaitDone(done, 8000);
        return *h;
    }();
    if (!*local || bc < 0.f) { UE_LOGW("dmghazard[host]: VERDICT INCONCLUSIVE -- both puppet calls were no-ops AND no local player/health for the control"); UE_LOGI("dmghazard[host]: DONE"); return; }
    InvokeAddPlayerDamageGT(*local, 5.f, "LOCAL player");
    ::Sleep(300);
    const float ac = ReadHostHealthGT();
    const float dc = (bc >= 0.f && ac >= 0.f) ? (bc - ac) : 0.f;
    UE_LOGI("dmghazard[host]: LOCAL control: host saveSlot.health %.2f -> %.2f (delta=%.2f)", bc, ac, dc);
    if (dc > 0.01f) {
        RestoreHostHealth(*before);
        UE_LOGW("dmghazard[host]: VERDICT = PUPPET-ENTRIES-EARLY-OUT -- BOTH 'Add Player Damage' AND "
                "'addDamage' are no-ops on the unpossessed puppet (zero host-health change), while the SAME "
                "'Add Player Damage' on the host's OWN possessed player dropped %.2f -> the damage path "
                "writes the shared saveSlot but GUARDS on possession (GetController()). So directly invoking "
                "either entry on a puppet is SAFE. RESIDUAL: the native enemy attack reaches the pawn via "
                "impactDamageCPP/overlap (a native hit-actor forward) -- if that bypasses the possession "
                "guard it could still corrupt, which is why those entries are intercepted on puppets. "
                "Host health restored to %.2f.", dc, *before);
    } else {
        UE_LOGW("dmghazard[host]: VERDICT INCONCLUSIVE -- no entry (puppet OR local control) changed host "
                "health; the damage calls aren't landing (param/resolution). Revise the probe / verify the "
                "UFunction names.");
    }
    UE_LOGI("dmghazard[host]: DONE");
}

// CLIENT side: nothing to drive -- just connect so the host's slot-1 puppet exists.
void IdleDmgHazardOnClient() {
    UE_LOGI("dmghazard[client]: connected -- idling so the host's slot-1 puppet exists for the #6 probe");
}

}  // namespace

void RunAutonomousDmgHazardTest() {
    const bool isHost = !IsClientRole();
    if (isHost) {
        ProbeDamageHazardOnHost();
    } else {
        IdleDmgHazardOnClient();
    }
}

DWORD WINAPI DmgHazardTestThread(LPVOID) {
    RunAutonomousDmgHazardTest();
    return 0;
}

}  // namespace harness::autotest
