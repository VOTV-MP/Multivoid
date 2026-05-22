#include "ue_wrap/reflection.h"

#include "ue_wrap/sig_scan.h"

#include <windows.h>

#include <cstdio>
#include <map>

namespace ue_wrap::reflection {
namespace {

// ---- Signatures (VOTV Alpha 0.9.0-n, UE4.27) ----------------------------
//
// FName::ToString: unique function prologue. Match = function address.
constexpr const char* kSigFNameToString =
    "48 89 5C 24 18 55 56 57 48 8B EC 48 83 EC 30 8B 01 48 8B F1 "
    "44 8B 49 04 8B F8 C1 EF 10 48 8B DA 0F B7 C8";

// GUObjectArray: a static init-guard that loads &GUObjectArray into rcx. The
// pattern's `lea rcx,[rip+disp32]` is at match+21 (disp32 at match+24, the
// instruction ends at match+28). Verified: every match in this binary decodes
// to GUObjectArray (0x...4D8F910 static).
constexpr const char* kSigGUObjectArray =
    "8B 05 ?? ?? ?? ?? 3B 05 ?? ?? ?? ?? 75 13 48 8D 15 ?? ?? ?? ?? "
    "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 45 33 C9 "
    "48 89 45 ?? 4C 8D 45";
constexpr size_t kGUObjLeaDispOff = 24;  // offset of the disp32 within the match
constexpr size_t kGUObjLeaEndOff = 28;   // rip base (end of the lea instruction)

// ---- UObject / FUObjectArray layout (standard UE4.27) -------------------
constexpr size_t kUObj_ClassPrivate = 0x10;
constexpr size_t kUObj_NamePrivate = 0x18;
constexpr size_t kUObj_OuterPrivate = 0x20;

constexpr size_t kArr_ObjObjects = 0x10;   // FUObjectArray.ObjObjects
constexpr size_t kChunk_Objects = 0x00;    // FUObjectItem** (chunk pointers)
constexpr size_t kChunk_NumElements = 0x14;
constexpr size_t kChunk_MaxElements = 0x10;
constexpr size_t kChunk_NumChunks = 0x1C;
constexpr int32_t kElemsPerChunk = 64 * 1024;  // FChunkedFixedUObjectArray
constexpr size_t kItemStride = 0x18;           // sizeof(FUObjectItem)

using FNameToStringFn = void(__fastcall*)(const FName*, FString*);

uintptr_t g_objArray = 0;
FNameToStringFn g_fnameToString = nullptr;

}  // namespace

uintptr_t GUObjectArrayAddr() { return g_objArray; }
uintptr_t FNameToStringAddr() { return reinterpret_cast<uintptr_t>(g_fnameToString); }
bool IsResolved() { return g_objArray && g_fnameToString; }

bool Resolve() {
    if (IsResolved()) return true;

    if (!g_fnameToString) {
        const uintptr_t hit = FindPattern(kSigFNameToString);
        if (hit) g_fnameToString = reinterpret_cast<FNameToStringFn>(hit);
    }
    if (!g_objArray) {
        const uintptr_t hit = FindPattern(kSigGUObjectArray);
        if (hit) {
            const int32_t disp = *reinterpret_cast<int32_t*>(hit + kGUObjLeaDispOff);
            g_objArray = hit + kGUObjLeaEndOff + disp;
        }
    }
    return IsResolved();
}

int32_t NumObjects() {
    if (!g_objArray) return 0;
    return *reinterpret_cast<int32_t*>(g_objArray + kArr_ObjObjects + kChunk_NumElements);
}

void* ObjectAt(int32_t index) {
    if (!g_objArray || index < 0) return nullptr;
    const uintptr_t objObjects = g_objArray + kArr_ObjObjects;
    if (index >= *reinterpret_cast<int32_t*>(objObjects + kChunk_NumElements)) return nullptr;
    auto** chunks = *reinterpret_cast<uint8_t***>(objObjects + kChunk_Objects);
    if (!chunks) return nullptr;
    uint8_t* chunk = chunks[index / kElemsPerChunk];
    if (!chunk) return nullptr;
    uint8_t* item = chunk + static_cast<size_t>(index % kElemsPerChunk) * kItemStride;
    return *reinterpret_cast<void**>(item);  // FUObjectItem.Object @ +0x00
}

const FName& NameOf(void* uobject) {
    return *reinterpret_cast<FName*>(reinterpret_cast<uint8_t*>(uobject) + kUObj_NamePrivate);
}

void* ClassOf(void* uobject) {
    return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(uobject) + kUObj_ClassPrivate);
}

void* OuterOf(void* uobject) {
    return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(uobject) + kUObj_OuterPrivate);
}

