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

#include "coop/config/config.h"

#include "coop/player/puppet_drive.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>

namespace harness::autotest {
namespace {

namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace E = ue_wrap::engine;
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
// SEPARATE playerRagdoll_C body, not the puppet's own mesh). IsAnyRigidBodyAwake on
// the body's SkeletalMesh @0x0230 confirms it is actually flopping (true while
// falling; goes false once it settles at rest). Returns false on a missing/dead body.
bool PuppetHasFloppingRagdollBody() {
    auto done = std::make_shared<std::atomic<int>>(0);
    auto ok = std::make_shared<int>(0);
    GT::Post([done, ok] {
        coop::RemotePlayer& rp = coop::puppet_drive::Puppet(1);
        void* body = rp.RagdollBody();
        if (body && R::IsLiveByIndex(body, rp.RagdollBodyIdx())) {  // recycle-proof (audit 2026-06-01)
            // Aragdoll_C::SkeletalMesh @0x0230 -- the body mesh component.
            void* mesh = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(body) + 0x0230);
            if (mesh && R::IsLive(mesh)) {
                if (void* awakeFn = R::FindFunction(R::FindClass(L"PrimitiveComponent"), L"IsAnyRigidBodyAwake")) {
                    ue_wrap::ParamFrame f(awakeFn);
                    if (ue_wrap::Call(mesh, f) && f.Get<bool>(L"ReturnValue")) *ok = 1;
                }
            }
        }
        done->store(1);
    });
    WaitDone(done, 8000);
    return *ok != 0;
}

// Aim the HOST player's camera at the slot-1 puppet's body, so an autonomous
// screenshot (mp.py ragdollshot) actually FRAMES the falling puppet. The puppet
// converges right next to the host but often off to the side / behind, out of the
// forward FOV. Computes the look-at from the host camera (actor + ~60 eye height)
// to the puppet's MESH world location (the limp body during the flop) and writes
// the host controller's ControlRotation. Game thread; bounded.
void AimHostAtPuppet() {
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([done] {
        void* local = coop::players::Registry::Get().Local();
        void* puppet = coop::puppet_drive::Puppet(1).GetActor();
        if (!local || !R::IsLive(local) || !puppet || !R::IsLive(puppet)) { done->store(1); return; }
        void* ctrl = E::GetController(local);
        if (!ctrl || !R::IsLive(ctrl)) { done->store(1); return; }
        const ue_wrap::FVector h = E::GetActorLocation(local);
        void* mesh = ue_wrap::puppet::GetSkeletalMeshComponent(puppet);
        const ue_wrap::FVector p = (mesh && R::IsLive(mesh)) ? E::GetComponentLocation(mesh)
                                                             : E::GetActorLocation(puppet);
        const float dx = p.X - h.X, dy = p.Y - h.Y, dz = p.Z - (h.Z + 60.f);
        const float horiz = std::sqrt(dx * dx + dy * dy);
        const float yaw = std::atan2(dy, dx) * 57.29578f;
        const float pitch = std::atan2(dz, horiz) * 57.29578f;
        E::SetControlRotation(ctrl, ue_wrap::FRotator{pitch, yaw, 0.f});
        UE_LOGI("ragdoll_test[host]: aimed host camera at puppet body (yaw=%.0f pitch=%.0f dist=%.0f)",
                yaw, pitch, std::sqrt(dx * dx + dy * dy + dz * dz));
        done->store(1);
    });
    WaitDone(done, 8000);
}

// Move the HOST ~280 units back from the puppet (same Z -> stays on the floor, no
// fall) so the fallen body is AHEAD in the frame, not directly under the host's
// own first-person legs (host + client spawn overlapping, so a straight-down view
// is just the host's own feet occluding the body). One-shot at the rising edge.
void PositionHostForShot() {
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([done] {
        void* local = coop::players::Registry::Get().Local();
        void* puppet = coop::puppet_drive::Puppet(1).GetActor();
        if (!local || !R::IsLive(local) || !puppet || !R::IsLive(puppet)) { done->store(1); return; }
        const ue_wrap::FVector h = E::GetActorLocation(local);
        const ue_wrap::FVector p = E::GetActorLocation(puppet);
        float dx = h.X - p.X, dy = h.Y - p.Y;            // direction AWAY from the puppet
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1.f) { dx = 1.f; dy = 0.f; } else { dx /= len; dy /= len; }
        const ue_wrap::FVector dst{ p.X + dx * 280.f, p.Y + dy * 280.f, h.Z };
        E::SetActorLocation(local, dst);
        UE_LOGI("ragdoll_test[host]: moved host to (%.0f,%.0f,%.0f) ~280u back from puppet for the shot",
                dst.X, dst.Y, dst.Z);
        done->store(1);
    });
    WaitDone(done, 8000);
}

