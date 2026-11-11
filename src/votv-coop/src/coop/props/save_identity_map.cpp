// coop/props/save_identity_map.cpp -- see header. Phase 1B: HOST-side build + log, NO wire.

#include "coop/props/save_identity_map.h"

#include "coop/element/element.h"          // ElementId, kInvalidId
#include "coop/creatures/kerfur_entity.h"            // IsKerfurPropClass (UClass* form)
#include "coop/props/prop_element_tracker.h"     // CollectTrackedPileTransforms / CollectTrackedKerfurTransforms
#include "ue_wrap/core/hot_path_guard.h"        // UE_ASSERT_GAME_THREAD
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"            // FindClass, IsDescendantOfAny, NameEquals, ClassNameOf
#include "ue_wrap/world/world_singleton.h"
#include "ue_wrap/core/types.h"                 // ue_wrap::FVector

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

namespace coop::save_identity_map {

namespace R  = ue_wrap::reflection;
namespace PT = coop::prop_element_tracker;

namespace {

// Both save arrays, betterObjectsData and betterPrimitivesData, are TArray<struct_bettersave>. A
// record's class is a soft class reference (TSoftClassPtr: a weak pointer, a tag, then the
// FSoftObjectPath whose AssetPathName names "/Game/...Pkg.Class_C"), its transform an FTransform
// (rotation quat, then the translation), its key an FName. The member offsets are read off the
// struct by reflection; the stride is its size on the FTransform's 16-byte alignment.
constexpr size_t kSoftPathNameOff  = 0x10;  // FSoftObjectPtr: FWeakObjectPtr(8) + tag(4) + pad(4)
constexpr size_t kTransformTransOff = 0x10;  // FTransform: FQuat Rotation(16), then Translation
struct RecordLayout {
    void*   cls = nullptr;  // the saveSlot_C class it was resolved for
    int32_t objects = -1, primitives = -1;
    int32_t stride = 0, klass = -1, transform = -1, key = -1;
};
RecordLayout g_layout;

const RecordLayout& LayoutFor(void* saveSlot) {
    void* cls = R::ClassOf(saveSlot);
    if (cls == g_layout.cls) return g_layout;
    g_layout = RecordLayout{};
    g_layout.cls = cls;
    g_layout.objects = R::FindPropertyOffset(cls, L"betterObjectsData");
    g_layout.primitives = R::FindPropertyOffset(cls, L"betterPrimitivesData");
    if (void* rec = R::ArrayInnerStruct(cls, L"betterObjectsData")) {
        g_layout.stride = (R::StructSize(rec) + 15) & ~15;
        g_layout.klass = R::FindPropertyOffsetByPrefix(rec, L"class_");
        g_layout.transform = R::FindPropertyOffsetByPrefix(rec, L"transform_");
        g_layout.key = R::FindPropertyOffsetByPrefix(rec, L"key_");
    }
    if (R::ArrayInnerStruct(cls, L"betterPrimitivesData") != R::ArrayInnerStruct(cls, L"betterObjectsData"))
        g_layout.primitives = -1;  // not the record type the walk reads
    UE_LOGI("save_identity_map: record layout objects@%d primitives@%d stride=%d class@%d transform@%d key@%d",
            g_layout.objects, g_layout.primitives, g_layout.stride, g_layout.klass, g_layout.transform,
            g_layout.key);
    return g_layout;
}

// A record's class, from its soft reference's asset path: the class named after the '.', when it is
// loaded (every class a live world holds is). Null for an empty reference or an unloaded class.
void* RecordClass(const uint8_t* rec, int32_t klassOff) {
    const R::FName path = *reinterpret_cast<const R::FName*>(rec + klassOff + kSoftPathNameOff);
    if (R::NameEquals(path, L"None")) return nullptr;
    const std::wstring s = R::ToString(path);
    const size_t dot = s.rfind(L'.');
    return R::FindClass(dot == std::wstring::npos ? s.c_str() : s.c_str() + dot + 1);
}

// Quantize a world location to a 0.01-unit grid so the host-local join tolerates any sub-0.01 FP jitter
// between saveObjects' record transform (-> betterObjectsData) and TryGetActorLocation (-> the eid bridge) while keeping
// distinct piles distinct. Two props within the quantum collapse into one bucket -> resolved by the
// same-class-same-location tiebreak (a deterministic rank-pairing; benign -- they are physically identical).
struct LocKey {
    int32_t x, y, z;
    bool operator==(const LocKey& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct LocKeyHash {
    size_t operator()(const LocKey& k) const {
        return (static_cast<size_t>(static_cast<uint32_t>(k.x)) * 73856093u) ^
               (static_cast<size_t>(static_cast<uint32_t>(k.y)) * 19349663u) ^
               (static_cast<size_t>(static_cast<uint32_t>(k.z)) * 83492791u);
    }
};
LocKey Quantize_(const ue_wrap::FVector& v) {
    return LocKey{static_cast<int32_t>(std::lround(v.X * 100.0)),
                  static_cast<int32_t>(std::lround(v.Y * 100.0)),
                  static_cast<int32_t>(std::lround(v.Z * 100.0))};
}

// One quantized-location bucket: the host eids of each keyless family whose live actor sits at this location,
// sorted ASCENDING. The save-array walk consumes them via a cursor so M identical entries at one location
// pair to the M eids by RANK -- a deterministic bijection (see the tiebreak note in BuildHostMap).
struct Bucket {
    std::vector<uint32_t> chip;   size_t chipCur = 0;
    std::vector<uint32_t> kerfur; size_t kerfurCur = 0;
};

}  // namespace

int BuildHostMap(IdMap& outMap) {
    UE_ASSERT_GAME_THREAD("save_identity_map::BuildHostMap");
    outMap.clear();

    // 1. Resolve saveSlot -- the persistent gamemode member (alive post-capture; save_capture.cpp:57 reads it
    //    the same way). Its Fstruct_save arrays are what loadObjects replays; reading THEM (not a fresh
    //    GetAllActorsWithInterface re-gather) is the assumption-free fix -- the array index IS the cross-peer
    //    ordinal. The 2b smoke FALSIFIED the live-gather order; the bind tripwire caught it at k=0.
    void* gm = ue_wrap::world_singleton::Gamemode();
    if (!gm) { UE_LOGW("save_identity_map: cannot build -- gamemode not found"); return 0; }
    static R::InstanceOffset sSaveSlot(L"save_slot");
    const int32_t saveOff = sSaveSlot.Of(gm);
    void* saveSlot = saveOff >= 0 ? *reinterpret_cast<void* const*>(reinterpret_cast<const uint8_t*>(gm) + saveOff)
                                  : nullptr;
    if (!saveSlot) { UE_LOGW("save_identity_map: cannot build -- save_slot null (no capture yet?)"); return 0; }
    const RecordLayout& L = LayoutFor(saveSlot);
    if (L.objects < 0 || L.stride <= 0 || L.klass < 0 || L.transform < 0 || L.key < 0) {
        UE_LOGE("save_identity_map: the save record layout did not resolve -- no map, no bind");
        return 0;
    }

    // 2. Build the location->eid bridge from the LIVE keyless actors ONCE (self-seeding -- the same idempotent
    //    mint CollectTracked{Pile,Kerfur}Transforms do; they ran at this capture instant). eid->location is
    //    inverted into per-location, per-family ASCENDING eid buckets, CONSUMED across all arrays via a cursor.
    void* chipBase = R::FindClass(L"actorChipPile_C");
    std::unordered_map<coop::element::ElementId, ue_wrap::FVector> pileXf, kerfurXf;
    PT::CollectTrackedPileTransforms(pileXf);
    PT::CollectTrackedKerfurTransforms(kerfurXf);
    std::unordered_map<LocKey, Bucket, LocKeyHash> byLoc;
    for (const auto& kv : pileXf)   byLoc[Quantize_(kv.second)].chip.push_back(static_cast<uint32_t>(kv.first));
    for (const auto& kv : kerfurXf) byLoc[Quantize_(kv.second)].kerfur.push_back(static_cast<uint32_t>(kv.first));
    for (auto& kv : byLoc) {
        std::sort(kv.second.chip.begin(), kv.second.chip.end());
        std::sort(kv.second.kerfur.begin(), kv.second.kerfur.end());
    }

    // 3. Parse the two save arrays IN THE CLIENT'S LOAD ORDER and concatenate their keyless chip/kerfur entries:
    //    betterObjectsData (the off-kerfurs + keyed world objects) is replayed by mainGamemode_C::loadObjects
    //    FIRST, then betterPrimitivesData (the chipPiles, which ignoreSave keeps out of the objects array) by its
    //    `load primitives`. The emitted SEQUENCE == the client's keyless fresh-spawn order. TIEBREAK (same-class-
    //    same-location): M identical entries at one quantized location (M>1) pair to the M ascending-sorted eids
    //    by RANK (i-th entry <-> i-th-smallest eid) -- a deterministic BIJECTION, BENIGN because the M props are
    //    physically identical (any consistent pairing is correct; the eid stays the identity).
    struct ArraySpec { int32_t off; const wchar_t* name; };
    const ArraySpec kArrays[] = {
        { L.objects,    L"betterObjectsData" },     // off-kerfurs and keyed world objects
        { L.primitives, L"betterPrimitivesData" },  // chipPiles
    };
    const size_t stride = static_cast<size_t>(L.stride);
    int chip = 0, kerfur = 0, missed = 0, ambiguousLocs = 0, nonKeyless = 0, totalRecords = 0;
    uint32_t ordinal = 0;  // running keyless ordinal across arrays (== the client bind's k; logged as index)
    for (const ArraySpec& spec : kArrays) {
        if (spec.off < 0) { UE_LOGW("save_identity_map: %ls unresolved -- skipped", spec.name); continue; }
        const uint8_t* hdr = reinterpret_cast<const uint8_t*>(saveSlot) + spec.off;
        void* const data = *reinterpret_cast<void* const*>(hdr + 0x0);
        const int32_t num = *reinterpret_cast<const int32_t*>(hdr + 0x8);
        if (!data || num <= 0) { UE_LOGI("save_identity_map: %ls empty (num=%d) -- skipped", spec.name, num); continue; }
        totalRecords += num;
        for (int32_t i = 0; i < num; ++i) {
            const uint8_t* e = reinterpret_cast<const uint8_t*>(data) + static_cast<size_t>(i) * stride;
            void* cls = RecordClass(e, L.klass);
            if (!cls) continue;
            Family fam;
            if (chipBase && R::IsDescendantOfAny(cls, &chipBase, 1)) fam = Family::ChipPile;
            else if (coop::kerfur_entity::IsKerfurPropClass(cls))    fam = Family::KerfurOff;
            else continue;  // not a chip/kerfur-prop class
            // Filter by CLASS (chip/kerfur-PROP lineage == the client's class-based thunk classification). For
            // chipPile the key is a DIAGNOSTIC only (nonKeyless) -- piles are genuinely keyless (ordinal pair).
            // For kerfurOff the key is the PAIRING IDENTITY (sidecar v3): it is the portable save key both peers
            // load from this same blob, so the client pairs its loading off-kerfur to this eid BY KEY (the
            // cursor-float retire regression fix). A key==None off-kerfur (untracked/jUuC) leaves the entry
            // keyless -> the client falls back to the ordinal cursor for it (unchanged from v2 behaviour).
            const R::FName key = *reinterpret_cast<const R::FName*>(e + L.key);
            const bool keyed = !R::NameEquals(key, L"None");
            if (keyed) ++nonKeyless;
            std::wstring entryKey;
            if (fam == Family::KerfurOff && keyed) entryKey = R::ToString(key);

            const ue_wrap::FVector loc = *reinterpret_cast<const ue_wrap::FVector*>(e + L.transform + kTransformTransOff);
            auto it = byLoc.find(Quantize_(loc));
            std::vector<uint32_t>* vec = nullptr; size_t* curp = nullptr;
            if (it != byLoc.end()) {
                if (fam == Family::ChipPile) { vec = &it->second.chip;   curp = &it->second.chipCur; }
                else                         { vec = &it->second.kerfur; curp = &it->second.kerfurCur; }
            }
            if (!vec || *curp >= vec->size()) {
                ++missed;  // saved entry with no live keyless twin at this loc -- surfaced below, never mis-bound
                continue;
            }
            if (*curp == 0 && vec->size() > 1) ++ambiguousLocs;  // M>1 identical entries at this loc (count once)
            const uint32_t eid = (*vec)[(*curp)++];              // rank-pairing tiebreak (i-th entry <-> i-th eid)
            // sidecar v2: carry the save-time position (the `loc` already read above -- the SAME value the
            // host-local join used) so the client can re-bind a GC-churned save native by position at quiescence.
            // sidecar v3: carry the kerfurOff portable key (`entryKey`, empty for chipPile) so the client pairs
            // the keyed family native<->eid BY KEY (cross-peer-stable) instead of by the load-order cursor.
            outMap.push_back(IdEntry{ordinal++, eid, static_cast<uint8_t>(fam), loc.X, loc.Y, loc.Z,
                                     std::move(entryKey)});
            if (fam == Family::ChipPile) ++chip; else ++kerfur;
        }
    }

    // Bridge-drain diagnostic: count live keyless eids NOT consumed by a save entry (a live actor with no save
    // record at its location -> it would be unmapped; expect 0 if every keyless native is saved + matched).
    int undrained = 0;
    for (const auto& kv : byLoc) {
        undrained += static_cast<int>(kv.second.chip.size() - kv.second.chipCur);
        undrained += static_cast<int>(kv.second.kerfur.size() - kv.second.kerfurCur);
    }
    UE_LOGI("save_identity_map: HOST map built (Phase A, parse save arrays) -- %zu entries (%d chipPile + %d "
            "kerfurOff) from %d records across betterObjectsData+betterPrimitivesData [%d non-None key (diag), %d unmatched, "
            "%d undrained bridge eid(s), %d ambiguous same-loc]. index = keyless ordinal (== client loadObjects "
            "spawn order); eid via host-local class+location join (bridge: %zu pile + %zu kerfur live eids).",
            outMap.size(), chip, kerfur, totalRecords, nonKeyless, missed, undrained, ambiguousLocs,
            pileXf.size(), kerfurXf.size());
    // Sample the first + last few entries so the host log shows the objIndex->eid->family pairs concretely.
    const size_t n = outMap.size();
    for (size_t j = 0; j < n; ++j) {
        if (j < 5 || j + 5 >= n) {
            const IdEntry& en = outMap[j];
            UE_LOGI("save_identity_map:   [%zu] ordinal=%u eid=%u family=%s key='%ls'", j, en.index, en.eid,
                    en.family == static_cast<uint8_t>(Family::ChipPile) ? "chipPile" : "kerfurOff",
                    en.key.c_str());  // sidecar v3: key non-empty for kerfurOff (the cross-peer pairing identity)
        } else if (j == 5 && n > 10) {
            UE_LOGI("save_identity_map:   ... %zu more ...", n - 10);
        }
    }
    return static_cast<int>(outMap.size());
}

// ---- Phase 2 sidecar wire framing ----------------------------------------------------------------------

namespace {
void AppendU32_(std::vector<uint8_t>& v, uint32_t x) {
    uint8_t b[4];
    std::memcpy(b, &x, 4);  // little-endian (x64); same-endian raw as the rest of the protocol
    v.insert(v.end(), b, b + 4);
}
void AppendF32_(std::vector<uint8_t>& v, float x) {
    uint8_t b[4];
    std::memcpy(b, &x, 4);  // raw IEEE-754 little-endian, same-endian as the rest of the protocol
    v.insert(v.end(), b, b + 4);
}
}  // namespace

void SerializeSidecar(const IdMap& map, std::vector<uint8_t>& out) {
    out.clear();
    const uint32_t count = static_cast<uint32_t>(map.size());
    out.reserve(kSidecarHeaderBytes + static_cast<size_t>(count) * (kSidecarFixedEntryBytes + 8));  // +8 ~= avg key
    out.insert(out.end(), kSidecarMagic, kSidecarMagic + 4);
    AppendU32_(out, kSidecarVersion);
    AppendU32_(out, count);
    for (const IdEntry& e : map) {
        AppendU32_(out, e.index);
        AppendU32_(out, e.eid);
        out.push_back(e.family);
        AppendF32_(out, e.savePosX);  // sidecar v2
        AppendF32_(out, e.savePosY);
        AppendF32_(out, e.savePosZ);
        // sidecar v3: portable save key (ASCII keys -- off-kerfur keys are alphanumeric identifiers; byte-narrow
        // each wchar like the protocol's className[64]). u16 length-prefixed; 0 for a keyless chipPile entry.
        const uint16_t keyLen = static_cast<uint16_t>(e.key.size() > 0xFFFFu ? 0xFFFFu : e.key.size());
        out.push_back(static_cast<uint8_t>(keyLen & 0xFF));
        out.push_back(static_cast<uint8_t>((keyLen >> 8) & 0xFF));
        for (uint16_t i = 0; i < keyLen; ++i) out.push_back(static_cast<uint8_t>(e.key[i] & 0xFF));
    }
}

bool DeserializeSidecar(const uint8_t* data, size_t len, IdMap& outMap, size_t& consumed) {
    outMap.clear();
    consumed = 0;
    if (!data || len < kSidecarHeaderBytes) return false;
    if (std::memcmp(data, kSidecarMagic, 4) != 0) return false;  // not our framing
    uint32_t ver = 0, count = 0;
    std::memcpy(&ver, data + 4, 4);
    std::memcpy(&count, data + 8, 4);
    if (ver != kSidecarVersion) return false;
    // sidecar v3: entries are VARIABLE-length, because the key trails each one, so the parse walks
    // and bounds-checks every entry rather than trusting a fixed stride. Any field or key that
    // would read past `len` aborts the whole parse and the caller treats it as no map.
    //
    // `count` is an unvalidated wire u32, and this reserve once ran BEFORE that bounds-checked
    // walk: a hostile host announced 0xFFFFFFFF in a 40-byte sidecar and OOM-killed the joining
    // client on the net thread. Since an entry is a fixed part plus a trailing key, the satisfiable
    // entry count has a hard CEILING of the remaining bytes over the fixed part, and anything above
    // it cannot be honoured by any body. The same shape the walk already uses for keyLen at its
    // `end - p` check.
    const size_t maxEntries = (len - kSidecarHeaderBytes) / kSidecarFixedEntryBytes;
    if (count > maxEntries) return false;  // unsatisfiable count -- malformed or hostile
    outMap.reserve(count);
    const uint8_t* p   = data + kSidecarHeaderBytes;
    const uint8_t* end = data + len;
    for (uint32_t i = 0; i < count; ++i) {
        if (static_cast<size_t>(end - p) < kSidecarFixedEntryBytes) { outMap.clear(); return false; }  // truncated
        IdEntry e{};
        std::memcpy(&e.index, p, 4);
        std::memcpy(&e.eid, p + 4, 4);
        e.family = p[8];
        std::memcpy(&e.savePosX, p + 9, 4);   // sidecar v2
        std::memcpy(&e.savePosY, p + 13, 4);
        std::memcpy(&e.savePosZ, p + 17, 4);
        const uint16_t keyLen = static_cast<uint16_t>(p[21] | (static_cast<uint16_t>(p[22]) << 8));  // sidecar v3
        p += kSidecarFixedEntryBytes;
        if (static_cast<size_t>(end - p) < keyLen) { outMap.clear(); return false; }  // truncated key
        if (keyLen > 0) {
            e.key.reserve(keyLen);
            for (uint16_t j = 0; j < keyLen; ++j) e.key.push_back(static_cast<wchar_t>(p[j]));  // ASCII widen
            p += keyLen;
        }
        outMap.push_back(std::move(e));
    }
    consumed = static_cast<size_t>(p - data);
    return true;
}

void LogReceivedMap(const IdMap& map) {
    int chip = 0, kerfur = 0;
    for (const IdEntry& e : map) {
        if (e.family == static_cast<uint8_t>(Family::ChipPile)) ++chip; else ++kerfur;
    }
    UE_LOGI("save_identity_map: CLIENT received sidecar map (Phase 2a transport checkpoint, NO bind) -- "
            "%zu entries (%d chipPile + %d kerfurOff). Should match the HOST BuildHostMap count + eids "
            "(diff the first/last 5 index->eid pairs against the host log).",
            map.size(), chip, kerfur);
    const size_t n = map.size();
    for (size_t j = 0; j < n; ++j) {
        if (j < 5 || j + 5 >= n) {
            const IdEntry& e = map[j];
            UE_LOGI("save_identity_map:   rx[%zu] index=%u eid=%u family=%s key='%ls'", j, e.index, e.eid,
                    e.family == static_cast<uint8_t>(Family::ChipPile) ? "chipPile" : "kerfurOff", e.key.c_str());
        } else if (j == 5 && n > 10) {
            UE_LOGI("save_identity_map:   ... %zu more ...", n - 10);
        }
    }
}

}  // namespace coop::save_identity_map
