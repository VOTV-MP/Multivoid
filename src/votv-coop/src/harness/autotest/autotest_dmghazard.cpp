// The puppet-damage hazard probe (VOTVCOOP_RUN_DMGHAZARD_TEST): fire mainPlayer_C's two damage
// entries and ignite at the host's own slot-1 puppet and judge by what coop::player_damage refused
// and what the HOST's saveSlot.health did; the same damage on the host's own player is the control
// that the calls land. Each phase ends on readiness, never a clock. Red:
// dev.player_damage_no_refusal. Interfaces and per-routine docs in harness/autotest.h.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/player/player_damage.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
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
// mainPlayer_C carries no per-actor health: it lives on UsaveSlot_C, reached through
// GameInstance->save_gameInst, one store per machine (vitals.h resolves it by name). So a damage
// entry invoked on an UNPOSSESSED host-side puppet -- a second mainPlayer_C, GetController()==null
// -- can drain the HOST'S OWN health, and only a runtime measurement settles it: the `addDamage`
// skipSetting guard and the subtraction itself live in BP bytecode.
//   HOST: read own saveSlot.health, invoke a damage entry on the slot-1 puppet, re-read. A DROP
//     means the hazard is real and native damage on a puppet has to be INTERCEPTED, not merely
//     relayed -- which coop/player/player_damage.h does for the three impact entries. No drop,
//     and a LOCAL control on the host's own player separates an early-out from a call that never
//     landed. Health is restored after; the hit is 5 of ~100.
//   CLIENT: connects, so the puppet exists. Host-only verdict.
// A null result also fits a wisp grab: wisp_attack_sync PRE-cancels Add Player Damage during one.

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

// Invoke AmainPlayer_C::ignite(fuel) on `target`. Game-thread only.
bool InvokeIgnite(void* target, float fuel) {
    if (!target || !R::IsLive(target)) return false;
    void* cls = R::FindClass(L"mainPlayer_C");
    void* fn = cls ? R::FindFunction(cls, L"ignite") : nullptr;
    if (!fn) { UE_LOGW("dmghazard: 'ignite' UFunction did not resolve"); return false; }
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<float>(L"fuel", fuel);
    return ue_wrap::Call(target, f);
}

// The puppet's own isBurning, by its reflected bool storage. -1 unread. Game-thread only.
int ReadBurningGT(void* actor) {
    auto done = std::make_shared<std::atomic<int>>(0);
    auto out = std::make_shared<int>(-1);
    GT::Post([actor, out, done] {
        int32_t off = -1;
        uint8_t mask = 0;
        if (actor && R::IsLive(actor) && R::FindBoolProperty(R::ClassOf(actor), L"isBurning", off, mask))
            *out = (static_cast<const uint8_t*>(actor)[off] & mask) ? 1 : 0;
        done->store(1);
    });
    WaitDone(done, 8000);
    return *out;
}

// What one call at a body came to: the refusal counted, the host's health dropped, or neither within
// the phase -- read every 50 ms until one shows, since a refusal or a health write lands within the
// call's own game-thread task or the next frame.
enum class Outcome { Refused, Landed, Nothing };
const char* Name(Outcome o) { return o == Outcome::Refused ? "REFUSED" : o == Outcome::Landed ? "LANDED" : "NOTHING"; }
constexpr DWORD kPhaseMs = 3000;
constexpr DWORD kPollMs  = 50;

template <class Refused, class Landed>
Outcome Await(Refused refused, Landed landed) {
    for (DWORD waited = 0; waited < kPhaseMs; waited += kPollMs) {
        if (refused()) return Outcome::Refused;
        if (landed()) return Outcome::Landed;
        ::Sleep(kPollMs);
    }
    return refused() ? Outcome::Refused : landed() ? Outcome::Landed : Outcome::Nothing;
}

