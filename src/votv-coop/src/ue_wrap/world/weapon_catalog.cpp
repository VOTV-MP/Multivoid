// ue_wrap/world/weapon_catalog.cpp -- see ue_wrap/world/weapon_catalog.h.
//
// Build = walk `list_weapons`' RowMap for {row name -> damage, the largest material multiplier, whether
// it swings}, then verify every damage against the reflected column (ue_wrap/engine/data_table reads
// both). Every member resolved by name off the table's RowStruct.

#include "ue_wrap/world/weapon_catalog.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/data_table.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <unordered_map>
#include <vector>

namespace ue_wrap::weapon_catalog {
namespace {

namespace R  = ue_wrap::reflection;
namespace DT = ue_wrap::data_table;

// A TArray<float> head inside a row.
struct FloatArrayRaw {
    const float* Data;
    int32_t      Num;
    int32_t      Max;
};

ue_wrap::CachedObjRef g_table;  // a cooked asset, so world-stamp-exempt
std::unordered_map<std::wstring, Swing> g_rows;

// SOFT: the table or a reflection path is not resolvable yet, retried on a throttle, since the table's
// lookup walks every object. HARD: a verdict about the layout or the data (members missing, the gate
// disagreeing, duplicate keys), which no retry changes.
enum class Outcome { Never, Soft, Hard, Valid };
Outcome  g_outcome = Outcome::Never;
uint64_t g_lastAttemptMs = 0;
constexpr uint64_t kRebuildThrottleMs = 3000;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Lowercased: an FName compares case-insensitively.
std::wstring Key(const std::wstring& s) {
    std::wstring k;
    k.reserve(s.size());
    for (wchar_t c : s) k.push_back((c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c);
    return k;
}

void Build() {
    g_outcome = Outcome::Soft;  // until a verdict or success says otherwise
    g_lastAttemptMs = NowMs();
    g_rows.clear();

    if (!g_table.Alive()) g_table.Set(R::FindObject(L"list_weapons", L"DataTable"));
    void* table = g_table.Raw();
    if (!table) return;
    void* rowStruct = DT::RowStruct(table);
    if (!rowStruct) {
        UE_LOGE("weapon_catalog: list_weapons' RowStruct did not resolve -- catalog INVALID");
        return;
    }
    const int32_t offDamage  = R::FindPropertyOffsetByPrefix(rowStruct, L"damage_");
    const int32_t offMontage = R::FindPropertyOffsetByPrefix(rowStruct, L"montage_");
    const int32_t offAttack  = R::FindPropertyOffsetByPrefix(rowStruct, L"attack_");
    const int32_t offMult    = R::FindPropertyOffsetByPrefix(rowStruct, L"matEffDmg_");
    if (offDamage < 0 || offMontage < 0 || offAttack < 0 || offMult < 0) {
        UE_LOGE("weapon_catalog: row struct members did not resolve (damage@%d montage@%d attack@%d "
                "matEffDmg@%d) -- catalog INVALID", offDamage, offMontage, offAttack, offMult);
        g_outcome = Outcome::Hard;
        return;
    }

    std::vector<DT::RowRef> refs;
    if (!DT::Rows(table, refs, "weapon_catalog")) return;
    std::vector<float> walkDamage;
    walkDamage.reserve(refs.size());
    int swings = 0;
    std::wstring swingList;  // said once, with the build: the table's own numbers
    for (const DT::RowRef& ref : refs) {
        const float damage = *reinterpret_cast<const float*>(ref.row + offDamage);
        const FloatArrayRaw mult = *reinterpret_cast<const FloatArrayRaw*>(ref.row + offMult);
        float largest = 1.f;
        if (mult.Data && mult.Num > 0 && mult.Num < 1024)
            for (int32_t i = 0; i < mult.Num; ++i)
                if (std::isfinite(mult.Data[i]) && mult.Data[i] > largest) largest = mult.Data[i];
        Swing sw;
        sw.canSwing = *reinterpret_cast<void* const*>(ref.row + offMontage) != nullptr &&
                      *reinterpret_cast<const uint8_t*>(ref.row + offAttack) != 0;
        sw.maxDamage = damage * largest;
        if (sw.canSwing) {
            ++swings;
            if (swingList.size() < 600) {
                wchar_t one[96];
                ::swprintf(one, 96, L" %ls %.0f", R::ToString(ref.key).c_str(), sw.maxDamage);
                swingList += one;
            }
        }
        walkDamage.push_back(damage);
        g_rows.emplace(Key(R::ToString(ref.key)), sw);
    }
    if (g_rows.size() != refs.size()) {
        UE_LOGE("weapon_catalog: %zu rows collapsed to %zu keys -- duplicate row names, catalog INVALID",
                refs.size(), g_rows.size());
        g_rows.clear();
        g_outcome = Outcome::Hard;
        return;
    }

    // The gate: the same damages, read by the engine through reflection alone.
    std::vector<std::wstring> column;
    const std::wstring mangled = DT::MemberName(rowStruct, L"damage_");
    if ((mangled.empty() || !DT::ColumnAsStrings(table, mangled, column)) &&
        !DT::ColumnAsStrings(table, L"damage", column)) {
        UE_LOGE("weapon_catalog: the damage column could not be read, so the walk cannot be verified -- "
                "catalog INVALID");
        g_rows.clear();
        return;
    }
    if (column.size() != walkDamage.size()) {
        UE_LOGE("weapon_catalog: gate length mismatch -- walk=%zu column=%zu -- catalog INVALID",
                walkDamage.size(), column.size());
        g_rows.clear();
        g_outcome = Outcome::Hard;
        return;
    }
    for (size_t i = 0; i < column.size(); ++i) {
        const float c = static_cast<float>(::_wtof(column[i].c_str()));
        if (std::fabs(c - walkDamage[i]) > 1e-3f * std::fmax(1.f, std::fabs(walkDamage[i]))) {
            UE_LOGE("weapon_catalog: gate DISAGREES at row %zu (walk=%.3f column=%.3f) -- the RowMap layout "
                    "assumption is wrong on this build; catalog INVALID", i, walkDamage[i], c);
            g_rows.clear();
            g_outcome = Outcome::Hard;
            return;
        }
    }
    g_outcome = Outcome::Valid;
    UE_LOGI("weapon_catalog: %zu rows, %d swing, verified against the reflected damage column; the most one "
            "swing deals:%ls", g_rows.size(), swings, swingList.c_str());
}

}  // namespace

bool Ready() {
    if (g_outcome == Outcome::Valid && g_table.Alive()) return true;
    if (g_outcome == Outcome::Hard) return false;
    if (g_outcome == Outcome::Soft && NowMs() - g_lastAttemptMs < kRebuildThrottleMs) return false;
    Build();
    return g_outcome == Outcome::Valid;
}

bool Lookup(const std::wstring& itemName, Swing& out) {
    out = Swing{};
    if (!Ready()) return false;
    auto it = g_rows.find(Key(itemName));
    if (it != g_rows.end()) out = it->second;
    return true;
}

}  // namespace ue_wrap::weapon_catalog
