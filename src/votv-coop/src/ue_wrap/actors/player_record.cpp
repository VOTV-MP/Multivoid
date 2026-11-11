// ue_wrap/actors/player_record.cpp -- see ue_wrap/actors/player_record.h.
//
// ThingsAndStuffLib is the game's own native library, so its functions carry no Blueprint body to read their
// parameter names from; the blueprints call them positionally. The frames are bound the same way
// (ue_wrap/engine/save_folder.cpp): the parameters in declaration order, checked by size.

#include "ue_wrap/actors/player_record.h"

#include "ue_wrap/actors/save_record.h"  // AllocZeroed, WriteArrHeader, ReadFNameAt
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/sdk_profile_names.h"

#include <cstring>
#include <cwchar>

namespace ue_wrap::player_record {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;
namespace SR = ue_wrap::save_record;

// FSoftObjectPtr: FWeakObjectPtr (8), int32 TagAtLastTest and its padding (8), then FSoftObjectPath, whose
// AssetPathName FName comes first and SubPathString FString after it.
constexpr int32_t kSoftPath = 0x10;
constexpr int32_t kSoftSubPath = 0x18;
constexpr int32_t kAlign = 16;                         // the record holds an FTransform
constexpr int32_t kMaxBytes = 64 * 1024 * 1024;        // a byte array header past this is not one

// ---- TArray<uint8> -----------------------------------------------------------------------------------------------

struct Bytes { uint8_t* data; int32_t num; int32_t max; };

Bytes ReadBytes(const void* at) {
    Bytes b{};
    std::memcpy(&b, at, sizeof(b));
    if (b.num < 0 || b.num > kMaxBytes || (b.num > 0 && !SR::PlausibleObjPtr(b.data))) return Bytes{};
    return b;
}

// An engine-owned copy of `in`, as the header a frame or a field takes. False when it does not allocate.
bool MintBytes(const std::vector<uint8_t>& in, Bytes& out) {
    out = Bytes{};
    if (in.empty()) return true;
    out.data = static_cast<uint8_t*>(R::EngineAlloc(in.size()));
    if (!out.data) return false;
    std::memcpy(out.data, in.data(), in.size());
    out.num = out.max = static_cast<int32_t>(in.size());
    return true;
}

// Copy the array at `at` out and free its buffer: an out-parameter the engine filled for this call alone.
void TakeBytes(void* at, std::vector<uint8_t>& out) {
    const Bytes b = ReadBytes(at);
    out.assign(b.data, b.data + b.num);
    Bytes raw{};
    std::memcpy(&raw, at, sizeof(raw));
    if (raw.data) R::EngineFree(raw.data);
    std::memset(at, 0, sizeof(Bytes));
}

// ---- ThingsAndStuffLib -------------------------------------------------------------------------------------------

void* g_libCdo = nullptr;
void* g_toBytesFn = nullptr;
void* g_setPropFn = nullptr;

bool ResolveLib() {
    if (!g_libCdo) g_libCdo = R::FindClassDefaultObject(L"ThingsAndStuffLib");
    if (!g_libCdo) return false;
    void* const cls = R::ClassOf(g_libCdo);
    if (!g_toBytesFn) g_toBytesFn = R::FindFunction(cls, L"ObjectToBytes");
    if (!g_setPropFn) g_setPropFn = R::FindFunction(cls, L"SetPropertyInObjectBytes");
    return g_toBytesFn && g_setPropFn;
}

// The frame's non-return parameters in declaration order, by size: 8 (an object, a class or an FName), 16 (an
// array or an FString), 1 (a bool). Said once per function when the shape is not the expected one.
struct Shape { std::vector<int32_t> eight, sixteen, one; };
bool ShapeOf(void* fn, size_t eight, size_t sixteen, size_t one, Shape& out) {
    out = Shape{};
    for (const R::ParamInfo& p : R::FunctionParams(fn)) {
        if (p.flags & P::cpf::ReturnParm) continue;
        if (p.size == 8) out.eight.push_back(p.offset);
        else if (p.size == 16) out.sixteen.push_back(p.offset);
        else if (p.size == 1) out.one.push_back(p.offset);
    }
    const bool ok = out.eight.size() == eight && out.sixteen.size() == sixteen && out.one.size() == one;
    if (!ok) {
        static void* s_named = nullptr;
        if (s_named != fn) {
            s_named = fn;
            std::wstring names;
            for (const R::ParamInfo& p : R::FunctionParams(fn)) names += p.name + L"(" + std::to_wstring(p.size) + L") ";
            UE_LOGE("player_record: %ls has an unexpected frame: %ls", R::ToString(R::NameOf(fn)).c_str(),
                    names.c_str());
        }
    }
    return ok;
}

// ObjectToBytes(Object, bCompress, out Bytes), uncompressed as the player's own record is.
bool ObjectToBytes(void* object, std::vector<uint8_t>& out) {
    out.clear();
    Shape s;
    if (!object || !ResolveLib() || !ShapeOf(g_toBytesFn, 1, 1, 1, s)) return false;
    ParamFrame f(g_toBytesFn);
    uint8_t* const d = static_cast<uint8_t*>(f.data());
    if (!d) return false;
    std::memcpy(d + s.eight[0], &object, sizeof(object));
    d[s.one[0]] = 0;
    const bool called = Call(g_libCdo, f);
    TakeBytes(d + s.sixteen[0], out);
    return called && !out.empty();
}

}  // namespace

