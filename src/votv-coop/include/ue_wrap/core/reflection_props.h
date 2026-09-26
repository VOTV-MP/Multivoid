// ue_wrap/core/reflection_props.h -- the FProperty / FField walk family: a UFunction's parameter frame,
// an instance property's offset, a struct's members, a bool bitfield's byte and mask, each read from the
// live FProperty chain rather than hardcoded, so it holds across builds. Part of ue_wrap::reflection, whose
// header includes this one; defined in reflection_props.cpp. No gameplay or network logic.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::reflection {

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

// Size in bytes of parameter `paramName` (ElementSize * ArrayDim), or -1. The name matches
// case-insensitively, as FindParamOffset's does: an FName renders in whichever casing was
// registered first in the process.
int32_t FindParamSize(void* function, const wchar_t* paramName);

// Byte offset of an instance property named `propName` on `owningClass` (a UClass*, live, as for
// FindFunction). Walks the class's own ChildProperties chain, then climbs the SuperStruct chain on
// a miss. -1 if not found. Cache the result; the walk is linear.
int32_t FindPropertyOffset(void* owningClass, const wchar_t* propName);

// A property's offset on the class of the instance it is asked with, for a wrapper that holds an
// instance rather than a class by name: the instance's class is loaded by construction, so nothing
// waits on a class to load. Kept per class object, held by its slot and serial as a weak pointer
// is, and asked again when an instance of another class object comes (a subclass, a class a new
// world reloaded, or a new class at a dead one's address). -1 when that class does not declare it,
// said once per class object. Game thread.
class InstanceOffset {
public:
    explicit constexpr InstanceOffset(const wchar_t* propName) : prop_(propName) {}
    int32_t Of(void* obj);

private:
    const wchar_t* prop_;
    void* cls_ = nullptr;
    int32_t clsIdx_ = -1;
    int32_t clsSerial_ = 0;
    int32_t off_ = -1;
};

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

// The UEnum* behind a byte-typed enum property (FByteProperty::Enum), e.g. a blueprint struct's
// TEnumAsByte member: the object to ask for an enumerator's display name. It is the first payload
// member after the FProperty base, the same boundary as FStructProperty::Struct, so both slots are
// probed and the candidate is validated as a live object of an enum meta-class. Null if the
// property is not found, is a plain byte, or no slot validates. Cache the result.
void* PropertyEnum(void* owningStruct, const wchar_t* propName);

// The size in bytes of one instance of `structOrClass` (UStruct::PropertiesSize): what a whole-value
// copy of a struct property moves. 0 for null.
int32_t StructSize(void* structOrClass);

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

}  // namespace ue_wrap::reflection
