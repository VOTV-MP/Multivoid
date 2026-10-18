// coop/creatures/geom_octa_sync.cpp -- Geom Octa's non-locomotion pose and montage mirror.

#include "coop/creatures/geom_octa_sync.h"

#include "coop/net/session.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace coop::geom_octa_sync {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace P = ue_wrap::profile;
namespace sg = ue_wrap::script_gate;

constexpr const wchar_t* kClass = L"geomOcta_C";
constexpr const wchar_t* kMontageName = L"geom_octa_Skeleton_Montage";

// The host runs targeting, pathing, attack and despawn callbacks. The client mirror has its Actor
// and movement ticks parked, but Blueprint timers and overlap delegates can still fire; refuse only
// these Octa-owned decision bodies and let the pose lane drive their visible results.
UE_FACT_FN(kRowReceiveTick, "geom_octa_sync", L"geomOcta_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowMove, "geom_octa_sync", L"geomOcta_C", L"move", Declared);
UE_FACT_FN(kRowCharge, "geom_octa_sync", L"geomOcta_C", L"charge", Declared);
UE_FACT_FN(kRowRotateTo, "geom_octa_sync", L"geomOcta_C", L"rotateTo", Declared);
UE_FACT_FN(kRowAggro, "geom_octa_sync", L"geomOcta_C", L"aggro", Declared);
UE_FACT_FN(kRowFire, "geom_octa_sync", L"geomOcta_C", L"fire", Declared);
UE_FACT_FN(kRowDespawn, "geom_octa_sync", L"geomOcta_C", L"despawnAttempt", Declared);
UE_FACT_FN(kRowAggroOverlap, "geom_octa_sync", L"geomOcta_C",
           L"BndEvt__geomOcta_propAggroSphere_K2Node_ComponentBoundEvent_1_ComponentBeginOverlapSignature__DelegateSignature", Declared);
UE_FACT_FN(kRowMinOverlap, "geom_octa_sync", L"geomOcta_C",
           L"BndEvt__geomOcta_minSphere_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature", Declared);
UE_FACT_FN(kRowMoveSuccess, "geom_octa_sync", L"geomOcta_C", L"OnSuccess_B50B45FF44BBE3579FA455929797BD91", Declared);
UE_FACT_FN(kRowMoveFail, "geom_octa_sync", L"geomOcta_C", L"OnFail_B50B45FF44BBE3579FA455929797BD91", Declared);
const std::array<const ue_wrap::facts::Function*, 11> kDecisionRows = {
    &kRowReceiveTick, &kRowMove, &kRowCharge, &kRowRotateTo, &kRowAggro, &kRowFire,
    &kRowDespawn, &kRowAggroOverlap, &kRowMinOverlap, &kRowMoveSuccess, &kRowMoveFail,
};
constexpr int kGateTagBase = 0x474F4300;  // 'GOC'
std::atomic<coop::net::Session*> g_session{nullptr};
bool g_gateRegistered[kDecisionRows.size()]{};
bool g_gateLive = false;

bool OnClient() {
    auto* session = g_session.load(std::memory_order_acquire);
    return session && session->running() && session->role() == coop::net::Role::Client;
}

sg::Verdict OnDecisionPre(const sg::Call&) {
    return OnClient() ? sg::Verdict::Cancel : sg::Verdict::Run;
}

void RegisterDecisionGates() {
    for (std::size_t i = 0; i < kDecisionRows.size(); ++i)
        if (!g_gateRegistered[i])
            g_gateRegistered[i] = sg::WatchName(*kDecisionRows[i], kGateTagBase + static_cast<int>(i),
                                                 &OnDecisionPre, nullptr);
}

struct Layout {
    void* cls = nullptr;
    int32_t aCh = -1, spwnAlpha = -1, octBeamLoop = -1, particleSystem = -1;
    int32_t isAnim = -1, isCharge = -1, despawn = -1;
    uint8_t isAnimMask = 0, isChargeMask = 0, despawnMask = 0;
    bool ready = false;
};
Layout g_layout;

bool IsOcta(void* actor) {
    if (!actor || !R::IsLive(actor)) return false;
    void* cls = R::ClassOf(actor);
    return cls && R::NameEquals(R::NameOf(cls), kClass);
}

// puppet::GetSkeletalMeshComponent resolves mainPlayer_C::mesh_playerVisible. Geom Octa is an
// ACharacter NPC: its animated body and AnimInstance live in ACharacter::Mesh instead.
void* OctaMesh(void* actor) {
    if (!actor || !R::IsLive(actor)) return nullptr;
    auto* bytes = static_cast<uint8_t*>(actor);
    void* mesh = *reinterpret_cast<void**>(bytes + P::off::ACharacter_Mesh);
    return mesh && R::IsLive(mesh) ? mesh : nullptr;
}

bool ResolveLayout(void* actor) {
    void* cls = R::ClassOf(actor);
    if (!cls || !R::NameEquals(R::NameOf(cls), kClass)) return false;
    if (g_layout.cls == cls) return g_layout.ready;
    Layout next{};
    next.cls = cls;
    next.aCh = R::FindPropertyOffset(cls, L"aCh");
    next.spwnAlpha = R::FindPropertyOffset(cls, L"spwnAlpha");
    next.octBeamLoop = R::FindPropertyOffset(cls, L"octBeamLoop");
    next.particleSystem = R::FindPropertyOffset(cls, L"ParticleSystem");
    const bool hasAnim = R::FindBoolProperty(cls, L"isAnim", next.isAnim, next.isAnimMask);
    const bool hasCharge = R::FindBoolProperty(cls, L"isCharge", next.isCharge, next.isChargeMask);
    const bool hasDespawn = R::FindBoolProperty(cls, L"Despawn", next.despawn, next.despawnMask);
    if (next.aCh < 0 || next.spwnAlpha < 0 || next.octBeamLoop < 0 || next.particleSystem < 0 ||
        !hasAnim || !hasCharge || !hasDespawn) {
        UE_LOGW("geom-octa-sync: geomOcta_C animation properties unresolved; extended pose disabled");
        g_layout = next;
        return false;
    }
    next.ready = true;
    g_layout = next;
    UE_LOGI("geom-octa-sync: resolved geomOcta_C animation fields (aCh=%d spwnAlpha=%d ParticleSystem=%d isAnim=%d isCharge=%d Despawn=%d)",
            next.aCh, next.spwnAlpha, next.particleSystem, next.isAnim, next.isCharge, next.despawn);
    return true;
}

bool ReadFlag(void* actor, int32_t offset, uint8_t mask) {
    return offset >= 0 && mask &&
           ((*reinterpret_cast<const uint8_t*>(static_cast<const uint8_t*>(actor) + offset) & mask) != 0);
}

void WriteFlag(void* actor, int32_t offset, uint8_t mask, bool value) {
    if (offset < 0 || !mask) return;
    auto* byte = reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(actor) + offset);
    if (value) *byte |= mask; else *byte &= static_cast<uint8_t>(~mask);
}

