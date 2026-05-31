// harness/autotest_vitals.cpp -- autonomous vitals-pillar wire test.
//
// Inc2b end-to-end ragdoll/faint sync test. Drives the FULL wire path:
//
//   CLIENT (driver): calls ragdollMode(true,true,false) on its LOCAL possessed
//     mainPlayer_C (exactly what the C-key / exhaustion-faint do internally),
//     waits, then forceGetUp() -- so its OWN isRagdoll flips 1 then 0.
//   HOST (observer): polls its slot-1 puppet (the client's body) and confirms
//     isRagdoll flips 0->1 then 1->0 -- driven PURELY by the client's pose
//     stream carrying PoseSnapshot.stateBits bit 1 (kStateBitRagdoll) +
//     RemotePlayer::SetTargetPose's reconcile applying ragdollMode/forceGetUp
//     on the puppet.
//
// This SUPERSEDES the prior standalone #8 probe (host drove its OWN puppet
// directly to prove ragdollMode works on an unpossessed orphan -- PASSED,
// commit 4d52d40). The e2e test exercises that same receiver-apply path AND
// the sender bit-pack AND the relay-free per-peer pose stream, so the
// standalone probe is retired (RULE 2 -- no parallel old + new verification).
//
// Reads/drives go through the ue_wrap::engine ragdoll wrappers (the production
// Inc2b code path), not raw reflection -- so the test validates the shipping
// accessors too. Game-thread work is posted via GT::Post; a bounded WaitDone
// guards every wait so a faulting call can't hang the smoke.
//
// Gated by env VOTVCOOP_RUN_RAGDOLL_TEST=1 (registered in autotest_dispatch.cpp;
// the smoke inherits it via mp.py's os.environ.copy()). Role read from
// VOTVCOOP_NET_ROLE: "client" => driver, anything else => host observer.

#include "harness/autotest.h"

#include "coop/net_pump.h"
#include "coop/players_registry.h"
#include "coop/remote_player.h"
#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/puppet.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/vitals.h"

#include <atomic>
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

// Read whether the slot-1 puppet's mesh has any awake rigid body == it is
// physically ragdolling (flopping). IsAnyRigidBodyAwake (UPrimitiveComponent) is
// the reliable signal -- IsSimulatingPhysics(emptyBone) false-negatives on a
// skeletal ragdoll. Returns false on a missing/unreadable puppet.
bool PuppetMeshIsFlopping() {
    auto done = std::make_shared<std::atomic<int>>(0);
    auto awake = std::make_shared<int>(0);
    GT::Post([done, awake] {
        void* puppet = coop::net_pump::Puppet(1).GetActor();
        if (puppet && R::IsLive(puppet)) {
            void* mesh = ue_wrap::puppet::GetSkeletalMeshComponent(puppet);
            if (mesh && R::IsLive(mesh)) {
                if (void* awakeFn = R::FindFunction(R::FindClass(L"PrimitiveComponent"), L"IsAnyRigidBodyAwake")) {
                    ue_wrap::ParamFrame f(awakeFn);
                    if (ue_wrap::Call(mesh, f) && f.Get<bool>(L"ReturnValue")) *awake = 1;
                }
            }
        }
        done->store(1);
    });
    WaitDone(done, 8000);
    return *awake != 0;
}

