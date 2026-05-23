#include "ue_wrap/reflection.h"

#include "ue_wrap/log.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/sig_scan.h"

#include <windows.h>

#include <vector>

namespace ue_wrap::reflection {
namespace {

// All version-specific knowledge lives in the profile (the porting surface).
namespace P = profile;
namespace O = profile::off;

using FNameToStringFn = void(__fastcall*)(const FName*, FString*);
using ProcessEventFn = void(__fastcall*)(void* self, void* function, void* params);

uintptr_t g_objArray = 0;
FNameToStringFn g_fnameToString = nullptr;
ProcessEventFn g_processEvent = nullptr;

}  // namespace

uintptr_t GUObjectArrayAddr() { return g_objArray; }
uintptr_t FNameToStringAddr() { return reinterpret_cast<uintptr_t>(g_fnameToString); }
uintptr_t ProcessEventAddr() { return reinterpret_cast<uintptr_t>(g_processEvent); }
bool IsResolved() { return g_objArray && g_fnameToString; }

bool Resolve() {
    if (!g_fnameToString) {
        const uintptr_t hit = FindPattern(P::kSigFNameToString);
        if (hit) g_fnameToString = reinterpret_cast<FNameToStringFn>(hit);
    }
    if (!g_objArray) {
        const uintptr_t hit = FindPattern(P::kSigGUObjectArray);
        if (hit) {
            const int32_t disp = *reinterpret_cast<int32_t*>(hit + P::kGUObjArrayLeaDispOff);
            g_objArray = hit + P::kGUObjArrayLeaEndOff + disp;
        }
    }
    if (!g_processEvent) {
        const uintptr_t hit = FindPattern(P::kSigProcessEvent);
        if (hit) g_processEvent = reinterpret_cast<ProcessEventFn>(hit);
    }
    return IsResolved();
}

bool CallFunction(void* object, void* function, void* params) {
    if (!g_processEvent || !object || !function) return false;
    g_processEvent(object, function, params);
    return true;
}

int32_t NumObjects() {
    if (!g_objArray) return 0;
    return *reinterpret_cast<int32_t*>(g_objArray + O::FUObjectArray_ObjObjects + O::Chunk_NumElements);
}

namespace {
// Address of the FUObjectItem (Object*, Flags, Cluster, Serial) for a slot, or
// null if the index is out of range / the chunk is unallocated.
uint8_t* ItemAt(int32_t index) {
    if (!g_objArray || index < 0) return nullptr;
    const uintptr_t objObjects = g_objArray + O::FUObjectArray_ObjObjects;
    if (index >= *reinterpret_cast<int32_t*>(objObjects + O::Chunk_NumElements)) return nullptr;
    auto** chunks = *reinterpret_cast<uint8_t***>(objObjects + O::Chunk_Objects);
    if (!chunks) return nullptr;
    uint8_t* chunk = chunks[index / O::ElemsPerChunk];
    if (!chunk) return nullptr;
    return chunk + static_cast<size_t>(index % O::ElemsPerChunk) * O::FUObjectItem_Stride;
}
}  // namespace

void* ObjectAt(int32_t index) {
    uint8_t* item = ItemAt(index);
    return item ? *reinterpret_cast<void**>(item) : nullptr;  // FUObjectItem.Object @ +0x00
}

bool IsLive(void* obj) {
    if (!obj) return false;
    const int32_t idx = *reinterpret_cast<int32_t*>(
        reinterpret_cast<uint8_t*>(obj) + O::UObject_InternalIndex);
    uint8_t* item = ItemAt(idx);
    if (!item || *reinterpret_cast<void**>(item) != obj) return false;  // slot empty/recycled
    // The slot check alone is NOT enough across a level transition: an actor the
    // engine is tearing down is flagged PendingKill (then Unreachable by GC) yet
    // still occupies its array slot until the purge completes. Calling UFunctions
    // on it -- GetActorLocation/SetViewTargetWithBlend/etc. -- is a use-after-free
    // (the tutorial-map load crashed here: read 0xffff...). Reject those flags so a
    // dying object reports not-live. (FUObjectItem.Flags @ +0x08; bit values per
    // UE4.27 EInternalObjectFlags -- matches UE4SS's own PendingKill guard.)
    const int32_t flags = *reinterpret_cast<int32_t*>(item + O::FUObjectItem_Flags);
    constexpr int32_t kKillFlags = 0x10000000 /*Unreachable*/ | 0x20000000 /*PendingKill*/;
    return (flags & kKillFlags) == 0;
}

const FName& NameOf(void* uobject) {
    return *reinterpret_cast<FName*>(reinterpret_cast<uint8_t*>(uobject) + O::UObject_NamePrivate);
}

void* ClassOf(void* uobject) {
    return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(uobject) + O::UObject_ClassPrivate);
}

