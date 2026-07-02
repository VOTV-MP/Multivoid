// coop/dev/client_model_probe.cpp -- see coop/dev/client_model_probe.h.

#include "coop/dev/client_model_probe.h"

#include "coop/player/client_model.h"
#include "coop/player/players_registry.h"
#include "coop/session/ini_config.h"

#include "ue_wrap/engine.h"
#include "ue_wrap/hot_path_guard.h"  // UE_ASSERT_GAME_THREAD
#include "ue_wrap/log.h"
#include "ue_wrap/puppet.h"
#include "ue_wrap/types.h"

#include <chrono>
#include <cmath>

namespace coop::dev::client_model_probe {
namespace {

namespace E = ue_wrap::engine;
namespace Pup = ue_wrap::puppet;
using clock = std::chrono::steady_clock;

enum class St { Idle, Settle, Done };
St g_st = St::Idle;
clock::time_point g_t0{};
int g_spawnRetries = 0;

constexpr int kSettleSec     = 8;   // world + player settle before the pair spawns (inert-probe shape)
constexpr int kMaxSpawnRetry = 10;  // BeginDeferred can refuse mid-transition; retry a few ticks then abort

// Actor yaw that makes a mainPlayer_C display puppet FACE the player at `pl` from `spot`.
// The mainPlayer mesh renders facing actor-forward (the BP's -90 +Y shim is internal to the
// mesh component -- see remote_player.cpp yaw notes), so bearing spot->player is the facing.
float YawToward(const ue_wrap::FVector& spot, const ue_wrap::FVector& pl) {
    return std::atan2(pl.Y - spot.Y, pl.X - spot.X) * 180.f / 3.14159265f;
}

}  // namespace

void Install() { /* no boot work -- drives from Tick's state machine */ }

void Tick(bool connected, bool /*isHost*/) {
    static const bool enabled = coop::ini_config::IsIniKeyTrue("client_model_probe");
    if (!enabled) return;
    UE_ASSERT_GAME_THREAD("client_model_probe::Tick");

    const auto now = clock::now();
    switch (g_st) {
    case St::Idle:
        g_t0 = now;
        g_st = St::Settle;
        UE_LOGI("[CLIENTMODEL-PROBE] armed (ini client_model_probe=1) -- settling %ds, then spawning the "
                "kel-vs-scientist comparison pair in front of the player (connected=%d). Solo host is the "
                "cleanest run.", kSettleSec, connected);
        break;

    case St::Settle: {
        if (std::chrono::duration_cast<std::chrono::seconds>(now - g_t0).count() < kSettleSec) break;
        void* local = coop::players::Registry::Get().Local();
        if (!local) break;  // world/player not up yet -- keep waiting
        void* kelSkin = Pup::GetMeshPlayerVisibleAsset(local);
        void* animCls = Pup::GetMeshPlayerVisibleAnimClass(local);
        if (!kelSkin) break;  // save-load hasn't dressed the player yet -- keep waiting
        void* sciMesh = coop::client_model::GetClientPuppetMesh();  // may be null (pak absent)

        // Place the pair ~3 m in front of WHERE THE CAMERA LOOKS (inert-probe shape: camera yaw,
        // not actor-forward), side by side 1.6 m apart, both facing back at the player.
        const ue_wrap::FVector pl = E::GetActorLocation(local);
        const ue_wrap::FRotator cr = E::GetCameraRotation();
        const float yaw = cr.Yaw * 3.14159265f / 180.f;
        const float fx = std::cos(yaw), fy = std::sin(yaw);
        const float rx = fy, ry = -fx;  // camera-right in the XY plane
        const ue_wrap::FVector center{ pl.X + fx * 300.f, pl.Y + fy * 300.f, pl.Z };
        const ue_wrap::FVector leftSpot { center.X + rx * 80.f, center.Y + ry * 80.f, center.Z };
        const ue_wrap::FVector rightSpot{ center.X - rx * 80.f, center.Y - ry * 80.f, center.Z };

        // LEFT: the local kel skin -- the known-good CONTROL (proves a display puppet renders here).
        void* kelA = Pup::SpawnPuppet(leftSpot, kelSkin, animCls);
        if (!kelA) {
            if (++g_spawnRetries >= kMaxSpawnRetry) {
                UE_LOGW("[CLIENTMODEL-PROBE] SpawnPuppet refused %d times -- abort.", g_spawnRetries);
                g_st = St::Done;
            }
            break;  // world mid-transition -- retry next tick
        }
        E::SetActorRotation(kelA, ue_wrap::FRotator{0.f, YawToward(leftSpot, pl), 0.f});
        UE_LOGI("[CLIENTMODEL-PROBE] LEFT control kel puppet=%p mesh=%p @(%.0f,%.0f,%.0f)",
                kelA, kelSkin, leftSpot.X, leftSpot.Y, leftSpot.Z);

        // RIGHT: our pak scientist mesh -- the SUBJECT.
        if (sciMesh) {
            void* sciA = Pup::SpawnPuppet(rightSpot, sciMesh, animCls);
            if (sciA) E::SetActorRotation(sciA, ue_wrap::FRotator{0.f, YawToward(rightSpot, pl), 0.f});
            UE_LOGI("[CLIENTMODEL-PROBE] RIGHT scientist puppet=%p mesh=%p @(%.0f,%.0f,%.0f) -- verdicts: "
                    "HL-scientist shape = cook WORKS; kel robot = render data is still the template; "
                    "nothing there = cook produced no usable render geometry.",
                    sciA, sciMesh, rightSpot.X, rightSpot.Y, rightSpot.Z);
        } else {
            UE_LOGW("[CLIENTMODEL-PROBE] scientist mesh NULL (pak absent / load failed) -- only the LEFT "
                    "kel control spawned");
        }
        UE_LOGI("[CLIENTMODEL-PROBE] pair spawned ~3 m ahead: LEFT=kel control, RIGHT=scientist. Look at them.");
        g_st = St::Done;
        break;
    }

    case St::Done:
        break;
    }
}

}  // namespace coop::dev::client_model_probe
