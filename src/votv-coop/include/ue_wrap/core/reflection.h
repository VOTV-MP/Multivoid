// ue_wrap/core/reflection.h -- standalone UE4.27 reflection access. The engine globals and
// functions are resolved by AOB signature (no UE4SS import), then exposed as typed accessors over
// GUObjectArray and FName. No gameplay or network logic. Signatures and offsets are for the
// targeted game build and are re-derived when the mod is brought up against a new one.

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

// UE4.27 FString, a TArray<TCHAR>: heap wide string plus count and capacity.
struct FString {
    wchar_t* Data;
    int32_t Num;
    int32_t Max;
};

// AOB-resolve GUObjectArray and FName::ToString in the main module. Idempotent; true once both
// are found.
bool Resolve();
bool IsResolved();

// The resolved ProcessEvent, for the detour to trampoline through (0 until Resolve succeeds).
uintptr_t ProcessEventAddr();

// Call a UFunction on `object` through UObject::ProcessEvent. `params` points to the function's
// parameter struct (inputs in, outputs and the return written back), nullptr for a
// parameterless function. False if ProcessEvent is unresolved. Game thread only.
bool CallFunction(void* object, void* function, void* params);

// True while the current thread is inside a CallFunction dispatch issued by our own code (a
// thread-local depth latch around the one ProcessEvent choke point), so an interceptor can tell
// mod-originated calls from the game's own; the context object cannot, since our re-arms target
// game objects.
bool InCoopDispatch();

// The call-attribution census (a dev instrument): when armed, CallFunction tallies its target
// UFunction so the dispatches our code authors per frame can be attributed to the polls that
// produce them. Off by default (one relaxed bool load).
void SetCoopCallCensus(bool on);
// Slot i's target UFunction and its cumulative count; false past the last populated slot.
bool CoopCallSiteAt(int i, void** outFn, unsigned long long* outCount);

// GUObjectArray.ObjObjects.NumElements, the count of allocated UObject slots.
int32_t NumObjects();

// UObjectBase* at an object index, or nullptr (slot empty or out of range).
void* ObjectAt(int32_t index);

// True if `obj` is still a live UObject: its GUObjectArray slot points back to it and it is not
// PendingKill or Unreachable. O(1). It reads obj->InternalIndex from the object's own memory
// first, so it is safe only on a pointer that is still mapped; a pointer the GC has already
// purged faults inside this call. For any raw UObject* cached past the current game-thread
// slice, use IsLiveByIndex with an index captured by InternalIndexOf while the object was known
// live.
bool IsLive(void* obj);

// The purge-safe liveness check. `internalIdx` must have been captured while `obj` was known
// live. It validates only through the GUObjectArray slot at that index, never the object's own
// memory, so a freed or recycled slot no longer equals `obj` and returns false without faulting.
bool IsLiveByIndex(void* obj, int32_t internalIdx);

// The FUObjectItem.SerialNumber at a slot: an array read only, safe on any thread and after a
// purge. The engine assigns serials on demand (0 until a weak reference asks for one) and resets
// the slot's serial to 0 when the object finishes destroying, so a captured nonzero serial that
// no longer matches means the slot was recycled; 0 for an out-of-range index.
int32_t SlotSerial(int32_t internalIdx);

// The slot's serial, allocated if it has none: the engine's own weak-pointer rule, a fresh number
// from the array's master counter compare-and-swapped into the slot. Nonzero for any mapped slot,
// so a reference that captures it can never be fooled by a same-address successor in the same
// slot. 0 for an out-of-range index. Any thread.
int32_t AllocateSlotSerial(int32_t internalIdx);

// A UObject's InternalIndex, its stable slot in GUObjectArray. Dereferences `obj`, so only for
// an object known live; -1 for null. Cache it for a later IsLiveByIndex.
int32_t InternalIndexOf(void* obj);

