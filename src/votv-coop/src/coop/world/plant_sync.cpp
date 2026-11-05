// coop/world/plant_sync.cpp -- see coop/world/plant_sync.h.

#include "coop/world/plant_sync.h"

#include "coop/element/intent_authority.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/roster_ledger.h"  // PerSlotState
#include "coop/props/prop_key_index.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/growing_plant.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::plant_sync {
namespace {

namespace GP = ue_wrap::growing_plant;
namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;
using Clock = std::chrono::steady_clock;
using Payload = coop::net::PlantStatePayload;

constexpr auto  kPoll = std::chrono::milliseconds(250);
constexpr auto  kGradual = std::chrono::seconds(5);     // how often a plant's slow change goes out
constexpr int   kSendsPerPoll = 32;

std::atomic<coop::net::Session*> g_session{nullptr};
Clock::time_point g_nextPoll{};
uint32_t g_world = 0;

// The edges a viewer sees at once; the rest of a plant's change is gradual.
bool Edge(const GP::Values& a, const GP::Values& b) {
    return (a.grow >= 1.f) != (b.grow >= 1.f) || (a.fruitGrow >= 1.f) != (b.fruitGrow >= 1.f) || a.amount != b.amount ||
           (a.water > 0.f) != (b.water > 0.f) || (a.fertilizer > 0.f) != (b.fertilizer > 0.f) || (a.dryness < 0.f) != (b.dryness < 0.f);
}

bool Same(const GP::Values& a, const GP::Values& b) {
    return a.grow == b.grow && a.fertilizer == b.fertilizer && a.water == b.water && a.fruitGrow == b.fruitGrow &&
           a.dryness == b.dryness && a.amount == b.amount;
}

uint8_t EditMask(const GP::Values& was, const GP::Values& now, bool rowChanged) {
    namespace PE = coop::net::plant_edit;
    uint8_t m = rowChanged ? PE::kRow : 0;
    if (was.grow != now.grow) m |= PE::kGrow;
    if (was.fertilizer != now.fertilizer) m |= PE::kFertilizer;
    if (was.water != now.water) m |= PE::kWater;
    if (was.fruitGrow != now.fruitGrow) m |= PE::kFruitGrow;
    if (was.dryness != now.dryness) m |= PE::kDryness;
    if (was.amount != now.amount) m |= PE::kAmount;
    return m;
}

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

GP::Values ValuesOf(const Payload& p) { return GP::Values{p.grow, p.fertilizer, p.water, p.fruitGrow, p.dryness, p.amount}; }

void ClassNameFromString(const std::wstring& s, coop::net::WireClassName& out) {
    std::memset(&out, 0, sizeof(out));
    const size_t n = (std::min)(s.size(), sizeof(out.data));
    for (size_t i = 0; i < n; ++i) out.data[i] = static_cast<char>(s[i] & 0xFF);
    out.len = static_cast<uint8_t>(n);
}

std::wstring StringFromClassName(const coop::net::WireClassName& c) {
    const size_t n = (std::min)(static_cast<size_t>(c.len), sizeof(c.data));
    std::wstring out;
    for (size_t i = 0; i < n; ++i) out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c.data[i])));
    return out;
}

// False when the plant cannot be read yet.
bool Describe(void* plant, const std::wstring& key, const GP::Values& v, Payload& p) {
    ue_wrap::FVector at{};
    ue_wrap::FRotator rot{};
    if (!E::TryGetActorLocation(plant, at) || !E::TryGetActorRotation(plant, rot)) return false;
    p = Payload{};
    coop::net::WireKeyFromString(key, p.key);
    ClassNameFromString(R::ToString(R::NameOf(R::ClassOf(plant))), p.cls);
    coop::net::WireKeyFromString(GP::Row(plant), p.row);
    coop::net::WireKeyFromString(GP::AttachParent(plant), p.attachParent);
    p.x = at.X;
    p.y = at.Y;
    p.z = at.Z;
    p.pitch = rot.Pitch;
    p.yaw = rot.Yaw;
    p.roll = rot.Roll;
    p.grow = v.grow;
    p.fertilizer = v.fertilizer;
    p.water = v.water;
    p.fruitGrow = v.fruitGrow;
    p.dryness = v.dryness;
    p.amount = v.amount;
    p.alive = 1;
    p.hideDirt = GP::HideDirt(plant) ? 1 : 0;
    return true;
}

