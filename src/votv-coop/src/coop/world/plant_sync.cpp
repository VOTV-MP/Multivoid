// coop/world/plant_sync.cpp -- see coop/world/plant_sync.h.

#include "coop/world/plant_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/world/plant_edit_gate.h"

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
uint32_t g_harvestSounds = 0;

// The harvesting player's game played it on that peer; this is the sound for everyone else.
void PlayHarvest(void* plant, const std::wstring& key) {
    GP::HarvestSound(plant);
    ++g_harvestSounds;
    UE_LOGI("plant_sync: another player harvested plant '%ls' -- its sound played here (#%u)", key.c_str(), g_harvestSounds);
}

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

void ClassNameFromString(const std::wstring& s, coop::net::WireClassName& out) {
    std::memset(&out, 0, sizeof(out));
    const size_t n = (std::min)(s.size(), sizeof(out.data));
    for (size_t i = 0; i < n; ++i) out.data[i] = static_cast<char>(s[i] & 0xFF);
    out.len = static_cast<uint8_t>(n);
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
//
// A name tried once is not tried again in a world unless it was found: a state for a class that cannot load comes with every
// plant of it, and the object index lists a class just loaded only after its drain.
std::unordered_map<std::wstring, bool> g_classLoads;   // name -> the load found it; cleared with the world
void* HostStateClass(const std::wstring& name) {
    void* cls = OI::ClassByName(name.c_str());
    if (!cls && name.size() > 2 && name.compare(name.size() - 2, 2, L"_C") == 0) {
        const auto tried = g_classLoads.find(name);
        if (tried == g_classLoads.end() || tried->second) {
            const std::wstring path = L"/Game/objects/" + name.substr(0, name.size() - 2) + L"." + name;
            cls = ue_wrap::asset_load::LoadObjectByPath(path.c_str());
            if (tried == g_classLoads.end()) {
                g_classLoads[name] = cls != nullptr;
                UE_LOGI("plant_sync: plant class '%ls' was not loaded here; loading %ls %hs", name.c_str(), path.c_str(),
                        cls ? "found it" : "found nothing");
            }
        }
    }
    return (cls && GP::IsPlantClass(cls)) ? cls : nullptr;
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

void HostPoll(coop::net::Session& s, Clock::time_point now) {
    std::vector<void*> plants;
    GP::List(plants);
    std::unordered_map<std::wstring, void*> index;
    for (void* a : plants) {
        std::wstring key = GP::Key(a);
        if (!key.empty()) index[key] = a;
    }
    std::vector<std::wstring> changed;
    coop::plant_edit_gate::ApplyEdits(s, index, changed);
    for (const std::wstring& key : changed) g_sent[key].pending = true;

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
            // One the host named before; a copy this player harvested is gone already.
            const bool copy = g_copies.erase(key) != 0;
            if (it != index.end()) {
                if (copy && GP::HarvestTakesPlant(it->second)) PlayHarvest(it->second, key);
                GP::Destroy(it->second);
            }
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
        GP::Values before;
        const bool picked = g_copies.count(key) && GP::Read(plant, before) && before.amount > 0 && p.amount == 0;
        GP::Apply(plant, row, ValuesOf(p));
        if (picked) PlayHarvest(plant, key);
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
    // What this player did is read before the host's states are applied: a state applied first writes its values over the
    // copy, and a change the player made since the last poll would then read as no change and never be sent.

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
    ApplyInbox(index);
}

void ResetClient() {
    g_classLoads.clear();
    g_copies.clear();
    g_gone.clear();
    g_loaded.clear();
    g_loadedRead = false;
    g_inbox.clear();
    g_inboxFullSaid = false;
}

}  // namespace

std::wstring StringFromClassName(const coop::net::WireClassName& c) {
    const size_t n = (std::min)(static_cast<size_t>(c.len), sizeof(c.data));
    std::wstring out;
    for (size_t i = 0; i < n; ++i) out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c.data[i])));
    return out;
}

GP::Values ValuesOf(const coop::net::PlantStatePayload& p) {
    return GP::Values{p.grow, p.fertilizer, p.water, p.fruitGrow, p.dryness, p.amount};
}

void* Materialize(const coop::net::PlantStatePayload& p, void* cls) {
    return GP::Spawn(cls, coop::net::StringFromWireKey(p.key), coop::net::StringFromWireKey(p.row),
                     coop::net::StringFromWireKey(p.attachParent), p.hideDirt != 0, {p.x, p.y, p.z}, {p.pitch, p.yaw, p.roll});
}

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
        coop::plant_edit_gate::OnWorldChange();
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

uint32_t HarvestSoundsPlayed() { return g_harvestSounds; }

void PlayHarvestSound(void* plant, const std::wstring& key) { PlayHarvest(plant, key); }

void QueueConnectBroadcastForSlot(int /*slot*/) {
    for (auto& entry : g_sent) entry.second.pending = true;
}

void OnDisconnect() {
    ResetClient();
    g_sent.clear();
    coop::plant_edit_gate::OnDisconnect();
    g_session.store(nullptr);
}

}  // namespace coop::plant_sync
