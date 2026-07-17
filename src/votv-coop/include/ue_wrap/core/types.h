// ue_wrap/types.h -- minimal UE value types we marshal into parameter frames.
//
// Engine-wrapper layer (principle 7). Plain PODs matching UE4.27's binary
// layout, so they can be memcpy'd straight into a UFunction parameter frame.
// Only what we actually pass to engine calls lives here.

#pragma once

#include <cmath>
#include <cstdint>

namespace ue_wrap {

// FVector -- 3 floats, 12 bytes (UE4.27 uses float, not double).
struct FVector {
    float X = 0.f, Y = 0.f, Z = 0.f;
};
static_assert(sizeof(FVector) == 12, "FVector layout");

// FTransform -- 48 bytes: FQuat Rotation (16) + FVector Translation (12+4 pad) +
// FVector Scale3D (12+4 pad). Confirmed by the live param dump (SpawnTransform
// size=48). Defaults to identity rotation, unit scale.
struct FTransform {
    float RotX = 0.f, RotY = 0.f, RotZ = 0.f, RotW = 1.f;  // 0x00 FQuat (identity)
    float TX = 0.f, TY = 0.f, TZ = 0.f, _padT = 0.f;       // 0x10 Translation
    float SX = 1.f, SY = 1.f, SZ = 1.f, _padS = 0.f;       // 0x20 Scale3D
};
static_assert(sizeof(FTransform) == 48, "FTransform layout");

// FRotator -- 3 floats (Pitch, Yaw, Roll), degrees. 12 bytes.
struct FRotator {
    float Pitch = 0.f, Yaw = 0.f, Roll = 0.f;
};
static_assert(sizeof(FRotator) == 12, "FRotator layout");

// FColor -- BGRA byte order (UE convention), 4 bytes.
struct FColor {
    uint8_t B = 0, G = 0, R = 0, A = 255;
};
static_assert(sizeof(FColor) == 4, "FColor layout");

// FVector2D -- 2 floats (screen coords / sizes), 8 bytes.
struct FVector2D {
    float X = 0.f, Y = 0.f;
};
static_assert(sizeof(FVector2D) == 8, "FVector2D layout");

// FLinearColor -- 4 floats RGBA (0..1), 16 bytes. The UCanvas draw API color type.
struct FLinearColor {
    float R = 0.f, G = 0.f, B = 0.f, A = 1.f;
};
static_assert(sizeof(FLinearColor) == 16, "FLinearColor layout");

// SavedMaterial -- one cached material-override slot for the Inc3 hurt flash:
// which component, which slot index, the original material to restore. Not
// marshaled into a param frame; it lives here (not engine.h) so the gameplay
// layer (RemotePlayer) can cache a std::vector of them WITHOUT pulling the full
// engine.h wrapper API into the widely-included remote_player.h.
struct SavedMaterial { void* component = nullptr; int32_t index = 0; void* original = nullptr; };

inline FTransform MakeTransform(const FVector& location) {
    FTransform t;
    t.TX = location.X;
    t.TY = location.Y;
    t.TZ = location.Z;
    return t;
}

// Normalize a degree angle into the canonical FRotator axis range (-180, 180].
// UE4's AController::GetControlRotation returns the RAW ControlRotation field,
// which the input system accumulates as unnormalized [0, 360): looking 10 deg
// DOWN reads back as Pitch=350, not -10. Two independent agents (2026-05-23)
// converged on this as the root cause of "puppet freezes when source looks
// below horizontal" -- the unnormalized 350 failed coop::net::ValidatePose's
// (-90, 90) bound and dropped the entire pose packet. The proper fix is at
// the wire boundary: produce values in the canonical axis range BEFORE they
// enter PoseSnapshot. Mirrors MTA's SCameraRotationSync bWrapInsteadOfClamp
// pattern (see research/findings/mta/mta-rotation-normalize-2026-05-23.md).
// Equivalent of UE4's FRotator::NormalizeAxis(float Angle).
inline float NormalizeAxis(float deg) {
    deg = std::fmod(deg, 360.f);
    if (deg > 180.f)  deg -= 360.f;
    if (deg < -180.f) deg += 360.f;
    return deg;
}

}  // namespace ue_wrap
