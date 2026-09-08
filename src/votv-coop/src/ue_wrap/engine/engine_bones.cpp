// ue_wrap/engine/engine_bones.cpp -- skeletal mesh bone queries. The public API is
// ue_wrap/engine/engine_bones.h; this TU implements the bone-related functions in
// `namespace ue_wrap::engine`.
//
// Read by the remote-player puppet (foot-on-ground placement, the head-bone anchored nameplate and
// the hand item's head anchor), by the ragdoll bone overlay and by the ragdoll acceptance arms.

#include "ue_wrap/engine/engine.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace ue_wrap::engine {
namespace {

namespace P = profile;
namespace R = reflection;

void* g_numBonesFn = nullptr, *g_boneNameFn = nullptr, *g_socketLocFn = nullptr;

bool ResolveBoneFns() {
    // All-resolved latch FIRST: without it every call paid TWO full GUObjectArray FindClass walks
    // -- 60-120 walks/s with the bone overlay ON, the per-frame-walk pattern that costs frames.
    // UFunctions are process-lifetime; resolve once.
    if (g_numBonesFn && g_boneNameFn && g_socketLocFn) return true;
    if (void* sk = R::FindClass(P::name::SkinnedMeshComponentClass)) {
        if (!g_numBonesFn) g_numBonesFn = R::FindFunction(sk, P::name::GetNumBonesFn);
        if (!g_boneNameFn) g_boneNameFn = R::FindFunction(sk, P::name::GetBoneNameFn);
    }
    if (void* sc = R::FindClass(P::name::SceneComponentClass)) {
        if (!g_socketLocFn) g_socketLocFn = R::FindFunction(sc, P::name::GetSocketLocationFn);
    }
    return g_numBonesFn && g_boneNameFn && g_socketLocFn;
}

// ---- bone-graph cache (CollectSkeletonBonePoints) ---------------------------------
// The graph (bone FNames + parent links) is constant per skeleton asset; only positions
// change per frame. Cache it per component so a visualizer refresh pays N GetSocketLocation
// calls, not N*(name+parent) enumerations. Keyed by comp pointer + bone count: a recycled
// pointer with a DIFFERENT skeleton almost surely differs in count and rebuilds; a same-
// count recycle re-resolves identical names for the only skeleton this is used on (the
// player-body rig) -- dev-diagnostic tolerance, documented here.
struct BoneGraphCache {
    int32_t n = 0;
    std::vector<std::array<uint8_t, 8>> names;  // per-bone FName bytes (the socket-loc key)
    std::vector<int32_t> parent;                // per-bone parent index, -1 = root
};
std::unordered_map<void*, BoneGraphCache> g_boneGraphs;  // game-thread only (like all of this TU)

bool BuildBoneGraph_(void* comp, int32_t n, BoneGraphCache& out) {
    // GetParentBone lives on USkinnedMeshComponent (runtime parent-chain: FName -> FName).
    static void* s_parentFn = nullptr;
    if (!s_parentFn) {
        if (void* sk = R::FindClass(P::name::SkinnedMeshComponentClass))
            s_parentFn = R::FindFunction(sk, L"GetParentBone");
    }
    if (!s_parentFn) return false;
    out.n = n;
    out.names.assign(static_cast<size_t>(n), {});
    out.parent.assign(static_cast<size_t>(n), -1);
    for (int32_t i = 0; i < n; ++i) {
        ParamFrame nf(g_boneNameFn);
        nf.Set<int32_t>(L"BoneIndex", i);
        if (!Call(comp, nf)) return false;
        nf.GetRaw(L"ReturnValue", out.names[static_cast<size_t>(i)].data(), 8);
    }
    for (int32_t i = 0; i < n; ++i) {
        ParamFrame pf(s_parentFn);
        pf.SetRaw(L"BoneName", out.names[static_cast<size_t>(i)].data(), 8);
        if (!Call(comp, pf)) continue;  // root/unresolved -> stays -1
        uint8_t pn[8] = {};
        pf.GetRaw(L"ReturnValue", pn, sizeof(pn));
        for (int32_t j = 0; j < n; ++j) {
            if (std::memcmp(pn, out.names[static_cast<size_t>(j)].data(), 8) == 0) {
                if (j != i) out.parent[static_cast<size_t>(i)] = j;
                break;
            }
        }
    }
    return true;
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

int CollectSkeletonBonePoints(void* skelMeshComp, std::vector<BonePoint>& out) {
    out.clear();
    if (!skelMeshComp || !ResolveBoneFns()) return 0;
    int32_t n = 0;
    { ParamFrame f(g_numBonesFn); if (Call(skelMeshComp, f)) n = f.Get<int32_t>(L"ReturnValue"); }
    if (n <= 0) return 0;
    // Eviction valve: ragdoll bodies churn per episode and entries are keyed by comp pointer --
    // clear the whole map past a small bound (the next Collect rebuilds one graph; dead keys are
    // never dereferenced, this only caps growth).
    if (g_boneGraphs.size() > 32) g_boneGraphs.clear();
    BoneGraphCache& cache = g_boneGraphs[skelMeshComp];
    if (cache.n != n) {
        if (!BuildBoneGraph_(skelMeshComp, n, cache)) { g_boneGraphs.erase(skelMeshComp); return 0; }
    }
    out.reserve(static_cast<size_t>(n));
    for (int32_t i = 0; i < n; ++i) {
        ParamFrame lf(g_socketLocFn);
        lf.SetRaw(L"InSocketName", cache.names[static_cast<size_t>(i)].data(), 8);
        if (!Call(skelMeshComp, lf)) { out.clear(); return 0; }  // all-or-nothing: parent
                                                                 // indices reference the FULL array
        out.push_back(BonePoint{lf.Get<FVector>(L"ReturnValue"), cache.parent[static_cast<size_t>(i)]});
    }
    return static_cast<int>(out.size());
}

bool GetBoneWorldLocationByName(void* skelMeshComp, const wchar_t* boneName, FVector& outLoc) {
    // The HOT-PATH-SAFE by-name variant: ONE GetSocketLocation dispatch per call. The
    // bone FName comes from the global name table (Conv_StringToName, cached per
    // distinct name below -- 'head' etc. are single GNames entries shared by every
    // mesh), NOT from a skeleton enumeration, which would walk the whole skeleton
    // per call. NOTE:
    // GetSocketLocation silently falls back to the COMPONENT transform when the mesh
    // has no such bone -- for HUD/audio anchors that "still glued to the mesh"
    // fallback is desirable. Returns false only when resolution fails outright.
    // Game thread only (like all of this TU -- the name cache is unsynchronized).
    if (!skelMeshComp || !boneName || !ResolveBoneFns()) return false;
    static std::unordered_map<std::wstring, R::FName> sNames;
    R::FName fn{};
    auto it = sNames.find(boneName);
    if (it != sNames.end()) {
        fn = it->second;
    } else {
        fn = ue_wrap::fname_utils::StringToFName(boneName);
        if (fn.ComparisonIndex == 0 && fn.Number == 0) return false;  // GNames not ready -- retry next call
        sNames.emplace(boneName, fn);
    }
    ParamFrame lf(g_socketLocFn);
    lf.SetRaw(L"InSocketName", &fn, sizeof(fn));
    if (!Call(skelMeshComp, lf)) return false;
    outLoc = lf.Get<FVector>(L"ReturnValue");
    return true;
}

bool GetBoneWorldRotationByName(void* skelMeshComp, const wchar_t* boneName, FRotator& outRot) {
    if (!skelMeshComp || !boneName || !ResolveBoneFns()) return false;
    // SceneComponent::GetSocketRotation (a bone name is a valid socket name) -- resolved
    // lazily here so the location-only readers stay unaffected.
    static void* g_socketRotFn = nullptr;
    if (!g_socketRotFn) {
        if (void* sc = R::FindClass(P::name::SceneComponentClass))
            g_socketRotFn = R::FindFunction(sc, L"GetSocketRotation");
    }
    if (!g_socketRotFn) return false;
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
            ParamFrame rf(g_socketRotFn);
            rf.SetRaw(L"InSocketName", name, sizeof(name));
            if (!Call(skelMeshComp, rf)) return false;
            outRot = rf.Get<FRotator>(L"ReturnValue");
            return true;
        }
    }
    return false;
}

}  // namespace ue_wrap::engine
