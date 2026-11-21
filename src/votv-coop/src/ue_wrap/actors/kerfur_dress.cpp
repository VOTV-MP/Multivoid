// ue_wrap/actors/kerfur_dress.cpp -- see ue_wrap/actors/kerfur_dress.h.

#include "ue_wrap/actors/kerfur_dress.h"

#include "ue_wrap/actors/kerfur.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/data_table.h"
#include "ue_wrap/engine/world_identity.h"

#include <cstdint>
#include <cstring>
#include <unordered_map>

namespace ue_wrap::kerfur_dress {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;
namespace DT = ue_wrap::data_table;

constexpr int32_t kMaxRows = 64;   // a drip longer than this is unreadable garbage

struct ArrayView {
    R::FName* data;
    int32_t num;
    int32_t max;
};

int32_t DripOffset(void* kerfur) {
    static int32_t sOff = -2;
    if (sOff == -2) sOff = R::FindPropertyOffset(R::ClassOf(kerfur), L"drip");
    return sOff;
}

int32_t BonesOffset(void* kerfur) {
    static int32_t sOff = -2;
    if (sOff == -2) sOff = R::FindPropertyOffset(R::ClassOf(kerfur), L"dripBones");
    return sOff;
}

bool CallNamed(void* obj, const wchar_t* fn) {
    void* const f = R::FindDispatchFunctionCached(R::ClassOf(obj), fn);
    if (!f) return false;
    ue_wrap::ParamFrame pf(f);
    return pf.valid() && ue_wrap::Call(obj, pf);
}

void* AnimInstanceOf(void* kerfur) {
    void* const mesh = *reinterpret_cast<void**>(static_cast<uint8_t*>(kerfur) + P::off::ACharacter_Mesh);
    if (!mesh || !R::IsLive(mesh)) return nullptr;
    void* const anim = *reinterpret_cast<void**>(static_cast<uint8_t*>(mesh) + P::off::USkeletalMesh_AnimScriptInstance);
    return anim && R::IsLive(anim) ? anim : nullptr;
}

bool ReadNames(void* kerfur, int32_t off, std::vector<std::wstring>& out) {
    out.clear();
    if (!ue_wrap::kerfur::IsKerfurActor(kerfur) || off < 0) return false;
    const auto* a = reinterpret_cast<const ArrayView*>(static_cast<const uint8_t*>(kerfur) + off);
    if (a->num < 0 || a->num > kMaxRows || (a->num > 0 && !a->data)) return false;
    for (int32_t i = 0; i < a->num; ++i) out.push_back(R::ToString(a->data[i]));
    return true;
}

std::wstring Lowered(const std::wstring& s) {
    std::wstring k = s;
    for (wchar_t& c : k)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    return k;
}

// list_kerfurDrip, read once per table object: the rows' `type` and `accessoryName`, by lowercase row name.
struct RowTable {
    ue_wrap::CachedObjRef table;
    std::unordered_map<std::wstring, RowFacts> rows;
    bool built = false;
    void* failedIn = nullptr;   // the world a failed read was made in: not walked or said again there
};
RowTable g_rowTable;

bool ReadRowTable() {
    g_rowTable.built = false;
    g_rowTable.rows.clear();
    void* const table = R::FindObject(L"list_kerfurDrip", L"DataTable");
    void* const rowStruct = table ? DT::RowStruct(table) : nullptr;
    if (!rowStruct) return false;
    const int32_t offType = R::FindPropertyOffsetByPrefix(rowStruct, L"type_");
    const int32_t offBone = R::FindPropertyOffsetByPrefix(rowStruct, L"accessoryName_");
    if (offType < 0 || offBone < 0) {
        UE_LOGE("kerfur_dress: list_kerfurDrip's row members did not resolve (type@%d accessoryName@%d)", offType, offBone);
        return false;
    }
    std::vector<DT::RowRef> refs;
    if (!DT::Rows(table, refs, "kerfur_dress")) return false;
    for (const DT::RowRef& ref : refs) {
        RowFacts f;
        f.found = true;
        f.type = ref.row[offType];
        f.bone = R::ToString(*reinterpret_cast<const R::FName*>(ref.row + offBone));
        g_rowTable.rows[Lowered(R::ToString(ref.key))] = f;
    }
    g_rowTable.table.Set(table);
    g_rowTable.built = true;
    UE_LOGI("kerfur_dress: list_kerfurDrip read, %zu rows", g_rowTable.rows.size());
    return true;
}

bool BuildRows() {
    if (g_rowTable.built && g_rowTable.table.Alive()) return true;
    void* const world = ue_wrap::world_identity::CurrentWorld();
    if (world && g_rowTable.failedIn == world) return false;
    if (ReadRowTable()) {
        g_rowTable.failedIn = nullptr;
        return true;
    }
    g_rowTable.failedIn = world;
    return false;
}

// The pat's montage: held as a slot-validated reference, found by one walk. A miss is latched for the world it was made in
// (a null world, boot or travel, is not latched), so a montage that is not loaded costs one walk per world, not one per pat.
void* PatMontage() {
    static ue_wrap::CachedObjRef sMontage;
    static void* sMissedIn = nullptr;
    if (sMontage.Alive()) return sMontage.Get();
    void* const world = ue_wrap::world_identity::CurrentWorld();
    if (world && sMissedIn == world) return nullptr;
    void* const found = R::FindObject(L"kerfurOmegaV1_Skeleton_Montage", L"AnimMontage");
    sMontage.Set(found);
    sMissedIn = found ? nullptr : world;
    return found;
}

}  // namespace

