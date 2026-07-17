// ue_wrap/inventory.cpp -- see ue_wrap/inventory.h.

#include "ue_wrap/actors/inventory.h"

#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/fstring_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/desk/signal_dynamic.h"
#include "ue_wrap/core/types.h"

#include <cstdint>
#include <cstring>

namespace ue_wrap::inventory {
namespace {

namespace R = ue_wrap::reflection;

// saveSlot.hpp player-scoped arrays.
constexpr int32_t kOff_inventoryData = 0x02E0;  // TArray<Fstruct_save>
constexpr int32_t kOff_equipment     = 0x0440;  // TArray<Fstruct_equipment>
constexpr int32_t kOff_hold          = 0x0450;  // TArray<Fstruct_equipment>

// Fstruct_save FIELD layout (struct_save.hpp; raw size 0xF8, ARRAY STRIDE 0x100 -- see kSaveStride
// below). class@0 + transform@0x10 + key@0x40 + 11 groups.
constexpr int32_t kSave_class      = 0x00;
constexpr int32_t kSave_transform  = 0x10;
constexpr int32_t kSave_key        = 0x40;
constexpr int32_t kSave_bools      = 0x48;
constexpr int32_t kSave_floats     = 0x58;
constexpr int32_t kSave_ints       = 0x68;
constexpr int32_t kSave_strings    = 0x78;
constexpr int32_t kSave_signals    = 0x88;  // TArray<Fstruct_signalDataDynamic> (flat)
constexpr int32_t kSave_classes    = 0x98;
constexpr int32_t kSave_vectors    = 0xA8;
constexpr int32_t kSave_rotators   = 0xB8;
constexpr int32_t kSave_transforms = 0xC8;
constexpr int32_t kSave_bytes      = 0xD8;
constexpr int32_t kSave_names      = 0xE8;
// ARRAY ELEMENT STRIDE = the 16-ALIGNED struct size, NOT the dump's raw `Size: 0xF8` line.
// Fstruct_save embeds an FTransform (16-aligned FQuat) at 0x10, so the struct's alignment is 16
// and its TArray element stride / embedded-field size is Align(0xF8,16) = 0x100. This is the
// SAME value the SDK reports for every embedded Fstruct_save field (struct_equipment.hpp data
// @0x10..0x110 = 0x100; saveSlot.hpp:38 drone size 0x100; dirthole_item/drone/eriePlush/kerfur).
// The engine's native loadObjects() iterates inventoryData at THIS stride; a mismatch makes it
// read element i+1's class at the wrong byte -> wild UClass* deref + heap over-read. (Using the
// raw 0xF8 was a real N>=2 crash, caught by the 2026-06-14 adversarial verify panel.) The dump's
// bottom-line `Size:` is PropertiesSize; the per-field embedded `size:` annotation is the stride.
constexpr int32_t kSaveStride      = 0x100;
constexpr int32_t kMxStride        = 0x10;   // Fstruct_mX wraps a single TArray<X> @ +0; 0x10 already 16-aligned

// Fstruct_equipment FIELD layout (struct_equipment.hpp; raw size 0x118, ARRAY STRIDE 0x120 -- see
// kEquipStride below).
constexpr int32_t kEquip_propName  = 0x00;   // Fstruct_propDynamic.name FName
constexpr int32_t kEquip_propKey   = 0x08;   // Fstruct_propDynamic.key  FName
constexpr int32_t kEquip_data      = 0x10;   // embedded Fstruct_save (in a 0x100 slot)
constexpr int32_t kEquip_tag       = 0x110;  // FName
// 16-ALIGNED stride (raw `Size: 0x118` -> Align(0x118,16) = 0x120). Fstruct_equipment embeds the
// 16-aligned Fstruct_save, so it too is 16-aligned. SDK-confirmed: struct_equipmentWear.hpp:6
// reports the embedded Fstruct_equipment as size 0x120 (next field @0x120). Same crash class as
// kSaveStride if the raw 0x118 is used for the equipment/hold arrays with N>=2 elements.
constexpr int32_t kEquipStride     = 0x120;
constexpr int32_t kSignalStride    = 0x70;   // Fstruct_signalDataDynamic; no 16-aligned member -> 0x70 (already /16)

// A corrupt/uninitialized TArray Num must never drive a runaway loop or a giant reserve.
constexpr int32_t kMaxArr = 200000;

void*   g_gm      = nullptr;
int32_t g_offSave = -1;

// (forward) plausibility of a heap/object pointer -- defined just below.
inline bool PlausibleObjPtr(const void* p);

struct Arr { const uint8_t* data = nullptr; int32_t num = 0; };
Arr ReadArr(const void* base, int32_t off) {
    Arr a;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(base) + off;
    std::memcpy(&a.data, p, sizeof(void*));   // TArray::Data
    std::memcpy(&a.num, p + 8, 4);            // TArray::Num
    // Reject a garbage header: negative/huge Num, or a non-empty array whose Data pointer is
    // not a plausible heap pointer (walking it would deref wild memory + fault the GT tick).
    if (a.num < 0 || a.num > kMaxArr || (a.num > 0 && !PlausibleObjPtr(a.data))) {
        a.data = nullptr;
        a.num = 0;
    }
    return a;
}

// A pointer that could plausibly be a live UObject: non-null, above the null page, 8-aligned,
// inside the Win64 user-address range. Rejects garbage misread as a pointer BEFORE the engine
// IsLive/NameOf derefs it (a wild pointer faults the whole game-thread tick).
inline bool PlausibleObjPtr(const void* p) {
    const uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return v >= 0x10000 && (v & 0x7) == 0 && v < 0x7FFFFFFFFFFFull;
}

std::wstring ReadFNameAt(const void* base, int32_t off) {
    R::FName n{};
    std::memcpy(&n, reinterpret_cast<const uint8_t*>(base) + off, sizeof(n));
    // ComparisonIndex 0 is ALWAYS NAME_None (regardless of Number) -- ToString on it can
    // null-deref the name pool. Also reject an implausibly huge index (garbage at a bad read).
    if (n.ComparisonIndex == 0 || n.ComparisonIndex > 0x0FFFFFFF) return {};
    return R::ToString(n);
}
std::wstring ReadFStringAt(const void* base, int32_t off) {
    R::FString s{};
    std::memcpy(&s, reinterpret_cast<const uint8_t*>(base) + off, sizeof(s));
    if (!PlausibleObjPtr(s.Data) || s.Num <= 1 || s.Num > 1000000) return {};
    return std::wstring(s.Data, static_cast<size_t>(s.Num - 1));  // Num includes the null term
}
std::wstring ReadClassNameAt(const void* base, int32_t off) {
    void* cls = nullptr;
    std::memcpy(&cls, reinterpret_cast<const uint8_t*>(base) + off, sizeof(void*));
    if (!PlausibleObjPtr(cls) || !R::IsLive(cls)) return {};
    return R::ToString(R::NameOf(cls));  // leaf name; re-resolved via FindClass on apply
}
std::array<float, 10> ReadXformAt(const void* base, int32_t off) {
    ue_wrap::FTransform t{};
    std::memcpy(&t, reinterpret_cast<const uint8_t*>(base) + off, sizeof(t));  // 48 B
    return {t.RotX, t.RotY, t.RotZ, t.RotW, t.TX, t.TY, t.TZ, t.SX, t.SY, t.SZ};
}

// Read a TArray<Fstruct_mX> at `off` into a vector<vector<X>>: each Fstruct_mX (0x10) wraps a
// TArray<X> at +0; `perElem(elemPtr, innerVec)` appends one X.
template <class V, class F>
void ReadGroups(const void* save, int32_t off, int32_t elemSize, std::vector<V>& out, F perElem) {
    const Arr outer = ReadArr(save, off);
    out.reserve(static_cast<size_t>(outer.num));
    for (int32_t i = 0; i < outer.num; ++i) {
        const uint8_t* mx = outer.data + static_cast<size_t>(i) * kMxStride;
        const Arr inner = ReadArr(mx, 0);
        V group;
        group.reserve(static_cast<size_t>(inner.num));
        for (int32_t j = 0; j < inner.num; ++j)
            perElem(inner.data + static_cast<size_t>(j) * elemSize, group);
        out.push_back(std::move(group));
    }
}

void ReadSaveRecord(const void* base, SaveRecord& rec) {
    rec.className = ReadClassNameAt(base, kSave_class);
    rec.xform     = ReadXformAt(base, kSave_transform);
    rec.key       = ReadFNameAt(base, kSave_key);
    ReadGroups(base, kSave_bools,   1, rec.bools,
               [](const uint8_t* e, std::vector<uint8_t>& g) { g.push_back(*e ? 1 : 0); });
    ReadGroups(base, kSave_floats,  4, rec.floats,
               [](const uint8_t* e, std::vector<float>& g) { float v; std::memcpy(&v, e, 4); g.push_back(v); });
    ReadGroups(base, kSave_ints,    4, rec.ints,
               [](const uint8_t* e, std::vector<int32_t>& g) { int32_t v; std::memcpy(&v, e, 4); g.push_back(v); });
    ReadGroups(base, kSave_strings, 16, rec.strings,
               [](const uint8_t* e, std::vector<std::wstring>& g) { g.push_back(ReadFStringAt(e, 0)); });
    ReadGroups(base, kSave_classes, 8, rec.classes,
               [](const uint8_t* e, std::vector<std::wstring>& g) { g.push_back(ReadClassNameAt(e, 0)); });
    ReadGroups(base, kSave_vectors, 12, rec.vectors,
               [](const uint8_t* e, std::vector<std::array<float, 3>>& g) { std::array<float, 3> v; std::memcpy(v.data(), e, 12); g.push_back(v); });
    ReadGroups(base, kSave_rotators, 12, rec.rotators,
               [](const uint8_t* e, std::vector<std::array<float, 3>>& g) { std::array<float, 3> v; std::memcpy(v.data(), e, 12); g.push_back(v); });
    ReadGroups(base, kSave_transforms, 48, rec.transforms,
               [](const uint8_t* e, std::vector<std::array<float, 10>>& g) { g.push_back(ReadXformAt(e, 0)); });
    ReadGroups(base, kSave_bytes,   1, rec.bytes,
               [](const uint8_t* e, std::vector<uint8_t>& g) { g.push_back(*e); });
    ReadGroups(base, kSave_names,   8, rec.names,
               [](const uint8_t* e, std::vector<std::wstring>& g) { g.push_back(ReadFNameAt(e, 0)); });
    // signals: TArray<Fstruct_signalDataDynamic> directly (reuse the proven 0x70 reader).
    const Arr sig = ReadArr(base, kSave_signals);
    rec.signals.reserve(static_cast<size_t>(sig.num));
    for (int32_t i = 0; i < sig.num; ++i) {
        ue_wrap::signal_dynamic::Row row;
        if (ue_wrap::signal_dynamic::ReadStruct(sig.data + static_cast<size_t>(i) * kSignalStride, row))
            rec.signals.push_back(std::move(row));
    }
}

void ReadEquipRecord(const void* base, EquipRecord& rec) {
    rec.propName = ReadFNameAt(base, kEquip_propName);
    rec.propKey  = ReadFNameAt(base, kEquip_propKey);
    ReadSaveRecord(reinterpret_cast<const uint8_t*>(base) + kEquip_data, rec.data);
    rec.tag      = ReadFNameAt(base, kEquip_tag);
}

}  // namespace

void* ResolveSaveSlot() {
    if (!g_gm || !R::IsLive(g_gm)) g_gm = R::FindObjectByClass(L"mainGamemode_C");
    if (!g_gm) return nullptr;
    if (g_offSave < 0) g_offSave = R::FindPropertyOffset(R::ClassOf(g_gm), L"saveSlot");
    if (g_offSave < 0) return nullptr;
    void* save = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(g_gm) + g_offSave);
    return (save && R::IsLive(save)) ? save : nullptr;
}