// HOST side: observe the slot-1 puppet (the client's body). Confirm its
// isRagdoll flips 0->1 (driven by the client over the wire) then 1->0.
void ObserveOnHost() {
    UE_LOGI("ragdoll_test[host]: observer armed -- polling slot-1 puppet for a "
            "wire-driven ragdoll (up to 120 s)");

    // Phase machine: 0 = waiting for puppet to exist; 1 = waiting for the
    // RISING edge (isRagdoll 0->1); 2 = waiting for the FALLING edge (1->0).
    int phase = 0;
    bool sawPuppet = false;
    bool sawFlop = false;
    for (int attempt = 0; attempt < 120 && phase < 3; ++attempt) {
        auto done = std::make_shared<std::atomic<int>>(0);
        auto isRag = std::make_shared<int>(-1);  // -1 = puppet not live / unreadable
        GT::Post([done, isRag] {
            void* puppet = coop::net_pump::Puppet(1).GetActor();
            if (!puppet || !R::IsLive(puppet)) { *isRag = -1; done->store(1); return; }
            bool isRagdoll = false, dead = false;
            if (E::ReadMainPlayerRagdollState(puppet, isRagdoll, dead)) {
                *isRag = isRagdoll ? 1 : 0;
            } else {
                *isRag = -1;  // offsets not resolved yet
            }
            done->store(1);
        });
        WaitDone(done, 8000);
        const int r = *isRag;

        if (phase == 0) {
            if (r >= 0 && !sawPuppet) {
                sawPuppet = true;
                UE_LOGI("ragdoll_test[host]: slot-1 puppet resolved (isRagdoll=%d) -- waiting for wire-driven ragdoll", r);
            }
            if (r >= 0) phase = 1;  // puppet readable -> start watching for the rising edge
        }
        if (phase == 1 && r == 1) {
            // Confirm the puppet mesh is PHYSICALLY flopping (not just the flag set).
            sawFlop = PuppetMeshIsFlopping();
            UE_LOGI("ragdoll_test[host]: observed RISING edge -- puppet isRagdoll 0->1 over the wire; "
                    "puppet mesh physically flopping=%d", sawFlop ? 1 : 0);
            phase = 2;
        }
        if (phase == 2 && r == 0) {
            UE_LOGI("ragdoll_test[host]: observed FALLING edge -- puppet isRagdoll 1->0 (recovered)");
            phase = 3;
        }
        ::Sleep(1000);
    }

    if (phase >= 3) {
        UE_LOGI("ragdoll_test[host]: VERDICT Inc2b e2e %s -- a client's local ragdoll "
                "propagated to its host puppet (rising + falling) purely via the pose stream; "
                "puppet mesh physically flopped=%d (own-mesh sim, leak-free)",
                sawFlop ? "PASS" : "PARTIAL (no flop)", sawFlop ? 1 : 0);
    } else if (!sawPuppet) {
        UE_LOGW("ragdoll_test[host]: VERDICT INCONCLUSIVE -- slot-1 puppet never resolved "
                "(no client connected? puppet never spawned)");
    } else if (phase == 1) {
        UE_LOGW("ragdoll_test[host]: VERDICT FAIL -- puppet resolved but never saw a "
                "wire-driven ragdoll (rising edge). Sender bit or receiver reconcile broken.");
    } else {
        UE_LOGW("ragdoll_test[host]: VERDICT PARTIAL -- saw the ragdoll RISING edge but not "
                "the recover (falling). forceGetUp recover path may be broken.");
    }
    UE_LOGI("ragdoll_test[host]: DONE");
}