bool ReadDrip(void* kerfur, std::vector<std::wstring>& out) {
    out.clear();
    return ue_wrap::kerfur::IsKerfurActor(kerfur) && ReadNames(kerfur, DripOffset(kerfur), out);
}

bool ReadDripBones(void* kerfur, std::vector<std::wstring>& out) {
    out.clear();
    return ue_wrap::kerfur::IsKerfurActor(kerfur) && ReadNames(kerfur, BonesOffset(kerfur), out);
}

bool ApplyDrip(void* kerfur, const std::vector<std::wstring>& names) {
    if (!ue_wrap::kerfur::IsKerfurActor(kerfur) || names.size() > static_cast<size_t>(kMaxRows)) return false;
    const int32_t off = DripOffset(kerfur);
    if (off < 0) return false;
    auto* a = reinterpret_cast<ArrayView*>(static_cast<uint8_t*>(kerfur) + off);
    R::FName* buf = nullptr;
    if (!names.empty()) {
        buf = static_cast<R::FName*>(R::EngineAlloc(names.size() * sizeof(R::FName)));
        if (!buf) return false;
        for (size_t i = 0; i < names.size(); ++i) buf[i] = ue_wrap::fname_utils::StringToFName(names[i]);
    }
    if (a->data) R::EngineFree(a->data);
    a->data = buf;
    a->num = a->max = static_cast<int32_t>(names.size());
    return CallNamed(kerfur, L"updateDrip");
}

bool Unequip(void* kerfur, const std::wstring& name) {
    if (!ue_wrap::kerfur::IsKerfurActor(kerfur) || name.empty()) return false;
    void* const f = R::FindDispatchFunctionCached(R::ClassOf(kerfur), L"unequipItem");
    if (!f) return false;
    ue_wrap::ParamFrame pf(f);
    if (!pf.valid()) return false;
    const int32_t n = static_cast<int32_t>(name.size()) + 1;
    struct { const wchar_t* data; int32_t num; int32_t max; } fs{name.c_str(), n, n};
    return pf.SetRaw(L"inputPin", &fs, sizeof(fs)) && ue_wrap::Call(kerfur, pf);
}

