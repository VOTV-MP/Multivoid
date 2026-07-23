// coop/dev/director/dup_verifier.cpp -- the container-race NO-DUP verifier (see dup_verifier.h).
//
// Self-contained instrument: it reads the raw global saveSlot.GObjStack + the player stores itself, by
// content SIGNATURE, NOT through container_contents_sync (the subsystem whose correctness it measures).
// It COUNTS + prints every matching row; the positive control (a solo take -> count must be 1) is what
// makes count==1 on a race mean "no dup" and not "instrument blind".

#include "coop/dev/director/dup_verifier.h"

#include "ue_wrap/actors/inventory.h"     // ResolveSaveSlot, ReadAll, PlayerInventory
#include "ue_wrap/actors/save_record.h"   // SaveRecord, ReadSaveRecord, ReadArr, kMxStride, kSaveStride
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace coop::director {
namespace {
namespace R   = ue_wrap::reflection;
namespace SR  = ue_wrap::save_record;
namespace INV = ue_wrap::inventory;

constexpr uint64_t kFnvOff = 0xcbf29ce484222325ULL;
constexpr uint64_t kFnvPr  = 0x100000001b3ULL;
uint64_t FnvBytes(uint64_t h, const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= kFnvPr; }
    return h;
}
uint64_t FnvStr(uint64_t h, const std::wstring& s) { return FnvBytes(h, s.data(), s.size() * sizeof(wchar_t)); }
uint64_t FnvU32(uint64_t h, uint32_t v) { return FnvBytes(h, &v, sizeof(v)); }

// A canonical content hash over the value groups (position/xform DELIBERATELY excluded -- position is
// never identity). Hashes each group's sizes + contents in a fixed order so equal items hash equal.
uint64_t ContentHash(const SR::SaveRecord& r) {
    uint64_t h = kFnvOff;
    auto vv_bytes = [&](const std::vector<std::vector<uint8_t>>& g){ h=FnvU32(h,(uint32_t)g.size()); for(auto&v:g){h=FnvU32(h,(uint32_t)v.size()); h=FnvBytes(h,v.data(),v.size());} };
    auto vv_i32   = [&](const std::vector<std::vector<int32_t>>& g){ h=FnvU32(h,(uint32_t)g.size()); for(auto&v:g){h=FnvU32(h,(uint32_t)v.size()); h=FnvBytes(h,v.data(),v.size()*sizeof(int32_t));} };
    auto vv_f32   = [&](const std::vector<std::vector<float>>& g){ h=FnvU32(h,(uint32_t)g.size()); for(auto&v:g){h=FnvU32(h,(uint32_t)v.size()); h=FnvBytes(h,v.data(),v.size()*sizeof(float));} };
    auto vv_wstr  = [&](const std::vector<std::vector<std::wstring>>& g){ h=FnvU32(h,(uint32_t)g.size()); for(auto&v:g){h=FnvU32(h,(uint32_t)v.size()); for(auto&s:v) h=FnvStr(h,s);} };
    vv_bytes(r.bools); vv_f32(r.floats); vv_i32(r.ints); vv_wstr(r.strings);
    vv_wstr(r.classes); vv_wstr(r.names); vv_bytes(r.bytes);
    auto vv_vec3  = [&](const std::vector<std::vector<std::array<float,3>>>& g){ h=FnvU32(h,(uint32_t)g.size()); for(auto&v:g){h=FnvU32(h,(uint32_t)v.size()); h=FnvBytes(h,v.data(),v.size()*sizeof(std::array<float,3>));} };
    vv_vec3(r.vectors); vv_vec3(r.rotators);
    h=FnvU32(h,(uint32_t)r.transforms.size()); for(auto&v:r.transforms){ h=FnvU32(h,(uint32_t)v.size()); h=FnvBytes(h,v.data(),v.size()*sizeof(std::array<float,10>)); }
    h=FnvU32(h,(uint32_t)r.signals.size());   // signal rows: hash count only (Row is opaque here)
    return h;
}

// Read the container's propInventory GObjStack slice base (the TArray<Fstruct_save> for its index).
// Self-contained: resolves the offsets directly, not via the sync lane. Null if unresolvable.
const uint8_t* ContainerSliceBase(void* containerActor, int32_t& outNum) {
    outNum = 0;
    if (!containerActor) return nullptr;
    const int32_t offPropInv = R::FindPropertyOffset(R::ClassOf(containerActor), L"propInventory");
    if (offPropInv < 0) return nullptr;
    void* inv = nullptr;
    std::memcpy(&inv, reinterpret_cast<const uint8_t*>(containerActor) + offPropInv, sizeof(inv));
    if (!inv || !R::IsLive(inv)) return nullptr;
    void* save = INV::ResolveSaveSlot();
    if (!save) return nullptr;
    const int32_t offStack = R::FindPropertyOffset(R::ClassOf(save), L"GObjStack");
    const int32_t offIndex = R::FindPropertyOffset(R::ClassOf(inv), L"Index");
    if (offStack < 0 || offIndex < 0) return nullptr;
    int32_t idx = -1;
    std::memcpy(&idx, reinterpret_cast<const uint8_t*>(inv) + offIndex, sizeof(idx));
    if (idx < 0) return nullptr;
    const SR::Arr outer = SR::ReadArr(save, offStack);
    if (idx >= outer.num) return nullptr;
    const uint8_t* slice = outer.data + static_cast<size_t>(idx) * SR::kMxStride;   // Fstruct_mObject
    const SR::Arr inner = SR::ReadArr(slice, 0);   // TArray<Fstruct_save> @ +0
    outNum = inner.num;
    return inner.data;
}
}  // namespace