// CLIENT side: drive the LOCAL possessed player into a ragdoll, then recover --
// the same ragdollMode/forceGetUp calls the C-key / faint do. The local
// isRagdoll flip rides the pose stream's kStateBitRagdoll to the host.
void DriveOnClient() {
    UE_LOGI("ragdoll_test[client]: driver armed -- waiting for the local player");

    // Wait for a live local mainPlayer_C (post-possession). Poll up to 60 s.
    auto local = std::make_shared<void*>(nullptr);
    for (int attempt = 0; attempt < 60 && !*local; ++attempt) {
        auto done = std::make_shared<std::atomic<int>>(0);
        GT::Post([local, done] {
            void* mp = coop::players::Registry::Get().Local();
            if (mp && R::IsLive(mp)) *local = mp;
            done->store(1);
        });
        WaitDone(done, 8000);
        if (!*local) ::Sleep(1000);
    }
    if (!*local) {
        UE_LOGW("ragdoll_test[client]: VERDICT INCONCLUSIVE -- no local player in 60 s; cannot drive");
        return;
    }

    // Let the host spawn the slot-1 puppet + arm its observer (the puppet
    // appears a few seconds after our first pose). 12 s is comfortably past
    // that on a LAN smoke.
    UE_LOGI("ragdoll_test[client]: local player resolved -- waiting 12 s for the host puppet to spawn");
    ::Sleep(12000);

    // Drive the local ragdoll (== what the C-key / exhaustion faint do). On the
    // possessed local player this is the normal in-game path, so it's safe.
    {
        auto done = std::make_shared<std::atomic<int>>(0);
        void* mp = *local;
        GT::Post([mp, done] {
            const bool ok = E::SetMainPlayerRagdollMode(mp, /*ragdoll=*/true, /*passOut=*/true, /*death=*/false);
            UE_LOGI("ragdoll_test[client]: drove LOCAL ragdollMode(1,1,0) -> ok=%d (host puppet should flop now)", ok ? 1 : 0);
            done->store(1);
        });
        if (!WaitDone(done, 8000)) {
            UE_LOGW("ragdoll_test[client]: ragdollMode call did not complete (faulted?) -- aborting drive");
            return;
        }
    }

    // Hold the ragdoll long enough for the host observer to catch the rising
    // edge over the wire.
    ::Sleep(5000);

    // Recover (== wakeup / get-up). Clears the local isRagdoll AnimBP gate;
    // the cleared bit rides the next pose so the host puppet gets up too.
    {
        auto done = std::make_shared<std::atomic<int>>(0);
        void* mp = *local;
        GT::Post([mp, done] {
            const bool ok = E::ForceMainPlayerGetUp(mp);
            UE_LOGI("ragdoll_test[client]: drove LOCAL forceGetUp() -> ok=%d (host puppet should recover now)", ok ? 1 : 0);
            done->store(1);
        });
        WaitDone(done, 8000);
    }
    ::Sleep(3000);
    UE_LOGI("ragdoll_test[client]: drive sequence DONE");
}

// ===================== Inc3 damage hurt-flash e2e test =====================
// CLIENT drives its OWN saveSlot.health DOWN in steps; vitals Inc1 streams the
// lower fraction; the HOST's slot-1 puppet SetVitals detects the drop, arms the
// hurt-flash, and Tick flashes the nameplate red -- proven cross-peer by the host
// reading the puppet's IsHurtFlashing(). No new wire (rides the health stream).

// HOST: poll the slot-1 puppet's hurt-flash state until it goes true (the wire-
// driven health drop flashed it) then false (the flash window expired).
void ObserveDamageOnHost() {
    UE_LOGI("damage_test[host]: observer armed -- polling slot-1 puppet hurt-flash (up to 120 s)");
    int phase = 0;  // 0 wait puppet, 1 wait flash ON, 2 wait flash OFF
    bool sawPuppet = false;
    for (int attempt = 0; attempt < 600 && phase < 3; ++attempt) {  // 600 * 200ms = 120 s
        auto done = std::make_shared<std::atomic<int>>(0);
        auto state = std::make_shared<int>(-1);  // -1 no puppet, 0 not flashing, 1 flashing
        GT::Post([done, state] {
            coop::RemotePlayer& rp = coop::net_pump::Puppet(1);
            if (!rp.valid()) { *state = -1; done->store(1); return; }
            *state = rp.IsHurtFlashing() ? 1 : 0;
            done->store(1);
        });
        WaitDone(done, 8000);
        const int s = *state;
        if (phase == 0 && s >= 0) {
            if (!sawPuppet) { sawPuppet = true;
                UE_LOGI("damage_test[host]: slot-1 puppet resolved (flashing=%d) -- waiting for a wire-driven hurt flash", s); }
            phase = 1;
        }
        if (phase == 1 && s == 1) {
            UE_LOGI("damage_test[host]: observed HURT FLASH ON -- puppet nameplate red, driven by the wire-streamed health drop (Inc3 WORKS)");
            phase = 2;
        }
        if (phase == 2 && s == 0) {
            UE_LOGI("damage_test[host]: observed HURT FLASH OFF -- nameplate restored");
            phase = 3;
        }
        ::Sleep(200);
    }
    if (phase >= 3)
        UE_LOGI("damage_test[host]: VERDICT Inc3 e2e PASS -- a client's health drop flashed its host puppet's nameplate red then restored (no new wire)");
    else if (!sawPuppet)
        UE_LOGW("damage_test[host]: VERDICT INCONCLUSIVE -- slot-1 puppet never resolved");
    else if (phase == 1)
        UE_LOGW("damage_test[host]: VERDICT FAIL -- puppet resolved but never saw a hurt flash (health-drop detect or flash broken)");
    else
        UE_LOGW("damage_test[host]: VERDICT PARTIAL -- saw flash ON but not the restore (flash timer never cleared)");
    UE_LOGI("damage_test[host]: DONE");
}

