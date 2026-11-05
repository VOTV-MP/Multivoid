// ue_wrap/actors/ambient_creature.cpp -- see ue_wrap/actors/ambient_creature.h.

#include "ue_wrap/actors/ambient_creature.h"

#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>
#include <cstdint>

namespace ue_wrap::ambient_creature {
namespace {

namespace R = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;
namespace P = ue_wrap::profile;

void Collect(const wchar_t* className, std::vector<void*>& out) {
    void* cls = OI::ClassByName(className);
    if (!cls) return;
    OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
        if (R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) static_cast<std::vector<void*>*>(ctx)->push_back(obj);
    }, &out);
}

void* ComponentNamed(void* actor, const wchar_t* field) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(actor), field);
    if (off < 0) return nullptr;
    return *reinterpret_cast<void**>(static_cast<uint8_t*>(actor) + off);
}

void SetVisible(void* comp, bool visible) {
    void* fn = comp ? R::FindDispatchFunctionCached(R::ClassOf(comp), P::name::SetVisibilityFn) : nullptr;
    if (!fn) return;
    ParamFrame f(fn);
    if (!f.valid()) return;
    f.Set<bool>(L"bNewVisibility", visible);
    f.Set<bool>(L"bPropagateToChildren", false);
    Call(comp, f);
}

}  // namespace

void ListDeer(std::vector<void*>& out) { Collect(P::name::NpcClass_Deer, out); }

void ListTrees(std::vector<void*>& out) { Collect(P::name::NpcClass_WalkingTree, out); }

bool ReadTree(void* tree, float& scale, bool& landed) {
    void* mesh = ComponentNamed(tree, L"StaticMesh");
    if (!mesh) return false;
    void* scaleFn = R::FindDispatchFunctionCached(R::ClassOf(mesh), L"K2_GetComponentScale");
    void* visibleFn = R::FindDispatchFunctionCached(R::ClassOf(mesh), L"IsVisible");
    if (!scaleFn || !visibleFn) return false;
    ParamFrame s(scaleFn);
    ParamFrame v(visibleFn);
    if (!s.valid() || !v.valid() || !Call(mesh, s) || !Call(mesh, v)) return false;
    scale = s.Get<FVector>(L"ReturnValue").X;
    landed = v.Get<bool>(L"ReturnValue");
    return true;
}

void WriteTree(void* tree, float scale, bool landed) {
    void* mesh = ComponentNamed(tree, L"StaticMesh");
    if (!mesh) return;
    if (void* fn = R::FindDispatchFunctionCached(R::ClassOf(mesh), L"SetWorldScale3D")) {
        ParamFrame f(fn);
        if (f.valid()) {
            f.Set<FVector>(L"NewScale", {scale, scale, scale});
            Call(mesh, f);
        }
    }
    SetVisible(mesh, landed);
    SetVisible(ComponentNamed(tree, L"rend"), false);
}

bool Destroy(void* creature) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(creature), L"K2_DestroyActor");
    return fn && component_calls::CallParamless(creature, fn);
}

bool Dissolve(void* deer) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(deer), L"disappear");
    return fn && component_calls::CallParamless(deer, fn);
}

void ListHoles(std::vector<void*>& out) { Collect(L"susDirtHole_C", out); }

void ListBranches(std::vector<void*>& out) { Collect(L"beehiveBranch_C", out); }

void* SpawnFixture(bool branch, const FVector& at, const FRotator& rot) {
    const wchar_t* name = branch ? L"beehiveBranch_C" : L"susDirtHole_C";
    void* cls = OI::ClassByName(name);
    if (!cls) cls = R::FindClass(name);
    void* a = cls ? engine::SpawnActor(cls, at) : nullptr;
    if (a) engine::SetActorRotation(a, rot);
    return a;
}

bool ReadBranch(void* branch, std::wstring& hiveKey, void*& hive) {
    void* cls = R::ClassOf(branch);
    const int32_t keyOff = R::FindPropertyOffset(cls, L"hiveKey");
    const int32_t hiveOff = R::FindPropertyOffset(cls, L"hive");
    if (keyOff < 0 || hiveOff < 0) return false;
    hiveKey = save_record::ReadFNameAt(branch, keyOff);
    void* nest = *reinterpret_cast<void**>(static_cast<uint8_t*>(branch) + hiveOff);
    hive = nest && R::IsLive(nest) ? nest : nullptr;
    return true;
}

bool BindBranchNest(void* branch, void* nest) {
    const int32_t hiveOff = R::FindPropertyOffset(R::ClassOf(branch), L"hive");
    if (hiveOff < 0 || !nest) return false;
    engine::WriteObjectField(branch, static_cast<size_t>(hiveOff), nest);
    return component_calls::CallParamlessNamed(branch, L"getBee");
}

bool ReleaseBranchNest(void* branch) { return component_calls::CallParamlessNamed(branch, L"touched"); }

void DropBranchNest(void* branch) {
    const int32_t hiveOff = R::FindPropertyOffset(R::ClassOf(branch), L"hive");
    if (hiveOff >= 0) engine::WriteObjectField(branch, static_cast<size_t>(hiveOff), nullptr);
}

bool HoleSpot(const FVector& point, FVector& at, FRotator& rot) {
    void* ctx = engine::GetWorldContext();
    static void* sGrass = nullptr;
    if (!sGrass || !R::IsLive(sGrass)) sGrass = R::FindObject(L"grass", L"PhysicalMaterial");
    if (!ctx || !sGrass) return false;
    const uint8_t kStatic = 0;
    trace::Hit hit;
    if (!trace::LineHitForObjects(ctx, {point.X, point.Y, point.Z + 1000.f}, {point.X, point.Y, point.Z - 1000.f}, &kStatic, 1,
                                  nullptr, 0, &hit) ||
        !hit.blocked || hit.physMaterial != sGrass)
        return false;
    at = hit.point;
    const FVector& n = hit.normal;
    rot = FRotator{std::atan2(n.Z, std::sqrt(n.X * n.X + n.Y * n.Y)) * 57.29578f, std::atan2(n.Y, n.X) * 57.29578f, 0.f};
    return true;
}

}  // namespace ue_wrap::ambient_creature
