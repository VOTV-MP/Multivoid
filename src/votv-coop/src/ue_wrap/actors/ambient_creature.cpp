// ue_wrap/actors/ambient_creature.cpp -- see ue_wrap/actors/ambient_creature.h.

#include "ue_wrap/actors/ambient_creature.h"

#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/class_reask.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>
#include <cstdint>
#include <optional>

namespace ue_wrap::ambient_creature {
namespace {

namespace R = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;
namespace CR = ue_wrap::class_reask;
namespace P = ue_wrap::profile;

void Collect(const wchar_t* className, std::vector<void*>& out) {
    void* cls = OI::ClassByName(className);
    if (!cls) return;
    OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
        if (R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) static_cast<std::vector<void*>*>(ctx)->push_back(obj);
    }, &out);
}

// Property offsets on the instance's class, kept per class object.
R::InstanceOffset g_staticMesh{L"StaticMesh"};
R::InstanceOffset g_rend{L"rend"};
R::InstanceOffset g_skeletalMesh{L"skeletalMesh"};
R::InstanceOffset g_hiveKey{L"hiveKey"};
R::InstanceOffset g_hive{L"hive"};

void* ComponentAt(void* actor, R::InstanceOffset& field) {
    const int32_t off = field.Of(actor);
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

// A bool field's place in a class, looked up when the class changes. An instance's class pointer names that instance's class
// whatever else has lived at the address, so a match cannot read another class's layout.
struct BoolField {
    const wchar_t* name;
    void* cls = nullptr;
    int32_t off = -1;
    uint8_t mask = 0;
    uint8_t* At(void* obj) {
        void* c = R::ClassOf(obj);
        if (c != cls) {
            cls = c;
            if (!R::FindBoolProperty(c, name, off, mask)) off = -1;
        }
        return off < 0 ? nullptr : static_cast<uint8_t*>(obj) + off;
    }
};
BoolField g_looking{L"looking"};
BoolField g_stand{L"stand"};

void* WalkerAnim(void* walker) {
    static void* s_cls = nullptr;
    static int32_t s_off = -1;
    void* c = R::ClassOf(walker);
    if (c != s_cls) {
        s_cls = c;
        s_off = R::FindPropertyOffset(c, L"anim");
    }
    return s_off < 0 ? nullptr : *reinterpret_cast<void**>(static_cast<uint8_t*>(walker) + s_off);
}

// The native functions and library objects the view test calls: they last the process, and two of the lookups walk the object
// table, while the test runs once a frame for each copy that hunts this player. A miss is asked again by class_reask::Latch (a
// class-set move, then each call for its load window). The parameter frames are kept with them: a call sets every input again
// and the call writes every output, so one frame serves each call and none is allocated per frame.
struct ViewCalls {
    void* sysCdo = nullptr;
    void* libCdo = nullptr;
    void* bounds = nullptr;
    void* frustum = nullptr;
    std::vector<R::ParamInfo> frustumParams;
    std::optional<ParamFrame> boundsFrame;
    std::optional<ParamFrame> frustumFrame;
    std::optional<ParamFrame> renderedFrame;  // the mesh class's WasRecentlyRendered; rebuilt if the function moves
    CR::Latch asked;
    bool Ready() {
        if (bounds && frustum && frustumParams.size() >= 4) return true;
        if (!asked.Due()) return false;
        if (!sysCdo) sysCdo = R::FindClassDefaultObject(L"KismetSystemLibrary");
        if (!libCdo) libCdo = R::FindClassDefaultObject(L"bpCodeLib");
        void* sysCls = sysCdo ? R::FindClass(L"KismetSystemLibrary") : nullptr;
        void* libCls = libCdo ? R::FindClass(L"bpCodeLib") : nullptr;
        if (!bounds && sysCls) bounds = R::FindFunction(sysCls, L"GetComponentBounds");
        if (!frustum && libCls) frustum = R::FindFunction(libCls, L"isBoxInFrustum");
        if (frustum && frustumParams.empty()) frustumParams = R::FunctionParams(frustum);
        return bounds && frustum && frustumParams.size() >= 4;
    }
};
ViewCalls g_view;

// The frame kept in `slot` for `fn`: built on first use and again if the function changed, null when the function cannot be framed.
ParamFrame* KeptFrame(std::optional<ParamFrame>& slot, void* fn) {
    if (slot && slot->function() != fn) slot.reset();
    if (!slot) slot.emplace(fn);
    if (!slot->valid()) {
        slot.reset();
        return nullptr;
    }
    return &*slot;
}

}  // namespace

