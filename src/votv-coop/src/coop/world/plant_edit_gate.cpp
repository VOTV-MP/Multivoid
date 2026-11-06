// coop/world/plant_edit_gate.cpp -- see coop/world/plant_edit_gate.h.

#include "coop/world/plant_edit_gate.h"

#include "coop/element/intent_authority.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/roster_ledger.h"  // PerSlotState
#include "coop/props/prop_key_index.h"
#include "coop/world/plant_sync.h"

#include "ue_wrap/actors/growing_plant.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"

#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::plant_edit_gate {
namespace {

namespace GP = ue_wrap::growing_plant;
namespace R = ue_wrap::reflection;
using Payload = coop::net::PlantStatePayload;

// A client's copy can be a poll behind the host, so only what its player changed may replace the host's value.
void MergeEdit(const Payload& p, GP::Values& v) {
    namespace PE = coop::net::plant_edit;
    if (p.editMask & PE::kGrow) v.grow = p.grow;
    if (p.editMask & PE::kFertilizer) v.fertilizer = p.fertilizer;
    if (p.editMask & PE::kWater) v.water = p.water;
    if (p.editMask & PE::kFruitGrow) v.fruitGrow = p.fruitGrow;
    if (p.editMask & PE::kDryness) v.dryness = p.dryness;
    if (p.editMask & PE::kAmount) v.amount = p.amount;
}

// The reach a sender's edit is judged against: a player's look-at trace is mainPlayer.armLength, 200 by default, so twice that,
// as the door's verbs are (door_verb_intent). The authority adds the plant's own bounds radius and a 600 uu allowance for the
// puppet's pose lagging the sender, so the bound refuses a plant the sender cannot be near and never a reach the game allows.
constexpr float kPlantReachUU = 400.0f;

// The bounds, each a value the game's own writers stay inside.
constexpr float kMaxFraction = 1.0f;       // grow and fruitGrow are fractions: the plant's own tests are >= 1
constexpr float kMaxWater = 20000.0f;      // rain adds only below 3600 and a can sets 600
constexpr float kMaxFertilizer = 20000.0f; // a fertilizer sets 600 and a poo 1200
constexpr float kMinDryness = -1.0f;       // -1 is a dead plant
constexpr float kMaxDryness = 20000.0f;    // the plant dies at its row's maxDryness, 10800 by default; twice that
constexpr int32_t kMaxAmount = 64;         // the berries of a ripe bush

// A sender's edits wait for the next poll; the queue is bounded since one that never gets a body would grow without end.
constexpr size_t kEditsPerSlotCap = 256;

enum Field : int { kFGrow, kFFertilizer, kFWater, kFFruitGrow, kFDryness, kFAmount, kFieldCount };
constexpr const char* kFieldName[kFieldCount] = {"grow", "fertilizer", "water", "fruitGrow", "dryness", "amount"};

struct EditSlot {
    std::vector<Payload> queue;
    bool fullSaid = false;   // a refused push, said once a streak
    bool waitSaid = false;   // the wait for the sender's body, said once a streak
};
coop::roster_ledger::PerSlotState<EditSlot> g_editSlots;

uint64_t g_refusedClass = 0, g_refusedReach = 0, g_clamped = 0;
uint64_t g_refusedKey = 0, g_refusedRow = 0, g_refusedParent = 0;
bool g_clampSaid[kFieldCount] = {};
bool g_keySaid = false, g_rowSaid = false, g_parentSaid = false;

// A refusal that is counted every time and said the first time of a session.
void RefuseOnce(uint64_t& count, bool& said, uint8_t slot, const wchar_t* what, const std::wstring& key, const wchar_t* why,
                const std::wstring& named) {
    ++count;
    if (said) return;
    said = true;
    UE_LOGW("plant_edit_gate: slot %u's %ls of '%ls' refused -- %ls '%ls' (said once a session)", static_cast<unsigned>(slot), what,
            key.c_str(), why, named.c_str());
}

template <class T>
void Bound(T& v, T lo, T hi, Field f, uint8_t slot) {
    const T cut = v < lo ? lo : (v > hi ? hi : v);
    if (cut == v) return;
    ++g_clamped;
    if (!g_clampSaid[f]) {
        g_clampSaid[f] = true;
        UE_LOGW("plant_edit_gate: slot %u's %s of %.1f is cut to %.1f (said once a session per field)", static_cast<unsigned>(slot),
                kFieldName[f], static_cast<double>(v), static_cast<double>(cut));
    }
    v = cut;
}

// The values this edit applies: all of a planting's, the masked ones of an edit of a plant the host holds.
void BoundEdit(Payload& p, bool planting, uint8_t slot) {
    namespace PE = coop::net::plant_edit;
    if (planting || (p.editMask & PE::kGrow)) Bound(p.grow, 0.f, kMaxFraction, kFGrow, slot);
    if (planting || (p.editMask & PE::kFertilizer)) Bound(p.fertilizer, 0.f, kMaxFertilizer, kFFertilizer, slot);
    if (planting || (p.editMask & PE::kWater)) Bound(p.water, 0.f, kMaxWater, kFWater, slot);
    if (planting || (p.editMask & PE::kFruitGrow)) Bound(p.fruitGrow, 0.f, kMaxFraction, kFFruitGrow, slot);
    if (planting || (p.editMask & PE::kDryness)) Bound(p.dryness, kMinDryness, kMaxDryness, kFDryness, slot);
    if (planting || (p.editMask & PE::kAmount)) Bound(p.amount, int32_t{0}, kMaxAmount, kFAmount, slot);
}

enum class Gate : uint8_t { Pass, Wait, Refused };

// Wait: the host has no body for the sender yet (its first pose has not arrived), so its reach cannot be measured and the edit
// stays queued for the body, as the door's verbs do. Refused is counted and said, for its first three and every twentieth.
Gate Judge(const coop::element::IntentSubject& subject, EditSlot& sl, uint8_t slot, const wchar_t* what, const std::wstring& key) {
    using coop::element::IntentOutcome;
    if (subject.outcome == IntentOutcome::NoBody) {
        if (!sl.waitSaid) {
            sl.waitSaid = true;
            UE_LOGI("plant_edit_gate: slot %u's plant edits wait: the host has no body for it yet", static_cast<unsigned>(slot));
        }
        return Gate::Wait;
    }
    sl.waitSaid = false;
    if (subject) return Gate::Pass;
    if (++g_refusedReach <= 3 || g_refusedReach % 20 == 0)
        UE_LOGI("plant_edit_gate: slot %u's %ls of '%ls' refused -- %s (%.0f uu of %.0f) (#%llu)", static_cast<unsigned>(slot), what,
                key.c_str(), coop::element::OutcomeName(subject.outcome), subject.distUU, subject.reachUU,
                static_cast<unsigned long long>(g_refusedReach));
    return Gate::Refused;
}

// False keeps the edit at its sender's queue head: the host has no body for the sender yet. Anything else is applied or
// refused, and consumed.
bool ApplyEdit(coop::net::Session& s, EditSlot& sl, uint8_t slot, Payload p, std::unordered_map<std::wstring, void*>& index,
               std::vector<std::wstring>& changed) {
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    if (key.empty()) {
        RefuseOnce(g_refusedKey, g_keySaid, slot, p.alive ? L"edit" : L"removal", key, L"it names no key:", key);
        return true;
    }
    const auto it = index.find(key);
    void* plant = it == index.end() ? nullptr : it->second;
    const coop::element::IntentTarget reach = coop::element::IntentTarget::ForClientIntent(s, slot, kPlantReachUU);
    if (!p.alive) {
        if (!plant) return true;   // the host holds none by that key
        const Gate gate = Judge(reach.Authorize(plant), sl, slot, L"removal", key);
        if (gate == Gate::Wait) return false;
        if (gate == Gate::Refused) return true;
        GP::Destroy(plant);
        index.erase(key);
        UE_LOGI("plant_edit_gate: a client's player removed plant '%ls'", key.c_str());
        return true;
    }
    const bool planting = plant == nullptr;
    void* plantClass = nullptr;
    if (planting) {
        // The class is the one the host's own list_farmPlants row plants, never one the wire names: the wire's name must be
        // that class's, and the class a plant class. A name from the wire starts no lookup of its own.
        const std::wstring className = coop::plant_sync::StringFromClassName(p.cls);
        const std::wstring row = coop::net::StringFromWireKey(p.row);
        if (!GP::IsFarmRow(row)) {
            RefuseOnce(g_refusedRow, g_rowSaid, slot, L"planting", key, L"no such list_farmPlants row", row);
            return true;
        }
        plantClass = GP::FarmRowPlantClass(row);
        if (!plantClass || !GP::IsPlantClass(plantClass) || _wcsicmp(R::ToString(R::NameOf(plantClass)).c_str(), className.c_str()) != 0) {
            if (++g_refusedClass <= 3 || g_refusedClass % 20 == 0)
                UE_LOGW("plant_edit_gate: slot %u's planting of '%ls' refused -- row '%ls' plants no plant class named '%ls' (#%llu)",
                        static_cast<unsigned>(slot), key.c_str(), row.c_str(), className.c_str(),
                        static_cast<unsigned long long>(g_refusedClass));
            return true;
        }
        const ue_wrap::FVector place{p.x, p.y, p.z};
        const Gate gate = Judge(reach.AuthorizeSegment(place, place), sl, slot, L"planting", key);
        if (gate == Gate::Wait) return false;
        if (gate == Gate::Refused) return true;
        // A plant's attachParent is its owner's key, so a pot's planting names the pot. A planting in a dirt hole would name
        // the hole, which belongs to the bury lane (not synced): refusing it here is intended, not a miss.
        const std::wstring parent = coop::net::StringFromWireKey(p.attachParent);
        if (!parent.empty()) {
            void* pot = coop::prop_key_index::Find(parent);
            if (!pot || !GP::IsPotClass(R::ClassOf(pot))) {
                RefuseOnce(g_refusedParent, g_parentSaid, slot, L"planting", key, L"the host holds no pot by key", parent);
                return true;
            }
        }
    } else {
        const Gate gate = Judge(reach.Authorize(plant), sl, slot, L"edit", key);
        if (gate == Gate::Wait) return false;
        if (gate == Gate::Refused) return true;
        // A changed row is one of the host's own, as a planting's is.
        const std::wstring row = coop::net::StringFromWireKey(p.row);
        if ((p.editMask & coop::net::plant_edit::kRow) && !GP::IsFarmRow(row)) {
            RefuseOnce(g_refusedRow, g_rowSaid, slot, L"edit", key, L"no such list_farmPlants row", row);
            return true;
        }
    }
    BoundEdit(p, planting, slot);
    if (planting) {
        plant = coop::plant_sync::Materialize(p, plantClass);   // the row's class, judged a plant class above
        if (!plant) {
            UE_LOGW("plant_edit_gate: a client's plant '%ls' of '%ls' did not spawn", key.c_str(),
                    coop::plant_sync::StringFromClassName(p.cls).c_str());
            return true;
        }
        index[key] = plant;
        UE_LOGI("plant_edit_gate: a client's player planted '%ls', row '%ls'", key.c_str(), coop::net::StringFromWireKey(p.row).c_str());
    }
    GP::Values v = coop::plant_sync::ValuesOf(p);
    if (!planting && GP::Read(plant, v)) MergeEdit(p, v);
    const bool takeRow = planting || (p.editMask & coop::net::plant_edit::kRow) != 0;
    GP::Apply(plant, takeRow ? coop::net::StringFromWireKey(p.row) : GP::Row(plant), v);
    changed.push_back(key);
    return true;
}

}  // namespace

