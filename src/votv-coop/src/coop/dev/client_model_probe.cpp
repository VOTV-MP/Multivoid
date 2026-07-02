// coop/dev/client_model_probe.cpp -- see coop/dev/client_model_probe.h.

#include "coop/dev/client_model_probe.h"

#include "coop/player/client_model.h"
#include "coop/player/players_registry.h"
#include "coop/session/ini_config.h"

#include "ue_wrap/asset_load.h"
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
        // The probe's fixed subject: the shipped scientist skin (v93 name-keyed API).
        void* sciMesh = coop::client_model::GetSkinMesh("hl_einstein_v1sc");  // may be null (pak absent)

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

        // TAKE 3 (2026-07-02). Take 1: both puppets read as clean kel -- the byte-parse proved our
        // cooked buffers ARE the scientist (1062v/52KB vs kel 17132v/1.78MB), so the kel pixels came
        // from the SECOND body: the inherited ACharacter::Mesh slot (mesh_playerVisible's
        // AttachParent) carries its own kel 1:1 overlap (puppet.cpp spawn notes). Take 2: hiding that
        // slot (SetVisibility+SetHiddenInGame, BOTH with propagate=false -- wrapper verified honest,
        // engine_component.cpp:173) made BOTH bodies vanish -> MEASURED: UE4 implicitly gates a
        // child's rendering by its AttachParent's visibility regardless of the propagate flag.
        // Hiding the parent is a dead end. Take 3 uses NO visibility flags: write the SAME mesh into
        // BOTH slots -- the game's own invariant ("identical skin asset on both, overlapping as ONE
        // body") preserved with our mesh. RIGHT then has scientist in BOTH layers: masking impossible.
        auto applyToNativeSlot = [](void* actor, void* mesh, const wchar_t* which) {
            void* slot = Pup::GetNativeBodyMeshComponent(actor);
            void* was = Pup::GetComponentSkeletalMeshAsset(slot);
            const bool ok = slot && E::SetSkeletalMesh(slot, mesh);
            UE_LOGI("[CLIENTMODEL-PROBE] %ls native ACharacter::Mesh slot=%p asset %p -> %p (set=%d; "
                    "same-mesh-in-both-slots, the game's own two-body invariant)",
                    which, slot, was, mesh, ok ? 1 : 0);
        };

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
        applyToNativeSlot(kelA, kelSkin, L"LEFT");
        UE_LOGI("[CLIENTMODEL-PROBE] LEFT control kel puppet=%p mesh=%p @(%.0f,%.0f,%.0f)",
                kelA, kelSkin, leftSpot.X, leftSpot.Y, leftSpot.Z);

        // RIGHT: our pak scientist mesh -- the SUBJECT, in BOTH body slots.
        if (sciMesh) {
            void* sciA = Pup::SpawnPuppet(rightSpot, sciMesh, animCls);
            if (sciA) {
                E::SetActorRotation(sciA, ue_wrap::FRotator{0.f, YawToward(rightSpot, pl), 0.f});
                applyToNativeSlot(sciA, sciMesh, L"RIGHT");

                // Rung 3 (texture ladder, COOP_CLIENT_MODEL.md 7): bind our cooked UTexture2D
                // onto the RIGHT puppet's slot-0 material via a MID. The slot material is
                // inst_kel4_body (a MIC of mat_object_sk) whose diffuse is texture parameter
                // 'tex' -- overriding it needs NO cooked material. Applied to BOTH body slots
                // (the two-body invariant). Graceful: tex null -> stays garbled kel (rung-2 look).
                void* sciTex = coop::client_model::GetSkinTexture("hl_einstein_v1sc");
                if (sciTex) {
                    int bound = 0;
                    void* comps[2] = { Pup::GetMeshPlayerVisibleComponent(sciA),
                                       Pup::GetNativeBodyMeshComponent(sciA) };
                    for (void* comp : comps) {
                        if (!comp) continue;
                        void* mid = E::CreateDynamicMaterialInstance(comp, 0);
                        if (mid && E::SetTextureParameterValue(mid, L"tex", sciTex)) ++bound;
                        UE_LOGI("[CLIENTMODEL-PROBE] RIGHT tex bind: comp=%p mid=%p tex=%p", comp, mid, sciTex);
                    }
                    UE_LOGI("[CLIENTMODEL-PROBE] RIGHT texture bound on %d/2 slots -- verdict: RIGHT torso-"
                            "colored (chest texture over the whole body) = TEXTURE COOK + MID BINDING WORK "
                            "(atlas is the last rung); RIGHT unchanged garbled = binding no-op (param name / "
                            "MID parent) -> read this log.", bound);
                } else {
                    UE_LOGW("[CLIENTMODEL-PROBE] tex_hl_einstein_v1sc NOT loaded (pak stale? path?) -- RIGHT "
                            "stays rung-2 garbled kel-material look");
                }
            }
            UE_LOGI("[CLIENTMODEL-PROBE] RIGHT scientist puppet=%p mesh=%p @(%.0f,%.0f,%.0f) -- verdicts "
                    "(same mesh in BOTH slots, nothing hidden): LEFT kel + RIGHT humanoid-blob = cook "
                    "geometry WORKS; LEFT kel + RIGHT empty = our render data fails to draw in-engine; "
                    "RIGHT still a clean kel = something re-applies the class default (new fact).",
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