void* AnimInstance(void* actor) {
    void* mesh = OctaMesh(actor);
    if (!mesh || !R::IsLive(mesh)) return nullptr;
    auto* instance = *reinterpret_cast<void**>(static_cast<uint8_t*>(mesh) + P::off::USkeletalMesh_AnimScriptInstance);
    return instance && R::IsLive(instance) ? instance : nullptr;
}

uint8_t EncodeAngle(float degrees, float minDegrees, float maxDegrees) {
    if (!std::isfinite(degrees)) degrees = 0.f;
    const float unit = (degrees - minDegrees) / (maxDegrees - minDegrees);
    const float clamped = unit < 0.f ? 0.f : (unit > 1.f ? 1.f : unit);
    return static_cast<uint8_t>(std::lround(clamped * 255.f));
}

float DecodeAngle(uint8_t encoded, float minDegrees, float maxDegrees) {
    return minDegrees + (static_cast<float>(encoded) / 255.f) * (maxDegrees - minDegrees);
}

void DriveChargeFx(void* actor, float aCh) {
    if (g_layout.particleSystem < 0) return;
    auto* bytes = static_cast<uint8_t*>(actor);
    void* particle = *reinterpret_cast<void**>(bytes + g_layout.particleSystem);
    if (!particle || !R::IsLive(particle)) return;

    // Replicate chareTL__UpdateFunc's two deterministic curves. Its callback cannot run on a parked
    // mirror: power = Lerp(1,5,aCh^3), scale = Lerp(2,8,aCh^1.5).
    const float alpha = aCh < 0.f ? 0.f : (aCh > 1.f ? 1.f : aCh);
    static void* fxClass = nullptr;
    static void* setFloatParameter = nullptr;
    if (!fxClass) fxClass = R::FindClass(L"FXSystemComponent");
    if (fxClass && !setFloatParameter) setFloatParameter = R::FindFunction(fxClass, L"SetFloatParameter");
    if (setFloatParameter) {
        static R::FName powerName{};
        if (powerName.ComparisonIndex == 0 && powerName.Number == 0)
            powerName = ue_wrap::fname_utils::StringToFName(L"power");
        if (powerName.ComparisonIndex != 0 || powerName.Number != 0) {
            ue_wrap::ParamFrame frame(setFloatParameter);
            if (frame.valid() && frame.SetRaw(L"ParameterName", &powerName, sizeof(powerName)) &&
                frame.Set<float>(L"Param", 1.f + 4.f * alpha * alpha * alpha))
                ue_wrap::Call(particle, frame);
        }
    }

    static void* sceneClass = nullptr;
    static void* setWorldScale = nullptr;
    if (!sceneClass) sceneClass = R::FindClass(P::name::SceneComponentClass);
    if (sceneClass && !setWorldScale) setWorldScale = R::FindFunction(sceneClass, L"SetWorldScale3D");
    if (setWorldScale) {
        const float curve = std::pow(alpha, 1.5f);
        const float scale = 2.f + 6.f * curve;
        const ue_wrap::FVector worldScale{scale, scale, scale};
        ue_wrap::ParamFrame frame(setWorldScale);
        if (frame.valid() && frame.SetRaw(L"NewScale", &worldScale, sizeof(worldScale)))
            ue_wrap::Call(particle, frame);
    }
}

