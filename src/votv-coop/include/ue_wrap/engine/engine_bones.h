// ue_wrap/engine/engine_bones.h -- skeletal-mesh bone reads: world positions and rotations by
// name, and the lowest bone, which is how a body is put on the ground. Engine-wrapper layer
// (principle 7): each call marshals one UFunction call or one reflected field access, with no
// gameplay, network or coop state. Game thread unless a declaration says otherwise.
// Implementation: src/ue_wrap/engine/engine_bones.cpp.

#pragma once

#include "ue_wrap/core/types.h"

#include <vector>
#include <cstdint>

namespace ue_wrap::engine {

// World Z of the lowest bone of the evaluated pose. A diagnostic: the kerfur skeleton mixes
// humanoid foot bones with a 'wheels_R_end', so this is not where a human skin's feet are.
bool GetLowestBoneWorldZ(void* skelMeshComp, float& outZ);

// World position of one bone: one GetSocketLocation dispatch per call (the FName cached per name),
// anchoring to the component transform when the bone is missing. False only on resolution failure.
// Game thread.
bool GetBoneWorldLocationByName(void* skelMeshComp, const wchar_t* boneName, FVector& outLoc);

// World rotation of a bone or socket (GetSocketRotation); false if not found. Game thread.
bool GetBoneWorldRotationByName(void* skelMeshComp, const wchar_t* boneName, FRotator& outRot);

// One bone of the evaluated pose: world position and the parent's index in the same array (-1 =
// root).
struct BonePoint {
    FVector world;
    int32_t parent;
};

// Every bone's world position and parent index; the bone graph is cached per component, the
// positions re-read each call (one dispatch per bone: a diagnostic budget, never a hot path).
// Returns the bone count. Game thread.
int CollectSkeletonBonePoints(void* skelMeshComp, std::vector<BonePoint>& out);

}  // namespace ue_wrap::engine
