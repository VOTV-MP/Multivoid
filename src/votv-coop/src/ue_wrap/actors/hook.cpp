// ue_wrap/actors/hook.cpp -- see the header for the three measured facts this file is shaped by.

#include "ue_wrap/actors/hook.h"

#include "ue_wrap/actors/prop.h"        // GetStaticMesh: the O(1) answer to a component name
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"    // StringToFName: the game's component lookup takes a name
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::hook {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace P = ue_wrap::profile;
namespace SR = ue_wrap::save_record;

// The allowlist, indexed by Kind. `hook_Child_C` is absent on purpose -- the header says why.
constexpr const wchar_t* kClassNames[static_cast<size_t>(Kind::Count)] = {
    L"hook_C",
    L"hook_flesh_C",
    L"rope_C",
};

void* g_classes[static_cast<size_t>(Kind::Count)] = {};

// A bool UPROPERTY's storage is a byte offset plus a bit mask: several flags pack into one byte, so
// a raw byte read cannot attribute a value to one flag.
struct BoolField {
    int32_t off  = -1;
    uint8_t mask = 0;
    bool resolved() const { return off >= 0 && mask != 0; }
};

BoolField g_attachedA, g_attachedB, g_isThrown, g_playerHooked, g_skipSave;
int32_t   g_offDist     = -1;  // float  hook_C::dist
int32_t   g_offCompA    = -1;  // UArrowComponent* hook_C::A -- the HEAD
int32_t   g_offCompB    = -1;  // UArrowComponent* hook_C::B -- the tail, which rides the thrower
int32_t   g_offActiveHk = -1;  // Ahook_C* mainPlayer_C::activeHook
int32_t   g_offMaxDist  = -1;  // float  hook_C::maxDist -- the arbiter's clamp on a wire dist
int32_t   g_offActorA   = -1;  // AActor* hook_C::actor_a -- what the head bit
int32_t   g_offActorB   = -1;  // AActor* hook_C::actor_b -- the tail's anchor
int32_t   g_offCompAField = -1;  // UPrimitiveComponent* hook_C::component_A -- the bitten component
int32_t   g_offAttachLocA = -1;  // FVector hook_C::attachLoc_A -- the head in component_A's frame
int32_t   g_offPhys       = -1;  // USphereComponent* hook_C::phys -- the flight sphere; set by throw only
int32_t   g_offConstraint = -1;  // UPhysicsConstraintComponent* hook_C::PhysicsConstraint -- the A-B tie

void* g_fnReceiveTick = nullptr;
void* g_fnGetData     = nullptr;
void* g_fnLoadData    = nullptr;
void* g_fnProcessKeys = nullptr;
void* g_fnSetLength   = nullptr;
void* g_fnAttachA     = nullptr;  // hook_C::attach_a, the programmatic attach
void* g_fnSetCompWorldLocRot = nullptr;  // USceneComponent::K2_SetWorldLocationAndRotation
void* g_fnAttachComp         = nullptr;  // USceneComponent::K2_AttachToComponent
void* g_fnDetachComp         = nullptr;  // USceneComponent::K2_DetachFromComponent
void* g_fnGetRootComp        = nullptr;  // AActor::K2_GetRootComponent
void* g_fnBreakConstraint    = nullptr;  // UPhysicsConstraintComponent::BreakConstraint
void* g_fnSetConstrained     = nullptr;  // UPhysicsConstraintComponent::SetConstrainedComponents

// The game's own component-by-name lookup, a static on its C++ library class. Optional: without it
// ComponentByName still answers for a prop's mesh, which is what a hook bites in practice, and
// says once what it could not resolve.
void*        g_cdoCodeLib      = nullptr;
void*        g_fnCompByName    = nullptr;
std::wstring g_compByNameActor;   // the first parameter's name, read off the live UFunction
std::wstring g_compByNameName;    // the second's
bool         g_codeLibMissing  = false;   // the class is not in this build: never walk for it again
bool         g_saidNoCodeLib   = false;

bool     g_resolved   = false;
bool     g_latchedOff = false;
int      g_attempts   = 0;
uint64_t g_nextTryMs  = 0;

// Five passes with a class in hand is a version mismatch, not a load-order wait: the members are
// declared on the class we just found.
constexpr int kMaxPostClassAttempts = 5;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool ReadBool(const void* obj, const BoolField& f) {
    if (!obj || !f.resolved()) return false;
    const uint8_t b = *(reinterpret_cast<const uint8_t*>(obj) + f.off);
    return (b & f.mask) != 0;
}

bool WriteBool(void* obj, const BoolField& f, bool on) {
    if (!obj || !f.resolved()) return false;
    uint8_t* p = reinterpret_cast<uint8_t*>(obj) + f.off;
    *p = on ? static_cast<uint8_t>(*p | f.mask) : static_cast<uint8_t>(*p & ~f.mask);
    return true;
}

void* ReadObj(const void* obj, int32_t off) {
    if (!obj || off < 0) return nullptr;
    return *reinterpret_cast<void* const*>(reinterpret_cast<const uint8_t*>(obj) + off);
}

// Everything the wrapper needs, in one predicate, so the resolve log can name what is missing.
bool AllResolved() {
    return g_classes[0] && g_attachedA.resolved() && g_attachedB.resolved() &&
           g_isThrown.resolved() && g_playerHooked.resolved() && g_skipSave.resolved() &&
           g_offDist >= 0 && g_offCompA >= 0 && g_offCompB >= 0 && g_offActiveHk >= 0 &&
           g_fnReceiveTick && g_fnGetData && g_fnLoadData && g_fnProcessKeys && g_fnSetLength &&
           g_offMaxDist >= 0 && g_offActorA >= 0 && g_offActorB >= 0 && g_fnAttachA &&
           g_fnSetCompWorldLocRot && g_fnAttachComp && g_fnDetachComp && g_fnGetRootComp &&
           g_offCompAField >= 0 && g_offAttachLocA >= 0 && g_offPhys >= 0 &&
           g_offConstraint >= 0 && g_fnBreakConstraint;
}

// The library lookup, resolved beside the class members but never gating them. The two parameter
// names are read off the live UFunction rather than guessed: a native static's parameter names are
// the one thing no dump in the tree records.
void ResolveCodeLib() {
    if ((g_cdoCodeLib && g_fnCompByName) || g_codeLibMissing) return;
    // A class of the game's own C++ module is loaded with the module, before any world, so a miss
    // is a build without it, not a load-order wait -- and a FindClass miss walks the whole object
    // array, so it is asked exactly once.
    void* cls = R::FindClass(L"bpCodeLib");
    if (!cls) { g_codeLibMissing = true; return; }
    if (!g_cdoCodeLib)   g_cdoCodeLib   = R::FindClassDefaultObject(L"bpCodeLib");
    if (!g_fnCompByName) g_fnCompByName = R::FindFunction(cls, L"getComponentObjectByName");
    if (!g_cdoCodeLib || !g_fnCompByName) return;
    std::vector<std::wstring> in;
    for (const R::ParamInfo& pi : R::FunctionParams(g_fnCompByName))
        if (!(pi.flags & P::cpf::ReturnParm)) in.push_back(pi.name);
    if (in.size() != 2) {
        // Not the two-argument lookup this was written against: a build difference, asked once.
        g_fnCompByName   = nullptr;
        g_codeLibMissing = true;
        return;
    }
    g_compByNameActor = in[0];
    g_compByNameName  = in[1];
}

}  // namespace