// CLIENT: lower the LOCAL player's saveSlot.health in steps (each a fresh DROP
// edge -> sustained flashing) so the host has ample window to observe. Writes
// the SAME saveSlot vitals Inc1 streams; values stay well above 0 (no death).
void DriveDamageOnClient() {
    UE_LOGI("damage_test[client]: driver armed -- waiting for the local player + save");
    namespace V = ue_wrap::vitals;
    // Wait until the save (saveSlot) resolves so the writes land.
    auto ready = std::make_shared<std::atomic<int>>(0);
    auto maxH = std::make_shared<float>(100.f);
    for (int attempt = 0; attempt < 60 && ready->load() == 0; ++attempt) {
        auto done = std::make_shared<std::atomic<int>>(0);
        GT::Post([ready, maxH, done] {
            float h = 0.f;
            if (V::Read(V::Field::Health, &h)) {
                V::Read(V::Field::MaxHealth, maxH.get());
                if (*maxH <= 0.f) *maxH = 100.f;
                ready->store(1);
            }
            done->store(1);
        });
        WaitDone(done, 8000);
        if (ready->load() == 0) ::Sleep(1000);
    }
    if (ready->load() == 0) {
        UE_LOGW("damage_test[client]: VERDICT INCONCLUSIVE -- saveSlot health never resolved; cannot drive");
        return;
    }
    UE_LOGI("damage_test[client]: save resolved (maxHealth=%.1f) -- waiting 5 s for the host puppet to spawn", *maxH);
    ::Sleep(5000);  // the host puppet spawns on our first pose (~immediately after connect)

    // Three descending writes (0.6 -> 0.4 -> 0.2 of max), 600 ms apart. Each is a
    // fresh drop edge -> re-arms the 500 ms flash -> ~1.7 s of flashing for the
    // host's 200 ms poll to catch.
    const float steps[3] = {0.6f, 0.4f, 0.2f};
    for (int i = 0; i < 3; ++i) {
        auto done = std::make_shared<std::atomic<int>>(0);
        const float target = *maxH * steps[i];
        GT::Post([target, done] {
            const bool ok = V::Write(V::Field::Health, target);
            UE_LOGI("damage_test[client]: wrote local health=%.1f -> ok=%d (Inc1 streams it; host puppet should flash)", target, ok ? 1 : 0);
            done->store(1);
        });
        WaitDone(done, 8000);
        ::Sleep(600);
    }
    ::Sleep(2000);
    UE_LOGI("damage_test[client]: drive sequence DONE");
}

}  // namespace

void RunAutonomousRagdollTest() {
    const std::string roleEnv = ReadEnv("VOTVCOOP_NET_ROLE");
    const bool isHost = (roleEnv != "client");  // default Host if unset
    if (isHost) {
        ObserveOnHost();
    } else {
        DriveOnClient();
    }
}

DWORD WINAPI RagdollTestThread(LPVOID) {
    RunAutonomousRagdollTest();
    return 0;
}

void RunAutonomousDamageTest() {
    const std::string roleEnv = ReadEnv("VOTVCOOP_NET_ROLE");
    const bool isHost = (roleEnv != "client");
    if (isHost) {
        ObserveDamageOnHost();
    } else {
        DriveDamageOnClient();
    }
}

DWORD WINAPI DamageTestThread(LPVOID) {
    RunAutonomousDamageTest();
    return 0;
}

}  // namespace harness::autotest