void* OuterOf(void* uobject) {
    return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(uobject) + O::UObject_OuterPrivate);
}

std::wstring ToString(const FName& name) {
    if (!g_fnameToString) return L"";
    // thread_local (not static): the scratch buffer is reused per thread, so two
    // threads calling ToString never race the same FString -- without paying a
    // per-call free of UE-allocated memory we can't release.
    thread_local FString scratch{nullptr, 0, 0};
    scratch.Num = 0;  // reuse the buffer; write from the start (one buffer per thread)
    g_fnameToString(&name, &scratch);
    if (!scratch.Data || scratch.Num <= 0) return L"";
    int len = scratch.Num;
    if (scratch.Data[len - 1] == L'\0') --len;  // Num counts the null terminator
    if (len <= 0) return L"";
    return std::wstring(scratch.Data, scratch.Data + len);
}

std::wstring ClassNameOf(void* uobject) {
    void* cls = uobject ? ClassOf(uobject) : nullptr;
    if (!cls) return L"";
    return ToString(NameOf(cls));
}

void* FindObject(const wchar_t* name, const wchar_t* className) {
    if (!name) return nullptr;
    const int32_t n = NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = ObjectAt(i);
        if (!obj) continue;
        if (ToString(NameOf(obj)) != name) continue;
        if (className && ClassNameOf(obj) != className) continue;
        return obj;
    }
    return nullptr;
}

void* FindClass(const wchar_t* className) {
    if (!className) return nullptr;
    const int32_t n = NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = ObjectAt(i);
        if (!obj) continue;
        if (ToString(NameOf(obj)) != className) continue;
        // Its meta-class identifies it as a class object. Every UClass-derived
        // meta-type ends in "Class" (Class, BlueprintGeneratedClass,
        // WidgetBlueprintGeneratedClass, AnimBlueprintGeneratedClass, DynamicClass,
        // LinkerPlaceholderClass, ...). Match the suffix so BP-generated classes
        // (UMG widgets, anim BPs) resolve too -- the exact-name match above already
        // excludes instances (which carry numeric suffixes).
        const std::wstring meta = ClassNameOf(obj);
        if (meta.size() >= 5 && meta.compare(meta.size() - 5, 5, L"Class") == 0) {
            return obj;
        }
    }
    return nullptr;
}

void* FindFunction(void* owningClass, const wchar_t* funcName) {
    if (!owningClass || !funcName) return nullptr;
    const int32_t n = NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = ObjectAt(i);
        if (!obj) continue;
        if (OuterOf(obj) != owningClass) continue;
        if (ClassNameOf(obj) != L"Function") continue;
        if (ToString(NameOf(obj)) == funcName) return obj;
    }
    return nullptr;
}

void* FindObjectByClass(const wchar_t* className) {
    if (!className) return nullptr;
    const int32_t n = NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = ObjectAt(i);
        if (!obj) continue;
        if (ClassNameOf(obj) != className) continue;
        // Skip the CDO (the archetype, named "Default__<Class>"); we want a
        // real instance.
        const std::wstring name = ToString(NameOf(obj));
        if (name.rfind(L"Default__", 0) == 0) continue;
        return obj;
    }
    return nullptr;
}

