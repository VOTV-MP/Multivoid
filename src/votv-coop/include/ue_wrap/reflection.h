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

// GUObjectArray.ObjObjects.NumElements (count of allocated UObject slots).
int32_t NumObjects();

// UObjectBase* at object index, or nullptr (slot empty / out of range).
void* ObjectAt(int32_t index);

// True if `obj` is still a live UObject (its GUObjectArray slot still points back
// to it). O(1): reads the object's InternalIndex and checks the slot. Use to guard
// a cached actor/object pointer before touching it -- a destroyed object's slot no
// longer matches, so this returns false instead of dereferencing freed memory.
bool IsLive(void* obj);

// Mark a UObject as part of the root set so UE4 GC never collects it. We pin
// runtime-constructed UObjects (NewObject / SpawnObject results) we want to
// keep referenced indefinitely from C++ -- C++ static void* doesn't qualify as
// a reachable reference for the GC reachability scan, so without rooting the
// object gets reaped on the next GC pass and our cached pointer dangles.
// Sets EInternalObjectFlags::RootSet (0x40000000) on FUObjectItem.Flags @+0x08.
// Returns false if obj is null or its index is out of range.
bool AddToRoot(void* obj);

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
// UClass*). Walks the class's own ChildProperties chain only (does NOT climb to
// SuperStruct). Returns -1 if not found. Used to locate fields like
// `UMovementComponent::Velocity` for direct memory access. Cache the result;
// the linear walk is fine one-shot but bad in a hot loop.
int32_t FindPropertyOffset(void* owningClass, const wchar_t* propName);

// Convenience: the object's class name as a string ("" if null).
std::wstring ClassNameOf(void* uobject);

// FName -> wide string via the engine's FName::ToString.
std::wstring ToString(const FName& name);

// Boot health check (logs to votv-coop.log via ue_wrap::log): detect+log the
// game/engine version, resolve every primitive, then FUNCTIONALLY validate them
// (name round-trip, known-class lookups) and print a PASS/FAIL verdict. On a new
// game build this is the fast path to "what broke" -- it pinpoints the failing
// signature/offset instead of crashing later.
void RunHealthCheck();

}  // namespace ue_wrap::reflection
