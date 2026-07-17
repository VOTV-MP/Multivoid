// ue_wrap/reflection.h -- standalone UE4.27 reflection access (no UE4SS).
//
// Engine-wrapper layer (principle 7). Resolves the engine globals/functions we
// need by AOB signature (RULE No.3), then exposes minimal typed accessors over
// GUObjectArray and FName. NO gameplay/network logic lives here.
//
// Signatures + offsets are for VOTV Alpha 0.9.0-n (UE4.27). They are re-derived
// when the mod is brought up against a new game version (version-tagging rule).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::reflection {

// UE4.27 FName (shipping, non-case-preserving): two int32s.
struct FName {
    int32_t ComparisonIndex;
    int32_t Number;
};

// UE4.27 FString == TArray<TCHAR>: heap wide string + count/capacity.
struct FString {
    wchar_t* Data;
    int32_t Num;
    int32_t Max;
};

// AOB-resolve GUObjectArray + FName::ToString in the main module. Idempotent;
// returns true once both are found.
bool Resolve();
bool IsResolved();

// Resolved addresses (0 until Resolve() succeeds), for diagnostics.
uintptr_t GUObjectArrayAddr();
uintptr_t FNameToStringAddr();
uintptr_t ProcessEventAddr();

// Call a UFunction on `object` via UObject::ProcessEvent -- the universal
// engine call path (drives BlueprintCallable/native UFunctions: SpawnActor
// helpers, K2_SetActorLocation, OpenLevel, ...). `params` points to the
// function's parameter struct (inputs in, outputs/return written back); pass
// nullptr for a no-parameter function. Returns false if ProcessEvent is
// unresolved. Must run on the game thread.
bool CallFunction(void* object, void* function, void* params);

// True while the CURRENT THREAD is inside a CallFunction dispatch issued by our own code (a
// thread-local depth latch around the one ProcessEvent choke point). Lets a UFunction
// interceptor discriminate mod-originated native calls from the game's own BP calls
// (rng_roll_census self-noise exclusion; the context object cannot discriminate -- our
// re-arms target game objects).
bool InCoopDispatch();

// GUObjectArray.ObjObjects.NumElements (count of allocated UObject slots).
int32_t NumObjects();

// UObjectBase* at object index, or nullptr (slot empty / out of range).
void* ObjectAt(int32_t index);

// True if `obj` is still a live UObject (its GUObjectArray slot still points
// back to it, and it is not PendingKill/Unreachable). O(1).
//
// WARNING: IsLive reads obj->InternalIndex from the object's OWN memory as its
// FIRST step, so it is only safe on a pointer that is at least still MAPPED. On
// a pointer the engine GC has already PURGED (backing store freed) that read
// itself access-violates -- IsLive cannot guard against its own argument being
// freed. For any raw UObject* cached LONGER than the current game-thread slice
// (the connect-edge prop snapshot held ~2000 actor pointers across ticks; a
// mass GC purge between ticks freed some without firing K2_DestroyActor), use
// IsLiveByIndex with an index captured via InternalIndexOf while the object was
// known live. (Root-caused 2026-05-30: this read was the connect-edge AV; see
// [[feedback-crash-firewall-requires-eha]] for the related freeze it triggered.)
bool IsLive(void* obj);

// GC-purge-safe liveness check. `internalIdx` must have been captured (via
// InternalIndexOf) while `obj` was known live. Validates ONLY through the
// GUObjectArray slot at that index -- it reads engine array metadata, never
// `obj`'s own (possibly-freed) memory -- so it is safe to call on a pointer the
// GC may have purged: a freed/recycled slot no longer equals `obj`, so it
// returns false WITHOUT faulting. FWeakObjectPtr-style guard for any raw
// UObject* cached past the current game-thread slice.
bool IsLiveByIndex(void* obj, int32_t internalIdx);

// Read a UObject's InternalIndex (its stable slot in GUObjectArray). MUST be
// called only when `obj` is known live (it dereferences `obj`). Returns -1 for
// null. Cache the result so the pointer can later be validated via
// IsLiveByIndex after the object may have been GC-purged.
int32_t InternalIndexOf(void* obj);