// SetPropertyInObjectBytes(Class, Bytes&, bCompress, PropertyName, Value) on `bytes` in place.
bool SetPropertyInBytes(void* cls, std::vector<uint8_t>& bytes, const std::wstring& prop, const std::wstring& value) {
    Shape s;
    if (!cls || bytes.empty() || !ResolveLib() || !ShapeOf(g_setPropFn, 2, 2, 1, s)) return false;
    const R::FName name = ue_wrap::fname_utils::StringToFName(prop);
    if (name.ComparisonIndex == 0) return false;
    Bytes in{};
    if (!MintBytes(bytes, in)) return false;
    std::wstring text = value;
    R::FString fs{};
    fs.Data = text.data();
    fs.Num = static_cast<int32_t>(text.size()) + 1;  // FString::Num counts the null; the native copies it
    fs.Max = fs.Num;
    ParamFrame f(g_setPropFn);
    uint8_t* const d = static_cast<uint8_t*>(f.data());
    if (!d) {
        R::EngineFree(in.data);
        return false;
    }
    std::memcpy(d + s.eight[0], &cls, sizeof(cls));
    std::memcpy(d + s.sixteen[0], &in, sizeof(in));
    d[s.one[0]] = 0;
    std::memcpy(d + s.eight[1], &name, sizeof(name));
    std::memcpy(d + s.sixteen[1], &fs, sizeof(fs));
    const bool called = Call(g_libCdo, f);
    std::vector<uint8_t> back;
    TakeBytes(d + s.sixteen[0], back);  // the array by reference: the native's result, in whatever buffer it left
    std::memset(d + s.sixteen[1], 0, sizeof(fs));  // ours, not the engine's
    if (!called || back.empty()) return false;
    bytes = std::move(back);
    return true;
}

// GetPropertyFromObjectBytes(Class, Bytes, bCompress, PropertyName, out Value) on `bytes`, the property's text form.
bool GetPropertyInBytes(void* cls, const std::vector<uint8_t>& bytes, const std::wstring& prop, std::wstring& value) {
    value.clear();
    static void* s_getFn = nullptr;
    Shape s;
    if (!cls || bytes.empty() || !ResolveLib()) return false;
    if (!s_getFn) s_getFn = R::FindFunction(R::ClassOf(g_libCdo), L"GetPropertyFromObjectBytes");
    if (!s_getFn || !ShapeOf(s_getFn, 2, 2, 1, s)) return false;
    const R::FName name = ue_wrap::fname_utils::StringToFName(prop);
    if (name.ComparisonIndex == 0) return false;
    Bytes in{};
    if (!MintBytes(bytes, in)) return false;
    ParamFrame f(s_getFn);
    uint8_t* const d = static_cast<uint8_t*>(f.data());
    if (!d) {
        R::EngineFree(in.data);
        return false;
    }
    std::memcpy(d + s.eight[0], &cls, sizeof(cls));
    std::memcpy(d + s.sixteen[0], &in, sizeof(in));
    d[s.one[0]] = 0;
    std::memcpy(d + s.eight[1], &name, sizeof(name));
    const bool called = Call(g_libCdo, f);
    // The bytes were a copy for this call: the native may keep or free them; the engine frees what it owns of the array.
    std::vector<uint8_t> ignored;
    TakeBytes(d + s.sixteen[0], ignored);
    R::FString fs{};
    std::memcpy(&fs, d + s.sixteen[1], sizeof(fs));
    if (called && fs.Data && fs.Num > 1 && fs.Num < 4096) value.assign(fs.Data, static_cast<size_t>(fs.Num - 1));
    if (fs.Data) R::EngineFree(fs.Data);
    std::memset(d + s.sixteen[1], 0, sizeof(fs));
    return called && !value.empty();
}