void ListDeer(std::vector<void*>& out) { Collect(P::name::NpcClass_Deer, out); }

void ListTrees(std::vector<void*>& out) { Collect(P::name::NpcClass_WalkingTree, out); }

bool ReadTree(void* tree, float& scale, bool& landed) {
    void* mesh = ComponentAt(tree, g_staticMesh);
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
    void* mesh = ComponentAt(tree, g_staticMesh);
    if (!mesh) return;
    if (void* fn = R::FindDispatchFunctionCached(R::ClassOf(mesh), L"SetWorldScale3D")) {
        ParamFrame f(fn);
        if (f.valid()) {
            f.Set<FVector>(L"NewScale", {scale, scale, scale});
            Call(mesh, f);
        }
    }
    SetVisible(mesh, landed);
    SetVisible(ComponentAt(tree, g_rend), false);
}

bool Destroy(void* creature) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(creature), L"K2_DestroyActor");
    return fn && component_calls::CallParamless(creature, fn);
}

bool Dissolve(void* deer) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(deer), L"disappear");
    return fn && component_calls::CallParamless(deer, fn);
}

void ListWalkers(std::vector<void*>& out) { Collect(L"prop_wMannequin_C", out); }

bool WriteWalkerStanding(void* walker, bool standing) {
    void* anim = WalkerAnim(walker);
    uint8_t* b = anim ? g_stand.At(anim) : nullptr;
    if (!b) return false;
    *b = standing ? (*b | g_stand.mask) : (*b & static_cast<uint8_t>(~g_stand.mask));
    return true;
}

namespace {
// A bool field of `obj` by name, read by reflection (the diagnostic reads only).
bool ReadBoolField(void* obj, const wchar_t* name, bool& out) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask) || off < 0) return false;
    out = (*(static_cast<const uint8_t*>(obj) + off) & mask) != 0;
    return true;
}
}  // namespace

WalkerView WalkerViewParts(void* walker, void* playerController, bool diagnostics) {
    WalkerView v;
    void* mesh = ComponentAt(walker, g_skeletalMesh);
    v.mesh = mesh;
    if (diagnostics && mesh) {
        v.visibleRead = ReadBoolField(mesh, L"bVisible", v.visible);
        v.hiddenInGameRead = ReadBoolField(mesh, L"bHiddenInGame", v.hiddenInGame);
        v.actorHiddenRead = ReadBoolField(walker, L"bHidden", v.actorHidden);
    }
    if (!mesh || !playerController) return v;
    void* renderedFn = R::FindDispatchFunctionCached(R::ClassOf(mesh), L"WasRecentlyRendered");
    if (!renderedFn || !g_view.Ready()) return v;
    ParamFrame* bounds = KeptFrame(g_view.boundsFrame, g_view.bounds);
    if (!bounds) return v;
    bounds->Set<void*>(L"Component", mesh);
    if (!Call(g_view.sysCdo, *bounds)) return v;
    v.origin = bounds->Get<FVector>(L"Origin");
    v.extent = bounds->Get<FVector>(L"BoxExtent");
    v.boundsRead = true;
    // The library's parameter names are the game's own: by their order, controller, origin, extent, return.
    const std::vector<R::ParamInfo>& names = g_view.frustumParams;
    ParamFrame* frustum = KeptFrame(g_view.frustumFrame, g_view.frustum);
    if (!frustum) return v;
    frustum->Set<void*>(names[0].name.c_str(), playerController);
    frustum->Set<FVector>(names[1].name.c_str(), v.origin);
    frustum->Set<FVector>(names[2].name.c_str(), v.extent);
    if (!Call(g_view.libCdo, *frustum)) return v;
    v.frustumRead = true;
    v.inFrustum = frustum->Get<bool>(names[3].name.c_str());
    // The game's test is a conjunction: a frustum miss ends it, and only a diagnostic reads the rest.
    if (!v.inFrustum && !diagnostics) return v;
    ParamFrame* rendered = KeptFrame(g_view.renderedFrame, renderedFn);
    if (!rendered) return v;
    rendered->Set<float>(L"Tolerance", 3.f);
    if (!Call(mesh, *rendered)) return v;
    v.renderedRead = true;
    v.rendered = rendered->Get<bool>(L"ReturnValue");
    return v;
}