// Mark a UObject as part of the root set so UE4 GC never collects it. We pin
// runtime-constructed UObjects (NewObject / SpawnObject results) we want to
// keep referenced indefinitely from C++ -- C++ static void* doesn't qualify as
// a reachable reference for the GC reachability scan, so without rooting the
// object gets reaped on the next GC pass and our cached pointer dangles.
// Sets EInternalObjectFlags::RootSet (0x40000000) on FUObjectItem.Flags @+0x08.
// Returns false if obj is null or its index is out of range.
bool AddToRoot(void* obj);

// Clear the RootSet flag AddToRoot set, making `obj` GC-eligible again. Pairs
// with AddToRoot on EVERY teardown of a runtime-pinned UObject (e.g. the trash
// proxy mirror): a destroyed-but-still-rooted object leaks its GUObjectArray
// slot forever. Clears EInternalObjectFlags::RootSet (0x40000000) on
// FUObjectItem.Flags @+0x08. Returns false if obj is null / index out of range.
// Destructor-SAFE -- a pure slot-flag clear, no UFunction dispatch / game-thread
// / mutex requirement (unlike K2_DestroyActor), so it can anchor the owned-proxy
// Element teardown as the structural no-leak backstop.
bool RemoveFromRoot(void* obj);

// UObjectBase accessors (offsets are the standard UE4.27 layout).
const FName& NameOf(void* uobject);   // NamePrivate  @ +0x18
void*        ClassOf(void* uobject);  // ClassPrivate @ +0x10
void*        OuterOf(void* uobject);  // OuterPrivate @ +0x20

// Lookups over GUObjectArray (linear walk; intended for one-time setup).
// All match on the object's NamePrivate (the leaf name, not a path).

// First object whose name == `name`; if `className` is non-null, also require
// its class name to match. Returns UObjectBase* or nullptr.
void* FindObject(const wchar_t* name, const wchar_t* className = nullptr);

// A UClass by name (its meta-class is Class/BlueprintGeneratedClass/etc).
// e.g. FindClass(L"mainPlayer_C"), FindClass(L"World").
void* FindClass(const wchar_t* className);

// A UFunction named `funcName` owned (Outer) by `owningClass`. Walks the
// class's Outer-children; does NOT climb to super classes.
void* FindFunction(void* owningClass, const wchar_t* funcName);

// First live INSTANCE whose class name == `className` (skips the class's CDO,
// i.e. names starting with "Default__"). Use for runtime singletons that exist
// by the time you call -- e.g. the GameInstance (a valid world context) or a
// live World. Returns UObjectBase* or nullptr. (Mirrors UE4SS FindFirstOf.)
void* FindObjectByClass(const wchar_t* className);

// The Class Default Object for a class given by name (the "Default__<Class>"
// object). Static BlueprintCallable UFunctions are dispatched on the CDO.
void* FindClassDefaultObject(const wchar_t* className);

// Count live INSTANCES whose class name == `className` (skips the CDO). Used to
// detect spawn/clobber (e.g. mainPlayer_C count 1 -> 2 after an orphan spawn).
int32_t CountObjectsByClass(const wchar_t* className);

// All live INSTANCES whose class name == `className` (skips the CDO). Linear
// GUObjectArray walk -- one-shot / low-rate use only (NOT per-frame). Used by
// firefly_sync to diff the ParticleSystemComponent set across one firefly tick.
std::vector<void*> FindObjectsByClass(const wchar_t* className);

// --- allocation-free name compares (RAM-balloon root-cause fix, 2026-06-10) ---
// ToString() constructs a std::wstring PER CALL (plus the engine FString render);
// every GUObjectArray walk used it per OBJECT scanned (~250k allocations/walk) --
// the "wstring bomb" behind the 19 GB Install incident, the seated-quadbike 5 fps
// collapse, and the v56 menu-window client balloon (3.1->11 GB before the save
// Request). These compare against the per-thread scratch buffer instead: zero
// allocations, identical match semantics to `ToString(name) == expected`.
bool NameEquals(const FName& name, const wchar_t* expected);
bool NameStartsWith(const FName& name, const wchar_t* prefix);
// Substring form (the reaper's world-name checks: "ntitled"/"menu" match the
// gameplay/menu worlds prefix-case-agnostically). Bounded scan, zero alloc.
bool NameContains(const FName& name, const wchar_t* needle);

// A discovered object (used by the component/child enumerator).
struct ObjectRef {
    std::wstring name;
    std::wstring className;
    void* object;
};