bool EnsureResolved() {
    if (g_resolved) return true;
    if (g_latchedOff) return false;
    const uint64_t now = NowMs();
    if (now < g_nextTryMs) return false;
    g_nextTryMs = now + 1000;

    for (size_t i = 0; i < static_cast<size_t>(Kind::Count); ++i)
        if (!g_classes[i]) g_classes[i] = R::FindClass(kClassNames[i]);
    // Only hook_C gates: the two variants load with the assets that use them and a session may
    // legitimately never see one. A Kind whose class never resolves simply cannot be spawned, and
    // SpawnMirror says so once.
    void* cls = g_classes[static_cast<size_t>(Kind::Hook)];
    if (!cls) return false;  // world has not loaded the class yet

    if (!g_attachedA.resolved())    R::FindBoolProperty(cls, L"attached_a",   g_attachedA.off,    g_attachedA.mask);
    if (!g_attachedB.resolved())    R::FindBoolProperty(cls, L"attached_b",   g_attachedB.off,    g_attachedB.mask);
    if (!g_isThrown.resolved())     R::FindBoolProperty(cls, L"isThrown",     g_isThrown.off,     g_isThrown.mask);
    if (!g_playerHooked.resolved()) R::FindBoolProperty(cls, L"playerHooked", g_playerHooked.off, g_playerHooked.mask);
    // Declared on Aactor_save_C; FindBoolProperty climbs the SuperStruct chain.
    if (!g_skipSave.resolved())     R::FindBoolProperty(cls, L"skipSave",     g_skipSave.off,     g_skipSave.mask);
    if (g_offDist  < 0) g_offDist  = R::FindPropertyOffset(cls, L"dist");
    if (g_offCompA < 0) g_offCompA = R::FindPropertyOffset(cls, L"A");
    if (g_offCompB < 0) g_offCompB = R::FindPropertyOffset(cls, L"B");
    if (g_offMaxDist < 0) g_offMaxDist = R::FindPropertyOffset(cls, L"maxDist");
    if (g_offActorA < 0) g_offActorA = R::FindPropertyOffset(cls, L"actor_a");
    if (g_offActorB < 0) g_offActorB = R::FindPropertyOffset(cls, L"actor_b");
    if (g_offCompAField < 0) g_offCompAField = R::FindPropertyOffset(cls, L"component_A");
    if (g_offAttachLocA < 0) g_offAttachLocA = R::FindPropertyOffset(cls, L"attachLoc_A");
    if (g_offPhys       < 0) g_offPhys       = R::FindPropertyOffset(cls, L"phys");
    if (g_offConstraint < 0) g_offConstraint = R::FindPropertyOffset(cls, L"PhysicsConstraint");

    if (!g_fnReceiveTick) g_fnReceiveTick = R::FindFunction(cls, L"ReceiveTick");
    if (!g_fnGetData)     g_fnGetData     = R::FindFunction(cls, L"getData");
    if (!g_fnLoadData)    g_fnLoadData    = R::FindFunction(cls, L"loadData");
    if (!g_fnProcessKeys) g_fnProcessKeys = R::FindFunction(cls, L"processKeys");
    if (!g_fnSetLength)   g_fnSetLength   = R::FindFunction(cls, L"setLength");
    if (!g_fnAttachA)     g_fnAttachA     = R::FindFunction(cls, L"attach_a");

    if (void* mp = R::FindClass(L"mainPlayer_C")) {
        if (g_offActiveHk < 0) g_offActiveHk = R::FindPropertyOffset(mp, L"activeHook");
    }
    if (void* sc = R::FindClass(L"SceneComponent")) {
        if (!g_fnSetCompWorldLocRot)
            g_fnSetCompWorldLocRot = R::FindFunction(sc, L"K2_SetWorldLocationAndRotation");
        if (!g_fnAttachComp) g_fnAttachComp = R::FindFunction(sc, L"K2_AttachToComponent");
        if (!g_fnDetachComp) g_fnDetachComp = R::FindFunction(sc, L"K2_DetachFromComponent");
    }
    if (void* ac = R::FindClass(L"Actor")) {
        if (!g_fnGetRootComp) g_fnGetRootComp = R::FindFunction(ac, L"K2_GetRootComponent");
    }
    if (void* pc = R::FindClass(P::name::PhysicsConstraintComponentClass)) {
        if (!g_fnBreakConstraint) g_fnBreakConstraint = R::FindFunction(pc, P::name::BreakConstraintFn);
    }
    ResolveCodeLib();

    if (AllResolved()) {
        g_resolved = true;
        UE_LOGI("hook: resolved (hook_C=%p flesh=%p rope=%p dist@0x%X A@0x%X B@0x%X "
                "activeHook@0x%X tick=%p getData=%p loadData=%p processKeys=%p)",
                g_classes[0], g_classes[1], g_classes[2], g_offDist, g_offCompA, g_offCompB,
                g_offActiveHk, g_fnReceiveTick, g_fnGetData, g_fnLoadData, g_fnProcessKeys);
        return true;
    }
    // No offset fallbacks: an unresolved member means this is not the class the wrapper was written
    // against, and a guessed offset would write into whatever now lives there.
    if (++g_attempts >= kMaxPostClassAttempts) {
        g_latchedOff = true;
        UE_LOGW("hook: resolution INCOMPLETE after %d passes with hook_C in hand "
                "(attached_a=%d attached_b=%d isThrown=%d playerHooked=%d skipSave=%d dist=%d "
                "A=%d B=%d activeHook=%d tick=%d getData=%d loadData=%d processKeys=%d "
                "setLength=%d setWorldLocRot=%d attach=%d detach=%d root=%d actor_a=%d actor_b=%d "
                "attach_a=%d component_A=%d attachLoc_A=%d phys=%d PhysicsConstraint=%d "
                "BreakConstraint=%d) -- the hook lane stays OFF and writes nothing; game version "
                "mismatch?",
                g_attempts, g_attachedA.resolved(), g_attachedB.resolved(), g_isThrown.resolved(),
                g_playerHooked.resolved(), g_skipSave.resolved(), g_offDist >= 0, g_offCompA >= 0,
                g_offCompB >= 0, g_offActiveHk >= 0, g_fnReceiveTick != nullptr,
                g_fnGetData != nullptr, g_fnLoadData != nullptr, g_fnProcessKeys != nullptr,
                g_fnSetLength != nullptr, g_fnSetCompWorldLocRot != nullptr,
                g_fnAttachComp != nullptr, g_fnDetachComp != nullptr, g_fnGetRootComp != nullptr,
                g_offActorA >= 0, g_offActorB >= 0, g_fnAttachA != nullptr, g_offCompAField >= 0,
                g_offAttachLocA >= 0, g_offPhys >= 0, g_offConstraint >= 0,
                g_fnBreakConstraint != nullptr);
    }
    return false;
}

