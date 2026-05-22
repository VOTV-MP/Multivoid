// ue_wrap/types.h -- minimal UE value types we marshal into parameter frames.
//
// Engine-wrapper layer (principle 7). Plain PODs matching UE4.27's binary
// layout, so they can be memcpy'd straight into a UFunction parameter frame.
// Only what we actually pass to engine calls lives here.

#pragma once

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

inline FTransform MakeTransform(const FVector& location) {
    FTransform t;
    t.TX = location.X;
    t.TY = location.Y;
    t.TZ = location.Z;
    return t;
}

}  // namespace ue_wrap
