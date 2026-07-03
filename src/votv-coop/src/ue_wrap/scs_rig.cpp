// ue_wrap/scs_rig.cpp -- see scs_rig.h. RE ground truth for the node/property
// shapes: research/pak_re/dump_scs.py over kerfurOmega.json (31 nodes: 14x
// eff_kerfurJointLife on skeleton bones + lifeLight on 'belly') and
// kerfurOmega_mynet.json (34 nodes: 9x eff_mynetEmitterLimb on limb bones,
// 8x eff_pofinStatic under the foot billboards, 11x decal_digitalGrid at the
// scene root, 3x eff_zapp spark_pofinStatic_Cue loops), 2026-07-03.

#include "ue_wrap/scs_rig.h"

#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/types.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

namespace ue_wrap::scs_rig {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

// ---- reflected property reads on arbitrary UObjects -----------------------
// Offsets are resolved per (declaring class, property) once and cached. All
// reads are RAW MEMORY reads of the live object -- a template object holds the
// EFFECTIVE value for every field (CDO-inherited defaults included), so no
// "was it serialized?" logic is needed.

// Class + property-offset caches. FindClass is a FULL GUObjectArray walk --
// without the class cache a rig build (~65 nodes x ~7 property reads) would
// re-walk the array hundreds of times ([[lesson-full-array-walk-cheap-filter-
// before-nameof]] cost class). Engine classes never unload, so the class
// cache needs no liveness churn; offsets are engine-lifetime constants.
std::unordered_map<std::wstring, void*> g_clsCache;
std::unordered_map<std::wstring, int32_t> g_propOff;

void* ClassByName(const wchar_t* className) {
    auto it = g_clsCache.find(className);
    if (it != g_clsCache.end()) return it->second;
    void* cls = R::FindClass(className);
    if (cls) g_clsCache.emplace(className, cls);  // negative results retry (load order)
    return cls;
}

// Offset of `prop` on `declaringClassName` (the class that DECLARES it --
// FindPropertyOffset does not climb SuperStruct). -1 if unresolved.
int32_t PropOff(const wchar_t* declaringClassName, const wchar_t* prop) {
    std::wstring key(declaringClassName);
    key += L'.';
    key += prop;
    auto it = g_propOff.find(key);
    if (it != g_propOff.end()) return it->second;
    void* cls = ClassByName(declaringClassName);
    if (!cls) return -1;  // not cached: retried once the class exists
    const int32_t off = R::FindPropertyOffset(cls, prop);
    g_propOff.emplace(std::move(key), off);
    if (off < 0)
        UE_LOGW("scs_rig: property %ls.%ls not found (engine layout drift?)",
                declaringClassName, prop);
    return off;
}

template <typename T>
bool ReadAt(void* obj, int32_t off, T& out) {
    if (!obj || off < 0) return false;
    std::memcpy(&out, reinterpret_cast<uint8_t*>(obj) + off, sizeof(T));
    return true;
}

struct TArrayRaw {
    void** Data;
    int32_t Num;
    int32_t Max;
};

// ---- GameplayStatics attached-spawn thunks --------------------------------

void* g_gsCdo = nullptr;
void* g_emitterAttachedFn = nullptr;
void* g_soundAttachedFn = nullptr;
void* g_decalAttachedFn = nullptr;
// AActor component construction (the nameplate/puppet-light path).
void* g_addCompFn = nullptr;
void* g_finishCompFn = nullptr;
// USceneComponent::K2_AttachToComponent + UPointLightComponent class.
void* g_attachFn = nullptr;
void* g_pointLightClass = nullptr;
void* g_setCastShadowsFn = nullptr;

bool ResolveThunks() {
    if (!g_gsCdo) g_gsCdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    if (g_gsCdo) {
        void* cls = R::ClassOf(g_gsCdo);
        if (cls) {
            if (!g_emitterAttachedFn) g_emitterAttachedFn = R::FindFunction(cls, L"SpawnEmitterAttached");
            if (!g_soundAttachedFn) g_soundAttachedFn = R::FindFunction(cls, L"SpawnSoundAttached");
            if (!g_decalAttachedFn) g_decalAttachedFn = R::FindFunction(cls, L"SpawnDecalAttached");
        }
    }
    if (!g_addCompFn || !g_finishCompFn) {
        if (void* actorCls = R::FindClass(P::name::ActorClassName)) {
            if (!g_addCompFn) g_addCompFn = R::FindFunction(actorCls, P::name::AddComponentByClassFn);
            if (!g_finishCompFn) g_finishCompFn = R::FindFunction(actorCls, P::name::FinishAddComponentFn);
        }
    }
    if (!g_attachFn) {
        if (void* sceneCls = R::FindClass(L"SceneComponent"))
            g_attachFn = R::FindFunction(sceneCls, L"K2_AttachToComponent");
    }
    if (!g_pointLightClass) g_pointLightClass = R::FindClass(L"PointLightComponent");
    if (g_pointLightClass && !g_setCastShadowsFn) {
        if (void* lightBase = R::FindClass(L"LightComponentBase"))
            g_setCastShadowsFn = R::FindFunction(lightBase, L"SetCastShadows");
    }
    return g_gsCdo && g_emitterAttachedFn && g_soundAttachedFn && g_decalAttachedFn;
}

// EAttachLocation::KeepRelativeOffset / EAttachmentRule::KeepRelative.
constexpr uint8_t kKeepRelativeOffset = 0;
constexpr uint8_t kAttachRuleKeepRelative = 0;

// One SCS node, flattened for instantiation.
struct Node {
    void* templateObj = nullptr;
    std::wstring className;   // template's class name (leaf)
    R::FName varName{};       // InternalVariableName
    R::FName attachTo{};      // AttachToName (bone/socket; None => parent origin)
    R::FName parentVar{};     // ParentComponentOrVariableName (None => scene root)
    FVector relLoc{0, 0, 0};
    FRotator relRot{0, 0, 0};
    FVector relScale{1, 1, 1};
};

bool NameIsNone(const R::FName& n) { return n.ComparisonIndex == 0; }

// Read one SCS_Node object into a flat Node. Returns false on a null template.
bool ReadNode(void* nodeObj, Node& out) {
    const int32_t offTmpl = PropOff(L"SCS_Node", L"ComponentTemplate");
    const int32_t offVar = PropOff(L"SCS_Node", L"InternalVariableName");
    const int32_t offAttach = PropOff(L"SCS_Node", L"AttachToName");
    const int32_t offParent = PropOff(L"SCS_Node", L"ParentComponentOrVariableName");
    if (!ReadAt(nodeObj, offTmpl, out.templateObj) || !out.templateObj) return false;
    ReadAt(nodeObj, offVar, out.varName);
    ReadAt(nodeObj, offAttach, out.attachTo);
    ReadAt(nodeObj, offParent, out.parentVar);
    out.className = R::ClassNameOf(out.templateObj);
    // Relative transform: effective values from the template's own memory
    // (USceneComponent fields; non-scene templates just skip).
    ReadAt(out.templateObj, PropOff(L"SceneComponent", L"RelativeLocation"), out.relLoc);
    ReadAt(out.templateObj, PropOff(L"SceneComponent", L"RelativeRotation"), out.relRot);
    ReadAt(out.templateObj, PropOff(L"SceneComponent", L"RelativeScale3D"), out.relScale);
    return true;
}

// Recursively collect a node subtree. `parentBone` carries the nearest
// ancestor's bone anchor so a child of a skipped node (mynet's pofinStatic
// bursts live under foot billboards) still lands on the right bone; relative
// offsets do NOT compose across skipped parents (measured: every skipped
// parent in the kerfur rigs sits at identity relative to its bone).
void CollectNodes(void* nodeObj, const R::FName& parentBone,
                  std::vector<std::pair<Node, R::FName>>& out, int depth) {
    if (!nodeObj || depth > 8 || out.size() > 128) return;
    Node n;
    if (!ReadNode(nodeObj, n)) return;
    const R::FName boneHere = !NameIsNone(n.attachTo) ? n.attachTo : parentBone;
    out.emplace_back(n, boneHere);
    TArrayRaw kids{};
    if (ReadAt(nodeObj, PropOff(L"SCS_Node", L"ChildNodes"), kids) &&
        kids.Data && kids.Num > 0 && kids.Num < 128) {
        for (int32_t i = 0; i < kids.Num; ++i)
            CollectNodes(kids.Data[i], boneHere, out, depth + 1);
    }
}

// ---- per-class instantiation ----------------------------------------------

void* SpawnEmitterAttachedNode(void* meshComp, const Node& n, const R::FName& bone) {
    void* tmpl = nullptr;
    if (!ReadAt(n.templateObj, PropOff(L"ParticleSystemComponent", L"Template"), tmpl) || !tmpl)
        return nullptr;  // a PSC with no Template renders nothing -- skip
    ParamFrame f(g_emitterAttachedFn);
    f.Set<void*>(L"EmitterTemplate", tmpl);
    f.Set<void*>(L"AttachToComponent", meshComp);
    f.SetRaw(L"AttachPointName", &bone, sizeof(bone));
    f.SetRaw(L"Location", &n.relLoc, sizeof(n.relLoc));
    f.SetRaw(L"Rotation", &n.relRot, sizeof(n.relRot));
    f.SetRaw(L"Scale", &n.relScale, sizeof(n.relScale));
    f.Set<uint8_t>(L"LocationType", kKeepRelativeOffset);
    f.Set<bool>(L"bAutoDestroy", false);
    f.Set<uint8_t>(L"PoolingMethod", 0);  // EPSCPoolMethod::None
    f.Set<bool>(L"bAutoActivate", true);  // player skin = the alive look; see skin_effects
    if (!Call(g_gsCdo, f)) return nullptr;
    return f.Get<void*>(L"ReturnValue");
}

void* SpawnSoundAttachedNode(void* anchorComp, const Node& n, const R::FName& bone) {
    // Cosmetic audio ONLY: the game's own naming convention marks effect audio
    // "eff_*" (mynet's eff_zapp spark loops); behavioral audio (Audio = meow,
    // kerfurEXE) keeps its plain name and stays with the AI actor.
    if (!R::NameStartsWith(n.varName, L"eff_")) return nullptr;
    void* sound = nullptr;
    if (!ReadAt(n.templateObj, PropOff(L"AudioComponent", L"Sound"), sound) || !sound)
        return nullptr;
    float volume = 1.f, pitch = 1.f;
    ReadAt(n.templateObj, PropOff(L"AudioComponent", L"VolumeMultiplier"), volume);
    ReadAt(n.templateObj, PropOff(L"AudioComponent", L"PitchMultiplier"), pitch);
    ParamFrame f(g_soundAttachedFn);
    f.Set<void*>(L"Sound", sound);
    f.Set<void*>(L"AttachToComponent", anchorComp);
    f.SetRaw(L"AttachPointName", &bone, sizeof(bone));
    f.SetRaw(L"Location", &n.relLoc, sizeof(n.relLoc));
    f.SetRaw(L"Rotation", &n.relRot, sizeof(n.relRot));
    f.Set<uint8_t>(L"LocationType", kKeepRelativeOffset);
    f.Set<bool>(L"bStopWhenAttachedToDestroyed", true);
    f.Set<float>(L"VolumeMultiplier", volume);
    f.Set<float>(L"PitchMultiplier", pitch);
    f.Set<float>(L"StartTime", 0.f);
    f.Set<void*>(L"AttenuationSettings", nullptr);  // the cue carries its own attenuation
    f.Set<void*>(L"ConcurrencySettings", nullptr);
    f.Set<bool>(L"bAutoDestroy", false);
    if (!Call(g_gsCdo, f)) return nullptr;
    return f.Get<void*>(L"ReturnValue");
}

void* SpawnDecalAttachedNode(void* anchorComp, const Node& n, const R::FName& bone) {
    void* mat = nullptr;
    if (!ReadAt(n.templateObj, PropOff(L"DecalComponent", L"DecalMaterial"), mat) || !mat)
        return nullptr;
    FVector size{128, 256, 256};
    ReadAt(n.templateObj, PropOff(L"DecalComponent", L"DecalSize"), size);
    ParamFrame f(g_decalAttachedFn);
    f.Set<void*>(L"DecalMaterial", mat);
    f.SetRaw(L"DecalSize", &size, sizeof(size));
    f.Set<void*>(L"AttachToComponent", anchorComp);
    f.SetRaw(L"AttachPointName", &bone, sizeof(bone));
    f.SetRaw(L"Location", &n.relLoc, sizeof(n.relLoc));
    f.SetRaw(L"Rotation", &n.relRot, sizeof(n.relRot));
    f.Set<uint8_t>(L"LocationType", kKeepRelativeOffset);
    f.Set<float>(L"LifeSpan", 0.f);  // rig lifetime == owner lifetime
    if (!Call(g_gsCdo, f)) return nullptr;
    return f.Get<void*>(L"ReturnValue");
}

// PointLightComponent has no GameplayStatics spawn helper: AddComponentByClass
// (deferred) -> copy the template's light fields (pre-registration raw writes =
// the archetype-copy the engine's own SCS instancing performs) -> finish ->
// attach -> post-registration setters for the render-state-coupled bits.
void* AddPointLightNode(void* actor, void* meshComp, const Node& n, const R::FName& bone) {
    if (!g_addCompFn || !g_finishCompFn || !g_attachFn || !g_pointLightClass) return nullptr;
    // Pass the TEMPLATE's relative transform to both halves of the deferred
    // add: even if FinishAddComponent re-applies its transform param over our
    // pre-finish field writes (unverified engine detail flagged in the
    // correctness audit), it re-applies the CORRECT one.
    FTransform tmplRel{};
    ue_wrap::engine::RotatorToQuat(n.relRot.Pitch, n.relRot.Yaw, n.relRot.Roll,
                                   tmplRel.RotX, tmplRel.RotY, tmplRel.RotZ, tmplRel.RotW);
    tmplRel.TX = n.relLoc.X;
    tmplRel.TY = n.relLoc.Y;
    tmplRel.TZ = n.relLoc.Z;
    tmplRel.SX = n.relScale.X;
    tmplRel.SY = n.relScale.Y;
    tmplRel.SZ = n.relScale.Z;
    ParamFrame add(g_addCompFn);
    add.Set<void*>(L"Class", g_pointLightClass);
    add.Set<bool>(L"bManualAttachment", true);
    add.SetRaw(L"RelativeTransform", &tmplRel, sizeof(tmplRel));
    add.Set<bool>(L"bDeferredFinish", true);
    if (!Call(actor, add)) return nullptr;
    void* comp = add.Get<void*>(L"ReturnValue");
    if (!comp) return nullptr;

    // Archetype copy of the plain light fields, template -> instance.
    auto copyF = [&](const wchar_t* declCls, const wchar_t* prop) {
        const int32_t off = PropOff(declCls, prop);
        float v;
        if (ReadAt(n.templateObj, off, v))
            std::memcpy(reinterpret_cast<uint8_t*>(comp) + off, &v, sizeof(v));
    };
    copyF(L"LightComponentBase", L"Intensity");
    copyF(L"PointLightComponent", L"AttenuationRadius");
    copyF(L"PointLightComponent", L"SourceRadius");
    {
        const int32_t off = PropOff(L"LightComponentBase", L"LightColor");
        uint32_t c;  // FColor (B,G,R,A)
        if (ReadAt(n.templateObj, off, c))
            std::memcpy(reinterpret_cast<uint8_t*>(comp) + off, &c, sizeof(c));
    }
    {  // relative placement, applied by the KeepRelative attach below
        const int32_t offL = PropOff(L"SceneComponent", L"RelativeLocation");
        const int32_t offR = PropOff(L"SceneComponent", L"RelativeRotation");
        if (offL >= 0) std::memcpy(reinterpret_cast<uint8_t*>(comp) + offL, &n.relLoc, sizeof(n.relLoc));
        if (offR >= 0) std::memcpy(reinterpret_cast<uint8_t*>(comp) + offR, &n.relRot, sizeof(n.relRot));
    }

    ParamFrame fin(g_finishCompFn);
    fin.Set<void*>(L"Component", comp);
    fin.Set<bool>(L"bManualAttachment", true);
    fin.SetRaw(L"RelativeTransform", &tmplRel, sizeof(tmplRel));
    Call(actor, fin);

    ParamFrame att(g_attachFn);
    att.Set<void*>(L"Parent", meshComp);
    att.SetRaw(L"SocketName", &bone, sizeof(bone));
    att.Set<uint8_t>(L"LocationRule", kAttachRuleKeepRelative);
    att.Set<uint8_t>(L"RotationRule", kAttachRuleKeepRelative);
    att.Set<uint8_t>(L"ScaleRule", kAttachRuleKeepRelative);
    att.Set<bool>(L"bWeldSimulatedBodies", false);
    Call(comp, att);

    // Render-state-coupled bits go through their setters, post-registration.
    // CastShadows: the kerfur template ships false (dump_scs.py lifeLight row) --
    // and a shadow-casting per-player point light would be a perf cliff anyway.
    if (g_setCastShadowsFn) {
        ParamFrame cs(g_setCastShadowsFn);
        cs.Set<bool>(L"bNewValue", false);
        Call(comp, cs);
    }
    // The template ships bVisible=false (makeSentient turns it on); the player
    // skin wears the alive look, so the light goes visible right here.
    ue_wrap::engine::SetSceneComponentVisibility(comp, true, false);
    return comp;
}

}  // namespace