Kind KindOf(void* actor) {
    if (!actor || !g_resolved) return Kind::Count;
    // An EXACT class compare, never a descent test: hook_Child_C descends from hook_C, is placed by
    // the level and attaches itself on every peer, so a descent test would mirror and double it.
    void* cls = R::ClassOf(actor);
    for (size_t i = 0; i < static_cast<size_t>(Kind::Count); ++i)
        if (cls && cls == g_classes[i]) return static_cast<Kind>(i);
    return Kind::Count;
}

void* ClassForKind(Kind k) {
    const size_t i = static_cast<size_t>(k);
    if (i >= static_cast<size_t>(Kind::Count)) return nullptr;
    return g_classes[i];
}

const wchar_t* ClassNameOf(Kind k) {
    const size_t i = static_cast<size_t>(k);
    if (i >= static_cast<size_t>(Kind::Count)) return L"";
    return kClassNames[i];
}

void* ActiveHookOf(void* mainPlayer) {
    if (!mainPlayer || !g_resolved) return nullptr;
    return ReadObj(mainPlayer, g_offActiveHk);
}

bool ReadState(void* hookActor, State& out) {
    if (!hookActor || !g_resolved) return false;
    out.attachedA    = ReadBool(hookActor, g_attachedA);
    out.attachedB    = ReadBool(hookActor, g_attachedB);
    out.thrown       = ReadBool(hookActor, g_isThrown);
    out.playerHooked = ReadBool(hookActor, g_playerHooked);
    out.dist = *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(hookActor) + g_offDist);
    // The HEAD's pose comes from the `A` component and not from the actor: during flight `A` is
    // re-parented onto the thrown `phys` sphere while the actor root stays at the camera.
    void* compA = ReadObj(hookActor, g_offCompA);
    if (compA) {
        out.aLoc = E::GetComponentLocation(compA);
        out.aRot = E::GetComponentWorldRotation(compA);
    }
    return true;
}