ItemSig SigOf(const SR::SaveRecord& rec) {
    ItemSig s;
    s.className = rec.className;
    s.key = rec.key;
    s.contentHash = ContentHash(rec);
    s.valid = !rec.className.empty();   // a null-class record is not a real item
    return s;
}

ItemSig CaptureContainerSlotSig(void* containerActor, int32_t slotIdx) {
    int32_t num = 0;
    const uint8_t* base = ContainerSliceBase(containerActor, num);
    if (!base || slotIdx < 0 || slotIdx >= num) return {};
    SR::SaveRecord rec;
    SR::ReadSaveRecord(base + static_cast<size_t>(slotIdx) * SR::kSaveStride, rec);
    const ItemSig s = SigOf(rec);
    UE_LOGI("dup_verifier: captured X from container slot %d -- cls=%ls key=%ls hash=%016llx valid=%d",
            slotIdx, s.className.c_str(), s.key.c_str(), static_cast<unsigned long long>(s.contentHash), s.valid ? 1 : 0);
    return s;
}

int CountItemInstances(const ItemSig& x, bool print) {
    if (!x.valid) { UE_LOGW("dup_verifier: CountItemInstances called with an INVALID signature -- refusing"); return -1; }
    void* save = INV::ResolveSaveSlot();
    if (!save) { UE_LOGW("dup_verifier: no saveSlot -- cannot count"); return -1; }

    int gobj = 0, player = 0, scannedRows = 0, scannedSlices = 0;

    // (1) The WHOLE global GObjStack -- every propInventory index (world containers AND, per
    // lesson_container_contents_live_in_one_global_gobjstack, player inventories carry a `player` flag
    // but live in the SAME store). Walk every outer element's inner TArray<Fstruct_save>.
    const int32_t offStack = R::FindPropertyOffset(R::ClassOf(save), L"GObjStack");
    if (offStack >= 0) {
        const SR::Arr outer = SR::ReadArr(save, offStack);
        for (int32_t i = 0; i < outer.num; ++i) {
            const uint8_t* slice = outer.data + static_cast<size_t>(i) * SR::kMxStride;
            const SR::Arr inner = SR::ReadArr(slice, 0);
            if (inner.num > 0) ++scannedSlices;
            for (int32_t j = 0; j < inner.num; ++j) {
                SR::SaveRecord rec;
                SR::ReadSaveRecord(inner.data + static_cast<size_t>(j) * SR::kSaveStride, rec);
                ++scannedRows;
                if (SigOf(rec) == x) {
                    ++gobj;
                    if (print) UE_LOGI("dup_verifier:   MATCH GObjStack[idx=%d][slot=%d] cls=%ls key=%ls",
                                       i, j, rec.className.c_str(), rec.key.c_str());
                }
            }
        }
    } else {
        UE_LOGW("dup_verifier: could not resolve saveSlot.GObjStack offset -- GObjStack count is BLIND");
    }

    // (2) The player stores read a DIFFERENT way (saveSlot.inventoryData/equipment/hold) -- a
    // cross-check against the GObjStack walk. If X shows here but NOT in GObjStack, the "player inv is in
    // GObjStack" premise is incomplete for this build and these must be ADDED to the authoritative count;
    // the positive control (phaseA/phaseB below == 1) is what decides whether player double-counts GObjStack.
    INV::PlayerInventory pinv;
    if (INV::ReadAll(pinv)) {
        int k = 0;
        for (const auto& r : pinv.inventory) { if (SigOf(r) == x) { ++player; if (print) UE_LOGI("dup_verifier:   MATCH inventoryData[%d] cls=%ls key=%ls", k, r.className.c_str(), r.key.c_str()); } ++k; }
        for (const auto& e : pinv.equipment) if (SigOf(e.data) == x) { ++player; if (print) UE_LOGI("dup_verifier:   MATCH equipment cls=%ls key=%ls", e.data.className.c_str(), e.data.key.c_str()); }
        for (const auto& e : pinv.hold)      if (SigOf(e.data) == x) { ++player; if (print) UE_LOGI("dup_verifier:   MATCH hold cls=%ls key=%ls", e.data.className.c_str(), e.data.key.c_str()); }
    }

    UE_LOGI("dup_verifier: COUNT X(cls=%ls key=%ls) -- GObjStack=%d player=%d (scanned %d rows in %d non-empty slices)",
            x.className.c_str(), x.key.c_str(), gobj, player, scannedRows, scannedSlices);
    // Primary count = GObjStack (the authoritative store per the lesson). `player` is logged as a
    // cross-check for the control to interpret (0 => disjoint / player IS a GObjStack slice; >0 => a
    // separate store to fold in). The control run resolves the topology before any race verdict.
    return gobj;
}

}  // namespace coop::director