void ResolveMontageFns(void*& getActive, void*& getPosition, void*& play, void*& setPosition, void*& stop) {
    static void* cls = nullptr;
    static void* sGetActive = nullptr;
    static void* sGetPosition = nullptr;
    static void* sPlay = nullptr;
    static void* sSetPosition = nullptr;
    static void* sStop = nullptr;
    if (!cls) cls = R::FindClass(L"AnimInstance");
    if (cls) {
        if (!sGetActive) sGetActive = R::FindFunction(cls, L"GetCurrentActiveMontage");
        if (!sGetPosition) sGetPosition = R::FindFunction(cls, L"Montage_GetPosition");
        if (!sPlay) sPlay = R::FindFunction(cls, L"Montage_Play");
        if (!sSetPosition) sSetPosition = R::FindFunction(cls, L"Montage_SetPosition");
        if (!sStop) sStop = R::FindFunction(cls, L"Montage_Stop");
    }
    getActive = sGetActive; getPosition = sGetPosition; play = sPlay; setPosition = sSetPosition; stop = sStop;
}

void* OctaMontage() {
    static void* montage = nullptr;
    if (!montage) montage = R::FindObject(kMontageName, L"AnimMontage");
    return montage;
}

bool ReadBeamLoop(void* actor) {
    if (g_layout.octBeamLoop < 0) return false;
    auto* bytes = static_cast<uint8_t*>(actor);
    void* component = *reinterpret_cast<void**>(bytes + g_layout.octBeamLoop);
    if (!component || !R::IsLive(component)) return false;
    static void* audioClass = nullptr;
    static void* isPlayingFn = nullptr;
    if (!audioClass) audioClass = R::FindClass(L"AudioComponent");
    if (audioClass && !isPlayingFn) isPlayingFn = R::FindFunction(audioClass, L"IsPlaying");
    if (!isPlayingFn) return false;
    ue_wrap::ParamFrame frame(isPlayingFn);
    return frame.valid() && ue_wrap::Call(component, frame) && frame.Get<bool>(L"ReturnValue");
}

