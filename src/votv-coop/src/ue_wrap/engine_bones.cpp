// ue_wrap/engine_bones.cpp -- Skeletal mesh bone queries.
//
// Extracted from ue_wrap/engine.cpp (2026-05-25 modular refactor).
// Public API lives in ue_wrap/engine.h; this TU implements the bone-related
// functions in `namespace ue_wrap::engine`.
//
// Used by:
//   - RemotePlayer (puppet foot-on-ground placement, head-bone anchored
//     nameplate, dual-chain Z measurement for symmetric grounding)
//   - one-shot diagnostics (DumpAllBonesWorldZ)

#include "ue_wrap/engine.h"

#include "ue_wrap/call.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace ue_wrap::engine {
namespace {

namespace P = profile;
namespace R = reflection;

void* g_numBonesFn = nullptr, *g_boneNameFn = nullptr, *g_socketLocFn = nullptr;

bool ResolveBoneFns() {
    if (void* sk = R::FindClass(P::name::SkinnedMeshComponentClass)) {
        if (!g_numBonesFn) g_numBonesFn = R::FindFunction(sk, P::name::GetNumBonesFn);
        if (!g_boneNameFn) g_boneNameFn = R::FindFunction(sk, P::name::GetBoneNameFn);
    }
    if (void* sc = R::FindClass(P::name::SceneComponentClass)) {
        if (!g_socketLocFn) g_socketLocFn = R::FindFunction(sc, P::name::GetSocketLocationFn);
    }
    return g_numBonesFn && g_boneNameFn && g_socketLocFn;
}

}  // namespace

bool GetLowestBoneWorldZ(void* skelMeshComp, float& outZ) {
    if (!skelMeshComp || !ResolveBoneFns()) return false;
    int32_t n = 0;
    { ParamFrame f(g_numBonesFn); if (Call(skelMeshComp, f)) n = f.Get<int32_t>(L"ReturnValue"); }
    if (n <= 0) return false;
    float minZ = 1.0e9f;
    bool any = false;
    std::wstring lowestName;
    for (int32_t i = 0; i < n; ++i) {
        uint8_t name[8] = {};
        { ParamFrame nf(g_boneNameFn); nf.Set<int32_t>(L"BoneIndex", i);
          if (!Call(skelMeshComp, nf)) continue;
          nf.GetRaw(L"ReturnValue", name, sizeof(name)); }
        ParamFrame lf(g_socketLocFn);
        lf.SetRaw(L"InSocketName", name, sizeof(name));
        if (!Call(skelMeshComp, lf)) continue;
        const FVector loc = lf.Get<FVector>(L"ReturnValue");
        if (!any || loc.Z < minZ) {
            minZ = loc.Z;
            lowestName = R::ToString(*reinterpret_cast<const R::FName*>(name));
            any = true;
        }
    }
    if (!any) return false;
    UE_LOGI("engine: lowest bone on mesh comp %p = '%ls' world Z=%.2f", skelMeshComp, lowestName.c_str(), minZ);
    outZ = minZ;
    return true;
}

void DumpAllBonesWorldZ(void* skelMeshComp) {
    if (!skelMeshComp || !ResolveBoneFns()) return;
    int32_t n = 0;
    { ParamFrame f(g_numBonesFn); if (Call(skelMeshComp, f)) n = f.Get<int32_t>(L"ReturnValue"); }
    if (n <= 0) { UE_LOGW("engine: DumpAllBonesWorldZ: 0 bones on comp %p", skelMeshComp); return; }
    std::string acc;
    char buf[160];
    for (int32_t i = 0; i < n; ++i) {
        uint8_t name[8] = {};
        { ParamFrame nf(g_boneNameFn); nf.Set<int32_t>(L"BoneIndex", i);
          if (!Call(skelMeshComp, nf)) continue;
          nf.GetRaw(L"ReturnValue", name, sizeof(name)); }
        ParamFrame lf(g_socketLocFn);
        lf.SetRaw(L"InSocketName", name, sizeof(name));
        if (!Call(skelMeshComp, lf)) continue;
        const FVector loc = lf.Get<FVector>(L"ReturnValue");
        const std::wstring s = R::ToString(*reinterpret_cast<const R::FName*>(name));
        // ToString returns UTF-16; collapse to ASCII for the log buffer.
        std::string asc; asc.reserve(s.size());
        for (wchar_t c : s) asc.push_back(static_cast<char>(c < 0x80 ? c : '?'));
        snprintf(buf, sizeof(buf), "    [%3d] %-32s world=(%.1f, %.1f, %.1f)\n", i, asc.c_str(), loc.X, loc.Y, loc.Z);
        acc += buf;
    }
    UE_LOGI("engine: DumpAllBonesWorldZ comp=%p (%d bones):\n%s", skelMeshComp, n, acc.c_str());
}

bool GetBoneWorldZByName(void* skelMeshComp, const wchar_t* boneName, float& outZ) {
    if (!skelMeshComp || !boneName || !ResolveBoneFns()) return false;
    // Find the bone INDEX whose FName matches the requested name. We can't pass an
    // FName directly without looking up its (ComparisonIndex, Number) -- enumerate
    // bones once, match by string, then call GetSocketLocation with the matched FName.
    int32_t n = 0;
    { ParamFrame f(g_numBonesFn); if (Call(skelMeshComp, f)) n = f.Get<int32_t>(L"ReturnValue"); }
    if (n <= 0) return false;
    for (int32_t i = 0; i < n; ++i) {
        uint8_t name[8] = {};
        { ParamFrame nf(g_boneNameFn); nf.Set<int32_t>(L"BoneIndex", i);
          if (!Call(skelMeshComp, nf)) continue;
          nf.GetRaw(L"ReturnValue", name, sizeof(name)); }
        const std::wstring s = R::ToString(*reinterpret_cast<const R::FName*>(name));
        if (s == boneName) {
            ParamFrame lf(g_socketLocFn);
            lf.SetRaw(L"InSocketName", name, sizeof(name));
            if (!Call(skelMeshComp, lf)) return false;
            outZ = lf.Get<FVector>(L"ReturnValue").Z;
            return true;
        }
    }
    return false;
}

}  // namespace ue_wrap::engine
