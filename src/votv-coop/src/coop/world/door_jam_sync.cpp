// coop/world/door_jam_sync.cpp -- see coop/world/door_jam_sync.h.

#include "coop/world/door_jam_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::door_jam_sync {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
using Clock = std::chrono::steady_clock;
using Payload = coop::net::AmbientFixtureStatePayload;

constexpr long long kPollMs = 1000;
constexpr float kSamePlaceCm = 50.f;

std::atomic<coop::net::Session*> g_session{nullptr};
Clock::time_point g_lastPoll{};

uint32_t Hash(const std::wstring& k) {
    uint32_t h = 2166136261u;
    for (wchar_t c : k) { h ^= static_cast<uint32_t>(c); h *= 16777619u; }
    return h ? h : 1u;
}

bool JammedOf(void* door) {
    int32_t off = -1;
    uint8_t mask = 0;
    return R::FindBoolProperty(R::ClassOf(door), L"jammed", off, mask) && off >= 0 && (*(static_cast<const uint8_t*>(door) + off) & mask) != 0;
}

void SetJammed(void* door, bool v) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(door), L"jammed", off, mask) || off < 0) return;
    uint8_t* p = static_cast<uint8_t*>(door) + off;
    *p = v ? static_cast<uint8_t>(*p | mask) : static_cast<uint8_t>(*p & ~mask);
}

void LiveDoors(std::vector<void*>& out) {
    out.clear();
    void* cls = ue_wrap::object_index::ClassByName(L"door_C");
    void* world = ue_wrap::world_identity::CurrentWorld();
    if (!cls || !world) return;
    std::vector<void*> found;
    ue_wrap::object_index::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
        if (obj) static_cast<std::vector<void*>*>(ctx)->push_back(obj);
    }, &found);
    for (void* obj : found)
        if (R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__") && ue_wrap::world_identity::WorldOf(obj) == world) out.push_back(obj);
}

// ---- HOST ----

struct Sent { void* actor; bool jammed; };
std::unordered_map<int32_t, Sent> g_sent;   // by the actor's internal index
bool g_resendAll = false;

bool Send(coop::net::Session* s, void* door, uint32_t id, const ue_wrap::FVector& at, bool jammed) {
    Payload p{};
    p.id = id;
    p.x = at.X; p.y = at.Y; p.z = at.Z;
    p.pitch = jammed ? 1.f : 0.f;
    p.family = coop::net::ambient_fixture::kDoorJam;
    p.alive = 1;
    return s->SendReliable(coop::net::ReliableKind::AmbientFixtureState, &p, sizeof(p));
}

void HostPoll(coop::net::Session* s) {
    std::vector<void*> doors;
    LiveDoors(doors);
    const bool connected = s->connected();
    for (void* d : doors) {
        const int32_t idx = R::InternalIndexOf(d);
        const bool j = JammedOf(d);
        auto it = g_sent.find(idx);
        const bool fresh = it == g_sent.end() || it->second.actor != d;
        if (fresh) it = g_sent.insert_or_assign(idx, Sent{d, false}).first;
        const bool changed = j != it->second.jammed;
        if (!connected) { if (changed) it->second.jammed = j; continue; }
        if (!changed && !(g_resendAll && j)) continue;
        ue_wrap::FVector at{};
        if (!E::TryGetActorLocation(d, at)) continue;
        if (Send(s, d, Hash(R::ToString(R::NameOf(d))), at, j)) {
            if (changed) UE_LOGI("door_jam: the host's door '%ls' is now %s", R::ToString(R::NameOf(d)).c_str(), j ? "jammed" : "free");
            it->second.jammed = j;
        }
    }
    if (connected) g_resendAll = false;
}

// ---- CLIENT ----

struct Want { ue_wrap::FVector at; bool jammed; bool done; };
std::unordered_map<uint32_t, std::vector<Want>> g_want;   // by name hash; a name may stand in several tiles

void ClientPoll() {
    bool owed = false;
    for (const auto& kv : g_want)
        for (const Want& w : kv.second) owed = owed || !w.done;
    if (!owed) return;
    std::vector<void*> doors;
    LiveDoors(doors);
    for (auto& kv : g_want)
        for (Want& w : kv.second) {
            if (w.done) continue;
            for (void* d : doors) {
                if (Hash(R::ToString(R::NameOf(d))) != kv.first) continue;
                ue_wrap::FVector at{};
                if (!E::TryGetActorLocation(d, at)) continue;
                const float dx = at.X - w.at.X, dy = at.Y - w.at.Y, dz = at.Z - w.at.Z;
                if (dx * dx + dy * dy + dz * dz > kSamePlaceCm * kSamePlaceCm) continue;
                SetJammed(d, w.jammed);
                w.done = true;
                UE_LOGI("door_jam: this peer's door '%ls' follows the host: %s", R::ToString(R::NameOf(d)).c_str(), w.jammed ? "jammed" : "free");
                break;
            }
        }
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    const bool host = s->role() == coop::net::Role::Host;
    if (!host && g_want.empty()) return;
    const auto now = Clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastPoll).count() < kPollMs) return;
    g_lastPoll = now;
    if (host) HostPoll(s);
    else ClientPoll();
}

void OnState(const Payload& p) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() == coop::net::Role::Host) return;
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || std::fabs(p.x) > coop::net::kMaxCoord || std::fabs(p.y) > coop::net::kMaxCoord ||
        std::fabs(p.z) > coop::net::kMaxCoord) {
        UE_LOGW("door_jam: a door placed outside the world -- dropped");
        return;
    }
    std::vector<Want>& v = g_want[p.id];
    const ue_wrap::FVector at{p.x, p.y, p.z};
    for (Want& w : v) {
        const float dx = w.at.X - at.X, dy = w.at.Y - at.Y, dz = w.at.Z - at.Z;
        if (dx * dx + dy * dy + dz * dz < kSamePlaceCm * kSamePlaceCm) {
            w.jammed = p.pitch > 0.5f;
            w.done = false;
            return;
        }
    }
    v.push_back(Want{at, p.pitch > 0.5f, false});
}

void QueueConnectBroadcastForSlot(int /*slot*/) { g_resendAll = true; }

void OnDisconnect() {
    g_want.clear();
    g_sent.clear();
    g_resendAll = false;
}

}  // namespace coop::door_jam_sync