// Resolve a TWeakObjectPtr {ObjectIndex, ObjectSerialNumber} to a live UObject*, or null. A slot
// read only: a stale pointer whose slot was recycled resolves to null, not to the new tenant
// (the serial compare does that). A PendingKill or Unreachable object is not live here either.
void* ResolveWeakObject(int32_t internalIdx, int32_t serial);

// The raw EInternalObjectFlags word at a UObject's slot, the field IsLiveByIndex tests, exposed
// whole so a diagnosis can tell PendingKill (marked, GC not yet there) from Unreachable
// (mid-purge). Bits (UE4.27): ReachableInCluster 1<<23, ClusterRoot 1<<24, Native 1<<25, Async
// 1<<26, AsyncLoading 1<<27, Unreachable 1<<28, PendingKill 1<<29, RootSet 1<<30. Reads obj's
// InternalIndex, so `obj` must be mapped. 0 for null or out of range, indistinguishable from
// "no flags": a diagnostic accessor, not a gate.
int32_t InternalFlagsOf(void* obj);

// The root-set primitives. A subsystem holds a `ue_wrap::GcPin` (ue_wrap/core/gc_pin.h) rather
// than calling these as a pair: the pin releases from its destructor, where a hand-written
// release conditioned on a liveness test once skipped exactly the teardown that needed it and
// leaked 871 rooted actors and a whole UWorld. `.github/ci/gc_pin_gate.ps1` polices direct
// callers.
// Mark a UObject as part of the root set so the GC never collects it. A C++ static void* is
// not a reachable reference for the GC scan, so an unrooted runtime-constructed object is
// reaped on the next pass and the cached pointer dangles. Sets EInternalObjectFlags::RootSet on
// the object's FUObjectItem flags word. False if obj is null or its index is out of range.
bool AddToRoot(void* obj);

// Clear the RootSet flag, making `obj` GC-eligible again; a destroyed-but-still-rooted object
// leaks its slot forever. A pure slot-flag clear, no dispatch and no game-thread requirement,
// but it reads the object's InternalIndex and walks GUObjectArray, so it is not safe at process
// teardown; `GcPin` owns that concern (its Release stands down once `StopReleases` has run) and
// is the only caller. False if obj is null, its slot was recycled, or the index is out of range.
bool RemoveFromRoot(void* obj);

// UObjectBase accessors, reading the standard UE4.27 fields by the offsets sdk_profile.h names.
const FName& NameOf(void* uobject);   // NamePrivate
void*        ClassOf(void* uobject);  // ClassPrivate
void*        OuterOf(void* uobject);  // OuterPrivate

// Lookups over GUObjectArray: linear walks, for one-time setup. All match on the object's
// NamePrivate (the leaf name, not a path).

// First object whose name is `name`; with a non-null `className`, its class name must match
// too.
void* FindObject(const wchar_t* name, const wchar_t* className = nullptr);

// A UClass by name (its meta-class is Class, BlueprintGeneratedClass or similar), e.g.
// FindClass(L"mainPlayer_C").
void* FindClass(const wchar_t* className);

// A UFunction named `funcName` owned (Outer) by `owningClass`. Walks the class's Outer-children
// only; does not climb to super classes.
void* FindFunction(void* owningClass, const wchar_t* funcName);

// First object whose class name is `className` in object-array order, skipping nulls and
// the class default object (names starting with "Default__"). There is NO liveness test and
// no world filter. After a menu-to-game cycle the departed world's instance sits at a lower
// index and wins, so a caller resolving a world-scoped actor this way can silently address
// the world it just left -- it once handed a joiner a save serialised from a gamemode whose
// world no longer existed, a structurally complete file describing nothing. A site reached
// only while a single world is live is fine; a site that can run across a world change
// should compare world_identity::WorldOf against CurrentWorld, a null stamp not being a
// rejection. Most of the world-scoped classes resolved this way -- the gamemode, the player,
// the day-night cycle, the fog, the menu widget, the black screen -- have never been checked
// against that question. The GameInstance is immortal by design.
void* FindObjectByClass(const wchar_t* className);