bool WalkerInView(void* walker, void* playerController) { return WalkerViewParts(walker, playerController).InView(); }

void* WalkerGazeController(void* worldContext) {
    if (!worldContext) return nullptr;
    static void* s_cdo = nullptr;
    static void* s_fn = nullptr;
    static CR::Latch s_asked;  // each ask walks the object table
    if ((!s_cdo || !s_fn) && s_asked.Due()) {
        if (!s_cdo) s_cdo = R::FindClassDefaultObject(L"GameplayStatics");
        if (!s_fn) {
            if (void* cls = R::FindClass(L"GameplayStatics")) s_fn = R::FindFunction(cls, L"GetPlayerController");
        }
    }
    if (!s_cdo || !s_fn) return nullptr;
    static std::optional<ParamFrame> s_frame;  // kept: every input is set again and the call writes the return value
    ParamFrame* f = KeptFrame(s_frame, s_fn);
    if (!f) return nullptr;
    f->Set<void*>(L"WorldContextObject", worldContext);
    f->Set<int32_t>(L"PlayerIndex", 0);
    return Call(s_cdo, *f) ? f->Get<void*>(L"ReturnValue") : nullptr;
}

bool ReadWalkerLooking(void* walker) {
    const uint8_t* b = g_looking.At(walker);
    return b && (*b & g_looking.mask) != 0;
}

void* WalkerClass() { return OI::ClassByName(L"prop_wMannequin_C"); }

bool IsWalker(void* actor) {
    // The class is held with the listing it was found under (a class unloaded and loaded again, even at the same address, answers
    // a new id), and asked for again by class_reask::Latch while it is null: a pointer compare per call, not a name hash.
    static void*       s_cls = nullptr;
    static uint64_t    s_listing = 0;
    static CR::Latch    s_asked;
    if (!actor) return false;
    if (s_cls && OI::ClassListingId(s_cls) != s_listing) {
        s_cls = nullptr;
        s_asked.Reset();
    }
    if (!s_cls && s_asked.Due()) {
        s_cls = WalkerClass();
        s_listing = s_cls ? OI::ClassListingId(s_cls) : 0;
    }
    return s_cls && R::ClassOf(actor) == s_cls;
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
    const int32_t keyOff = g_hiveKey.Of(branch);
    const int32_t hiveOff = g_hive.Of(branch);
    if (keyOff < 0 || hiveOff < 0) return false;
    hiveKey = save_record::ReadFNameAt(branch, keyOff);
    void* nest = *reinterpret_cast<void**>(static_cast<uint8_t*>(branch) + hiveOff);
    hive = nest && R::IsLive(nest) ? nest : nullptr;
    return true;
}

bool BindBranchNest(void* branch, void* nest) {
    const int32_t hiveOff = g_hive.Of(branch);
    if (hiveOff < 0 || !nest) return false;
    engine::WriteObjectField(branch, static_cast<size_t>(hiveOff), nest);
    return component_calls::CallParamlessNamed(branch, L"getBee");
}

bool ReleaseBranchNest(void* branch) { return component_calls::CallParamlessNamed(branch, L"touched"); }

void DropBranchNest(void* branch) {
    const int32_t hiveOff = g_hive.Of(branch);
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
