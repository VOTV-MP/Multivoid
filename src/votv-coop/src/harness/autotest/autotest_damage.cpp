// harness/autotest_damage.cpp -- the Inc3 damage hurt-flash e2e autotest
// (VOTVCOOP_RUN_DAMAGE_TEST): the CLIENT drives its OWN saveSlot.health down in
// steps; the HOST observes its slot-1 puppet hurt-flash + body material swap.
// Extracted verbatim from harness/autotest_vitals.cpp (2026-07-19 s27 dissolve);
// interfaces + per-routine docs in harness/autotest.h.

#include "harness/autotest.h"
#include "harness/screenshot.h"

#include "coop/config/config.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/session/teleport_client.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/actors/vitals.h"

#include <atomic>
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
            coop::RemotePlayer& rp = coop::puppet_drive::Puppet(1);
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
                    void* puppet = coop::puppet_drive::Puppet(1).GetActor();
                    if (!puppet || !R::IsLive(puppet)) { d2->store(1); return; }
                    const ue_wrap::FVector P = E::GetActorLocation(puppet);
                    // Frame the puppet DYNAMICALLY: teleport the host to ~3.5 m in
                    // front of the puppet (toward -X) looking back at it, so the red
                    // body is in the screenshot regardless of where the puppet is
                    // (static test-pose teleports desync the puppet on a big jump).
                    coop::teleport_client::ApplyLocally({P.X + 280.f, P.Y, P.Z + 20.f,
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
                        void* puppet = coop::puppet_drive::Puppet(1).GetActor();
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
                void* puppet = coop::puppet_drive::Puppet(1).GetActor();
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

}  // namespace

void RunAutonomousDamageTest() {
    const std::string roleEnv = cfg::ReadEnv("VOTVCOOP_NET_ROLE");
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