bool WriteSkipSave(void* hookActor, bool on) {
    if (!g_resolved) return false;
    return WriteBool(hookActor, g_skipSave, on);
}

void* ReceiveTickFunction() { return g_resolved ? g_fnReceiveTick : nullptr; }

void* SpawnMirror(Kind kind, const FVector& aLoc, const FRotator& aRot) {
    if (!EnsureResolved()) return nullptr;
    void* cls = ClassForKind(kind);
    if (!cls) {
        static bool sSaid[static_cast<size_t>(Kind::Count)] = {};
        const size_t i = static_cast<size_t>(kind);
        if (i < static_cast<size_t>(Kind::Count) && !sSaid[i]) {
            sSaid[i] = true;
            UE_LOGW("hook: no mirror for kind %u -- its class has not loaded in this world", (unsigned)i);
        }
        return nullptr;
    }
    void* actor = E::BeginDeferredSpawn(cls, aLoc, aRot);
    if (!actor) return nullptr;

    // IN THE WINDOW, before the finish: the save mark. hook_C is an Aactor_save_C and the game's
    // save walk asks every one of them; a mirror standing on the HOST would otherwise be written
    // into the one save in the session, and come back on the next load as an unattached hook lying
    // where this one happened to be.
    if (!WriteSkipSave(actor, true)) {
        UE_LOGE("hook: skipSave unwritable on a mirror -- refusing to finish the spawn rather than "
                "leave an actor the host's save would swallow");
        E::DestroyActor(actor);
        return nullptr;
    }

    if (!E::FinishDeferredSpawn(actor, aLoc, aRot)) {
        E::DestroyActor(actor);
        return nullptr;
    }
    // The Blueprint body is cancelled at the ReceiveTick interceptor by the lane that owns the
    // mirror table; this is the cheap complement, so the engine does not even dispatch.
    E::SetActorTickEnabled(actor, false);
    E::SetActorEnableCollision(actor, false);
    return actor;
}