// All UObjects whose Outer == `outer` (an actor's default subobjects: its
// components live here). Linear walk; one-time inspection use.
std::vector<ObjectRef> ChildObjectsOf(void* outer);

// DEBUG: probe UStruct::SuperStruct's byte offset by scanning the Actor class
// for the qword that equals the Object class pointer (Actor's super). Logs the
// match. Pointer compares only -- safe even if our guess is wrong.
void DebugProbeSuperStructOffset();

// One SuperStruct hop: the immediate parent UStruct of `cls` (nullptr at the
// chain root / null input). THE primitive for a caller that must climb the
// chain itself (e.g. resolving a UFunction on the DECLARING ancestor --
// FindFunction is exact-owner). Keeps the UStruct_SuperStruct offset in the
// wrapper layer (Principle 7).
void* SuperStructOf(void* cls);

// Walk `cls`'s SuperStruct chain checking each hop against the array
// `bases[0..nBases)`. Returns true iff `cls` or any ancestor up to
// `maxHops` levels matches any base. Cache-friendly inner loop (all
// candidate bases checked per hop -- avoids walking the chain once
// per base). Use this from gameplay code instead of hand-writing a
// SuperStruct hop loop so the UStruct_SuperStruct offset stays in the
// wrapper layer (Principle 7).
bool IsDescendantOfAny(void* cls, void* const* bases, size_t nBases,
                       int maxHops = 16);

// ---- UFunction parameter reflection --------------------------------------
// To call a UFunction via ProcessEvent we must hand it a parameter frame with
// each argument at the exact byte offset the engine expects. Rather than
// hardcode those offsets (fragile across builds), we read them from the live
// UFunction's FProperty chain -- correct-by-construction and version-portable.

// One parameter of a UFunction (a CPF_Parm FProperty), in declaration order.
struct ParamInfo {
    std::wstring name;
    int32_t offset;   // byte offset within the parameter frame (Offset_Internal)
    int32_t size;     // ElementSize * ArrayDim
    uint64_t flags;   // EPropertyFlags (test cpf::Parm / OutParm / ReturnParm)
};

// All CPF_Parm properties of `function` (a UFunction*), in declaration order
// (includes the return value, which carries CPF_ReturnParm).
std::vector<ParamInfo> FunctionParams(void* function);

// Size in bytes to allocate for the parameter frame (UFunction::PropertiesSize,
// >= ParmsSize). 0 if `function` is null.
int32_t FunctionFrameSize(void* function);

// Byte offset of parameter `paramName` in the frame, or -1 if not found.
int32_t FindParamOffset(void* function, const wchar_t* paramName);

// Byte offset of an INSTANCE property named `propName` on `owningClass` (a
// UClass*). Walks the class's own ChildProperties chain, then CLIMBS the
// SuperStruct chain on miss (audit fix 2026-05-25; this comment previously
// claimed local-only -- stale, corrected 2026-07-03 when scs_rig relied on
// the climb for ULocalLightComponent fields queried via PointLightComponent).
// Returns -1 if not found. Used to locate fields like
// `UMovementComponent::Velocity` for direct memory access. Cache the result;
// the linear walk is fine one-shot but bad in a hot loop.
int32_t FindPropertyOffset(void* owningClass, const wchar_t* propName);

// PREFIX-matched variant for GUID-mangled BP struct members: a UserDefinedStruct
// member renders as "decoded_5_A9CAC26F480C342A406FFFB77DD0AB68" -- the human
// prefix ("decoded_") is stable across recooks, the GUID suffix is not. Pass a
// UScriptStruct* (see PropertyInnerStruct) or a UClass*; same SuperStruct-
// climbing walk as FindPropertyOffset. Returns the FIRST prefix match (include
// the trailing underscore in the prefix so "size_" can't match
// "sizeFactor_..."). -1 if not found. Cache the result.
int32_t FindPropertyOffsetByPrefix(void* owningStruct, const wchar_t* prefix);

// The inner UScriptStruct* of a struct-typed instance property
// (FStructProperty::Struct) -- THE way to reach a BP struct's type object for
// member-offset resolution: deterministic via the owning class's own property
// chain, immune to global-name collisions, load order, and the struct asset's
// runtime object name. The slot offset within FStructProperty is build-
// dependent (0x70 stock UE4.27 / 0x78 padded), so the first call probes both
// and VALIDATES the candidate through GUObjectArray liveness + its meta-class
// name (the wrong slot holds an FField*, which can never validate), then
// caches the calibrated slot process-wide. Null if the property isn't found
// or no slot validates.
void* PropertyInnerStruct(void* owningClass, const wchar_t* propName);

