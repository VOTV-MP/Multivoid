// coop/world/plant_sync.cpp -- see coop/world/plant_sync.h.

#include "coop/world/plant_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/growing_plant.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::plant_sync {
namespace {

namespace GP = ue_wrap::growing_plant;
namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;
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

void* Materialize(const Payload& p) {
    return GP::Spawn(StringFromClassName(p.cls), coop::net::StringFromWireKey(p.key), coop::net::StringFromWireKey(p.row),
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
std::vector<Payload> g_edits;

void HostApplyEdits(std::unordered_map<std::wstring, void*>& index) {
    std::vector<Payload> edits;
    edits.swap(g_edits);
    for (const Payload& p : edits) {
        const std::wstring key = coop::net::StringFromWireKey(p.key);
        auto it = index.find(key);
        if (!p.alive) {
            if (it != index.end()) GP::Destroy(it->second);
            index.erase(key);
            UE_LOGI("plant_sync: a client's player removed plant '%ls'", key.c_str());
            continue;
        }
        void* plant = it == index.end() ? nullptr : it->second;
        const bool known = plant != nullptr;
        if (!plant) {
            plant = Materialize(p);
            if (!plant) {
                UE_LOGW("plant_sync: a client's plant '%ls' of '%ls' did not spawn", key.c_str(), StringFromClassName(p.cls).c_str());
                continue;
            }
            index[key] = plant;
            UE_LOGI("plant_sync: a client's player planted '%ls', row '%ls'", key.c_str(), coop::net::StringFromWireKey(p.row).c_str());
        }
        GP::Values v = ValuesOf(p);
        if (known && GP::Read(plant, v)) MergeEdit(p, v);
        const bool takeRow = !known || (p.editMask & coop::net::plant_edit::kRow) != 0;
        GP::Apply(plant, takeRow ? coop::net::StringFromWireKey(p.row) : GP::Row(plant), v);
        g_sent[key].pending = true;
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
    HostApplyEdits(index);

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
};
std::unordered_map<std::wstring, Copy> g_copies;
std::unordered_set<std::wstring> g_gone;      // removed here; ignored until the host's end arrives
std::unordered_set<void*> g_loaded;           // what the load brought and the host has not named yet
bool g_loadedRead = false;
std::vector<Payload> g_inbox;
bool g_parkedAny = false;

void Park(void* plant) {
    if (!E::IsActorTickEnabled(plant)) return;
    E::SetActorTickEnabled(plant, false);
    g_parkedAny = true;
}

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
            plant = Materialize(p);
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
        GP::Read(plant, c.applied);
    }
}

void ClientPoll(coop::net::Session& s) {
    // The winter bush's spawner plants bushes of the family on its own tick: on a client it would plant its own.
    std::vector<void*> spawners;
    GP::ListBushSpawners(spawners);
    for (void* a : spawners) {
        if (!E::IsActorTickEnabled(a)) continue;
        Park(a);
        UE_LOGI("plant_sync: parked the winter bush spawner");
    }
    std::vector<void*> plants;
    GP::List(plants);
    // A join's load makes and unmakes plants for its own reasons; none of that was this player.
    if (!coop::net_pump::HasAnnouncedWorldReady() || coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle) {
        for (void* a : plants) Park(a);
        return;
    }
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
    for (const auto& entry : index) Park(entry.second);
}

void ResetClient() {
    g_copies.clear();
    g_gone.clear();
    g_loaded.clear();
    g_loadedRead = false;
    g_inbox.clear();
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

void OnState(const coop::net::PlantStatePayload& p) { g_inbox.push_back(p); }

void OnEdit(const coop::net::PlantStatePayload& p) { g_edits.push_back(p); }

void QueueConnectBroadcastForSlot(int /*slot*/) {
    for (auto& entry : g_sent) entry.second.pending = true;
}

void OnDisconnect() {
    if (g_parkedAny) {
        std::vector<void*> plants;
        GP::List(plants);
        for (void* a : plants) E::SetActorTickEnabled(a, true);
        std::vector<void*> spawners;
        GP::ListBushSpawners(spawners);
        for (void* a : spawners) E::SetActorTickEnabled(a, true);
    }
    g_parkedAny = false;
    ResetClient();
    g_sent.clear();
    g_edits.clear();
    g_session.store(nullptr);
}

}  // namespace coop::plant_sync