void ProbeDamageHazardOnHost() {
    namespace PD = coop::player_damage;
    UE_LOGI("dmghazard[host]: probe armed -- the damage verb, addDamage and ignite at the slot-1 puppet, "
            "judged by the refusals and the host's own saveSlot.health");
    constexpr DWORD kReadyMs = 180'000;   // the client boots, joins, downloads and loads first
    if (!WaitPeerWorldReady(1, kReadyMs)) {
        UE_LOGW("dmghazard[host]: VERDICT INCONCLUSIVE -- no client was seated and world-ready in slot 1");
        UE_LOGI("dmghazard[host]: DONE");
        return;
    }
    // The puppet is the host's own milestone after the slot's world-ready: it spawns once the peer's
    // pose arrives. Read every 50 ms until it and this machine's player resolve, within a phase.
    auto puppet = std::make_shared<void*>(nullptr);
    auto local = std::make_shared<void*>(nullptr);
    constexpr DWORD kPuppetMs = 30'000;
    for (DWORD waited = 0; (!*puppet || !*local) && waited < kPuppetMs; waited += kPollMs) {
        auto done = std::make_shared<std::atomic<int>>(0);
        GT::Post([puppet, local, done] {
            void* p = coop::puppet_drive::Puppet(1).GetActor();
            if (p && R::IsLive(p)) *puppet = p;
            void* mp = coop::players::Registry::Get().Local();
            if (mp && R::IsLive(mp)) *local = mp;
            done->store(1);
        });
        WaitDone(done, 8000);
        if (!*puppet || !*local) ::Sleep(kPollMs);
    }
    const float before = ReadHostHealthGT();
    if (!*puppet || !*local || before < 0.f) {
        UE_LOGW("dmghazard[host]: VERDICT INCONCLUSIVE -- puppet=%p local=%p health=%.2f unread after world-ready",
                *puppet, *local, before);
        UE_LOGI("dmghazard[host]: DONE");
        return;
    }
    const auto healthBelow = [](float h) { return [h] { const float n = ReadHostHealthGT(); return n >= 0.f && n < h - 0.01f; }; };

    // 1. The damage verb at the puppet: refused, never written to this machine's health.
    const uint32_t d1 = PD::RefusedDamage();
    const float h1 = ReadHostHealthGT();
    InvokeAddPlayerDamageGT(*puppet, 5.f, "PUPPET");
    const Outcome o1 = Await([d1] { return PD::RefusedDamage() > d1; }, healthBelow(h1));
    UE_LOGI("dmghazard[host]: 'Add Player Damage' at the puppet -> %s (health %.2f -> %.2f)", Name(o1), h1,
            ReadHostHealthGT());

    // 2. addDamage at the puppet, the entry a native hit forwards to: its nested verb is refused.
    const uint32_t d2 = PD::RefusedDamage();
    const float h2 = ReadHostHealthGT();
    InvokeAddDamageGT(*puppet, *puppet, 5.f, "PUPPET");
    const Outcome o2 = Await([d2] { return PD::RefusedDamage() > d2; }, healthBelow(h2));
    UE_LOGI("dmghazard[host]: 'addDamage' at the puppet -> %s (health %.2f -> %.2f)", Name(o2), h2,
            ReadHostHealthGT());

    // 3. ignite at the puppet: refused, the puppet never burning here.
    const uint32_t i3 = PD::RefusedIgnites();
    {
        auto d = std::make_shared<std::atomic<int>>(0);
        void* target = *puppet;
        GT::Post([target, d] { InvokeIgnite(target, 10.f); d->store(1); });
        WaitDone(d, 8000);
    }
    void* pup = *puppet;
    const Outcome o3 = Await([i3] { return PD::RefusedIgnites() > i3; }, [pup] { return ReadBurningGT(pup) == 1; });
    UE_LOGI("dmghazard[host]: 'ignite' at the puppet -> %s (puppet isBurning=%d)", Name(o3), ReadBurningGT(pup));

    // 4. The control: the same damage verb at the host's own player lands, so a REFUSED above is the
    //    refusal and not a call that never reached the verb.
    const float h4 = ReadHostHealthGT();
    InvokeAddPlayerDamageGT(*local, 5.f, "LOCAL player");
    const Outcome o4 = Await([] { return false; }, healthBelow(h4));
    UE_LOGI("dmghazard[host]: control 'Add Player Damage' at this machine's player -> %s (health %.2f -> %.2f)",
            Name(o4), h4, ReadHostHealthGT());
    RestoreHostHealth(before);

    const bool refusedAll = o1 == Outcome::Refused && o2 != Outcome::Landed && o3 == Outcome::Refused;
    const bool controlLanded = o4 == Outcome::Landed;
    if (o1 == Outcome::Landed || o2 == Outcome::Landed || o3 == Outcome::Landed) {
        UE_LOGW("dmghazard[host]: VERDICT FAIL -- a peer's puppet took %s%s%s on this machine (health restored to "
                "%.2f)", o1 == Outcome::Landed ? "the damage verb " : "", o2 == Outcome::Landed ? "addDamage " : "",
                o3 == Outcome::Landed ? "fire" : "", before);
    } else if (refusedAll && controlLanded) {
        UE_LOGI("dmghazard[host]: VERDICT PASS -- the damage verb and ignite at a peer's puppet were refused, "
                "addDamage wrote nothing, and the same verb at this machine's player landed (health restored to "
                "%.2f)", before);
    } else {
        UE_LOGW("dmghazard[host]: VERDICT INCONCLUSIVE -- verb=%s addDamage=%s ignite=%s control=%s: a call that "
                "neither was refused nor landed did not reach its verb", Name(o1), Name(o2), Name(o3), Name(o4));
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
