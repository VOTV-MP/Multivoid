// harness/autotest_vitals.cpp -- autonomous vitals-pillar probes.
//
// Inc2a verification probe. Answers vitals-design MUST-VERIFY #8: does
// mainPlayer_C::ragdollMode() drive an UNPOSSESSED puppet (GetController()==null),
// or does the BP early-out on possession? A BP UFunction's bytecode is not
// statically readable (IDA sees only the ProcessInternal thunk), so this fires
// ragdollMode on the host's slot-1 puppet and DIFFS isRagdoll + ragdollActor
// before/after -- the methodology's "verify behaviour by diffing observable
// state" rule. It also exercises the RECOVER path (forceGetUp) that Inc2b's
// falling edge needs.
//
// This is diagnostic/autotest code -> the documented exception to the
// no-raw-read rule. It reads fields by reflection-resolved offset
// (FindPropertyOffset, name-based + recook-safe) purely to LOG state; the
// production Inc2b sender/receiver reads go through a ue_wrap wrapper.
//
// Host-only (the slot-1 puppet is the unpossessed orphan). Gated by env
// VOTVCOOP_RUN_RAGDOLL_TEST=1 (registered in autotest_dispatch.cpp). The smoke
// inherits the env via mp.py's os.environ.copy(), so the host process picks it
// up; the client self-skips on the role check.

#include "harness/autotest.h"

#include "coop/net_pump.h"
#include "coop/remote_player.h"
#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace harness::autotest {
namespace {

namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace E = ue_wrap::engine;

std::string ReadEnv(const char* name) {
    char buf[256] = {};
    const DWORD n = ::GetEnvironmentVariableA(name, buf, sizeof(buf));
    return (n > 0 && n < sizeof(buf)) ? std::string(buf) : std::string();
}

// Bounded spin-wait on a game-thread task's completion flag. Returns true if the
// task signalled (1=ok / 2=fail), false if it never completed within timeoutMs
// -- which would mean the posted task faulted (the SEH firewall ate the AV and
// the done flag was never set). A bound is mandatory here: ragdollMode on an
// unpossessed puppet is the unknown we're testing and COULD fault, and an
// unbounded wait would hang the whole smoke.
bool WaitDone(const std::shared_ptr<std::atomic<int>>& d, int timeoutMs) {
    for (int i = 0; i < timeoutMs / 5 && d->load() == 0; ++i) ::Sleep(5);
    return d->load() != 0;
}

}  // namespace