// The class a host's state names, on a client: the object index's, or loaded from its asset when this peer has none yet
// (a plant class loads with the first plant of it, and the host's may be the first here). Plant classes live under
// /Game/objects/, one asset to a class. The name is the host's, so loading it is a load the host asked
// for; the class is checked against the family all the same. A client's planting on the host loads nothing: its class
// is its row's (GP::FarmRowPlantClass).
void* HostStateClass(const std::wstring& name) {
    void* cls = OI::ClassByName(name.c_str());
    if (!cls && name.size() > 2 && name.compare(name.size() - 2, 2, L"_C") == 0) {
        const std::wstring path = L"/Game/objects/" + name.substr(0, name.size() - 2) + L"." + name;
        cls = ue_wrap::asset_load::LoadObjectByPath(path.c_str());
        UE_LOGI("plant_sync: plant class '%ls' was not loaded here; loading %ls %hs", name.c_str(), path.c_str(),
                cls ? "found it" : "found nothing");
    }
    return (cls && GP::IsPlantClass(cls)) ? cls : nullptr;
}

void* Materialize(const Payload& p, void* cls) {
    return GP::Spawn(cls, coop::net::StringFromWireKey(p.key), coop::net::StringFromWireKey(p.row),
                     coop::net::StringFromWireKey(p.attachParent), p.hideDirt != 0, {p.x, p.y, p.z}, {p.pitch, p.yaw, p.roll});
}

// HOST: what each plant last sent.
struct Sent {
    GP::Values v{};
    std::wstring row;
    Clock::time_point at{};
    bool pending = true;   // has to go out whatever it changed
    Payload last{};        // what its end sends
    bool everSent = false;
};
std::unordered_map<std::wstring, Sent> g_sent;

// The edit gate. A client's edit is its player's act, so the host takes it as that act could make it: it names a key, a planting
// plants the class of its own list_farmPlants row (a wire class that differs is refused) and names a pot the host holds (or
// none), a changed row is one of those rows, every edit lies within the sender's reach and every value within the bounds below.
//
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
    UE_LOGW("plant_sync: slot %u's %ls of '%ls' refused -- %ls '%ls' (said once a session)", static_cast<unsigned>(slot), what,
            key.c_str(), why, named.c_str());
}

template <class T>
void Bound(T& v, T lo, T hi, Field f, uint8_t slot) {
    const T cut = v < lo ? lo : (v > hi ? hi : v);
    if (cut == v) return;
    ++g_clamped;
    if (!g_clampSaid[f]) {
        g_clampSaid[f] = true;
        UE_LOGW("plant_sync: slot %u's %s of %.1f is cut to %.1f (said once a session per field)", static_cast<unsigned>(slot),
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
            UE_LOGI("plant_sync: slot %u's plant edits wait: the host has no body for it yet", static_cast<unsigned>(slot));
        }
        return Gate::Wait;
    }
    sl.waitSaid = false;
    if (subject) return Gate::Pass;
    if (++g_refusedReach <= 3 || g_refusedReach % 20 == 0)
        UE_LOGI("plant_sync: slot %u's %ls of '%ls' refused -- %s (%.0f uu of %.0f) (#%llu)", static_cast<unsigned>(slot), what,
                key.c_str(), coop::element::OutcomeName(subject.outcome), subject.distUU, subject.reachUU,
                static_cast<unsigned long long>(g_refusedReach));
    return Gate::Refused;
}

