// harness/autotest_puppetframe.cpp -- the puppet-frame nameplate CAPTURE RIG
// (VOTVCOOP_RUN_PUPPET_FRAME, the mp.py puppetshot scenario): frame the slot-1
// STANDING puppet up close for an autonomous nameplate screenshot. A capture
// rig, not a vitals test (the s26 ledger flag). Extracted verbatim from
// harness/autotest_vitals.cpp (2026-07-19 s27 dissolve); interfaces +
// per-routine docs in harness/autotest.h.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <cmath>
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
        void* puppet = coop::puppet_drive::Puppet(1).GetActor();
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
        GT::Post([done, v] { *v = coop::puppet_drive::Puppet(1).valid(); done->store(1); });
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

void RunAutonomousPuppetFrame() {
    const std::string roleEnv = cfg::ReadEnv("VOTVCOOP_NET_ROLE");
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

}  // namespace harness::autotest