void* SpawnCanonical(Kind kind, const FVector& aLoc, const FRotator& aRot) {
    if (!EnsureResolved()) return nullptr;
    void* cls = ClassForKind(kind);
    if (!cls) return nullptr;
    void* actor = E::BeginDeferredSpawn(cls, aLoc, aRot);
    if (!actor) return nullptr;
    if (!E::FinishDeferredSpawn(actor, aLoc, aRot)) {
        E::DestroyActor(actor);
        return nullptr;
    }
    return actor;
}

bool DriveMirror(void* mirror, const FVector& aLoc, const FRotator& aRot, float dist) {
    if (!mirror || !g_resolved) return false;
    void* compA = ReadObj(mirror, g_offCompA);
    if (!compA) return false;
    ParamFrame f(g_fnSetCompWorldLocRot);
    if (!f.valid()) return false;
    f.Set<FVector>(L"NewLocation", aLoc);
    f.Set<FRotator>(L"NewRotation", aRot);
    f.Set<bool>(L"bSweep", false);
    f.Set<bool>(L"bTeleport", true);
    if (!Call(compA, f)) return false;
    return SetMirrorLength(mirror, dist);
}

bool SetMirrorLength(void* mirror, float dist) {
    if (!mirror || !g_resolved) return false;
    // The cable length is the game's own verb, not a field write: setLength clamps `dist`, writes
    // the three constraint limits AND `Cable.CableLength = dist / 1.5` in one call, which is the
    // ratio a hand-written mirror gets wrong.
    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(mirror) + g_offDist) = dist;
    ParamFrame sl(g_fnSetLength);
    if (!sl.valid()) return false;
    return Call(mirror, sl);
}