bool ReadAll(PlayerInventory& out) {
    out.inventory.clear();
    out.equipment.clear();
    out.hold.clear();
    void* save = ResolveSaveSlot();
    if (!save) return false;
    const Arr inv = ReadArr(save, kOff_inventoryData);
    out.inventory.reserve(static_cast<size_t>(inv.num));
    for (int32_t i = 0; i < inv.num; ++i) {
        SaveRecord r;
        ReadSaveRecord(inv.data + static_cast<size_t>(i) * kSaveStride, r);
        out.inventory.push_back(std::move(r));
    }
    const Arr eq = ReadArr(save, kOff_equipment);
    out.equipment.reserve(static_cast<size_t>(eq.num));
    for (int32_t i = 0; i < eq.num; ++i) {
        EquipRecord r;
        ReadEquipRecord(eq.data + static_cast<size_t>(i) * kEquipStride, r);
        out.equipment.push_back(std::move(r));
    }
    const Arr hd = ReadArr(save, kOff_hold);
    out.hold.reserve(static_cast<size_t>(hd.num));
    for (int32_t i = 0; i < hd.num; ++i) {
        EquipRecord r;
        ReadEquipRecord(hd.data + static_cast<size_t>(i) * kEquipStride, r);
        out.hold.push_back(std::move(r));
    }
    return true;
}

