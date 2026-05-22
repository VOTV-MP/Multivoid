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

// GUObjectArray.ObjObjects.NumElements (count of allocated UObject slots).
int32_t NumObjects();

// UObjectBase* at object index, or nullptr (slot empty / out of range).
void* ObjectAt(int32_t index);

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

// A UFunction named `funcName` owned (Outer) by `owningClass`.
void* FindFunction(void* owningClass, const wchar_t* funcName);

// Convenience: the object's class name as a string ("" if null).
std::wstring ClassNameOf(void* uobject);

// FName -> wide string via the engine's FName::ToString.
std::wstring ToString(const FName& name);

// One-shot self-validation: resolve, wait for the engine to populate objects,
// then dump the FUObjectArray header + a sample of object/class names to
// `outPath`. Proves standalone reflection reads the live object graph.
void RunSelfTest(const wchar_t* outPath);

}  // namespace ue_wrap::reflection