// The Class Default Object of a class given by name. Static BlueprintCallable UFunctions are
// dispatched on the CDO.
void* FindClassDefaultObject(const wchar_t* className);

// Count of live instances whose class name is `className`, skipping the CDO.
int32_t CountObjectsByClass(const wchar_t* className);

// All live instances whose class name is `className`, skipping the CDO. A linear walk: one-shot
// or low-rate use only, never per frame.
std::vector<void*> FindObjectsByClass(const wchar_t* className);

// Allocation-free name compares. ToString constructs a std::wstring per call plus the engine's
// FString render, and a GUObjectArray walk calling it per object is a quarter-million
// allocations per walk. These compare against the per-thread scratch buffer instead, with the
// same match semantics as `ToString(name) == expected`.
bool NameEquals(const FName& name, const wchar_t* expected);
bool NameStartsWith(const FName& name, const wchar_t* prefix);
// The substring form, bounded and allocation-free. Its consumer is world_identity's classifier
// ("ntitled" matches the live UWorld's "Untitled_1" whatever its case).
bool NameContains(const FName& name, const wchar_t* needle);

// A discovered object, from the child enumerator.
struct ObjectRef {
    std::wstring name;
    std::wstring className;
    void* object;
};

// All UObjects whose Outer is `outer` (an actor's default subobjects: its components live
// here). A linear walk, for one-time inspection.
std::vector<ObjectRef> ChildObjectsOf(void* outer);

// A debug probe for UStruct::SuperStruct's byte offset: scans the Actor class for the qword
// equal to the Object class pointer and logs the match. Pointer compares only.
void DebugProbeSuperStructOffset();

// One SuperStruct hop: the immediate parent UStruct of `cls`, nullptr at the chain root. The
// primitive for a caller that must climb the chain itself (resolving a UFunction on the
// declaring ancestor, since FindFunction is exact-owner); keeps the SuperStruct offset in this
// layer.
void* SuperStructOf(void* cls);

// True iff `cls` or any ancestor within `maxHops` matches any of `bases[0..nBases)`. All bases
// are checked per hop, so the chain is walked once. Gameplay code uses this rather than its own
// hop loop, so the SuperStruct offset stays in this layer.
bool IsDescendantOfAny(void* cls, void* const* bases, size_t nBases,
                       int maxHops = 16);

// UFunction parameter reflection. A ProcessEvent call needs a parameter frame with each argument
// at the byte offset the engine expects; the offsets are read from the live UFunction's
// FProperty chain rather than hardcoded, so they hold across builds.

// One parameter of a UFunction (a CPF_Parm FProperty), in declaration order.
struct ParamInfo {
    std::wstring name;
    int32_t offset;   // byte offset within the parameter frame (Offset_Internal)
    int32_t size;     // ElementSize * ArrayDim
    uint64_t flags;   // EPropertyFlags (test cpf::Parm / OutParm / ReturnParm)
};

// All CPF_Parm properties of `function` (a UFunction*) in declaration order, the return value
// (CPF_ReturnParm) included.
std::vector<ParamInfo> FunctionParams(void* function);

// Bytes to allocate for the parameter frame (UFunction::PropertiesSize, at least ParmsSize); 0
// for null.
int32_t FunctionFrameSize(void* function);

// Byte offset of parameter `paramName` in the frame, or -1.
int32_t FindParamOffset(void* function, const wchar_t* paramName);

// Byte offset of an instance property named `propName` on `owningClass` (a UClass*). Walks the
// class's own ChildProperties chain, then climbs the SuperStruct chain on a miss. -1 if not
// found. Cache the result; the walk is linear.
int32_t FindPropertyOffset(void* owningClass, const wchar_t* propName);