// False keeps the edit at its sender's queue head: the host has no body for the sender yet. Anything else is applied or
// refused, and consumed.
bool HostApplyEdit(coop::net::Session& s, EditSlot& sl, uint8_t slot, Payload p, std::unordered_map<std::wstring, void*>& index) {
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
        UE_LOGI("plant_sync: a client's player removed plant '%ls'", key.c_str());
        return true;
    }
    const bool planting = plant == nullptr;
    void* plantClass = nullptr;
    if (planting) {
        // The class is the one the host's own list_farmPlants row plants, never one the wire names: the wire's name must be
        // that class's, and the class a plant class. A name from the wire starts no lookup of its own.
        const std::wstring className = StringFromClassName(p.cls);
        const std::wstring row = coop::net::StringFromWireKey(p.row);
        if (!GP::IsFarmRow(row)) {
            RefuseOnce(g_refusedRow, g_rowSaid, slot, L"planting", key, L"no such list_farmPlants row", row);
            return true;
        }
        plantClass = GP::FarmRowPlantClass(row);
        if (!plantClass || !GP::IsPlantClass(plantClass) || _wcsicmp(R::ToString(R::NameOf(plantClass)).c_str(), className.c_str()) != 0) {
            if (++g_refusedClass <= 3 || g_refusedClass % 20 == 0)
                UE_LOGW("plant_sync: slot %u's planting of '%ls' refused -- row '%ls' plants no plant class named '%ls' (#%llu)",
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
        plant = Materialize(p, plantClass);   // the row's class, judged a plant class above
        if (!plant) {
            UE_LOGW("plant_sync: a client's plant '%ls' of '%ls' did not spawn", key.c_str(), StringFromClassName(p.cls).c_str());
            return true;
        }
        index[key] = plant;
        UE_LOGI("plant_sync: a client's player planted '%ls', row '%ls'", key.c_str(), coop::net::StringFromWireKey(p.row).c_str());
    }
    GP::Values v = ValuesOf(p);
    if (!planting && GP::Read(plant, v)) MergeEdit(p, v);
    const bool takeRow = planting || (p.editMask & coop::net::plant_edit::kRow) != 0;
    GP::Apply(plant, takeRow ? coop::net::StringFromWireKey(p.row) : GP::Row(plant), v);
    g_sent[key].pending = true;
    return true;
}

void HostApplyEdits(coop::net::Session& s, std::unordered_map<std::wstring, void*>& index) {
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        EditSlot& sl = g_editSlots[slot];
        size_t done = 0;
        while (done < sl.queue.size() && HostApplyEdit(s, sl, slot, sl.queue[done], index)) ++done;
        if (done == 0) continue;
        sl.queue.erase(sl.queue.begin(), sl.queue.begin() + static_cast<std::ptrdiff_t>(done));
        sl.fullSaid = false;
    }
}

void HostPoll(coop::net::Session& s, Clock::time_point now) {
    std::vector<void*> plants;
    GP::List(plants);
    std::unordered_map<std::wstring, void*> index;
    for (void* a : plants) {
        std::wstring key = GP::Key(a);
        if (!key.empty()) index[key] = a;
    }
    HostApplyEdits(s, index);

    int sends = 0;
    for (const auto& entry : index) {
        if (sends >= kSendsPerPoll) break;
        GP::Values v;
        if (!R::IsLive(entry.second) || !GP::Read(entry.second, v)) continue;
        const std::wstring row = GP::Row(entry.second);
        Sent& sent = g_sent[entry.first];
        const bool due = sent.pending || !sent.everSent || sent.row != row || Edge(sent.v, v) ||
                         (!Same(sent.v, v) && now - sent.at >= kGradual);
        if (!due) continue;
        Payload p;
        if (!Describe(entry.second, entry.first, v, p)) continue;
        if (!s.SendReliable(coop::net::ReliableKind::PlantState, &p, sizeof(p))) continue;
        ++sends;
        sent.v = v;
        sent.row = row;
        sent.at = now;
        sent.pending = false;
        sent.everSent = true;
        sent.last = p;
    }
    for (auto it = g_sent.begin(); it != g_sent.end();) {
        if (index.count(it->first)) {
            ++it;
            continue;
        }
        if (it->second.everSent) {
            Payload end = it->second.last;
            end.alive = 0;
            if (!s.SendReliable(coop::net::ReliableKind::PlantState, &end, sizeof(end))) {
                ++it;
                continue;
            }
            UE_LOGI("plant_sync: plant '%ls' ended", it->first.c_str());
        }
        it = g_sent.erase(it);
    }
}