void DriveBeamLoop(void* actor, bool shouldPlay) {
    if (g_layout.octBeamLoop < 0) return;
    auto* bytes = static_cast<uint8_t*>(actor);
    void* component = *reinterpret_cast<void**>(bytes + g_layout.octBeamLoop);
    if (!component || !R::IsLive(component)) return;
    static void* audioClass = nullptr;
    static void* isPlayingFn = nullptr;
    static void* playFn = nullptr;
    static void* stopFn = nullptr;
    if (!audioClass) audioClass = R::FindClass(L"AudioComponent");
    if (audioClass) {
        if (!isPlayingFn) isPlayingFn = R::FindFunction(audioClass, L"IsPlaying");
        if (!playFn) playFn = R::FindFunction(audioClass, L"Play");
        if (!stopFn) stopFn = R::FindFunction(audioClass, L"Stop");
    }
    if (!isPlayingFn) return;
    ue_wrap::ParamFrame query(isPlayingFn);
    if (!query.valid() || !ue_wrap::Call(component, query)) return;
    const bool playing = query.Get<bool>(L"ReturnValue");
    if (playing == shouldPlay) return;
    if (shouldPlay && playFn) {
        ue_wrap::ParamFrame play(playFn);
        if (play.valid()) { play.Set<float>(L"StartTime", 0.f); ue_wrap::Call(component, play); }
    } else if (!shouldPlay && stopFn) {
        ue_wrap::ParamFrame stop(stopFn);
        if (stop.valid()) ue_wrap::Call(component, stop);
    }
}

bool ActiveMontage(void* anim, void* montage, void* getActive, void* getPosition, float& position) {
    if (!anim || !montage || !getActive || !getPosition) return false;
    ue_wrap::ParamFrame active(getActive);
    if (!active.valid() || !ue_wrap::Call(anim, active)) return false;
    if (active.Get<void*>(L"ReturnValue") != montage) return false;
    ue_wrap::ParamFrame frame(getPosition);
    if (!frame.valid()) return false;
    frame.Set<void*>(L"Montage", montage);
    if (!ue_wrap::Call(anim, frame)) return false;
    position = frame.Get<float>(L"ReturnValue");
    return true;
}

void DriveMontage(void* anim, bool active, float position) {
    if (!anim) return;
    void *getActive = nullptr, *getPosition = nullptr, *play = nullptr, *setPosition = nullptr, *stop = nullptr;
    ResolveMontageFns(getActive, getPosition, play, setPosition, stop);
    void* montage = OctaMontage();
    if (!montage) return;
    float ignored = 0.f;
    const bool playing = ActiveMontage(anim, montage, getActive, getPosition, ignored);
    if (active) {
        if (!playing && play) {
            ue_wrap::ParamFrame frame(play);
            if (frame.valid()) {
                frame.Set<void*>(L"MontageToPlay", montage);
                frame.Set<float>(L"InPlayRate", 1.f);
                ue_wrap::Call(anim, frame);
            }
        }
        if (setPosition) {
            ue_wrap::ParamFrame frame(setPosition);
            if (frame.valid()) {
                frame.Set<void*>(L"Montage", montage);
                frame.Set<float>(L"NewPosition", position);
                ue_wrap::Call(anim, frame);
            }
        }
    } else if (playing && stop) {
        ue_wrap::ParamFrame frame(stop);
        if (frame.valid()) {
            frame.Set<float>(L"InBlendOutTime", 0.05f);
            frame.Set<void*>(L"Montage", montage);
            ue_wrap::Call(anim, frame);
        }
    }
}

}  // namespace