namespace {

// ---- betterObjectsData -------------------------------------------------------------------------------------------

struct Layout {
    void* saveCls = nullptr;
    int32_t arrayOff = -1, stride = 0, classOff = -1, actorDataOff = -1;
};
Layout g_layout;

// The array's offset on the save class, its element stride and the two members read here, resolved for the class
// `save` is. False when any does not resolve.
bool ResolveLayout(void* save) {
    void* const cls = save ? R::ClassOf(save) : nullptr;
    if (!cls) return false;
    if (g_layout.saveCls == cls) return g_layout.arrayOff >= 0;
    g_layout = Layout{};
    g_layout.saveCls = cls;
    const int32_t arrayOff = R::FindPropertyOffset(cls, L"betterObjectsData");
    void* const st = R::ArrayInnerStruct(cls, L"betterObjectsData");
    const int32_t size = st ? R::StructSize(st) : 0;
    const int32_t classOff = st ? R::FindPropertyOffsetByPrefix(st, L"class_") : -1;
    const int32_t dataOff = st ? R::FindPropertyOffsetByPrefix(st, L"actorData_") : -1;
    if (arrayOff < 0 || size <= 0 || classOff < 0 || dataOff < 0) {
        UE_LOGE("player_record: %ls.betterObjectsData does not resolve (array=%d size=%d class=%d actorData=%d) -- "
                "the player's record is not read or written", R::ToString(R::NameOf(cls)).c_str(), arrayOff, size,
                classOff, dataOff);
        return false;
    }
    g_layout.arrayOff = arrayOff;
    g_layout.stride = (size + kAlign - 1) / kAlign * kAlign;
    g_layout.classOff = classOff;
    g_layout.actorDataOff = dataOff;
    return true;
}

struct Records { uint8_t* data = nullptr; int32_t num = 0; };

Records RecordsOf(void* save) {
    if (!ResolveLayout(save)) return Records{};
    const SR::Arr a = SR::ReadArr(save, g_layout.arrayOff);
    return Records{const_cast<uint8_t*>(a.data), a.num};
}

uint8_t* RecordAt(const Records& r, int32_t i) { return r.data + static_cast<size_t>(i) * g_layout.stride; }

// The record's class path as its soft reference holds it, "/Game/.../mainPlayer.mainPlayer_C".
std::wstring ClassPathOf(const uint8_t* record) {
    return SR::ReadFNameAt(record + g_layout.classOff, kSoftPath);
}

bool IsPlayerRecord(const uint8_t* record) {
    const std::wstring path = ClassPathOf(record);
    const size_t dot = path.rfind(L'.');
    const std::wstring leaf = dot == std::wstring::npos ? path : path.substr(dot + 1);
    return ::_wcsicmp(leaf.c_str(), P::name::MainPlayerClass) == 0;
}

// The record's actorData takes `bytes` (an engine-owned copy), the buffer it had freed: nothing else refers to it.
bool ReplaceActorData(uint8_t* record, const std::vector<uint8_t>& bytes) {
    Bytes fresh{};
    if (!MintBytes(bytes, fresh)) return false;
    Bytes old{};
    std::memcpy(&old, record + g_layout.actorDataOff, sizeof(old));
    std::memcpy(record + g_layout.actorDataOff, &fresh, sizeof(fresh));
    if (old.data) R::EngineFree(old.data);
    return true;
}

}  // namespace

bool CaptureLocal(void* pawn, std::vector<uint8_t>& out) {
    out.clear();
    if (!pawn || !R::IsLive(pawn)) return false;
    void* const saveCls = R::FindClass(P::name::SaveSlotClass);
    void* const st = saveCls ? R::ArrayInnerStruct(saveCls, L"betterObjectsData") : nullptr;
    static int32_t s_dataOff = -2, s_classOff = -2, s_size = 0;
    if (s_dataOff == -2 && st) {
        s_dataOff = R::FindPropertyOffsetByPrefix(st, L"actorData_");
        s_classOff = R::FindPropertyOffsetByPrefix(st, L"class_");
        s_size = R::StructSize(st);
    }
    if (s_dataOff < 0 || s_classOff < 0) return false;
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(pawn), L"betterGetData");
    if (!fn) return false;
    ParamFrame f(fn);
    const int32_t off = f.ParamOffset(L"betterData");
    if (!f.valid() || off < 0 || off + s_size > f.FrameSize()) return false;
    const bool called = Call(pawn, f);
    uint8_t* const rec = static_cast<uint8_t*>(f.data()) + off;
    TakeBytes(rec + s_dataOff, out);
    // The record's class path string, minted for this frame alone.
    R::FString sub{};
    std::memcpy(&sub, rec + s_classOff + kSoftSubPath, sizeof(sub));
    if (sub.Data) R::EngineFree(sub.Data);
    std::memset(rec + s_classOff + kSoftSubPath, 0, sizeof(sub));
    return called && !out.empty();
}

