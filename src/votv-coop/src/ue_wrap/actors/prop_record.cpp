// ue_wrap/actors/prop_record.cpp -- see ue_wrap/actors/prop_record.h.

#include "ue_wrap/actors/prop_record.h"

#include "ue_wrap/actors/save_record.h"   // PropBaseClass, IsPropClass, PlausibleObjPtr
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"

#include <chrono>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <unordered_map>

namespace ue_wrap::prop_record {
namespace {

namespace R  = ue_wrap::reflection;
namespace SR = ue_wrap::save_record;

// EPropertyFlags::CPF_SaveGame: the Blueprint "SaveGame" checkbox, the set ActorToBytes serialises.
constexpr uint64_t kCpfSaveGame = 0x1000000ull;

// FSoftObjectPtr: FWeakObjectPtr (8), int32 TagAtLastTest and its padding (8), FSoftObjectPath {FName, FString}.
constexpr int32_t kSoftSubPath = 0x18;
constexpr int32_t kMaxBytes    = 64 * 1024 * 1024;   // a byte array header past this is not one

// ---- the struct_betterSave layout, read off the live struct ---------------------------------------

struct Layout {
    int32_t size = 0;            // PropertiesSize: what a whole-value copy between two frames moves
    int32_t classOff = -1;       // the soft class, whose path string betterGetData mints for the call alone
    int32_t actorDataOff = -1;   // TArray<uint8>
};
Layout g_layout;
bool   g_layoutOk = false;

bool PrefixMatch(const std::wstring& name, const wchar_t* prefix) {
    size_t i = 0;
    for (; prefix[i]; ++i)
        if (i >= name.size() || std::towlower(name[i]) != std::towlower(prefix[i])) return false;
    return true;
}

// The member whose name starts with `prefix` (the GUID tails are not stable), checked against its FField class and
// size. False, with one line, when it does not check out.
bool Member(void* st, const std::vector<R::StructFieldInfo>& fields, const wchar_t* prefix, const wchar_t* fieldClass,
            int32_t size, int32_t& off) {
    for (const R::StructFieldInfo& f : fields) {
        if (!PrefixMatch(f.name, prefix)) continue;
        std::wstring cls;
        if (R::PropertyOffsetAndClass(st, f.name.c_str(), off, cls) && cls == fieldClass && f.size == size) return true;
        UE_LOGW("prop_record: struct_betterSave.%ls does not check out (class %ls, size %d, want %ls/%d) -- props' "
                "save state is not read or written", prefix, cls.c_str(), f.size, fieldClass, size);
        return false;
    }
    UE_LOGW("prop_record: struct_betterSave has no %ls member -- props' save state is not read or written", prefix);
    return false;
}

// Asked again at most once a second while the struct is not loaded yet.
bool EnsureLayout() {
    if (g_layoutOk) return true;
    static std::chrono::steady_clock::time_point s_lastTry{};
    const auto now = std::chrono::steady_clock::now();
    if (s_lastTry != std::chrono::steady_clock::time_point{} && now - s_lastTry < std::chrono::seconds(1)) return false;
    s_lastTry = now;
    void* const st = R::FindObject(L"struct_betterSave", L"UserDefinedStruct");
    if (!st) return false;
    const std::vector<R::StructFieldInfo> fields = R::EnumerateStructFields(st);
    Layout l;
    l.size = R::StructSize(st);
    if (l.size <= 0 || !Member(st, fields, L"class_", L"SoftClassProperty", 0x28, l.classOff) ||
        !Member(st, fields, L"actorData_", L"ArrayProperty", 16, l.actorDataOff))
        return false;
    g_layout = l;
    g_layoutOk = true;
    return true;
}

// ---- TArray<uint8> in a frame ----------------------------------------------------------------------

struct Bytes { uint8_t* data; int32_t num; int32_t max; };

bool ReadBytes(const void* at, std::vector<uint8_t>& out) {
    Bytes b{};
    std::memcpy(&b, at, sizeof(b));
    if (b.num < 0 || b.num > kMaxBytes || (b.num > 0 && !SR::PlausibleObjPtr(b.data))) return false;
    out.assign(b.data, b.data + b.num);
    return true;
}

// Free the array at `at` and leave it empty: an out-parameter the engine filled for this call alone.
void DropBytes(void* at) {
    Bytes b{};
    std::memcpy(&b, at, sizeof(b));
    if (b.data) R::EngineFree(b.data);
    std::memset(at, 0, sizeof(Bytes));
}

// Free the soft class's path string betterGetData minted into the frame.
void DropSoftSubPath(uint8_t* record) {
    R::FString s{};
    std::memcpy(&s, record + g_layout.classOff + kSoftSubPath, sizeof(s));
    if (s.Data) R::EngineFree(s.Data);
    std::memset(record + g_layout.classOff + kSoftSubPath, 0, sizeof(s));
}

// ---- the per-class answers ---------------------------------------------------------------------------

struct Verbs {
    void* get = nullptr;
    void* load = nullptr;
    bool carries = false;
};
std::unordered_map<void*, Verbs> g_verbs;

// The flag constant is the engine's, and a build that moved it would read every class as stateless. Aprop_C's
// own `key` is SaveGame (it is the first thing a record restores), so it is asked once before any class is.
bool SaveGameFlagHolds(void* propBase) {
    static int s_state = 0;   // 0 unasked, 1 holds, -1 does not
    if (s_state == 0) {
        s_state = -1;
        for (const R::StructFieldInfo& f : R::EnumerateStructFields(propBase))
            if (::_wcsicmp(f.name.c_str(), L"key") == 0 && (f.flags & kCpfSaveGame)) { s_state = 1; break; }
        if (s_state < 0)
            UE_LOGE("prop_record: Aprop_C::key does not carry the SaveGame flag the lane tests for -- no prop is read "
                    "as carrying state, so no record is published");
    }
    return s_state > 0;
}

// Whether any class from `cls` up to, not including, Aprop_C keeps state of its own.
bool DeclaresState(void* cls, void* propBase) {
    for (void* c = cls; c && c != propBase; c = R::SuperStructOf(c)) {
        if (R::FindFunction(c, L"betterGetData")) return true;
        for (const R::StructFieldInfo& f : R::EnumerateStructFields(c))
            if (f.flags & kCpfSaveGame) return true;
    }
    return false;
}

const Verbs* VerbsFor(void* cls) {
    void* const propBase = cls ? SR::PropBaseClass() : nullptr;
    if (!propBase) return nullptr;   // prop_C not loaded yet: not an answer for any class
    const auto it = g_verbs.find(cls);
    if (it != g_verbs.end()) return &it->second;
    Verbs v;
    if (SaveGameFlagHolds(propBase) && SR::IsPropClass(cls)) {
        v.get  = R::FindDispatchFunction(cls, L"betterGetData", nullptr);
        v.load = R::FindDispatchFunction(cls, L"betterLoadData", nullptr);
        v.carries = v.get && v.load && DeclaresState(cls, propBase);
    }
    return &(g_verbs[cls] = v);
}

// Run betterGetData on `actor` through `get`; the record it leaves is at `offOut` in the frame.
bool RunGet(void* actor, ParamFrame& get, int32_t& offOut) {
    offOut = get.ParamOffset(L"betterData");
    return get.valid() && offOut >= 0 && offOut + g_layout.size <= get.FrameSize() && Call(actor, get);
}

int g_applying = 0;   // Apply calls in progress; game thread
struct ApplyingScope {
    ApplyingScope() { ++g_applying; }
    ~ApplyingScope() { --g_applying; }
};

}  // namespace

bool CarriesState(void* cls) {
    const Verbs* v = VerbsFor(cls);
    return v && v->carries;
}

bool Capture(void* actor, std::vector<uint8_t>& bytes) {
    bytes.clear();
    if (!actor || !EnsureLayout()) return false;
    const Verbs* v = VerbsFor(R::ClassOf(actor));
    if (!v || !v->carries) return false;
    ParamFrame get(v->get);
    int32_t off = -1;
    if (!RunGet(actor, get, off)) return false;
    uint8_t* const rec = static_cast<uint8_t*>(get.data()) + off;
    const bool read = ReadBytes(rec + g_layout.actorDataOff, bytes);
    DropBytes(rec + g_layout.actorDataOff);
    DropSoftSubPath(rec);
    return read;
}

bool Apply(void* actor, const std::vector<uint8_t>& bytes) {
    const ApplyingScope applying;
    if (!actor || bytes.empty() || bytes.size() > static_cast<size_t>(kMaxBytes) || !EnsureLayout()) return false;
    void* const cls = R::ClassOf(actor);
    const Verbs* v = VerbsFor(cls);
    if (!v || !v->carries) return false;

    // The receiver's own record: its identity is what betterLoadData seats, and the call also leaves the actor's
    // remaining lifespan in its field, which is read below.
    ParamFrame get(v->get);
    int32_t getOff = -1;
    if (!RunGet(actor, get, getOff)) return false;
    uint8_t* const own = static_cast<uint8_t*>(get.data()) + getOff;
    ParamFrame load(v->load);
    const int32_t loadOff = load.ParamOffset(L"betterData");
    if (!load.valid() || loadOff < 0 || loadOff + g_layout.size > load.FrameSize()) {
        DropBytes(own + g_layout.actorDataOff);
        DropSoftSubPath(own);
        return false;
    }
    uint8_t* const in  = static_cast<uint8_t*>(load.data()) + loadOff;
    std::memcpy(in, own, static_cast<size_t>(g_layout.size));

    // The sender's stream replaces the receiver's own in the copy. Both buffers are engine-owned and are freed below,
    // once betterLoadData has read the stream.
    Bytes mine{};
    std::memcpy(&mine, own + g_layout.actorDataOff, sizeof(mine));
    Bytes theirs{};
    theirs.data = static_cast<uint8_t*>(R::EngineAlloc(bytes.size()));
    if (!theirs.data) {
        DropBytes(own + g_layout.actorDataOff);
        DropSoftSubPath(own);
        return false;
    }
    std::memcpy(theirs.data, bytes.data(), bytes.size());
    theirs.num = theirs.max = static_cast<int32_t>(bytes.size());
    std::memcpy(in + g_layout.actorDataOff, &theirs, sizeof(theirs));

    static int32_t s_lifeOff = -2;
    if (s_lifeOff == -2) s_lifeOff = R::FindPropertyOffset(SR::PropBaseClass(), L"remainingLifespan");
    float ownLife = 0.f;
    if (s_lifeOff >= 0) std::memcpy(&ownLife, static_cast<const uint8_t*>(actor) + s_lifeOff, sizeof(ownLife));

    const bool called = Call(actor, load);

    // The frames' buffers are done with: betterLoadData reads its stream and keeps no pointer into it.
    if (mine.data) R::EngineFree(mine.data);
    std::memset(own + g_layout.actorDataOff, 0, sizeof(Bytes));
    R::EngineFree(theirs.data);
    std::memset(in + g_layout.actorDataOff, 0, sizeof(Bytes));
    DropSoftSubPath(own);
    std::memset(in + g_layout.classOff + kSoftSubPath, 0, sizeof(R::FString));   // the same pointer, freed above
    if (!called) return false;

    // The stream carries the sender's remaining lifespan and betterLoadData set it. Put the receiver's own back.
    if (s_lifeOff >= 0) {
        float now = 0.f;
        std::memcpy(&now, static_cast<const uint8_t*>(actor) + s_lifeOff, sizeof(now));
        if (now != ownLife) {
            if (void* const fn = R::FindDispatchFunctionCached(cls, L"SetLifeSpan")) {
                ParamFrame set(fn);
                if (set.valid() && set.Set<float>(L"InLifespan", ownLife)) Call(actor, set);
            }
        }
    }
    return true;
}

bool IsApplying() { return g_applying > 0; }

void ResetCache() {
    g_verbs.clear();
    g_layout = Layout{};
    g_layoutOk = false;
    SR::ResetCodecCache();   // the prop base class it shares is world-scoped too
}

}  // namespace ue_wrap::prop_record