void* FindClassDefaultObject(const wchar_t* className) {
    if (!className) return nullptr;
    std::wstring defName = L"Default__";
    defName += className;
    return FindObject(defName.c_str());
}

std::vector<ObjectRef> ChildObjectsOf(void* outer) {
    std::vector<ObjectRef> out;
    if (!outer) return out;
    const int32_t n = NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = ObjectAt(i);
        if (!obj || OuterOf(obj) != outer) continue;
        out.push_back({ToString(NameOf(obj)), ClassNameOf(obj), obj});
    }
    return out;
}

void DebugProbeSuperStructOffset() {
    void* actorCls = FindClass(L"Actor");
    void* objectCls = FindClass(L"Object");
    void* pawnCls = FindClass(L"Pawn");
    UE_LOGI("superstruct probe: Actor=%p Object=%p Pawn=%p", actorCls, objectCls, pawnCls);
    if (!actorCls || !objectCls) return;
    auto* base = reinterpret_cast<uint8_t*>(actorCls);
    for (size_t off = 0x28; off <= 0x80; off += 8) {
        void* q = *reinterpret_cast<void**>(base + off);
        if (q == objectCls) {
            UE_LOGI("  Actor[0x%02zx] == Object class -> SuperStruct offset = 0x%02zx", off, off);
        } else if (q == pawnCls && pawnCls) {
            UE_LOGI("  Actor[0x%02zx] == Pawn class (unexpected)", off);
        }
    }
}

int32_t CountObjectsByClass(const wchar_t* className) {
    if (!className) return 0;
    int32_t count = 0;
    const int32_t n = NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = ObjectAt(i);
        if (!obj) continue;
        if (ClassNameOf(obj) != className) continue;
        if (ToString(NameOf(obj)).rfind(L"Default__", 0) == 0) continue;  // skip CDO
        ++count;
    }
    return count;
}

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

int32_t FindPropertyOffset(void* owningClass, const wchar_t* propName) {
    if (!owningClass || !propName) return -1;
    auto* field = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(owningClass) +
                                               O::UStruct_ChildProperties);
    while (field) {
        if (ToString(FieldName(field)) == propName) {
            return *reinterpret_cast<int32_t*>(field + O::FProperty_Offset_Internal);
        }
        field = *reinterpret_cast<uint8_t**>(field + O::FField_Next);
    }
    return -1;
}

int32_t FindParamOffset(void* function, const wchar_t* paramName) {
    if (!function || !paramName) return -1;
    for (const ParamInfo& p : FunctionParams(function)) {
        if (p.name == paramName) return p.offset;
    }
    return -1;
}

namespace {

// Log the running exe's version + size and warn if it differs from the build
// the profile was derived against -- the first thing to check on a new release.
void LogGameVersion() {
    wchar_t exe[MAX_PATH] = {};
    ::GetModuleFileNameW(::GetModuleHandleW(nullptr), exe, MAX_PATH);

    unsigned long long sizeBytes = 0;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (::GetFileAttributesExW(exe, GetFileExInfoStandard, &fad)) {
        sizeBytes = (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    }

    // Exe file version (catches engine-version changes that shift offsets).
    unsigned ms = 0, ls = 0;
    DWORD dummy = 0;
    const DWORD vsz = ::GetFileVersionInfoSizeW(exe, &dummy);
    if (vsz) {
        std::vector<uint8_t> buf(vsz);
        if (::GetFileVersionInfoW(exe, 0, vsz, buf.data())) {
            VS_FIXEDFILEINFO* ffi = nullptr;
            UINT len = 0;
            if (::VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&ffi), &len) && ffi) {
                ms = ffi->dwFileVersionMS;
                ls = ffi->dwFileVersionLS;
            }
        }
    }

