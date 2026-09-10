// ue_wrap/engine/scs_rig.cpp -- see scs_rig.h. The node and property shapes come from the
// reflection dumps of the two kerfur skins: the base skin carries joint-life spark emitters
// on skeleton bones and a life light on the belly, all dormant; the mynet skin carries limb
// emitters each with a grid decal and a static burst child, two foot billboards each with a
// decal child, and root spark loops. The mynet templates author absolute rotation on every
// grid decal (the projection box stays world-vertical, a floor grid under the limbs) and
// tick-off on every electricity emitter (the sim never advances, authored-off decoration).
// Those flags are honoured bit-exactly through the reflected bool-property reads.

#include "ue_wrap/engine/scs_rig.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

namespace ue_wrap::scs_rig {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

// Reflected property reads on arbitrary objects. All reads are raw memory reads of the live
// object: a template holds the effective value of every field, inherited defaults included, so
// no was-it-serialised logic is needed.
//
// Offsets are engine-lifetime constants and a rig build (dozens of nodes, several property
// reads each) asks for the same handful over and over, so they are resolved per declaring class
// and property once and kept here. The classes themselves are NOT kept: FindClass has its own
// cache, and that one revalidates the array slot and re-compares the name before answering --
// which a bare pointer parked in a local map cannot do.
std::unordered_map<std::wstring, int32_t> g_propOff;

// Offset of `prop` on `declaringClassName` (the class that declares it; the offset lookup does
// not climb the super chain). -1 if unresolved.
int32_t PropOff(const wchar_t* declaringClassName, const wchar_t* prop) {
    std::wstring key(declaringClassName);
    key += L'.';
    key += prop;
    auto it = g_propOff.find(key);
    if (it != g_propOff.end()) return it->second;
    void* cls = R::FindClass(declaringClassName);
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

// Read a bitfield bool off a component template using the bool property's real byte offset
// and bit mask. A template holds the effective value of every flag, so the masked bit is the
// authored truth: no CDO baselines, no heuristics (a guess against the CDO fails on the first
// template that overrides two flags in one packed byte, and this one did, flooding every
// skin with the violet belly light).
struct BoolProp {
    int32_t off = -1;
    uint8_t mask = 0;
};
std::unordered_map<std::wstring, BoolProp> g_boolProps;

const BoolProp& BoolPropOf(const wchar_t* declClass, const wchar_t* prop) {
    std::wstring key(declClass);
    key += L'.';
    key += prop;
    auto it = g_boolProps.find(key);
    if (it != g_boolProps.end()) return it->second;
    void* cls = R::FindClass(declClass);
    if (!cls) {
        static const BoolProp kUnresolved{};
        return kUnresolved;  // class not loaded yet: not cached, retried
    }
    BoolProp bp;
    if (!R::FindBoolProperty(cls, prop, bp.off, bp.mask))
        UE_LOGW("scs_rig: bool property %ls.%ls not found (engine layout drift?)",
                declClass, prop);
    return g_boolProps.emplace(std::move(key), bp).first->second;
}

bool TemplateFlag(void* templateObj, const wchar_t* declClass, const wchar_t* prop,
                  bool fallback) {
    const BoolProp& bp = BoolPropOf(declClass, prop);
    uint8_t b = 0;
    if (bp.off < 0 || !ReadAt(templateObj, bp.off, b)) return fallback;
    return (b & bp.mask) != 0;
}

// The tick function's start-enabled flag inside the component's primary-tick struct member
// (offsets composed once). Offset -2 is unresolved, -1 is a failed resolution.
BoolProp g_startTick{-2, 0};

bool TemplateStartsTickEnabled(void* templateObj) {
    if (g_startTick.off == -2) {
        g_startTick.off = -1;
        void* cls = R::FindClass(L"ActorComponent");
        const int32_t tickOff = PropOff(L"ActorComponent", L"PrimaryComponentTick");
        void* tickStruct =
            (cls && tickOff >= 0) ? R::PropertyInnerStruct(cls, L"PrimaryComponentTick") : nullptr;
        int32_t innerOff = -1;
        uint8_t mask = 0;
        if (tickStruct && R::FindBoolProperty(tickStruct, L"bStartWithTickEnabled", innerOff, mask)) {
            g_startTick.off = tickOff + innerOff;
            g_startTick.mask = mask;
        } else {
            UE_LOGW("scs_rig: PrimaryComponentTick.bStartWithTickEnabled unresolved -- "
                    "template tick authoring not honored");
        }
    }
    uint8_t b = 0;
    if (g_startTick.off < 0 || !ReadAt(templateObj, g_startTick.off, b)) return true;
    return (b & g_startTick.mask) != 0;
}

struct TArrayRaw {
    void** Data;
    int32_t Num;
    int32_t Max;
};

// The gameplay-statics attached-spawn thunks.

void* g_gsCdo = nullptr;
void* g_emitterAttachedFn = nullptr;
void* g_soundAttachedFn = nullptr;
void* g_decalAttachedFn = nullptr;
// Actor component construction (the nameplate and puppet-light path).
void* g_addCompFn = nullptr;
void* g_finishCompFn = nullptr;
// The scene component attach call and the point light class.
void* g_attachFn = nullptr;
void* g_pointLightClass = nullptr;
void* g_setCastShadowsFn = nullptr;
// Template-fidelity setters: the scene component's SetAbsolute (world-anchored transform
// axes) and the component's SetComponentTickEnabled (authored-off simulation).
void* g_setAbsoluteFn = nullptr;
void* g_setTickEnabledFn = nullptr;

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
    if (!g_attachFn || !g_setAbsoluteFn) {
        if (void* sceneCls = R::FindClass(L"SceneComponent")) {
            if (!g_attachFn) g_attachFn = R::FindFunction(sceneCls, L"K2_AttachToComponent");
            if (!g_setAbsoluteFn) g_setAbsoluteFn = R::FindFunction(sceneCls, L"SetAbsolute");
        }
    }
    if (!g_setTickEnabledFn) {
        if (void* acCls = R::FindClass(L"ActorComponent"))
            g_setTickEnabledFn = R::FindFunction(acCls, L"SetComponentTickEnabled");
    }
    if (!g_pointLightClass) g_pointLightClass = R::FindClass(L"PointLightComponent");
    if (g_pointLightClass && !g_setCastShadowsFn) {
        if (void* lightBase = R::FindClass(L"LightComponentBase"))
            g_setCastShadowsFn = R::FindFunction(lightBase, L"SetCastShadows");
    }
    return g_gsCdo && g_emitterAttachedFn && g_soundAttachedFn && g_decalAttachedFn;
}

// The keep-relative-offset attach location and the keep-relative attachment rule.
constexpr uint8_t kKeepRelativeOffset = 0;
constexpr uint8_t kAttachRuleKeepRelative = 0;

// One construction-script node, flattened for instantiation.
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

// Read one construction-script node object into a flat node. False on a null template.
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
    // Relative transform: effective values from the template's own memory (scene component
    // fields; non-scene templates skip).
    ReadAt(out.templateObj, PropOff(L"SceneComponent", L"RelativeLocation"), out.relLoc);
    ReadAt(out.templateObj, PropOff(L"SceneComponent", L"RelativeRotation"), out.relRot);
    ReadAt(out.templateObj, PropOff(L"SceneComponent", L"RelativeScale3D"), out.relScale);
    return true;
}