// One instance member of a UStruct/UClass, in declaration order.
struct StructFieldInfo {
    std::wstring name;   // the FField name (BP members carry a "_NN_GUID" tail)
    int32_t offset;      // byte offset within an instance (Offset_Internal)
    int32_t size;        // ElementSize * ArrayDim
    uint64_t flags;      // EPropertyFlags
};

// Enumerate the OWN instance members of `structOrClass` (a UScriptStruct* -- e.g.
// via PropertyInnerStruct -- or a UClass*), in declaration order. Walks only the
// object's OWN ChildProperties chain (no SuperStruct climb: a BP UserDefinedStruct
// inlines all its members, and callers that want inherited fields ask by name).
// Empty if `structOrClass` is null or has no members. The linear walk is one-shot
// friendly; cache the result rather than calling per frame. Game-thread only.
std::vector<StructFieldInfo> EnumerateStructFields(void* structOrClass);

// A bool UPROPERTY's REAL storage: byte offset within the object plus the bit
// mask inside that byte, straight from the FBoolProperty payload {FieldSize,
// ByteOffset, ByteMask, FieldMask} that sits right after the FProperty base.
// This is THE way to read `uint8 flag : 1` bitfields (bVisible, bAutoActivate,
// bAbsoluteRotation, ...): several flags pack into one byte, so a raw byte
// read cannot attribute a value to a specific flag (the 2026-07-03 lifeLight
// XOR-heuristic failure). The payload slot is build-dependent like
// FStructProperty::Struct; the first call calibrates it against the engine
// invariant `SceneComponent CDO has bVisible set` and caches process-wide.
// Works on UClass* and UScriptStruct* owners (same UStruct field walk; struct
// members like FTickFunction::bStartWithTickEnabled resolve via
// PropertyInnerStruct first). Returns false if the property or a valid payload
// isn't found. Cache the result; the walk is linear.
bool FindBoolProperty(void* owningStruct, const wchar_t* propName,
                      int32_t& outByteOffset, uint8_t& outMask);

// Convenience: the object's class name as a string ("" if null).
std::wstring ClassNameOf(void* uobject);

// FName -> wide string via the engine's FName::ToString.
std::wstring ToString(const FName& name);

// Free an engine-allocated buffer via FMalloc::Free (GMalloc). Use ONLY on memory
// the ENGINE allocated (e.g. an FString/FText buffer an engine UFunction wrote into
// our frame, or the orphaned FName::ToString scratch) -- NEVER on CRT/`new` memory.
// No-op until Resolve() has located GMalloc (AOB via FMemory::Realloc). Closes the
// gap that previously forced "deliberate" engine-buffer leaks. Game-thread or any
// thread (FMalloc::Free is internally synchronized).
void EngineFree(void* enginePtr);

// Allocate `size` bytes on the ENGINE heap (GMalloc) via FMalloc::Realloc(nullptr,size,align)
// -- Realloc(null,n) == Malloc(n), and Realloc is the exact vtable slot kSigFMemoryRealloc is
// matched from (verified-in-use), so this avoids relying on the never-exercised Malloc slot.
// `align`=0 means DEFAULT_ALIGNMENT (16, enough for FTransform-bearing structs). Use ONLY when
// the engine must later own/free the buffer (e.g. populating a saveSlot TArray<Fstruct_save>
// the game's load/save/GC will read then FMemory::Free) -- pairing with EngineFree keeps the
// allocator matched. Returns nullptr until GMalloc is resolved or on a zero size. Any thread.
void* EngineAlloc(size_t size, uint32_t align = 0);

// Boot health check (logs to votv-coop.log via ue_wrap::log): detect+log the
// game/engine version, resolve every primitive, then FUNCTIONALLY validate them
// (name round-trip, known-class lookups) and print a PASS/FAIL verdict. On a new
// game build this is the fast path to "what broke" -- it pinpoints the failing
// signature/offset instead of crashing later.
void RunHealthCheck();

}  // namespace ue_wrap::reflection