    UE_LOGI("target build: game=%s engine=%s", P::kTargetGameVersion, P::kTargetEngineVersion);
    UE_LOGI("running exe : %ls", exe);
    UE_LOGI("exe fileversion=%u.%u.%u.%u  size=%llu bytes",
            ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF, sizeBytes);
    if constexpr (P::kExpectedExeSize != 0) {
        if (sizeBytes != P::kExpectedExeSize) {
            UE_LOGW("exe size differs from the build these signatures target (%llu) -- "
                    "AOBs/offsets are SUSPECT; re-derive sdk_profile.h for this build.",
                    P::kExpectedExeSize);
        }
    }
}

}  // namespace

void RunHealthCheck() {
    int fails = 0;
    auto check = [&](bool ok, const char* what) {
        if (ok) {
            UE_LOGI("  [ OK ] %s", what);
        } else {
            UE_LOGE("  [FAIL] %s", what);
            ++fails;
        }
    };

    UE_LOGI("---- SDK health check ----");
    LogGameVersion();

    // 1) AOB resolution. Log each address + RVA, so a bad sig is obvious.
    Resolve();
    uintptr_t base = 0;
    size_t imgSize = 0;
    MainModuleRange(base, imgSize);
    auto rva = [&](uintptr_t a) { return a ? a - base : 0; };
    UE_LOGI("resolve: GUObjectArray=%p (rva 0x%zx)", reinterpret_cast<void*>(g_objArray), rva(g_objArray));
    UE_LOGI("resolve: FName::ToString=%p (rva 0x%zx)",
            reinterpret_cast<void*>(g_fnameToString), rva(reinterpret_cast<uintptr_t>(g_fnameToString)));
    UE_LOGI("resolve: ProcessEvent=%p (rva 0x%zx)",
            reinterpret_cast<void*>(g_processEvent), rva(reinterpret_cast<uintptr_t>(g_processEvent)));
    check(g_objArray != 0, "GUObjectArray signature");
    check(g_fnameToString != nullptr, "FName::ToString signature");
    check(g_processEvent != nullptr, "ProcessEvent signature");

    // The proxy loads before the engine; wait for the object array to populate.
    int32_t n = 0;
    for (int i = 0; i < 120; ++i) {
        n = NumObjects();
        if (n >= 10000) break;
        ::Sleep(500);
    }
    UE_LOGI("NumObjects()=%d", n);
    check(n >= 10000, "object array populated (offsets sane)");

    // 2) Functional validation -- proves the sigs/offsets actually WORK, not
    //    just that an AOB matched something (catches matching the wrong site).
    if (g_objArray && g_fnameToString) {
        // Round-trip a known engine name. Index 1 is the UObject class "Object".
        const std::wstring objName = ToString(NameOf(ObjectAt(1)));
        UE_LOGI("name round-trip: object[1] = '%ls' (expect 'Object')", objName.c_str());
        check(objName == L"Object", "FName::ToString round-trip");

        void* clsObject = FindClass(L"Object");
        void* clsActor = FindClass(P::name::ActorClass);
        void* clsWorld = FindClass(P::name::WorldClass);
        check(clsObject != nullptr, "FindClass(Object)");
        check(clsActor != nullptr, "FindClass(Actor)");
        check(clsWorld != nullptr, "FindClass(World)");
        if (clsActor) {
            void* fn = FindFunction(clsActor, P::name::SetActorLocationFn);
            UE_LOGI("FindFunction(Actor, %ls) = %p", P::name::SetActorLocationFn, fn);
            check(fn != nullptr, "FindFunction(Actor, K2_SetActorLocation)");
        }
    }

    // 3) Gameplay-content signals (informational; absent at the menu).
    void* clsMainPlayer = FindClass(P::name::MainPlayerClass);
    UE_LOGI("content: %ls = %s (loads with the gameplay map; absent at menu)",
            P::name::MainPlayerClass, clsMainPlayer ? "present" : "not loaded");

    if (fails == 0) {
        UE_LOGI("==== HEALTH: PASS ====");
    } else {
        UE_LOGE("==== HEALTH: FAIL (%d issue(s)) -- sdk_profile.h likely needs "
                "re-derivation for this build ====", fails);
    }
}

}  // namespace ue_wrap::reflection