// The prefix-matched variant for GUID-mangled BP struct members: a UserDefinedStruct member
// renders as "decoded_5_A9CAC26F480C342A406FFFB77DD0AB68", where the human prefix is stable
// across recooks and the GUID suffix is not. Takes a UScriptStruct* (see PropertyInnerStruct) or
// a UClass*; the same climbing walk. Returns the first prefix match (include the trailing
// underscore, so "size_" cannot match "sizeFactor_..."); -1 if none. Cache the result.
int32_t FindPropertyOffsetByPrefix(void* owningStruct, const wchar_t* prefix);

// The inner UScriptStruct* of a struct-typed instance property (FStructProperty::Struct): the
// way to reach a BP struct's type object for member-offset resolution, immune to global-name
// collisions, load order and the struct asset's runtime name. The slot offset within
// FStructProperty is build-dependent (0x70 stock UE4.27, 0x78 padded), so the first call probes
// both, validates the candidate through GUObjectArray liveness and its meta-class name (the
// wrong slot holds an FField*, which never validates), and caches the slot process-wide. Null
// if the property is not found or no slot validates.
void* PropertyInnerStruct(void* owningClass, const wchar_t* propName);

// One instance member of a UStruct or UClass, in declaration order.
struct StructFieldInfo {
    std::wstring name;   // the FField name (BP members carry a "_NN_GUID" tail)
    int32_t offset;      // byte offset within an instance (Offset_Internal)
    int32_t size;        // ElementSize * ArrayDim
    uint64_t flags;      // EPropertyFlags
};

// The own instance members of `structOrClass` (a UScriptStruct*, e.g. from PropertyInnerStruct,
// or a UClass*) in declaration order. No SuperStruct climb: a BP UserDefinedStruct inlines all
// its members, and a caller wanting inherited fields asks by name. Empty for null or no
// members. Cache the result. Game thread only.
std::vector<StructFieldInfo> EnumerateStructFields(void* structOrClass);

// A bool UPROPERTY's real storage: the byte offset within the object and the bit mask inside
// that byte, from the FBoolProperty payload {FieldSize, ByteOffset, ByteMask, FieldMask} after
// the FProperty base. The way to read a `uint8 flag : 1` bitfield: several flags pack into one
// byte, so a raw byte read cannot attribute a value to one flag. The payload slot is
// build-dependent like FStructProperty::Struct; the first call calibrates it against the engine
// invariant "the SceneComponent CDO has bVisible set" and caches it process-wide. Works on
// UClass* and UScriptStruct* owners. False if the property or a valid payload is not found.
// Cache the result.
bool FindBoolProperty(void* owningStruct, const wchar_t* propName,
                      int32_t& outByteOffset, uint8_t& outMask);

// The object's class name as a string ("" if null).
std::wstring ClassNameOf(void* uobject);

// FName to wide string through the engine's FName::ToString.
std::wstring ToString(const FName& name);

// Free an engine-allocated buffer through FMalloc::Free (GMalloc): only for memory the engine
// allocated (an FString or FText buffer an engine UFunction wrote into our frame), never CRT or
// `new` memory. A no-op until Resolve has located GMalloc. Any thread; FMalloc::Free is
// internally synchronised.
void EngineFree(void* enginePtr);

// Allocate `size` bytes on the engine heap through FMalloc::Realloc(nullptr, size, align), the
// vtable slot the signature is matched from (Realloc(null, n) is Malloc(n)). `align` 0 means
// the default 16. Only for a buffer the engine must later own and free (a TArray the game's
// load, save or GC will read and release); pair with EngineFree otherwise. Null until GMalloc
// is resolved or on a zero size. Any thread.
void* EngineAlloc(size_t size, uint32_t align = 0);

// The boot health check: logs the game and engine version, resolves every primitive, then
// validates them functionally (a name round-trip, known-class lookups) and prints a verdict; on
// a new game build it names the failing signature or offset instead of crashing later. Returns
// the number of failed checks, 0 when the profile matches this build. The danger it catches is
// not a null pointer (game_thread::Install already refuses one) but an AOB that matched the
// wrong site: non-null, wrong, and visible only to the functional checks. Callers act on a
// non-zero result.
int RunHealthCheck();

}  // namespace ue_wrap::reflection
