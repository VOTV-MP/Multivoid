// ue_wrap/reflection_props.cpp -- the FProperty / FField walk family:
// function params, struct fields, property/param offsets, the calibrated
// FStructProperty::Struct and FBoolProperty payload slots.
//
// EXTRACTED from reflection.cpp 2026-07-10 (877 LOC, past the 800 soft cap;
// this was the flagged extraction). Behavior preserved byte-for-byte; the
// family is self-contained (public reflection primitives + profile offsets;
// its calibration statics move with it, no shared private state).

#include "ue_wrap/reflection.h"

#include "ue_wrap/log.h"
#include "ue_wrap/sdk_profile.h"

#include <cwchar>
#include <string>
#include <vector>

namespace ue_wrap::reflection {

namespace P = profile;
namespace O = profile::off;

// FField (not UObject) carries its name at a different offset than UObject.
static const FName& FieldName(void* field) {
    return *reinterpret_cast<FName*>(reinterpret_cast<uint8_t*>(field) + O::FField_NamePrivate);
}

std::vector<ParamInfo> FunctionParams(void* function) {
    std::vector<ParamInfo> params;
    if (!function) return params;
    auto* field = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(function) +
                                               O::UStruct_ChildProperties);
    while (field) {
        const uint64_t flags = *reinterpret_cast<uint64_t*>(field + O::FProperty_PropertyFlags);
        if (flags & profile::cpf::Parm) {
            ParamInfo p;
            p.name = ToString(FieldName(field));
            p.offset = *reinterpret_cast<int32_t*>(field + O::FProperty_Offset_Internal);
            p.size = *reinterpret_cast<int32_t*>(field + O::FProperty_ElementSize) *
                     *reinterpret_cast<int32_t*>(field + O::FProperty_ArrayDim);
            p.flags = flags;
            params.push_back(std::move(p));
        }
        field = *reinterpret_cast<uint8_t**>(field + O::FField_Next);
    }
    return params;
}

int32_t FunctionFrameSize(void* function) {
    if (!function) return 0;
    return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(function) +
                                       O::UStruct_PropertiesSize);
}

std::vector<StructFieldInfo> EnumerateStructFields(void* structOrClass) {
    std::vector<StructFieldInfo> fields;
    if (!structOrClass) return fields;
    // Same FField chain FunctionParams walks, but every member (no CPF_Parm
    // filter) and no SuperStruct climb -- a UScriptStruct holds all its members
    // in its own ChildProperties.
    auto* field = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(structOrClass) +
                                               O::UStruct_ChildProperties);
    while (field) {
        StructFieldInfo f;
        f.name = ToString(FieldName(field));
        f.offset = *reinterpret_cast<int32_t*>(field + O::FProperty_Offset_Internal);
        f.size = *reinterpret_cast<int32_t*>(field + O::FProperty_ElementSize) *
                 *reinterpret_cast<int32_t*>(field + O::FProperty_ArrayDim);
        f.flags = *reinterpret_cast<uint64_t*>(field + O::FProperty_PropertyFlags);
        fields.push_back(std::move(f));
        field = *reinterpret_cast<uint8_t**>(field + O::FField_Next);
    }
    return fields;
}

int32_t FindPropertyOffset(void* owningClass, const wchar_t* propName) {
    if (!owningClass || !propName) return -1;
    // Walk OWN ChildProperties, then climb SuperStruct on miss. UE4 BP-
    // generated classes inline BP-added properties directly into the leaf
    // class's chain (BP compiler convention), so the original local-only
    // scan happened to work for every existing call site -- but the moment
    // a future accessor targets an inherited native UE4 UPROPERTY (e.g.
    // `APawn::bCanAffectNavigationGeneration` queried with `mainPlayer_C`
    // as the owner), the local-only scan would silently return -1.
    // Audit fix 2026-05-25 (commit ~09c003b): walk SuperStruct chain to
    // close the latent footgun. Bound the loop at 32 hops to cap pathological
    // cycles.
    void* cls = owningClass;
    for (int hops = 0; hops < 32 && cls; ++hops) {
        auto* field = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(cls) +
                                                   O::UStruct_ChildProperties);
        while (field) {
            // Case-insensitive (FName first-registration casing -- see NameEquals).
            if (::_wcsicmp(ToString(FieldName(field)).c_str(), propName) == 0) {
                return *reinterpret_cast<int32_t*>(field + O::FProperty_Offset_Internal);
            }
            field = *reinterpret_cast<uint8_t**>(field + O::FField_Next);
        }
        cls = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(cls) + O::UStruct_SuperStruct);
    }
    return -1;
}