void ApplyEdits(coop::net::Session& s, std::unordered_map<std::wstring, void*>& index, std::vector<std::wstring>& changed) {
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        EditSlot& sl = g_editSlots[slot];
        size_t done = 0;
        while (done < sl.queue.size() && ApplyEdit(s, sl, slot, sl.queue[done], index, changed)) ++done;
        if (done == 0) continue;
        sl.queue.erase(sl.queue.begin(), sl.queue.begin() + static_cast<std::ptrdiff_t>(done));
        sl.fullSaid = false;
    }
}

void OnEdit(uint8_t senderSlot, const coop::net::PlantStatePayload& p) {
    if (senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    EditSlot& sl = g_editSlots[senderSlot];
    if (sl.queue.size() >= kEditsPerSlotCap) {
        if (!sl.fullSaid) {
            sl.fullSaid = true;
            UE_LOGW("plant_edit_gate: slot %u has %zu plant edits waiting; later ones are dropped until they drain",
                    static_cast<unsigned>(senderSlot), sl.queue.size());
        }
        return;
    }
    sl.queue.push_back(p);
}

Counts GetCounts() {
    return Counts{g_refusedClass, g_refusedReach, g_clamped, g_refusedKey, g_refusedRow, g_refusedParent};
}

void OnDisconnect() {
    if (g_refusedClass || g_refusedReach || g_clamped || g_refusedKey || g_refusedRow || g_refusedParent)
        UE_LOGI("plant_edit_gate: session end -- refused for class=%llu reach=%llu key=%llu row=%llu parent=%llu, values clamped=%llu",
                static_cast<unsigned long long>(g_refusedClass), static_cast<unsigned long long>(g_refusedReach),
                static_cast<unsigned long long>(g_refusedKey), static_cast<unsigned long long>(g_refusedRow),
                static_cast<unsigned long long>(g_refusedParent), static_cast<unsigned long long>(g_clamped));
    g_refusedClass = g_refusedReach = g_clamped = g_refusedKey = g_refusedRow = g_refusedParent = 0;
    g_keySaid = g_rowSaid = g_parentSaid = false;
    for (bool& said : g_clampSaid) said = false;
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) g_editSlots[slot] = EditSlot{};
}

}  // namespace coop::plant_edit_gate