// CLIENT: each plant this client knows is the host's, and what it last wrote there.
struct Copy {
    void* actor = nullptr;
    GP::Values applied{};
    std::wstring row;
    GP::Values host{};   // the host's last values the inbox applied here, whatever this player did since
    bool hostSeen = false;
};
std::unordered_map<std::wstring, Copy> g_copies;
std::unordered_set<std::wstring> g_gone;      // removed here; ignored until the host's end arrives
std::unordered_set<void*> g_loaded;           // what the load brought and the host has not named yet
bool g_loadedRead = false;
std::vector<Payload> g_inbox;
// A plant state is 212 bytes; the cap is a few times the plants of a late save, and the host sends a changing plant again.
constexpr size_t kInboxCap = 2048;
bool g_inboxFullSaid = false;

void ApplyInbox(std::unordered_map<std::wstring, void*>& index) {
    std::vector<Payload> inbox;
    inbox.swap(g_inbox);
    for (const Payload& p : inbox) {
        const std::wstring key = coop::net::StringFromWireKey(p.key);
        auto it = index.find(key);
        if (!p.alive) {
            g_gone.erase(key);
            g_copies.erase(key);
            if (it != index.end()) GP::Destroy(it->second);
            index.erase(key);
            UE_LOGI("plant_sync: plant '%ls' ended here", key.c_str());
            continue;
        }
        if (g_gone.count(key)) continue;
        void* plant = it == index.end() ? nullptr : it->second;
        if (!plant) {
            plant = Materialize(p, HostStateClass(StringFromClassName(p.cls)));
            if (!plant) {
                UE_LOGW("plant_sync: plant '%ls' of '%ls' did not spawn", key.c_str(), StringFromClassName(p.cls).c_str());
                continue;
            }
            index[key] = plant;
            UE_LOGI("plant_sync: plant '%ls' row '%ls' stands here", key.c_str(), coop::net::StringFromWireKey(p.row).c_str());
        }
        const std::wstring row = coop::net::StringFromWireKey(p.row);
        GP::Apply(plant, row, ValuesOf(p));
        g_loaded.erase(plant);
        Copy& c = g_copies[key];
        c.actor = plant;
        c.row = row;
        c.host = ValuesOf(p);
        c.hostSeen = true;
        GP::Read(plant, c.applied);
    }
}

void ClientPoll(coop::net::Session& s) {
    // A join's load makes and unmakes plants for its own reasons; none of that was this player.
    if (!coop::net_pump::HasAnnouncedWorldReady() || coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle) return;
    std::vector<void*> plants;
    GP::List(plants);
    if (!g_loadedRead) {
        g_loadedRead = true;
        g_loaded.insert(plants.begin(), plants.end());
        UE_LOGI("plant_sync: the load brought %zu plants", g_loaded.size());
    }
    std::unordered_map<std::wstring, void*> index;
    for (void* a : plants) {
        std::wstring key = GP::Key(a);
        if (!key.empty()) index[key] = a;
    }
    ApplyInbox(index);

    for (auto it = g_copies.begin(); it != g_copies.end();) {
        auto live = index.find(it->first);
        if (live == index.end()) {
            // Its end came from no message, so this player took it.
            Payload end{};
            coop::net::WireKeyFromString(it->first, end.key);
            end.alive = 0;
            if (!s.SendReliable(coop::net::ReliableKind::PlantEdit, &end, sizeof(end))) {
                ++it;
                continue;
            }
            g_gone.insert(it->first);
            UE_LOGI("plant_sync: this player removed plant '%ls'", it->first.c_str());
            it = g_copies.erase(it);
            continue;
        }
        GP::Values now;
        const std::wstring row = GP::Row(live->second);
        if (GP::Read(live->second, now) && (!Same(now, it->second.applied) || row != it->second.row)) {
            Payload p;
            const bool described = Describe(live->second, it->first, now, p);
            p.editMask = EditMask(it->second.applied, now, row != it->second.row);
            if (described && s.SendReliable(coop::net::ReliableKind::PlantEdit, &p, sizeof(p))) {
                it->second.applied = now;
                it->second.row = row;
                UE_LOGI("plant_sync: this player changed plant '%ls' water=%.0f grow=%.2f amount=%d", it->first.c_str(), now.water, now.grow, now.amount);
            }
        }
        ++it;
    }

    // One the load did not bring and the host never named was planted here.
    for (const auto& entry : index) {
        if (g_copies.count(entry.first) || g_loaded.count(entry.second) || g_gone.count(entry.first)) continue;
        GP::Values v;
        Payload p;
        if (!GP::Read(entry.second, v) || !Describe(entry.second, entry.first, v, p)) continue;
        if (!s.SendReliable(coop::net::ReliableKind::PlantEdit, &p, sizeof(p))) continue;
        Copy& c = g_copies[entry.first];
        c.actor = entry.second;
        c.applied = v;
        c.row = GP::Row(entry.second);
        UE_LOGI("plant_sync: this player planted '%ls'", entry.first.c_str());
    }
}

