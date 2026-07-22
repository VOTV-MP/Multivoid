// ue_wrap/actors/save_record.cpp -- see ue_wrap/actors/save_record.h.
//
// Extracted verbatim from inventory.cpp 2026-07-22 (behavior preserved; the field-offset
// table, the strides, the plausibility gates and the per-group lambdas are unchanged).

#include "ue_wrap/actors/save_record.h"

#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/fstring_utils.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/desk/signal_dynamic.h"

#include <cstring>

namespace ue_wrap::save_record {
namespace {

namespace R = ue_wrap::reflection;

// Fstruct_save FIELD layout (struct_save.hpp; raw size 0xF8, ARRAY STRIDE 0x100 -- see
// kSaveStride in the header). class@0 + transform@0x10 + key@0x40 + 11 groups.
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

}  // namespace

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

void WriteArrHeader(void* base, int32_t off, void* data, int32_t num) {
    uint8_t* p = reinterpret_cast<uint8_t*>(base) + off;
    std::memcpy(p, &data, sizeof(void*));
    std::memcpy(p + 8, &num, 4);
    std::memcpy(p + 12, &num, 4);
}

void* AllocZeroed(size_t count, size_t elemSize) {
    if (!count) return nullptr;
    void* buf = R::EngineAlloc(count * elemSize);
    if (buf) std::memset(buf, 0, count * elemSize);
    return buf;
}

std::wstring ReadFNameAt(const void* base, int32_t off) {
    R::FName n{};
    std::memcpy(&n, reinterpret_cast<const uint8_t*>(base) + off, sizeof(n));
    // ComparisonIndex 0 is ALWAYS NAME_None (regardless of Number) -- ToString on it can
    // null-deref the name pool. Also reject an implausibly huge index (garbage at a bad read).
    if (n.ComparisonIndex == 0 || n.ComparisonIndex > 0x0FFFFFFF) return {};
    return R::ToString(n);
}

void WriteFNameField(void* dst, const std::wstring& leaf) {
    R::FName n{0, 0};  // NAME_None for empty
    if (!leaf.empty()) n = ue_wrap::fname_utils::StringToFName(leaf);
    std::memcpy(dst, &n, sizeof(n));
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

}  // namespace ue_wrap::save_record