bool InventoryBytes(void* pawn, std::vector<uint8_t>& out) {
    out.clear();
    if (!pawn || !R::IsLive(pawn)) return false;
    static R::InstanceOffset s_comp(L"inventoryComponent_player");
    const int32_t off = s_comp.Of(pawn);
    if (off < 0) return false;
    void* comp = nullptr;
    std::memcpy(&comp, static_cast<const uint8_t*>(pawn) + off, sizeof(comp));
    return comp && R::IsLive(comp) && ObjectToBytes(comp, out);
}

int CountInSave(void* save) {
    const Records r = RecordsOf(save);
    if (!ResolveLayout(save)) return -1;
    int n = 0;
    for (int32_t i = 0; i < r.num; ++i) n += IsPlayerRecord(RecordAt(r, i)) ? 1 : 0;
    return n;
}

int WriteToSave(void* save, const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) return -1;
    const Records r = RecordsOf(save);
    if (!ResolveLayout(save)) return -1;
    int n = 0;
    for (int32_t i = 0; i < r.num; ++i) {
        uint8_t* const rec = RecordAt(r, i);
        if (!IsPlayerRecord(rec)) continue;
        if (!ReplaceActorData(rec, bytes)) return -1;
        ++n;
    }
    return n;
}

int RemoveFromSave(void* save) {
    const Records r = RecordsOf(save);
    if (!ResolveLayout(save)) return -1;
    int32_t kept = 0;
    int removed = 0;
    for (int32_t i = 0; i < r.num; ++i) {
        uint8_t* const rec = RecordAt(r, i);
        if (IsPlayerRecord(rec)) {
            // The two buffers the record owns; its FNames own none.
            Bytes data{};
            std::memcpy(&data, rec + g_layout.actorDataOff, sizeof(data));
            if (data.data) R::EngineFree(data.data);
            R::FString sub{};
            std::memcpy(&sub, rec + g_layout.classOff + kSoftSubPath, sizeof(sub));
            if (sub.Data) R::EngineFree(sub.Data);
            ++removed;
            continue;
        }
        if (kept != i) std::memcpy(RecordAt(r, kept), rec, static_cast<size_t>(g_layout.stride));
        ++kept;
    }
    if (removed) {
        std::memset(RecordAt(r, kept), 0, static_cast<size_t>(removed) * g_layout.stride);
        std::memcpy(static_cast<uint8_t*>(save) + g_layout.arrayOff + 8, &kept, sizeof(kept));  // TArray::Num
    }
    return removed;
}

void* PlayerClass(void* save) {
    if (void* const cls = R::FindClass(P::name::MainPlayerClass)) return cls;
    const Records r = RecordsOf(save);
    for (int32_t i = 0; i < r.num; ++i) {
        const uint8_t* const rec = RecordAt(r, i);
        if (!IsPlayerRecord(rec)) continue;
        const std::wstring path = ClassPathOf(rec);
        if (void* const cls = ue_wrap::asset_load::LoadObjectByPath(path.c_str())) return cls;
        UE_LOGW("player_record: the player class '%ls' did not load", path.c_str());
        return nullptr;
    }
    return nullptr;
}

int SetPropertiesInSave(void* save, const std::vector<std::pair<std::wstring, std::wstring>>& props) {
    const Records r = RecordsOf(save);
    if (!ResolveLayout(save)) return -1;
    void* cls = nullptr;
    int n = 0;
    for (int32_t i = 0; i < r.num; ++i) {
        uint8_t* const rec = RecordAt(r, i);
        if (!IsPlayerRecord(rec)) continue;
        if (!cls && !(cls = PlayerClass(save))) return -1;
        const Bytes b = ReadBytes(rec + g_layout.actorDataOff);
        std::vector<uint8_t> bytes(b.data, b.data + b.num);
        for (const auto& [prop, value] : props) {
            if (!SetPropertyInBytes(cls, bytes, prop, value)) {
                UE_LOGW("player_record: %ls did not write into the player's record", prop.c_str());
                return -1;
            }
        }
        if (!ReplaceActorData(rec, bytes)) return -1;
        ++n;
    }
    return n;
}

bool CopyStartupEquipment(void* save) {
    if (!save || !R::IsLive(save)) return false;
    void* const fn = R::FindFunction(R::ClassOf(save), L"reset_player_equipment");
    if (!fn) {
        UE_LOGW("player_record: %ls.reset_player_equipment does not resolve", R::ClassNameOf(save).c_str());
        return false;
    }
    ParamFrame f(fn);
    return Call(save, f);
}

}  // namespace ue_wrap::player_record