void ResetClient() {
    g_copies.clear();
    g_gone.clear();
    g_loaded.clear();
    g_loadedRead = false;
    g_inbox.clear();
    g_inboxFullSaid = false;
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session); }

void Tick() {
    coop::net::Session* s = g_session.load();
    if (!s) return;
    const Clock::time_point now = Clock::now();
    if (now < g_nextPoll) return;
    g_nextPoll = now + kPoll;
    const uint32_t world = ue_wrap::world_identity::Generation();
    if (world != g_world) {
        g_world = world;
        ResetClient();
        g_sent.clear();
    }
    if (s->role() == coop::net::Role::Host) HostPoll(*s, now);
    else if (s->role() == coop::net::Role::Client) ClientPoll(*s);
}

void OnState(const coop::net::PlantStatePayload& p) {
    if (g_inbox.size() >= kInboxCap) {
        if (!g_inboxFullSaid) {
            g_inboxFullSaid = true;
            UE_LOGW("plant_sync: %zu plant states wait to be applied; later ones are dropped until the inbox drains (the host "
                    "sends each changing plant again)", g_inbox.size());
        }
        return;
    }
    g_inbox.push_back(p);
}

bool LastHostValues(const std::wstring& key, GP::Values& out) {
    const auto it = g_copies.find(key);
    if (it == g_copies.end() || !it->second.hostSeen) return false;
    out = it->second.host;
    return true;
}

void OnEdit(uint8_t senderSlot, const coop::net::PlantStatePayload& p) {
    if (senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    EditSlot& sl = g_editSlots[senderSlot];
    if (sl.queue.size() >= kEditsPerSlotCap) {
        if (!sl.fullSaid) {
            sl.fullSaid = true;
            UE_LOGW("plant_sync: slot %u has %zu plant edits waiting; later ones are dropped until they drain",
                    static_cast<unsigned>(senderSlot), sl.queue.size());
        }
        return;
    }
    sl.queue.push_back(p);
}

Counts GetCounts() {
    return Counts{g_refusedClass, g_refusedReach, g_clamped, g_refusedKey, g_refusedRow, g_refusedParent};
}

void QueueConnectBroadcastForSlot(int /*slot*/) {
    for (auto& entry : g_sent) entry.second.pending = true;
}

void OnDisconnect() {
    ResetClient();
    g_sent.clear();
    if (g_refusedClass || g_refusedReach || g_clamped || g_refusedKey || g_refusedRow || g_refusedParent)
        UE_LOGI("plant_sync: session end -- refused for class=%llu reach=%llu key=%llu row=%llu parent=%llu, values clamped=%llu",
                static_cast<unsigned long long>(g_refusedClass), static_cast<unsigned long long>(g_refusedReach),
                static_cast<unsigned long long>(g_refusedKey), static_cast<unsigned long long>(g_refusedRow),
                static_cast<unsigned long long>(g_refusedParent), static_cast<unsigned long long>(g_clamped));
    g_refusedClass = g_refusedReach = g_clamped = g_refusedKey = g_refusedRow = g_refusedParent = 0;
    g_keySaid = g_rowSaid = g_parentSaid = false;
    for (bool& said : g_clampSaid) said = false;
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) g_editSlots[slot] = EditSlot{};
    g_session.store(nullptr);
}

}  // namespace coop::plant_sync
