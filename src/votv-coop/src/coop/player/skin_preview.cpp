// coop/player/skin_preview.cpp -- see coop/player/skin_preview.h.
//
// The mannequin is a real inert mainPlayer_C puppet spawned through the proven
// ue_wrap::puppet::SpawnPuppet path (CMC tick off, actor tick off, gamemode.mainPlayer
// protected, AnimBP live) -- the same shape network puppets use. It is LOCAL-ONLY:
// never a slot, never on the wire. Collision is disabled so it cannot block or be
// picked up. The skin under the cursor is applied live via client_model::ApplySkinToBody
// (the two-body invariant), so hovering a tile is a real 3D preview.

#include "coop/player/skin_preview.h"

#include "coop/player/client_model.h"
#include "coop/player/local_body.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>

namespace coop::skin_preview {
namespace {

namespace Pup = ue_wrap::puppet;
namespace E   = ue_wrap::engine;
namespace R   = ue_wrap::reflection;
namespace CM  = coop::client_model;

// Game-thread mannequin state.
void*       g_mannequin    = nullptr;
int32_t     g_mannequinIdx = -1;
std::string g_appliedSkin;   // the skin currently on the mannequin

// Render-thread -> game-thread handoff.
std::atomic<uint64_t> g_lastActiveMs{0};  // last Activate() call -- the keep-alive stamp
std::atomic<bool>     g_hasPending{false};
std::string           g_pendingSkin;      // guarded by g_pendingMu
std::mutex            g_pendingMu;

// Despawn the mannequin this long after the skins section stops being shown.
constexpr uint64_t kHideAfterMs = 3000;

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void SpawnMannequin() {
    void* local = coop::players::Registry::Get().Local();
    if (!local || !R::IsLive(local)) return;  // menu / no pawn yet
    void* mesh = Pup::GetMeshPlayerVisibleAsset(local);
    void* anim = Pup::GetMeshPlayerVisibleAnimClass(local);
    void* m = Pup::SpawnPuppet(E::GetCameraLocation(), mesh, anim);
    if (!m) {
        UE_LOGW("skin_preview: mannequin spawn failed");
        return;
    }
    E::SetActorEnableCollision(m, false);  // display-only: no block, no pickup, no step-on
    g_mannequin    = m;
    g_mannequinIdx = R::InternalIndexOf(m);
    g_appliedSkin.clear();  // force a fresh apply on the next Tick
    UE_LOGI("skin_preview: mannequin spawned %p (display-only)", m);
}

void HideMannequin() {
    if (!g_mannequin) return;
    if (R::IsLiveByIndex(g_mannequin, g_mannequinIdx)) E::DestroyActor(g_mannequin);
    g_mannequin    = nullptr;
    g_mannequinIdx = -1;
    g_appliedSkin.clear();
    UE_LOGI("skin_preview: mannequin hidden");
}

}  // namespace

void Activate(const std::string& name) {
    g_lastActiveMs.store(NowMs(), std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(g_pendingMu);
        g_pendingSkin = name;
        g_hasPending.store(true, std::memory_order_relaxed);
    }
}

void Tick() {
    const uint64_t now = NowMs();
    const uint64_t lastActive = g_lastActiveMs.load(std::memory_order_relaxed);
    const bool panelActive = (lastActive != 0) && (now - lastActive) <= kHideAfterMs;

    if (!panelActive) {
        if (g_mannequin) HideMannequin();
        return;
    }
    if (!g_mannequin) SpawnMannequin();
    if (!g_mannequin || !R::IsLiveByIndex(g_mannequin, g_mannequinIdx)) return;

    // Apply the pending skin (hovered / current) if it changed.
    std::string want;
    {
        std::lock_guard<std::mutex> lk(g_pendingMu);
        if (g_hasPending.exchange(false, std::memory_order_relaxed)) want = g_pendingSkin;
    }
    if (!want.empty() && want != g_appliedSkin) {
        void* nativeMesh = coop::local_body::NativeBodyMesh();
        if (CM::ApplySkinToBody(g_mannequin, want, nativeMesh)) {
            UE_LOGI("skin_preview: applied '%s' to mannequin", want.c_str());
        } else {
            UE_LOGW("skin_preview: apply '%s' failed (mesh missing?) -- mannequin keeps previous",
                    want.c_str());
        }
        g_appliedSkin = want;
    }

    // Reposition in front of the camera, offset to one side so the F1 window does
    // not fully cover it, facing the player. Tune kForwardCm / kRightCm / the Z
    // drop after a hands-on look.
    const ue_wrap::FVector  camLoc = E::GetCameraLocation();
    const ue_wrap::FRotator camRot = E::GetCameraRotation();
    const float yaw = camRot.Yaw * 0.01745329252f;  // deg -> rad (UE forward = (cos yaw, sin yaw))
    const ue_wrap::FVector fwd{ std::cos(yaw), std::sin(yaw), 0.f };
    const ue_wrap::FVector right{ -std::sin(yaw), std::cos(yaw), 0.f };
    constexpr float kForwardCm = 260.f;
    constexpr float kRightCm   = 150.f;
    const ue_wrap::FVector pos{
        camLoc.X + fwd.X * kForwardCm + right.X * kRightCm,
        camLoc.Y + fwd.Y * kForwardCm + right.Y * kRightCm,
        camLoc.Z - 20.f };
    E::SetActorLocation(g_mannequin, pos);
    E::SetActorRotation(g_mannequin, ue_wrap::FRotator{0.f, camRot.Yaw + 180.f, 0.f});
}

void OnDisconnect() {
    HideMannequin();
    g_lastActiveMs.store(0, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_pendingMu);
    g_pendingSkin.clear();
    g_hasPending.store(false, std::memory_order_relaxed);
}

}  // namespace coop::skin_preview