int InstantiateCosmetics(void* actor, void* meshComp, void* rootComp,
                         void* bpClass, std::vector<void*>& outComponents) {
    if (!actor || !meshComp || !bpClass || !ResolveThunks()) return 0;
    if (!rootComp) rootComp = meshComp;

    void* scs = nullptr;
    if (!ReadAt(bpClass, PropOff(L"BlueprintGeneratedClass", L"SimpleConstructionScript"), scs) || !scs)
        return 0;  // native class or no construction script
    TArrayRaw roots{};
    if (!ReadAt(scs, PropOff(L"SimpleConstructionScript", L"RootNodes"), roots) ||
        !roots.Data || roots.Num <= 0 || roots.Num > 128)
        return 0;

    std::vector<std::pair<Node, R::FName>> nodes;
    nodes.reserve(48);
    const R::FName none{};
    for (int32_t i = 0; i < roots.Num; ++i)
        CollectNodes(roots.Data[i], none, nodes, 0);

    int made = 0;
    for (const auto& [n, bone] : nodes) {
        // Bone-anchored nodes ride the skin mesh; root-anchored ones (mynet's
        // grid decals under the actor) ride the actor root so they track the
        // capsule, not a bone.
        void* anchor = NameIsNone(bone) ? rootComp : meshComp;
        void* comp = nullptr;
        if (n.className == L"ParticleSystemComponent")
            comp = SpawnEmitterAttachedNode(anchor, n, bone);
        else if (n.className == L"DecalComponent")
            comp = SpawnDecalAttachedNode(anchor, n, bone);
        else if (n.className == L"AudioComponent")
            comp = SpawnSoundAttachedNode(anchor, n, bone);
        else if (n.className == L"PointLightComponent")
            comp = AddPointLightNode(actor, anchor, n, bone);
        if (comp) {
            outComponents.push_back(comp);
            ++made;
        }
    }
    return made;
}

}  // namespace ue_wrap::scs_rig