bool AttachTailTo(void* mirror, void* ownerRootActor) {
    if (!mirror || !g_resolved) return false;
    void* compB = ReadObj(mirror, g_offCompB);
    if (!compB) return false;

    if (!ownerRootActor) {
        ParamFrame d(g_fnDetachComp);
        if (!d.valid()) return false;
        d.Set<uint8_t>(L"LocationRule", uint8_t{1});  // EDetachmentRule::KeepWorld
        d.Set<uint8_t>(L"RotationRule", uint8_t{1});
        d.Set<uint8_t>(L"ScaleRule",    uint8_t{1});
        d.Set<bool>(L"bCallModify", true);
        return Call(compB, d);
    }

    ParamFrame rc(g_fnGetRootComp);
    if (!rc.valid() || !Call(ownerRootActor, rc)) return false;
    void* root = rc.Get<void*>(L"ReturnValue");
    if (!root) return false;

    ParamFrame f(g_fnAttachComp);
    if (!f.valid()) return false;
    R::FName none{};
    f.Set<void*>(L"Parent", root);
    f.SetRaw(L"SocketName", &none, sizeof(none));
    f.Set<uint8_t>(L"LocationRule", uint8_t{2});  // EAttachmentRule::SnapToTarget
    f.Set<uint8_t>(L"RotationRule", uint8_t{2});
    f.Set<uint8_t>(L"ScaleRule",    uint8_t{1});  // KeepWorld: do not inherit the puppet's scale
    f.Set<bool>(L"bWeldSimulatedBodies", false);
    return Call(compB, f);
}

bool CaptureRecord(void* hookActor, SR::SaveRecord& out) {
    if (!hookActor || !g_resolved) return false;
    return SR::CaptureRecordVia(hookActor, g_fnGetData, out);
}

bool AdoptRecord(void* hookActor, const SR::SaveRecord& r) {
    if (!hookActor || !g_resolved) return false;
    if (!SR::ApplyRecordVia(hookActor, g_fnLoadData, r)) return false;

    // processKeys is what turns the two saved attach KEYS into this world's two actors and
    // re-attaches; without it the record is a pose and nothing else.
    ParamFrame pk(g_fnProcessKeys);
    if (pk.valid()) Call(hookActor, pk);
    return true;
}

bool WriteAttachedFlags(void* hookActor, bool attachedA, bool attachedB) {
    if (!hookActor || !g_resolved) return false;
    return WriteBool(hookActor, g_attachedA, attachedA) &&
           WriteBool(hookActor, g_attachedB, attachedB);
}

bool AttachedActors(void* hookActor, void*& outA, void*& outB) {
    outA = outB = nullptr;
    if (!hookActor || !g_resolved) return false;
    outA = ReadObj(hookActor, g_offActorA);
    outB = ReadObj(hookActor, g_offActorB);
    return true;
}

bool WriteActiveHook(void* mainPlayer, void* hookActor) {
    if (!mainPlayer || !g_resolved || g_offActiveHk < 0) return false;
    *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(mainPlayer) + g_offActiveHk) = hookActor;
    return true;
}

bool AttachHead(void* hookActor, void* actor, void* component, const FVector& location,
                const FVector& normal, void* actorAttach, bool checkLen, bool unfreezeFrozen) {
    if (!hookActor || !g_resolved || !actor || !component) return false;
    ParamFrame f(g_fnAttachA);
    if (!f.valid()) return false;
    // `hit` stays zeroed: the verb takes the replace set when actorReplace is valid.
    f.Set<void*>(L"ActorAttach", actorAttach);
    f.Set<bool>(L"checkLen", checkLen);
    f.Set<void*>(L"actorReplace", actor);
    f.Set<void*>(L"componentReplace", component);
    f.Set<FVector>(L"locationReplace", location);
    f.Set<FVector>(L"normalReplace", normal);
    f.Set<bool>(L"unfreezeFrozen", unfreezeFrozen);
    return Call(hookActor, f);
}

bool ReadBite(void* hookActor, Bite& out) {
    out = Bite{};
    if (!hookActor || !g_resolved) return false;
    out.actor     = ReadObj(hookActor, g_offActorA);
    out.component = ReadObj(hookActor, g_offCompAField);
    out.localHead = *reinterpret_cast<const FVector*>(
        reinterpret_cast<const uint8_t*>(hookActor) + g_offAttachLocA);
    out.thrown    = ReadObj(hookActor, g_offPhys) != nullptr;
    return true;
}