int32_t FindParamOffset(void* function, const wchar_t* paramName) {
    if (!function || !paramName) return -1;
    for (const ParamInfo& p : FunctionParams(function)) {
        // Case-insensitive (FName first-registration casing -- see NameEquals).
        if (::_wcsicmp(p.name.c_str(), paramName) == 0) return p.offset;
    }
    return -1;
}

namespace {

// The FField* of an instance property by (case-insensitive) name -- the same
// SuperStruct-climbing walk as FindPropertyOffset, returning the field itself
// so callers can read property-subclass payloads (FStructProperty::Struct).
uint8_t* FindPropertyField(void* owningClass, const wchar_t* propName) {
    if (!owningClass || !propName) return nullptr;
    void* cls = owningClass;
    for (int hops = 0; hops < 32 && cls; ++hops) {
        auto* field = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(cls) +
                                                   O::UStruct_ChildProperties);
        while (field) {
            if (::_wcsicmp(ToString(FieldName(field)).c_str(), propName) == 0)
                return field;
            field = *reinterpret_cast<uint8_t**>(field + O::FField_Next);
        }
        cls = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(cls) + O::UStruct_SuperStruct);
    }
    return nullptr;
}

bool PlausibleHeapPtr(void* p) {
    const auto v = reinterpret_cast<uintptr_t>(p);
    return v > 0x10000 && v < 0x0000800000000000ull && (v & 7) == 0;
}

// True iff `p` is a live UObject whose class is a script-struct meta-class.
// Used to VALIDATE a candidate FStructProperty::Struct slot read: the wrong
// slot holds an FField* (PostConstructLinkNext), which is not in GUObjectArray
// and can never pass IsLive's slot back-pointer check.
bool LooksLikeScriptStruct(void* p) {
    if (!PlausibleHeapPtr(p) || !IsLive(p)) return false;
    void* cls = ClassOf(p);
    if (!cls) return false;
    const FName& cn = NameOf(cls);
    return NameEquals(cn, L"UserDefinedStruct") || NameEquals(cn, L"ScriptStruct");
}

// FStructProperty::Struct slot offset, calibrated once per process (build-
// dependent: 0x70 on stock UE4.27 FProperty, 0x78 on padded builds).
int32_t g_structPropSlot = -1;

}  // namespace

void* PropertyInnerStruct(void* owningClass, const wchar_t* propName) {
    uint8_t* field = FindPropertyField(owningClass, propName);
    if (!field) return nullptr;
    if (g_structPropSlot >= 0) {
        void* p = *reinterpret_cast<void**>(field + g_structPropSlot);
        return LooksLikeScriptStruct(p) ? p : nullptr;
    }
    for (int32_t cand : { 0x70, 0x78 }) {
        void* p = *reinterpret_cast<void**>(field + cand);
        if (LooksLikeScriptStruct(p)) {
            g_structPropSlot = cand;
            UE_LOGI("reflection: FStructProperty::Struct slot calibrated to +0x%X "
                    "(property '%ls' -> struct '%ls')",
                    cand, propName, ToString(NameOf(p)).c_str());
            return p;
        }
    }
    return nullptr;
}