// ===== INCREMENT 4 -- the WRITE side =========================================================
namespace {

// Write a UE TArray header {Data, Num, Max=Num} at base+off. (Max==Num is correct for a
// freshly-built array the engine will read then, on its next mutation, realloc wholesale.)
void WriteArrHeader(void* base, int32_t off, void* data, int32_t num) {
    uint8_t* p = reinterpret_cast<uint8_t*>(base) + off;
    std::memcpy(p, &data, sizeof(void*));
    std::memcpy(p + 8, &num, 4);
    std::memcpy(p + 12, &num, 4);
}

// Allocate `count*elemSize` engine bytes, zeroed. Null (-> caller writes an empty array) on a
// zero count or an EngineAlloc failure (GMalloc unresolved). Zeroing makes every unset FString/
// FName/TArray field a valid empty (null,0,0) -- no garbage the engine could deref.
void* AllocZeroed(size_t count, size_t elemSize) {
    if (!count) return nullptr;
    void* buf = R::EngineAlloc(count * elemSize);
    if (buf) std::memset(buf, 0, count * elemSize);
    return buf;
}

void WriteFNameField(void* dst, const std::wstring& leaf) {
    R::FName n{0, 0};  // NAME_None for empty
    if (!leaf.empty()) n = ue_wrap::fname_utils::StringToFName(leaf);
    std::memcpy(dst, &n, sizeof(n));
}

void WriteClassField(void* dst, const std::wstring& leaf) {
    void* cls = leaf.empty() ? nullptr : R::FindClass(leaf.c_str());
    std::memcpy(dst, &cls, sizeof(void*));
}

// Build the outer TArray<Fstruct_mX> for a value group (vector<vector<X>>): each Fstruct_mX
// (0x10) wraps a TArray<X> at +0; `writeOne(elemPtr, x)` writes one X into a zeroed slot.
template <class V, class F>
void WriteGroups(void* save, int32_t off, int32_t elemSize, const std::vector<V>& groups, F writeOne) {
    void* outer = AllocZeroed(groups.size(), kMxStride);
    if (!outer) { WriteArrHeader(save, off, nullptr, 0); return; }
    for (size_t i = 0; i < groups.size(); ++i) {
        const V& inner = groups[i];
        uint8_t* mx = reinterpret_cast<uint8_t*>(outer) + i * kMxStride;
        void* ib = AllocZeroed(inner.size(), static_cast<size_t>(elemSize));
        if (!ib) { WriteArrHeader(mx, 0, nullptr, 0); continue; }
        for (size_t j = 0; j < inner.size(); ++j)
            writeOne(reinterpret_cast<uint8_t*>(ib) + j * elemSize, inner[j]);
        WriteArrHeader(mx, 0, ib, static_cast<int32_t>(inner.size()));
    }
    WriteArrHeader(save, off, outer, static_cast<int32_t>(groups.size()));
}

void WriteSaveRecord(uint8_t* base, const SaveRecord& r) {
    WriteClassField(base + kSave_class, r.className);
    ue_wrap::FTransform t;  // identity defaults; we set all 10 packed floats
    t.RotX = r.xform[0]; t.RotY = r.xform[1]; t.RotZ = r.xform[2]; t.RotW = r.xform[3];
    t.TX = r.xform[4]; t.TY = r.xform[5]; t.TZ = r.xform[6];
    t.SX = r.xform[7]; t.SY = r.xform[8]; t.SZ = r.xform[9];
    std::memcpy(base + kSave_transform, &t, sizeof(t));
    WriteFNameField(base + kSave_key, r.key);
    WriteGroups(base, kSave_bools, 1, r.bools,
                [](uint8_t* e, uint8_t v) { *e = v ? 1 : 0; });
    WriteGroups(base, kSave_floats, 4, r.floats,
                [](uint8_t* e, float v) { std::memcpy(e, &v, 4); });
    WriteGroups(base, kSave_ints, 4, r.ints,
                [](uint8_t* e, int32_t v) { std::memcpy(e, &v, 4); });
    WriteGroups(base, kSave_strings, 16, r.strings,
                [](uint8_t* e, const std::wstring& v) { ue_wrap::fstring_utils::MintFString(v, e); });
    WriteGroups(base, kSave_classes, 8, r.classes,
                [](uint8_t* e, const std::wstring& v) { WriteClassField(e, v); });
    WriteGroups(base, kSave_vectors, 12, r.vectors,
                [](uint8_t* e, const std::array<float, 3>& v) { std::memcpy(e, v.data(), 12); });
    WriteGroups(base, kSave_rotators, 12, r.rotators,
                [](uint8_t* e, const std::array<float, 3>& v) { std::memcpy(e, v.data(), 12); });
    WriteGroups(base, kSave_transforms, 48, r.transforms,
                [](uint8_t* e, const std::array<float, 10>& v) {
                    ue_wrap::FTransform tt;
                    tt.RotX = v[0]; tt.RotY = v[1]; tt.RotZ = v[2]; tt.RotW = v[3];
                    tt.TX = v[4]; tt.TY = v[5]; tt.TZ = v[6];
                    tt.SX = v[7]; tt.SY = v[8]; tt.SZ = v[9];
                    std::memcpy(e, &tt, sizeof(tt));
                });
    WriteGroups(base, kSave_bytes, 1, r.bytes,
                [](uint8_t* e, uint8_t v) { *e = v; });
    WriteGroups(base, kSave_names, 8, r.names,
                [](uint8_t* e, const std::wstring& v) { WriteFNameField(e, v); });
    // signals: a FLAT TArray<Fstruct_signalDataDynamic> (0x70). Reuse the proven live writer
    // (engine-mints the row FStrings, interns the FNames, zeroes image).
    void* sb = AllocZeroed(r.signals.size(), kSignalStride);
    if (!sb) { WriteArrHeader(base, kSave_signals, nullptr, 0); return; }
    for (size_t i = 0; i < r.signals.size(); ++i)
        // Ignore the bool: the slot is AllocZeroed, so a partial mint (KismetStringLibrary
        // unresolved) leaves a valid empty row (null FStrings / NAME_None / empty image) -- a
        // safe blank fallback, never garbage. We never crash a whole inventory over one signal.
        ue_wrap::signal_dynamic::WriteStructLive(
            reinterpret_cast<uint8_t*>(sb) + i * kSignalStride, r.signals[i]);
    WriteArrHeader(base, kSave_signals, sb, static_cast<int32_t>(r.signals.size()));
}

void WriteEquipRecord(uint8_t* base, const EquipRecord& r) {
    WriteFNameField(base + kEquip_propName, r.propName);
    WriteFNameField(base + kEquip_propKey, r.propKey);
    WriteSaveRecord(base + kEquip_data, r.data);  // embedded Fstruct_save (0x100 slot)
    WriteFNameField(base + kEquip_tag, r.tag);
}

// Build one engine TArray<RecordT> from `recs` and overwrite the header at saveSlot+off. The
// old buffer is intentionally orphaned (see ApplyToSaveObject's contract).
template <class RecordT, class F>
void BuildAndSwapArray(void* saveSlot, int32_t off, int32_t stride,
                       const std::vector<RecordT>& recs, F writeOne) {
    const int32_t num = static_cast<int32_t>(recs.size());
    void* buf = AllocZeroed(static_cast<size_t>(num), static_cast<size_t>(stride));
    if (buf) {
        for (int32_t i = 0; i < num; ++i)
            writeOne(reinterpret_cast<uint8_t*>(buf) + static_cast<size_t>(i) * stride, recs[i]);
        WriteArrHeader(saveSlot, off, buf, num);
    } else {
        WriteArrHeader(saveSlot, off, nullptr, 0);  // num==0 or alloc failed -> empty
    }
}

}  // namespace