bool IsHookFamily(void* actor) {
    if (!actor || !g_resolved) return false;
    void* base = g_classes[static_cast<size_t>(Kind::Hook)];
    return R::IsDescendantOfAny(R::ClassOf(actor), &base, 1);
}

void* PhysicsConstraintOf(void* hookActor) {
    if (!hookActor || !g_resolved) return nullptr;
    return ReadObj(hookActor, g_offConstraint);
}

bool BreakConstraint(void* hookActor) {
    void* comp = PhysicsConstraintOf(hookActor);
    if (!comp || !g_fnBreakConstraint) return false;
    ParamFrame f(g_fnBreakConstraint);
    if (!f.valid()) return false;
    return Call(comp, f);
}

void* SetConstrainedComponentsFunction() {
    // Independent of the hook classes on purpose: a seam on this native has to be in place
    // before the first hook of a loading world builds its tie, and the engine class is always
    // loaded. A UFunction outlives every world, so the lookup is a one-time resolve.
    if (!g_fnSetConstrained) {
        if (void* pc = R::FindClass(P::name::PhysicsConstraintComponentClass))
            g_fnSetConstrained = R::FindFunction(pc, P::name::SetConstrainedComponentsFn);
    }
    return g_fnSetConstrained;
}

void* ComponentByName(void* actor, const wchar_t* name) {
    if (!actor || !name || !*name) return nullptr;
    // The prop's mesh is what a hook bites in practice, and its name is one compare away.
    if (void* mesh = ue_wrap::prop::GetStaticMesh(actor)) {
        if (R::NameEquals(R::NameOf(mesh), name)) return mesh;
    }
    ResolveCodeLib();
    if (!g_cdoCodeLib || !g_fnCompByName) {
        if (!g_saidNoCodeLib) {
            g_saidNoCodeLib = true;
            UE_LOGW("hook: bpCodeLib::getComponentObjectByName did not resolve -- a component that "
                    "is not the prop's mesh cannot be found by name (said once)");
        }
        return nullptr;
    }
    const R::FName n = ue_wrap::fname_utils::StringToFName(name);
    if (n.ComparisonIndex == 0 && n.Number == 0) return nullptr;
    ParamFrame f(g_fnCompByName);
    if (!f.valid()) return nullptr;
    f.Set<void*>(g_compByNameActor.c_str(), actor);
    f.SetRaw(g_compByNameName.c_str(), &n, sizeof(n));
    if (!Call(g_cdoCodeLib, f)) return nullptr;
    return f.Get<void*>(L"ReturnValue");
}

float MaxDistOf(void* hookActor) {
    if (!hookActor || !g_resolved || g_offMaxDist < 0) return 0.f;
    return *reinterpret_cast<const float*>(
        reinterpret_cast<const uint8_t*>(hookActor) + g_offMaxDist);
}

void ResetCache() {
    for (auto& c : g_classes) c = nullptr;
    g_attachedA = g_attachedB = g_isThrown = g_playerHooked = g_skipSave = BoolField{};
    g_offDist = g_offCompA = g_offCompB = g_offActiveHk = g_offMaxDist = -1;
    g_offActorA = g_offActorB = -1;
    g_offCompAField = g_offAttachLocA = g_offPhys = g_offConstraint = -1;
    g_fnReceiveTick = g_fnGetData = g_fnLoadData = g_fnProcessKeys = g_fnSetLength = nullptr;
    g_fnAttachA = nullptr;
    g_fnSetCompWorldLocRot = g_fnAttachComp = g_fnDetachComp = g_fnGetRootComp = nullptr;
    g_fnBreakConstraint = nullptr;   // g_fnSetConstrained is an engine-class UFunction and stays
    g_cdoCodeLib = g_fnCompByName = nullptr;
    g_compByNameActor.clear();
    g_compByNameName.clear();
    g_codeLibMissing = false;
    g_resolved = false;
    g_latchedOff = false;
    g_attempts = 0;
    g_nextTryMs = 0;
}

}  // namespace ue_wrap::hook