void RunAutonomousRagdollTest() {
    const std::string roleEnv = ReadEnv("VOTVCOOP_NET_ROLE");
    const bool isHost = (roleEnv != "client");  // default Host if unset
    if (!isHost) {
        UE_LOGI("ragdoll_test: client role -- skipping (host-only probe)");
        return;
    }
    UE_LOGI("ragdoll_test: host probe armed -- polling up to 90 s for the slot-1 puppet");

    struct Resolved {
        void* puppet = nullptr;
        int32_t idx = -1;
        void* cls = nullptr;
        void* ragdollFn = nullptr;
        void* getUpFn = nullptr;
        void* ctrl = nullptr;
        int32_t offIsRagdoll = -1, offDead = -1, offRagdollActor = -1;
        bool ok = false;
    };
    auto rsv = std::make_shared<Resolved>();
    auto done = std::make_shared<std::atomic<int>>(0);  // 0 pending, 1 ok, 2 fail

    // ---- Poll for the puppet. It exists only after the client connects + sends
    // its first pose (tens of seconds into a LAN smoke), so fire as soon as it's
    // there; give up after 90 s.
    for (int attempt = 0; attempt < 90 && !rsv->ok; ++attempt) {
        done->store(0);
        GT::Post([rsv, done] {
            void* puppet = coop::net_pump::Puppet(1).GetActor();
            if (!puppet || !R::IsLive(puppet)) { done->store(2); return; }
            rsv->puppet = puppet;
            rsv->idx = R::InternalIndexOf(puppet);
            rsv->cls = R::ClassOf(puppet);
            if (!rsv->cls) { done->store(2); return; }
            rsv->ragdollFn = R::FindFunction(rsv->cls, L"ragdollMode");
            rsv->getUpFn = R::FindFunction(rsv->cls, L"forceGetUp");
            rsv->offIsRagdoll = R::FindPropertyOffset(rsv->cls, L"isRagdoll");
            rsv->offDead = R::FindPropertyOffset(rsv->cls, L"dead");
            rsv->offRagdollActor = R::FindPropertyOffset(rsv->cls, L"ragdollActor");
            rsv->ctrl = E::GetController(puppet);
            rsv->ok = (rsv->ragdollFn && rsv->offIsRagdoll >= 0 && rsv->offRagdollActor >= 0);
            done->store(rsv->ok ? 1 : 2);
        });
        if (!WaitDone(done, 8000)) {
            UE_LOGW("ragdoll_test: resolve task did not complete (faulted?) -- aborting");
            return;
        }
        if (!rsv->ok) ::Sleep(1000);
    }
    if (!rsv->ok) {
        UE_LOGW("ragdoll_test: gave up -- no live puppet / ragdollMode UFunction in 90 s");
        return;
    }
    UE_LOGI("ragdoll_test: resolved puppet=%p ctrl=%p (null=unpossessed) ragdollFn=%p getUpFn=%p "
            "off isRagdoll=0x%X dead=0x%X ragdollActor=0x%X",
            rsv->puppet, rsv->ctrl, rsv->ragdollFn, rsv->getUpFn,
            rsv->offIsRagdoll, rsv->offDead, rsv->offRagdollActor);

    auto isRag = std::make_shared<int>(0);
    auto ragAct = std::make_shared<void*>(nullptr);
    auto snapshot = [&](const char* label) {
        done->store(0);
        GT::Post([rsv, label, done, isRag, ragAct] {
            if (!R::IsLiveByIndex(rsv->puppet, rsv->idx)) {
                UE_LOGW("ragdoll_test [%s]: puppet not live", label);
                *isRag = -1; *ragAct = nullptr; done->store(1); return;
            }
            auto* b = reinterpret_cast<uint8_t*>(rsv->puppet);
            const bool r = *reinterpret_cast<bool*>(b + rsv->offIsRagdoll);
            const bool d = rsv->offDead >= 0 ? *reinterpret_cast<bool*>(b + rsv->offDead) : false;
            void* ra = *reinterpret_cast<void**>(b + rsv->offRagdollActor);
            *isRag = r ? 1 : 0; *ragAct = ra;
            UE_LOGI("ragdoll_test [%s]: isRagdoll=%d dead=%d ragdollActor=%p", label, r ? 1 : 0, d ? 1 : 0, ra);
            done->store(1);
        });
        WaitDone(done, 8000);
    };

    snapshot("BEFORE");
    const int beforeRag = *isRag;
    void* beforeActor = *ragAct;

    // ---- ragdollMode(true, true, false) = FAINT (ragdoll + passOut, NOT death).
    // If a Set returns 0 the param name was wrong (SetRaw logs the bad name); the
    // verdict accounts for that by reporting the set results.
    done->store(0);
    GT::Post([rsv, done] {
        ue_wrap::ParamFrame f(rsv->ragdollFn);
        const bool s1 = f.Set<bool>(L"ragdoll", true);
        const bool s2 = f.Set<bool>(L"passOut", true);
        const bool s3 = f.Set<bool>(L"death", false);
        const bool ok = ue_wrap::Call(rsv->puppet, f);
        UE_LOGI("ragdoll_test: ragdollMode(1,1,0) set(ragdoll=%d,passOut=%d,death=%d) -> Call=%d",
                s1, s2, s3, ok);
        done->store(1);
    });
    if (!WaitDone(done, 8000)) {
        UE_LOGW("ragdoll_test: ragdollMode call did not complete (faulted on unpossessed puppet?) "
                "-- VERDICT #8 INCONCLUSIVE (call unsafe). Inc2b needs a redesign before any wire.");
        return;
    }

    ::Sleep(600);  // let the BP / ragdoll-actor spawn timeline settle
    snapshot("AFTER-RAGDOLL");
    const int afterRag = *isRag;
    void* afterActor = *ragAct;

    // ---- Recover. The Inc2b falling edge needs a call that un-ragdolls an
    // UNPOSSESSED puppet. forceGetUp() is an animation-driven getup that appears
    // to need the possessed pawn (a first probe pass showed it leaving isRagdoll=1),
    // so test the SYMMETRIC INVERSE ragdollMode(false,false,false) here -- if that
    // clears isRagdoll + despawns ragdollActor, it's the receiver's recover call.
    // forceGetUp is kept as a logged fallback.
    done->store(0);
    GT::Post([rsv, done] {
        ue_wrap::ParamFrame f(rsv->ragdollFn);
        f.Set<bool>(L"ragdoll", false);
        f.Set<bool>(L"passOut", false);
        f.Set<bool>(L"death", false);
        const bool ok = ue_wrap::Call(rsv->puppet, f);
        UE_LOGI("ragdoll_test: ragdollMode(0,0,0) recover -> Call=%d", ok);
        done->store(1);
    });
    WaitDone(done, 8000);
    ::Sleep(600);
    snapshot("AFTER-RECOVER-RAGDOLLMODE");
    const int recovRag = *isRag;
    if (recovRag == 1 && rsv->getUpFn) {
        // ragdollMode(0,0,0) didn't clear it -- try forceGetUp as a fallback.
        done->store(0);
        GT::Post([rsv, done] {
            ue_wrap::ParamFrame f(rsv->getUpFn);
            const bool ok = ue_wrap::Call(rsv->puppet, f);
            UE_LOGI("ragdoll_test: forceGetUp() fallback -> Call=%d", ok);
            done->store(1);
        });
        WaitDone(done, 8000);
        ::Sleep(600);
        snapshot("AFTER-RECOVER-FORCEGETUP");
    }
    // Recover finding for Inc2b's falling edge: ragdollMode(0,0,0) does NOT clear
    // isRagdoll on an unpossessed puppet (recovRag stays 1); forceGetUp() DOES
    // clear the isRagdoll AnimBP gate. The spawned AplayerRagdoll_C actor lingers
    // after either -- its cleanup is a hands-on Inc2b detail.
    UE_LOGI("ragdoll_test: recover -- ragdollMode(0,0,0) left isRagdoll=%d; "
            "forceGetUp() is the call that clears the AnimBP gate (see AFTER-RECOVER-* above)",
            recovRag);

    // ---- Verdict: the #8 answer.
    const bool unpossessed = (rsv->ctrl == nullptr);
    const bool ragdolled = (beforeRag == 0 && afterRag == 1) ||
                           (beforeActor == nullptr && afterActor != nullptr);
    if (unpossessed && ragdolled) {
        UE_LOGI("ragdoll_test: VERDICT #8 PASS -- ragdollMode drives an UNPOSSESSED puppet "
                "(ctrl=null, isRagdoll %d->%d, ragdollActor %p->%p). Inc2b wire is GO.",
                beforeRag, afterRag, beforeActor, afterActor);
    } else if (!unpossessed) {
        UE_LOGW("ragdoll_test: VERDICT #8 INCONCLUSIVE -- puppet is POSSESSED (ctrl=%p); not the "
                "unpossessed-orphan case the wire targets. Re-check puppet spawn.", rsv->ctrl);
    } else {
        UE_LOGW("ragdoll_test: VERDICT #8 FAIL -- ragdollMode did NOT ragdoll the unpossessed "
                "puppet (isRagdoll %d->%d, ragdollActor %p->%p). The BP likely early-outs on "
                "GetController()==null; Inc2b receiver-apply must be redesigned (RULE 1, no bool poke).",
                beforeRag, afterRag, beforeActor, afterActor);
    }
    UE_LOGI("ragdoll_test: DONE");
}

DWORD WINAPI RagdollTestThread(LPVOID) {
    RunAutonomousRagdollTest();
    return 0;
}

}  // namespace harness::autotest