bool ApplyToSaveObject(void* saveSlot, const PlayerInventory& inv) {
    if (!saveSlot || !R::IsLive(saveSlot)) return false;
    if (void* probe = R::EngineAlloc(16)) {  // GMalloc probe: a failure -> we'd silently write
        R::EngineFree(probe);                // empty arrays over a real inventory; refuse instead
    } else {
        UE_LOGW("inventory: ApplyToSaveObject -- EngineAlloc unavailable (GMalloc unresolved); "
                "refusing to write empty inventory over saveSlot %p", saveSlot);
        return false;
    }
    BuildAndSwapArray(saveSlot, kOff_inventoryData, kSaveStride, inv.inventory,
                      [](uint8_t* e, const SaveRecord& r) { WriteSaveRecord(e, r); });
    BuildAndSwapArray(saveSlot, kOff_equipment, kEquipStride, inv.equipment,
                      [](uint8_t* e, const EquipRecord& r) { WriteEquipRecord(e, r); });
    BuildAndSwapArray(saveSlot, kOff_hold, kEquipStride, inv.hold,
                      [](uint8_t* e, const EquipRecord& r) { WriteEquipRecord(e, r); });
    UE_LOGI("inventory: ApplyToSaveObject(%p) wrote inventory=%zu equipment=%zu hold=%zu",
            saveSlot, inv.inventory.size(), inv.equipment.size(), inv.hold.size());
    return true;
}

}  // namespace ue_wrap::inventory