bool PutOn(void* kerfur, void* player, void* holdObject, const std::wstring& row) {
    if (!ue_wrap::kerfur::IsKerfurActor(kerfur) || !player || row.empty()) return false;
    ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(kerfur), L"playerUsedOn"));
    const R::FName held = ue_wrap::fname_utils::StringToFName(row);
    // The hit and the component stay empty: the Omega's body reads neither.
    return f.valid() && f.Set<void*>(L"player", player) && f.Set<void*>(L"holdObject", holdObject) &&
           f.SetRaw(L"holdPropName", &held, sizeof(held)) && ue_wrap::Call(kerfur, f);
}

bool ReadRow(const std::wstring& row, RowFacts& out) {
    out = RowFacts{};
    if (!BuildRows()) return false;
    const auto it = g_rowTable.rows.find(Lowered(row));
    if (it != g_rowTable.rows.end()) out = it->second;
    return true;
}

bool PlayPat(void* kerfur) {
    if (!ue_wrap::kerfur::IsKerfurActor(kerfur)) return false;
    static void* sPlay = nullptr;
    static void* sJump = nullptr;
    void* const montage = PatMontage();
    if (!sPlay || !sJump) {
        if (void* const cls = R::FindClass(L"AnimInstance")) {
            sPlay = R::FindFunction(cls, L"Montage_Play");
            sJump = R::FindFunction(cls, L"Montage_JumpToSection");
        }
    }
    void* const anim = AnimInstanceOf(kerfur);
    if (!anim || !montage || !sPlay) {
        static bool sSaid = false;
        if (!sSaid) {
            sSaid = true;
            UE_LOGW("kerfur_dress: the pat montage did not resolve (anim=%p montage=%p play=%p)", anim, montage, sPlay);
        }
        return false;
    }
    ue_wrap::ParamFrame pf(sPlay);
    const bool played = pf.valid() && pf.Set<void*>(L"MontageToPlay", montage) && pf.Set<float>(L"InPlayRate", 1.f) &&
                        ue_wrap::Call(anim, pf);
    if (played && sJump) {
        const R::FName section = ue_wrap::fname_utils::StringToFName(L"pat");
        ue_wrap::ParamFrame jf(sJump);
        if (jf.valid() && jf.SetRaw(L"SectionName", &section, sizeof(section)) && jf.Set<void*>(L"Montage", montage))
            ue_wrap::Call(anim, jf);
    }
    return played;
}

bool IsPatPlaying(void* kerfur) {
    if (!ue_wrap::kerfur::IsKerfurActor(kerfur)) return false;
    static void* sIsPlaying = nullptr;
    if (!sIsPlaying)
        if (void* const cls = R::FindClass(L"AnimInstance")) sIsPlaying = R::FindFunction(cls, L"Montage_IsPlaying");
    void* const anim = AnimInstanceOf(kerfur);
    void* const montage = PatMontage();
    if (!anim || !montage || !sIsPlaying) return false;
    ue_wrap::ParamFrame pf(sIsPlaying);
    return pf.valid() && pf.Set<void*>(L"Montage", montage) && ue_wrap::Call(anim, pf) && pf.Get<bool>(L"ReturnValue");
}

bool CanPat(void* kerfur) {
    uint8_t state = 0, face = 0;
    bool spooky = false;
    if (!ue_wrap::kerfur::ReadKerfurState(kerfur, state, spooky, face)) return false;
    return state != 3 && state != 4 && state != 5;
}

bool RefreshViewer(void* kerfur) {
    static R::InstanceOffset sViewer{L"viewer"};
    static R::InstanceOffset sCapturing{L"capturingACtor"};
    if (!ue_wrap::kerfur::IsKerfurActor(kerfur)) return false;
    const int32_t vOff = sViewer.Of(kerfur);
    if (vOff < 0) return false;
    void* const viewer = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(kerfur) + vOff);
    if (!viewer || !R::IsLive(viewer)) return false;
    // The Omega's `viewer` is never cleared when the screen closes; the screen is open on this Omega only while the one
    // viewer still captures it.
    const int32_t cOff = sCapturing.Of(viewer);
    if (cOff < 0 || *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(viewer) + cOff) != kerfur) return false;
    return CallNamed(viewer, L"genList");
}

}  // namespace ue_wrap::kerfur_dress