void Capture(void* actor, coop::net::EntityPoseSnapshot& pose) {
    if (!IsOcta(actor) || !ResolveLayout(actor)) return;
    const auto* bytes = static_cast<const uint8_t*>(actor);
    pose.lookAtX = *reinterpret_cast<const float*>(bytes + g_layout.aCh);
    pose.lookAtY = *reinterpret_cast<const float*>(bytes + g_layout.spwnAlpha);
    pose.kerfState = 0;
    if (ReadFlag(actor, g_layout.isAnim, g_layout.isAnimMask)) pose.kerfState |= coop::net::kEntityPoseGeomOctaIsAnim;
    if (ReadFlag(actor, g_layout.isCharge, g_layout.isChargeMask)) pose.kerfState |= coop::net::kEntityPoseGeomOctaIsCharge;
    if (ReadFlag(actor, g_layout.despawn, g_layout.despawnMask)) pose.kerfState |= coop::net::kEntityPoseGeomOctaDespawn;
    if (ReadBeamLoop(actor)) pose.kerfState |= coop::net::kEntityPoseGeomOctaBeamLoop;

    void* mesh = OctaMesh(actor);
    if (mesh && R::IsLive(mesh)) {
        const ue_wrap::FRotator rotation = E::GetComponentWorldRotation(mesh);
        pose.bodyYaw = rotation.Yaw;
        pose.kerfFace = EncodeAngle(rotation.Pitch, -90.f, 90.f);
        pose._pad = EncodeAngle(rotation.Roll, -180.f, 180.f);
    }
    pose.lookAtZ = 0.f;
    void *getActive = nullptr, *getPosition = nullptr, *play = nullptr, *setPosition = nullptr, *stop = nullptr;
    ResolveMontageFns(getActive, getPosition, play, setPosition, stop);
    float montagePosition = 0.f;
    if (ActiveMontage(AnimInstance(actor), OctaMontage(), getActive, getPosition, montagePosition)) {
        pose.kerfState |= coop::net::kEntityPoseGeomOctaMontage;
        pose.lookAtZ = montagePosition;
    }
    pose.stateBits |= coop::net::kEntityPoseBitGeomOcta;
}

void Apply(void* actor, const coop::net::EntityPoseSnapshot& pose) {
    if (!(pose.stateBits & coop::net::kEntityPoseBitGeomOcta) || !IsOcta(actor) || !ResolveLayout(actor)) return;
    auto* bytes = static_cast<uint8_t*>(actor);
    *reinterpret_cast<float*>(bytes + g_layout.aCh) = pose.lookAtX;
    *reinterpret_cast<float*>(bytes + g_layout.spwnAlpha) = pose.lookAtY;
    DriveChargeFx(actor, pose.lookAtX);
    WriteFlag(actor, g_layout.isAnim, g_layout.isAnimMask,
              (pose.kerfState & coop::net::kEntityPoseGeomOctaIsAnim) != 0);
    WriteFlag(actor, g_layout.isCharge, g_layout.isChargeMask,
              (pose.kerfState & coop::net::kEntityPoseGeomOctaIsCharge) != 0);
    WriteFlag(actor, g_layout.despawn, g_layout.despawnMask,
              (pose.kerfState & coop::net::kEntityPoseGeomOctaDespawn) != 0);
    DriveBeamLoop(actor, (pose.kerfState & coop::net::kEntityPoseGeomOctaBeamLoop) != 0);
    if (void* mesh = OctaMesh(actor)) {
        ue_wrap::FRotator rotation = E::GetComponentWorldRotation(mesh);
        rotation.Pitch = DecodeAngle(pose.kerfFace, -90.f, 90.f);
        rotation.Yaw = pose.bodyYaw;
        rotation.Roll = DecodeAngle(pose._pad, -180.f, 180.f);
        E::SetComponentWorldRotation(mesh, rotation);
    }
    DriveMontage(AnimInstance(actor), (pose.kerfState & coop::net::kEntityPoseGeomOctaMontage) != 0,
                 pose.lookAtZ);
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    RegisterDecisionGates();
}

void Tick() {
    if (g_gateLive) return;
    RegisterDecisionGates();
    sg::ResolvePendingNames();
    for (std::size_t i = 0; i < kDecisionRows.size(); ++i)
        if (!g_gateRegistered[i] || !sg::NameWatchLive(*kDecisionRows[i], kGateTagBase + static_cast<int>(i)))
            return;
    g_gateLive = true;
    UE_LOGI("geom-octa-sync: %zu Octa AI/overlap bodies watched; clients use host-authored motion and attacks",
            kDecisionRows.size());
}

}  // namespace coop::geom_octa_sync
