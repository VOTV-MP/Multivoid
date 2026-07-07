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
#include "harness/screenshot.h"

#include "coop/session/net_pump.h"
#include "coop/player/player_damage.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/dev/teleport_client.h"
#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/puppet.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/vitals.h"

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

// Read whether the slot-1 puppet has a LIVE ragdoll display body whose mesh is
// physically simulating (2026-06-01 xray-actor rework: the visible flop is a
// SEPARATE playerRagdoll_C body, not the puppet's own mesh). IsAnyRigidBodyAwake on
// the body's SkeletalMesh @0x0230 confirms it is actually flopping (true while
// falling; goes false once it settles at rest). Returns false on a missing/dead body.
bool PuppetHasFloppingRagdollBody() {
    auto done = std::make_shared<std::atomic<int>>(0);
    auto ok = std::make_shared<int>(0);
    GT::Post([done, ok] {
        coop::RemotePlayer& rp = coop::net_pump::Puppet(1);
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
        void* puppet = coop::net_pump::Puppet(1).GetActor();
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
        void* puppet = coop::net_pump::Puppet(1).GetActor();
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
        void* puppet = coop::net_pump::Puppet(1).GetActor();
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
            coop::RemotePlayer& rp = coop::net_pump::Puppet(1);
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
        const std::string h = ReadEnv("VOTVCOOP_RAGDOLL_HOLD_MS");
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
            // Confirm the BODY material swap took (slot-0 should be the solid-red
            // hurt material) + capture a screenshot for the visual check.
            {
                auto d2 = std::make_shared<std::atomic<int>>(0);
                GT::Post([d2] {
                    void* puppet = coop::net_pump::Puppet(1).GetActor();
                    if (!puppet || !R::IsLive(puppet)) { d2->store(1); return; }
                    const ue_wrap::FVector P = E::GetActorLocation(puppet);
                    // Frame the puppet DYNAMICALLY: teleport the host to ~3.5 m in
                    // front of the puppet (toward -X) looking back at it, so the red
                    // body is in the screenshot regardless of where the puppet is
                    // (static test-pose teleports desync the puppet on a big jump).
                    coop::dev::teleport_client::ApplyLocally({P.X + 280.f, P.Y, P.Z + 20.f,
                                                              /*pitch*/ -2.f, /*yaw*/ 180.f, /*roll*/ 0.f});
                    void* mesh = ue_wrap::puppet::GetSkeletalMeshComponent(puppet);
                    std::wstring matName = L"<?>";
                    if (mesh && R::IsLive(mesh)) {
                        if (void* gm = R::FindFunction(R::FindClass(L"PrimitiveComponent"), L"GetMaterial")) {
                            ue_wrap::ParamFrame f(gm); f.Set<int32_t>(L"ElementIndex", 0);
                            if (ue_wrap::Call(mesh, f)) { void* m = f.Get<void*>(L"ReturnValue");
                                if (m) matName = R::ToString(R::NameOf(m)); }
                        }
                    }
                    UE_LOGI("damage_test[host]: framed puppet@(%.0f,%.0f,%.0f); body slot-0 material = '%ls' (expect the gore hurt skin)",
                            P.X, P.Y, P.Z, matName.c_str());
                    d2->store(1);
                });
                WaitDone(d2, 8000);
                ::Sleep(300);  // let the host teleport + render settle before the grab
                // Re-confirm the body is STILL red at capture time (the sustained
                // drive keeps the flash open ~4 s, so it should be).
                {
                    auto d4 = std::make_shared<std::atomic<int>>(0);
                    GT::Post([d4] {
                        void* puppet = coop::net_pump::Puppet(1).GetActor();
                        if (puppet && R::IsLive(puppet)) {
                            void* mesh = ue_wrap::puppet::GetSkeletalMeshComponent(puppet);
                            if (mesh && R::IsLive(mesh)) {
                                if (void* gm = R::FindFunction(R::FindClass(L"PrimitiveComponent"), L"GetMaterial")) {
                                    ue_wrap::ParamFrame f(gm); f.Set<int32_t>(L"ElementIndex", 0);
                                    if (ue_wrap::Call(mesh, f)) { void* m = f.Get<void*>(L"ReturnValue");
                                        UE_LOGI("damage_test[host]: AT CAPTURE body slot-0 = '%ls' (must still be the gore hurt skin)",
                                                m ? R::ToString(R::NameOf(m)).c_str() : L"<null>"); }
                                }
                            }
                        }
                        d4->store(1);
                    });
                    WaitDone(d4, 8000);
                }
                // Capture the red body while the flash window is still open.
                // Pure GDI PrintWindow of the host window -- any thread, no toast.
                const bool shot = harness::screenshot::Capture(L"damage-flash-on");
                UE_LOGI("damage_test[host]: screenshot 'damage-flash-on' saved=%d", shot ? 1 : 0);
            }
            phase = 2;
        }
        if (phase == 2 && s == 0) {
            UE_LOGI("damage_test[host]: observed HURT FLASH OFF -- nameplate restored");
            // Confirm the BODY material restored to the skin (NOT stuck red).
            auto d3 = std::make_shared<std::atomic<int>>(0);
            GT::Post([d3] {
                void* puppet = coop::net_pump::Puppet(1).GetActor();
                if (puppet && R::IsLive(puppet)) {
                    void* mesh = ue_wrap::puppet::GetSkeletalMeshComponent(puppet);
                    if (mesh && R::IsLive(mesh)) {
                        if (void* gm = R::FindFunction(R::FindClass(L"PrimitiveComponent"), L"GetMaterial")) {
                            ue_wrap::ParamFrame f(gm); f.Set<int32_t>(L"ElementIndex", 0);
                            if (ue_wrap::Call(mesh, f)) {
                                void* m = f.Get<void*>(L"ReturnValue");
                                UE_LOGI("damage_test[host]: puppet body slot-0 material after flash = '%ls' (expect the kel skin, NOT the gore hurt skin)",
                                        m ? R::ToString(R::NameOf(m)).c_str() : L"<null>");
                            }
                        }
                    }
                }
                d3->store(1);
            });
            WaitDone(d3, 8000);
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
    UE_LOGI("damage_test[client]: save resolved (maxHealth=%.1f) -- waiting 6 s for the host puppet to spawn", *maxH);
    ::Sleep(6000);  // the host puppet spawns on our first pose (~immediately after connect)

    // SUSTAINED drop: ~12 small monotonic writes (0.92 -> ~0.48 of max) 300 ms
    // apart. Each is a fresh drop edge that re-arms the 500 ms flash, so the puppet
    // flashes CONTINUOUSLY for ~4 s -- ample for the host to detect + frame +
    // screenshot the red body mid-flash. Stays well above 0 (no death).
    for (int i = 0; i < 12; ++i) {
        auto done = std::make_shared<std::atomic<int>>(0);
        const float target = *maxH * (0.92f - 0.04f * i);
        GT::Post([target, done] {
            const bool ok = V::Write(V::Field::Health, target);
            UE_LOGI("damage_test[client]: wrote local health=%.1f -> ok=%d (Inc1 streams it; host puppet should flash)", target, ok ? 1 : 0);
            done->store(1);
        });
        WaitDone(done, 8000);
        ::Sleep(300);
    }
    ::Sleep(1500);
    UE_LOGI("damage_test[client]: drive sequence DONE");
}

// ===================== #6 puppet-damage hazard PROBE =====================
// MUST-VERIFY #6 (gates Inc3-WIRE -- the combat loop). Static RE (IDA + SDK,
// 2026-05-31) found NO per-actor health field on mainPlayer_C: health lives ONLY
// on UsaveSlot_C (health@0x428), reached via GameInstance->save_gameInst -- ONE
// per machine. So invoking `Add Player Damage` on a host-side UNPOSSESSED puppet
// (a 2nd mainPlayer_C, GetController()==null) should drain the HOST'S OWN saveSlot
// health. This probe LOCKS that at runtime (the static RE's lone wiggle is the
// `addDamage` skipSetting guard + the BP-bytecode subtract that no static tool sees).
//
//   HOST: read own saveSlot.health -> invoke Add Player Damage(5) on the slot-1
//     puppet -> re-read. A DROP == the shared-saveSlot corruption hazard is REAL
//     (=> Inc3-WIRE must INTERCEPT native damage on puppets, not just relay it).
//     If no drop, a LOCAL control (same call on the host's own player) tells a
//     BP early-out (writes the slot, but skips unpossessed pawns) from a call that
//     never landed. Host health is restored after, so the smoke stays alive/clean.
//   CLIENT: just connects so the slot-1 puppet exists for the host.
// Damage is tiny (5 HP from ~100); the session is disposable. Host-only verdict.

// Invoke AmainPlayer_C::"Add Player Damage"(Damage) on `target`. Returns true iff
// the UFunction resolved AND the call dispatched. Game-thread only. (Harness uses
// raw ParamFrame like the sibling tests -- the shipping Inc3-WIRE will add a proper
// ue_wrap wrapper; this is a throwaway diagnostic.)
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

// Invoke AmainPlayer_C::addDamage(Actor, Damage, Hit, impact, skipSetting=false) on
// `target` -- the HIT-ACTOR-keyed entry the native enemy/physics-impact path forwards
// to (impactDamageCPP, the npc_zombie attack-sphere overlap). skipSetting=false ==
// "do write the health value". Hit(FHitResult)/impact(FVector) stay zero-init (the
// frame is zeroed); a well-formed BP null-checks them and the SEH firewall + bounded
// WaitDone contain any fault. Game-thread only.
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

// GT-posted wrappers: invoke + log dispatch, bounded-wait for completion. `who` is a
// string literal (lives forever -> safe to capture by pointer).
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
            "addDamage) on the slot-1 puppet, diff host saveSlot.health (MUST-VERIFY #6)");

    // Wait for BOTH the slot-1 puppet (client connected) AND the host saveSlot.
    auto puppet = std::make_shared<void*>(nullptr);
    auto before = std::make_shared<float>(-1.f);
    for (int attempt = 0; attempt < 60 && (!*puppet || *before < 0.f); ++attempt) {
        auto done = std::make_shared<std::atomic<int>>(0);
        GT::Post([puppet, before, done] {
            void* p = coop::net_pump::Puppet(1).GetActor();
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
        UE_LOGW("dmghazard[host]: VERDICT #6 = SHARED-SAVESLOT CORRUPTION CONFIRMED -- invoking %s on a "
                "host-side UNPOSSESSED puppet drained the HOST'S OWN saveSlot.health (Add Player Damage "
                "delta=%.2f, addDamage delta=%.2f). Health is the per-machine shared saveSlot. Inc3-WIRE "
                "MUST intercept native damage on puppets (GetController()==null) and route it to the owner "
                "as a reliable PlayerDamage event -- never let a host-side puppet's damage path run "
                "(host health restored to %.2f).",
                (d1 > 0.01f ? "Add Player Damage" : "addDamage"), d1, d2, *before);
        UE_LOGI("dmghazard[host]: DONE");
        return;
    }

    // Neither puppet entry dropped host health -> control: the SAME Add Player Damage
    // on the host's OWN possessed player. Tells "both entries early-out on the
    // unpossessed puppet" (writes work, guarded by possession -> safe) from "calls
    // never landed" (param/resolution bug -> INCONCLUSIVE, revise the probe).
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
        UE_LOGW("dmghazard[host]: VERDICT #6 = PUPPET-ENTRIES-EARLY-OUT -- BOTH 'Add Player Damage' AND "
                "'addDamage' are no-ops on the unpossessed puppet (zero host-health change), while the SAME "
                "'Add Player Damage' on the host's OWN possessed player dropped %.2f -> the damage path "
                "writes the shared saveSlot but GUARDS on possession (GetController()). So directly invoking "
                "either entry on a puppet is SAFE. RESIDUAL: VOTV's native enemy attack reaches the pawn via "
                "impactDamageCPP/overlap (native hit-actor forward) -- if that bypasses the possession guard "
                "it could still corrupt; Inc3-WIRE should still OBSERVE/intercept damage on puppets (needed "
                "for event routing regardless). Host health restored to %.2f.", dc, *before);
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
                coop::RemotePlayer& rp = coop::net_pump::Puppet(1);
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

// ---- Puppet-frame nameplate shot (PROPER, non-ragdoll capture) ----------------
// Frame the slot-1 STANDING puppet for an autonomous nameplate screenshot
// (mp.py puppetshot): stand the host a few metres back + aim the camera at the
// puppet's HEAD so the whole body AND the ImGui "Client" nameplate sit IN frame.
// No ragdoll, no motion -- the antithesis of the ragdoll-shot crutch (which flopped
// the puppet + aimed down at the body so the head fell past the top edge). `reposition`
// is true only on the FIRST call (move the host back once); later calls re-aim only,
// so the host isn't teleported every tick.
void FramePuppetForNameplate(bool reposition) {
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([done, reposition] {
        void* local = coop::players::Registry::Get().Local();
        void* puppet = coop::net_pump::Puppet(1).GetActor();
        if (!local || !R::IsLive(local) || !puppet || !R::IsLive(puppet)) { done->store(1); return; }
        void* ctrl = E::GetController(local);
        if (!ctrl || !R::IsLive(ctrl)) { done->store(1); return; }
        // Aim/position against the VISIBLE MESH (mirrors AimHostAtPuppet) -- the
        // mainPlayer_C ACTOR pivot sits up high (pose-drive alignment), so using the
        // actor Z would float the host at head height + aim over the body.
        const ue_wrap::FVector pa = E::GetActorLocation(puppet);
        void* mesh = ue_wrap::puppet::GetSkeletalMeshComponent(puppet);
        const ue_wrap::FVector pm = (mesh && R::IsLive(mesh)) ? E::GetComponentLocation(mesh) : pa;
        ue_wrap::FVector eye = E::GetActorLocation(local);
        if (reposition) {
            // Stand ~220 u back from the puppet (CLOSE, per user) at the HOST's OWN
            // floor Z (eye.Z); back direction from the current horizontal offset.
            float bx = eye.X - pa.X, by = eye.Y - pa.Y;
            const float len = std::sqrt(bx * bx + by * by);
            if (len < 1.f) { bx = 1.f; by = 0.f; } else { bx /= len; by /= len; }
            const ue_wrap::FVector dst{ pa.X + bx * 220.f, pa.Y + by * 220.f, eye.Z };
            E::SetActorLocation(local, dst);
            eye = dst;
        }
        // Aim above the mesh centre (+110 cm, ~the head) so the head sits mid-frame
        // and the nameplate (anchor = head + 30 cm) tucks right over it, in view.
        const float dx = pm.X - eye.X, dy = pm.Y - eye.Y, dz = (pm.Z + 110.f) - (eye.Z + 60.f);
        const float horiz = std::sqrt(dx * dx + dy * dy);
        const float yaw = std::atan2(dy, dx) * 57.29578f;
        const float pitch = std::atan2(dz, horiz) * 57.29578f;
        E::SetControlRotation(ctrl, ue_wrap::FRotator{pitch, yaw, 0.f});
        if (reposition)
            UE_LOGI("puppet-frame[host]: host at (%.0f,%.0f,%.0f) aimed at mesh (yaw=%.0f pitch=%.1f)",
                    eye.X, eye.Y, eye.Z, yaw, pitch);
        done->store(1);
    });
    WaitDone(done, 8000);
}

// Destroy EVERY kerfurOmega NPC in the world (the save parks 2 at the spawn, which
// clutter/occlude the puppet) so the nameplate shot is clean -- per the user. A
// GUObjectArray walk by class-name substring; K2_DestroyActor each. Test-only (the
// process is killed right after the capture). Game thread, one-shot.
void DestroyAllKerfurs() {
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([done] {
        // Filter to ACTORS: 'kerfur' also matches the kerfur AnimBP instances (incl.
        // the player puppet's own AnimInstance), and K2_DestroyActor on a non-actor
        // via ProcessEvent would run Actor bytecode on the wrong layout -> corruption.
        void* actorCls = R::FindClass(L"Actor");
        if (!actorCls) { UE_LOGW("puppet-frame[host]: Actor class unresolved -- skip kerfur cleanup"); done->store(1); return; }
        void* bases[1] = { actorCls };
        int destroyed = 0;
        const int n = R::NumObjects();  // int32_t == int on Win64
        for (int i = 0; i < n; ++i) {
            void* obj = R::ObjectAt(i);
            if (!obj || !R::IsLive(obj)) continue;
            void* cls = R::ClassOf(obj);
            if (!cls) continue;
            const std::wstring cn = R::ToString(R::NameOf(cls));
            if (cn.find(L"kerfur") != std::wstring::npos && R::IsDescendantOfAny(cls, bases, 1)) {
                if (E::DestroyActor(obj)) ++destroyed;
            }
        }
        UE_LOGI("puppet-frame[host]: destroyed %d kerfur NPC actor(s) for a clean nameplate shot", destroyed);
        done->store(1);
    });
    WaitDone(done, 8000);
}

// HOST: wait for the slot-1 puppet to spawn + settle, REMOVE the kerfur NPCs, then
// frame the puppet UP CLOSE every tick (reposition ~220u back + aim at the head) and
// HOLD. No ragdoll.
void FrameStandingPuppetOnHost() {
    UE_LOGI("puppet-frame[host]: waiting for the slot-1 puppet to spawn...");
    bool announced = false;
    bool cleared = false;
    int settle = 0;
    for (int attempt = 0; attempt < 600; ++attempt) {  // ~300 s cap; mp.py kills after the shot
        auto done = std::make_shared<std::atomic<int>>(0);
        auto v = std::make_shared<bool>(false);
        GT::Post([done, v] { *v = coop::net_pump::Puppet(1).valid(); done->store(1); });
        WaitDone(done, 8000);
        if (*v) {
            if (++settle >= 6) {  // let the puppet converge from the spawn placeholder
                if (!cleared) { DestroyAllKerfurs(); cleared = true; }
                FramePuppetForNameplate(/*reposition=*/true);  // close + tracking
                if (!announced) {
                    announced = true;
                    UE_LOGI("puppet-frame[host]: positioned + aimed CLOSE at the STANDING puppet "
                            "(kerfurs removed) -- PUPPET-FRAME READY");
                }
            }
        }
        ::Sleep(500);
    }
}

void IdlePuppetOnClient() {
    UE_LOGI("puppet-frame[client]: connected -- standing still (no ragdoll) for the host's nameplate shot");
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

void RunAutonomousPuppetFrame() {
    const std::string roleEnv = ReadEnv("VOTVCOOP_NET_ROLE");
    const bool isHost = (roleEnv != "client");  // default Host if unset
    if (isHost) {
        FrameStandingPuppetOnHost();
    } else {
        IdlePuppetOnClient();
    }
}

DWORD WINAPI PuppetFrameThread(LPVOID) {
    RunAutonomousPuppetFrame();
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

void RunAutonomousDmgHazardTest() {
    const std::string roleEnv = ReadEnv("VOTVCOOP_NET_ROLE");
    const bool isHost = (roleEnv != "client");
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

void RunAutonomousPlayerDamageTest() {
    const std::string roleEnv = ReadEnv("VOTVCOOP_NET_ROLE");
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