// Recursively collect a node subtree. `parentBone` carries the nearest ancestor's bone anchor
// so a child of a skipped node (the mynet static bursts live under the foot billboards) still
// lands on the right bone; relative offsets do not compose across skipped parents (every
// skipped parent in the kerfur rigs sits at identity relative to its bone).
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

// Per-class instantiation.

void* SpawnEmitterAttachedNode(void* meshComp, const Node& n, const R::FName& bone) {
    void* tmpl = nullptr;
    if (!ReadAt(n.templateObj, PropOff(L"ParticleSystemComponent", L"Template"), tmpl) || !tmpl)
        return nullptr;  // a PSC with no Template renders nothing -- skip
    // Template-faithful activation: the kerfur joint-life sparks ship auto-activate off (only the
    // sentient path turns them on); the mynet emitters keep the default on. The template byte
    // holds the effective value.
    if (!TemplateFlag(n.templateObj, L"ActorComponent", L"bAutoActivate", true))
        return nullptr;  // dormant-by-authoring: nothing to show
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
    f.Set<bool>(L"bAutoActivate", true);
    if (!Call(g_gsCdo, f)) return nullptr;
    return f.Get<void*>(L"ReturnValue");
}

void* SpawnSoundAttachedNode(void* anchorComp, const Node& n, const R::FName& bone) {
    // Cosmetic audio only: the game's own naming convention marks effect audio with an "eff_"
    // prefix (the mynet spark loops); behavioural audio keeps its plain name and stays with the
    // AI actor.
    if (!R::NameStartsWith(n.varName, L"eff_")) return nullptr;
    if (!TemplateFlag(n.templateObj, L"ActorComponent", L"bAutoActivate", true))
        return nullptr;  // dormant-by-authoring
    void* sound = nullptr;
    if (!ReadAt(n.templateObj, PropOff(L"AudioComponent", L"Sound"), sound) || !sound)
        return nullptr;
    float volume = 1.f, pitch = 1.f;
    void* attenuation = nullptr;  // template-effective (mynet's zapp: att_small)
    ReadAt(n.templateObj, PropOff(L"AudioComponent", L"VolumeMultiplier"), volume);
    ReadAt(n.templateObj, PropOff(L"AudioComponent", L"PitchMultiplier"), pitch);
    ReadAt(n.templateObj, PropOff(L"AudioComponent", L"AttenuationSettings"), attenuation);
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
    f.Set<void*>(L"AttenuationSettings", attenuation);
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

// A point light has no gameplay-statics spawn helper: add the component deferred, copy the
// template's light fields (pre-registration raw writes, the archetype copy the engine's own
// construction-script instancing performs), finish, attach, then the post-registration
// setters for the render-state-coupled bits.
void* AddPointLightNode(void* actor, void* meshComp, const Node& n, const R::FName& bone) {
    if (!g_addCompFn || !g_finishCompFn || !g_attachFn || !g_pointLightClass) return nullptr;
    // Template-faithful visibility: the base skin's life light ships invisible (only the sentient
    // path turns it on), so a light the game keeps dark is not instanced at all. Fallback false:
    // when the flag cannot be proven visible (layout drift), a missing glow is the cheap failure;
    // a wrongly lit per-player light floods every screen.
    if (!TemplateFlag(n.templateObj, L"SceneComponent", L"bVisible", false))
        return nullptr;
    // Pass the template's relative transform to both halves of the deferred add: if the finish
    // call re-applies its transform parameter over the pre-finish field writes, it re-applies
    // the correct one.
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

    // Archetype copy of the plain light fields, template to instance.
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

    // Render-state-coupled bits go through their setters, post-registration. Cast shadows: the
    // kerfur template ships false, and a shadow-casting per-player point light would be a
    // performance cliff anyway.
    if (g_setCastShadowsFn) {
        ParamFrame cs(g_setCastShadowsFn);
        cs.Set<bool>(L"bNewValue", false);
        Call(comp, cs);
    }
    return comp;
}

// Post-spawn template fidelity shared by every component kind.
void ApplyTemplateFidelity(void* comp, const Node& n) {
    // Absolute transform axes: the mynet grid decals author absolute rotation (the projection box
    // stays world-vertical whichever limb bone the decal rides; a keep-relative attach alone
    // tumbles the box with the bone until it swallows the camera) and its electricity emitters
    // author absolute scale. SetAbsolute reinterprets the already-applied relative values in
    // world space, exactly how the engine's own construction-script instancing treats an
    // absolute-flagged template.
    const bool absLoc = TemplateFlag(n.templateObj, L"SceneComponent", L"bAbsoluteLocation", false);
    const bool absRot = TemplateFlag(n.templateObj, L"SceneComponent", L"bAbsoluteRotation", false);
    const bool absScale = TemplateFlag(n.templateObj, L"SceneComponent", L"bAbsoluteScale", false);
    if ((absLoc || absRot || absScale) && g_setAbsoluteFn) {
        ParamFrame f(g_setAbsoluteFn);
        f.Set<bool>(L"bNewAbsoluteLocation", absLoc);
        f.Set<bool>(L"bNewAbsoluteRotation", absRot);
        f.Set<bool>(L"bNewAbsoluteScale", absScale);
        Call(comp, f);
    }
    // Tick authoring: the mynet electricity emitters ship tick-off, so the native sim never
    // advances past its initial state. The gameplay-statics spawn registers the tick enabled;
    // disabling it synchronously, before this frame's tick groups run, restores the never-ticked
    // native state. Left running, the continuously emitting systems flood the screen.
    if (g_setTickEnabledFn && !TemplateStartsTickEnabled(n.templateObj)) {
        ParamFrame f(g_setTickEnabledFn);
        f.Set<bool>(L"bEnabled", false);
        Call(comp, f);
    }
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
        // Bone-anchored nodes ride the skin mesh; root-anchored ones (the mynet grid decals under
        // the actor) ride the actor root so they track the capsule, not a bone.
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
            ApplyTemplateFidelity(comp, n);
            outComponents.push_back(comp);
            ++made;
        }
    }
    return made;
}

}  // namespace ue_wrap::scs_rig