// [probe ragdoll-geom] -- decisive welded-root-vs-real-fall diagnostic. RE
// (wf_4f87bbb5) concluded the puppet kel body is VISIBLE-not-hidden during the
// flop but DEGENERATE; the leading hypothesis is a WELDED ROOT (mesh_playerVisible
// simulated without detaching from its parent -> the rig can't translate to the
// floor). This samples whether the body's bounds + lowest bone actually DROP
// toward the host's feet during the flop, or stay pinned at chest height.
// Read-only (no leak on the tickless orphan). Game thread.
void ProbePuppetRagdollGeometry(const char* tag) {
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([done, tag] {
        void* puppet = coop::puppet_drive::Puppet(1).GetActor();
        void* local = coop::players::Registry::Get().Local();
        if (!puppet || !R::IsLive(puppet)) { done->store(1); return; }
        ue_wrap::FVector origin{}, extent{};
        const bool okB = E::GetActorBounds(puppet, false, origin, extent);
        const float bottom = origin.Z - extent.Z;
        void* mesh = ue_wrap::puppet::GetSkeletalMeshComponent(puppet);
        const float compZ = (mesh && R::IsLive(mesh)) ? E::GetComponentLocation(mesh).Z : 0.f;
        float lowBoneZ = 0.f;
        const bool okBone = (mesh && R::IsLive(mesh)) ? E::GetLowestBoneWorldZ(mesh, lowBoneZ) : false;
        const float hostZ = (local && R::IsLive(local)) ? E::GetActorLocation(local).Z : 0.f;
        UE_LOGI("[probe ragdoll-geom %s]: bounds origin.Z=%.0f bottom=%.0f(ok=%d) compZ=%.0f "
                "lowestBoneZ=%.0f(ok=%d) hostActorZ=%.0f -> floor~%.0f (body FALLS if bottom/bone "
                "approach the floor; WELDED if they stay ~compZ chest height)",
                tag, origin.Z, bottom, okB ? 1 : 0, compZ, lowBoneZ, okBone ? 1 : 0,
                hostZ, hostZ - 88.f);
        done->store(1);
    });
    WaitDone(done, 8000);
}