std::wstring ToString(const FName& name) {
    if (!g_fnameToString) return L"";
    static FString scratch{nullptr, 0, 0};
    scratch.Num = 0;  // reuse the buffer; write from the start (one buffer, freed never)
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
        // Its meta-class identifies it as a class object.
        const std::wstring meta = ClassNameOf(obj);
        if (meta == L"Class" || meta == L"BlueprintGeneratedClass" ||
            meta == L"DynamicClass" || meta == L"LinkerPlaceholderClass") {
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

void RunSelfTest(const wchar_t* outPath) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, outPath, L"w") != 0 || !f) return;

    if (!Resolve()) {
        std::fprintf(f, "RESOLVE FAILED: GUObjectArray=%p FName::ToString=%p\n",
                     reinterpret_cast<void*>(g_objArray),
                     reinterpret_cast<void*>(g_fnameToString));
        std::fclose(f);
        return;
    }

    uintptr_t base = 0;
    size_t imgSize = 0;
    MainModuleRange(base, imgSize);
    std::fprintf(f, "main module base=%p size=0x%zx\n", reinterpret_cast<void*>(base), imgSize);
    std::fprintf(f, "GUObjectArray   = %p (rva 0x%zx)\n", reinterpret_cast<void*>(g_objArray), g_objArray - base);
    std::fprintf(f, "FName::ToString = %p (rva 0x%zx)\n\n",
                 reinterpret_cast<void*>(g_fnameToString),
                 reinterpret_cast<uintptr_t>(g_fnameToString) - base);

    // Wait for the engine to populate the object array (proxy loads pre-engine).
    int32_t n = 0;
    for (int i = 0; i < 120; ++i) {
        n = NumObjects();
        if (n >= 10000) break;
        ::Sleep(500);
    }

    // Raw FUObjectArray header (proves/refutes the assumed layout).
    std::fprintf(f, "FUObjectArray header bytes:\n");
    auto* hdr = reinterpret_cast<uint8_t*>(g_objArray);
    for (int i = 0; i < 0x40; ++i) {
        std::fprintf(f, "%02X ", hdr[i]);
        if ((i & 0xF) == 0xF) std::fprintf(f, "\n");
    }
    const uintptr_t oo = g_objArray + kArr_ObjObjects;
    std::fprintf(f, "\nObjObjects @ +0x%zx: Objects(chunks)=%p Max=%d Num=%d NumChunks=%d\n\n",
                 kArr_ObjObjects,
                 *reinterpret_cast<void**>(oo + kChunk_Objects),
                 *reinterpret_cast<int32_t*>(oo + kChunk_MaxElements),
                 *reinterpret_cast<int32_t*>(oo + kChunk_NumElements),
                 *reinterpret_cast<int32_t*>(oo + kChunk_NumChunks));

    std::fprintf(f, "NumObjects()=%d\n\n", n);

    // Walk: sample first names, and tally a few classes of interest.
    int sampled = 0;
    int valid = 0;
    long mainPlayer = 0, controllers = 0, gamemode = 0;
    std::fprintf(f, "--- sample (first 30 valid objects) ---\n");
    for (int32_t i = 0; i < n; ++i) {
        void* obj = ObjectAt(i);
        if (!obj) continue;
        ++valid;
        void* cls = ClassOf(obj);
        if (!cls) continue;
        const std::wstring objName = ToString(NameOf(obj));
        const std::wstring clsName = ToString(NameOf(cls));
        if (clsName == L"mainPlayer_C") ++mainPlayer;
        if (clsName == L"mainGamemode_C") ++gamemode;
        if (clsName == L"PlayerController" || clsName.find(L"PlayerController") != std::wstring::npos) ++controllers;
        if (sampled < 30) {
            std::fprintf(f, "[%6d] obj=%-28ls class=%ls\n", i, objName.c_str(), clsName.c_str());
            ++sampled;
        }
    }
    std::fprintf(f, "\nvalid objects walked = %d\n", valid);
    std::fprintf(f, "instances: mainPlayer_C=%ld  mainGamemode_C=%ld  *PlayerController*=%ld\n\n",
                 mainPlayer, gamemode, controllers);

    // --- lookups (the building blocks for the C++ orphan-spawn port) ---
    auto report = [&](const wchar_t* what, void* obj) {
        if (obj) {
            std::fprintf(f, "  %-40ls = %p  (class %ls)\n", what, obj, ClassNameOf(obj).c_str());
        } else {
            std::fprintf(f, "  %-40ls = NOT FOUND\n", what);
        }
    };
    std::fprintf(f, "--- lookups ---\n");
    void* clsMainPlayer = FindClass(L"mainPlayer_C");
    void* clsActor = FindClass(L"Actor");
    void* clsWorld = FindClass(L"World");
    report(L"FindClass(mainPlayer_C)", clsMainPlayer);
    report(L"FindClass(Actor)", clsActor);
    report(L"FindClass(World)", clsWorld);
    report(L"FindClass(mainGamemode_C)", FindClass(L"mainGamemode_C"));
    if (clsActor) {
        report(L"FindFunction(Actor, K2_SetActorLocation)", FindFunction(clsActor, L"K2_SetActorLocation"));
        report(L"FindFunction(Actor, K2_DestroyActor)", FindFunction(clsActor, L"K2_DestroyActor"));
    }
    // A live World instance (proves we can find runtime objects, not just classes).
    void* worldInst = nullptr;
    for (int32_t i = 0; i < n && !worldInst; ++i) {
        void* obj = ObjectAt(i);
        if (obj && ClassNameOf(obj) == L"World") worldInst = obj;
    }
    report(L"first World instance", worldInst);

    std::fclose(f);
}

}  // namespace ue_wrap::reflection