namespace {

// FBoolProperty payload slot (= sizeof(FProperty), same boundary as
// FStructProperty::Struct above). -1 until calibrated.
int32_t g_boolPropSlot = -1;

// Payload shape check on {FieldSize, ByteOffset, ByteMask, FieldMask}:
// FieldSize must be 1 (uint8-backed UPROPERTY bool) and ByteMask a single set
// bit. The wrong candidate slot reads bytes out of a pointer, which
// practically never passes both.
bool BoolPayloadShapeOk(const uint8_t* payload) {
    return payload[0] == 1 && payload[1] <= 3 && payload[2] != 0 &&
           (payload[2] & (payload[2] - 1)) == 0;
}

// Calibrate g_boolPropSlot against an engine invariant: USceneComponent's CDO
// has bVisible SET (components are visible by default). A candidate slot must
// both look like a payload AND read that bit as 1 off the CDO.
void CalibrateBoolPropSlot() {
    if (g_boolPropSlot >= 0) return;
    void* cls = FindClass(L"SceneComponent");
    void* cdo = FindClassDefaultObject(L"SceneComponent");
    uint8_t* field = cls ? FindPropertyField(cls, L"bVisible") : nullptr;
    if (field && cdo) {
        const int32_t base = *reinterpret_cast<int32_t*>(field + O::FProperty_Offset_Internal);
        for (int32_t cand : { 0x70, 0x78 }) {
            const uint8_t* payload = field + cand;
            if (!BoolPayloadShapeOk(payload)) continue;
            const uint8_t byte = *(reinterpret_cast<const uint8_t*>(cdo) + base + payload[1]);
            if (byte & payload[2]) {
                g_boolPropSlot = cand;
                UE_LOGI("reflection: FBoolProperty payload slot calibrated to +0x%X "
                        "(SceneComponent CDO bVisible mask %02X)", cand, payload[2]);
                return;
            }
        }
    }
    UE_LOGW("reflection: FBoolProperty slot calibration failed (SceneComponent "
            "bVisible anchor unavailable) -- bitfield reads fall back to shape-only");
}

}  // namespace

bool FindBoolProperty(void* owningStruct, const wchar_t* propName,
                      int32_t& outByteOffset, uint8_t& outMask) {
    uint8_t* field = FindPropertyField(owningStruct, propName);
    if (!field) return false;
    const int32_t base = *reinterpret_cast<int32_t*>(field + O::FProperty_Offset_Internal);
    CalibrateBoolPropSlot();
    if (g_boolPropSlot >= 0) {
        const uint8_t* payload = field + g_boolPropSlot;
        if (!BoolPayloadShapeOk(payload)) return false;  // not a bool property
        outByteOffset = base + payload[1];
        outMask = payload[2];
        return true;
    }
    // Calibration anchor unavailable: accept the first shape-valid candidate.
    for (int32_t cand : { 0x70, 0x78 }) {
        const uint8_t* payload = field + cand;
        if (BoolPayloadShapeOk(payload)) {
            outByteOffset = base + payload[1];
            outMask = payload[2];
            return true;
        }
    }
    return false;
}

int32_t FindPropertyOffsetByPrefix(void* owningStruct, const wchar_t* prefix) {
    if (!owningStruct || !prefix) return -1;
    // BP UserDefinedStruct members carry GUID-mangled names
    // ("decoded_5_A9CAC26F..."): the human prefix is stable across recooks,
    // the GUID suffix is not -- so struct-member access resolves by prefix.
    // Same walk as FindPropertyOffset (UScriptStruct and UClass share the
    // UStruct ChildProperties layout); NameStartsWith is the zero-alloc
    // case-insensitive compare (FName first-registration casing).
    void* s = owningStruct;
    for (int hops = 0; hops < 32 && s; ++hops) {
        auto* field = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(s) +
                                                   O::UStruct_ChildProperties);
        while (field) {
            if (NameStartsWith(FieldName(field), prefix)) {
                return *reinterpret_cast<int32_t*>(field + O::FProperty_Offset_Internal);
            }
            field = *reinterpret_cast<uint8_t**>(field + O::FField_Next);
        }
        s = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(s) + O::UStruct_SuperStruct);
    }
    return -1;
}


}  // namespace ue_wrap::reflection