// HOST side: observe the slot-1 puppet (the client's body). Confirm its
// isRagdoll flips 0->1 (driven by the client over the wire) then 1->0.
void ObserveOnHost() {
    UE_LOGI("ragdoll_test[host]: observer armed -- polling slot-1 puppet for a "
            "wire-driven ragdoll (up to 120 s)");

    // Phase machine: 0 = waiting for puppet to exist; 1 = puppet up, settle +
    // frame the STANDING puppet (before-shot) then wait for the RISING edge
    // (isRagdoll 0->1); 2 = waiting for the FALLING edge (1->0).
    int phase = 0;
    bool sawPuppet = false;
    bool sawFlop = false;
    bool positioned = false;  // host moved back + aimed at the standing puppet
    int settle = 0;           // poll ticks waited for the puppet to converge
    for (int attempt = 0; attempt < 120 && phase < 3; ++attempt) {
        auto done = std::make_shared<std::atomic<int>>(0);
        auto isRag = std::make_shared<int>(-1);  // -1 = no puppet, 0 = up, 1 = ragdoll-displayed
        GT::Post([done, isRag] {
            coop::RemotePlayer& rp = coop::puppet_drive::Puppet(1);
            if (!rp.valid()) { *isRag = -1; done->store(1); return; }
            // The xray-actor rework state: a spawned playerRagdoll_C display body
            // (IsRagdollDisplayed) is the authoritative "this puppet is ragdolled"
            // signal -- the puppet's own isRagdoll field is no longer driven.
            *isRag = rp.IsRagdollDisplayed() ? 1 : 0;
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
        // phase 1: puppet is up. Settle (let it converge from the spawn placeholder),
        // then move the host back + aim at the STANDING puppet and announce the
        // before-shot is ready; then keep tracking until the ragdoll fires.
        if (phase == 1 && !positioned) {
            if (++settle >= 4) {
                PositionHostForShot();
                AimHostAtPuppet();
                positioned = true;
                UE_LOGI("ragdoll_test[host]: host positioned + aimed at STANDING puppet -- BEFORE-SHOT READY");
                ProbePuppetRagdollGeometry("standing");  // baseline before the flop
            }
        } else if (phase == 1 && positioned) {
            AimHostAtPuppet();  // keep the standing puppet framed until it flops
            if (r == 1) {
                // Confirm the SEPARATE playerRagdoll_C display body is PHYSICALLY
                // flopping (not just the display latch) -- IsAnyRigidBodyAwake on the
                // body mesh @0x0230 (true while it falls, false once it settles).
                sawFlop = PuppetHasFloppingRagdollBody();
                UE_LOGI("ragdoll_test[host]: observed RISING edge -- puppet ragdoll-displayed over the "
                        "wire; separate playerRagdoll_C body physically flopping=%d", sawFlop ? 1 : 0);
                phase = 2;
            }
        }
        if (phase == 2 && r == 1) {
            AimHostAtPuppet();  // track the puppet (anchored over the flopping body)
        }
        if (phase == 2 && r == 0) {
            UE_LOGI("ragdoll_test[host]: observed FALLING edge -- puppet ragdoll body destroyed (recovered)");
            phase = 3;
        }
        ::Sleep(1000);
    }

    if (phase >= 3) {
        UE_LOGI("ragdoll_test[host]: VERDICT ragdoll e2e %s -- a client's local ragdoll "
                "propagated to its host puppet (rising + falling) purely via the pose stream; the puppet "
                "spawned a SEPARATE playerRagdoll_C body that physically flopped=%d (death-free, no host kill)",
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
            // FAITHFUL driver (2026-06-01): VOTV's REAL ragdollMode on the local
            // possessed player -- exactly what the C-key faint does, so the CLIENT
            // ragdolls LOCALLY too (the test now matches real play). The old direct
            // isRagdoll write was a leak-era workaround; the +5GB was the prop
            // re-snapshot leak (fixed 999708a), and the SP probe confirmed real
            // ragdollMode is leak-free. The isRagdoll flip rides the wire's
            // kStateBitRagdoll to the host, which pelvis-attaches its puppet to a
            // playerRagdoll_C body. (true,false,false) = ragdoll, no faint screen,
            // no death.
            const bool ok = E::SetMainPlayerRagdollMode(mp, /*ragdoll=*/true, /*passOut=*/false, /*death=*/false);
            UE_LOGI("ragdoll_test[client]: ragdollMode(true,false,false) on the LOCAL player -> ok=%d "
                    "(real VOTV ragdoll -- client ragdolls locally; the flag rides the wire to the host puppet)", ok ? 1 : 0);
            done->store(1);
        });
        if (!WaitDone(done, 8000)) {
            UE_LOGW("ragdoll_test[client]: ragdollMode call did not complete (faulted?) -- aborting drive");
            return;
        }
    }

    // Hold the ragdoll. Default 5 s (enough for the host observer's rising-edge
    // poll). VOTVCOOP_RAGDOLL_HOLD_MS overrides it -- the `mp.py ragdollshot`
    // scenario sets a longer hold so the orchestrator can grab 2 host screenshots
    // of the puppet WHILE it is flopped (proving it falls limp, not rigid).
    int holdMs = 5000;
    {
        const std::string h = cfg::ReadEnv("VOTVCOOP_RAGDOLL_HOLD_MS");
        if (!h.empty()) {
            const int v = std::atoi(h.c_str());
            if (v > 0 && v <= 120000) holdMs = v;
        }
    }
    UE_LOGI("ragdoll_test[client]: holding ragdoll for %d ms", holdMs);
    ::Sleep(holdMs);

    // Recover (== wakeup / get-up). Clears the local isRagdoll AnimBP gate;
    // the cleared bit rides the next pose so the host puppet gets up too.
    {
        auto done = std::make_shared<std::atomic<int>>(0);
        void* mp = *local;
        GT::Post([mp, done] {
            const bool ok = E::ForceMainPlayerGetUp(mp);
            UE_LOGI("ragdoll_test[client]: forceGetUp on the LOCAL player -> ok=%d (real recover; the cleared "
                    "isRagdoll rides the wire so the host puppet detaches + destroys its ragdoll body)", ok ? 1 : 0);
            done->store(1);
        });
        WaitDone(done, 8000);
    }
    ::Sleep(3000);
    UE_LOGI("ragdoll_test[client]: drive sequence DONE");
}

}  // namespace

void RunAutonomousRagdollTest() {
    const std::string roleEnv = cfg::ReadEnv("VOTVCOOP_NET_ROLE");
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

}  // namespace harness::autotest
