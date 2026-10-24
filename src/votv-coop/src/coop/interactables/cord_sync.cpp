// coop/interactables/cord_sync.cpp -- keyed cord/socket links, relayed by the host.

#include "coop/interactables/cord_sync.h"

#include "coop/element/object_scan_hub.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/cord.h"
#include "ue_wrap/engine/world_identity.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop::cord_sync {
namespace {
namespace R = ue_wrap::reflection;
using EntryMap = std::unordered_map<std::wstring, void*>;
std::atomic<coop::net::Session*> g_session{nullptr};
EntryMap g_props, g_sockets;
EntryMap g_foundProps, g_foundSockets;
std::unordered_map<std::wstring, std::wstring> g_lastSent;
struct Pending { coop::net::CordLinkPayload payload; std::chrono::steady_clock::time_point deadline; };
std::unordered_map<std::wstring, Pending> g_pending;
uint32_t g_indexGen = 0;
uint32_t g_snapshotOwed = 0;
bool g_applying = false;
auto g_nextPoll = std::chrono::steady_clock::time_point{};
constexpr size_t kMaxPending = 128;
constexpr auto kPendingTtl = std::chrono::seconds(30);
struct ApplyScope {
    bool previous;
    ApplyScope() : previous(g_applying) { g_applying = true; }
    ~ApplyScope() { g_applying = previous; }
};

bool IndexCurrent() { return g_indexGen == ue_wrap::world_identity::Generation(); }

void Park(const std::wstring& key, const coop::net::CordLinkPayload& payload) {
    if (key.empty() || key == L"None") return;
    if (g_pending.size() >= kMaxPending && g_pending.find(key) == g_pending.end()) {
        UE_LOGW("cord_sync: pending link table full; dropping unresolved key");
        return;
    }
    g_pending[key] = Pending{payload, std::chrono::steady_clock::now() + kPendingTtl};
}

void HubBegin(void*, bool) {
    g_foundProps.clear();
    g_foundSockets.clear();
}

void HubMatch(void*, void* actor) {
    if (!R::IsLive(actor) || R::NameStartsWith(R::NameOf(actor), L"Default__")) return;
    std::wstring key = ue_wrap::cord::Key(actor);
    if (key.empty() || key == L"None") return;
    if (ue_wrap::cord::IsCordedProp(actor)) g_foundProps.emplace(std::move(key), actor);
    else if (ue_wrap::cord::IsSocket(actor)) g_foundSockets.emplace(std::move(key), actor);
}

size_t HubComplete(void*, bool full, uint32_t gen) {
    auto merge = [full](EntryMap& into, EntryMap& found) {
        if (full) into.swap(found);
        else for (auto& [key, actor] : found) into[key] = actor;
    };
    merge(g_props, g_foundProps);
    merge(g_sockets, g_foundSockets);
    auto reap = [](EntryMap& map) {
        for (auto it = map.begin(); it != map.end();) {
            if (R::IsLive(it->second)) ++it;
            else it = map.erase(it);
        }
    };
    reap(g_props); reap(g_sockets);
    g_indexGen = gen;
    return g_props.size() + g_sockets.size();
}

void Register() {
    static bool registered = false;
    if (registered) return;
    registered = true;
    coop::element::scan_hub::Register({"cord", nullptr, &ue_wrap::cord::EnsureResolved,
        [](void* actor) { return ue_wrap::cord::IsCordedProp(actor) || ue_wrap::cord::IsSocket(actor); },
        &HubBegin, &HubMatch, &HubComplete, 15});
}

std::wstring SocketKey(void* prop) {
    void* socket = ue_wrap::cord::SocketOf(prop);
    if (!socket || !R::IsLive(socket)) return {};
    return ue_wrap::cord::Key(socket);
}

void Send(const std::wstring& propKey, const std::wstring& socketKey, int skipSlot = -1) {
    auto* session = g_session.load(std::memory_order_acquire);
    if (!session || propKey.empty() || propKey == L"None") return;
    coop::net::CordLinkPayload payload{};
    coop::net::WireKeyFromString(propKey, payload.propKey);
    if (!socketKey.empty() && socketKey != L"None") coop::net::WireKeyFromString(socketKey, payload.socketKey);
    session->SendReliable(coop::net::ReliableKind::CordLinkState, &payload, sizeof(payload), skipSlot);
}

void Apply(const coop::net::CordLinkPayload& payload) {
    const std::wstring propKey = coop::net::StringFromWireKey(payload.propKey);
    const std::wstring socketKey = coop::net::StringFromWireKey(payload.socketKey);
    if (propKey.empty()) return;
    auto propIt = g_props.find(propKey);
    if (propIt == g_props.end() || !R::IsLive(propIt->second)) {
        Park(propKey, payload);
        return;
    }
    void* prop = propIt->second;
    void* socket = nullptr;
    if (!socketKey.empty()) {
        auto it = g_sockets.find(socketKey);
        if (it == g_sockets.end() || !R::IsLive(it->second)) {
            Park(propKey, payload);
            return;
        }
        socket = it->second;
    }
    if (SocketKey(prop) == socketKey) { g_pending.erase(propKey); return; }
    g_lastSent[propKey] = socketKey;
    ApplyScope applying;
    // Connect already disconnects any previous endpoint before attaching the new one.
    // Running both transitions here sends a stale unplug through the Blueprint callbacks.
    if (socket) ue_wrap::cord::ApplyLink(prop, socket);
    else ue_wrap::cord::ApplyUnplug(prop);
    g_pending.erase(propKey);
}

void SendSnapshot(int slot) {
    auto* session = g_session.load(std::memory_order_acquire);
    if (!session || session->role() != coop::net::Role::Host || !IndexCurrent()) {
        if (slot >= 0 && slot < 32) g_snapshotOwed |= 1u << slot;
        return;
    }
    for (const auto& [key, prop] : g_props) {
        if (!R::IsLive(prop)) continue;
        Send(key, SocketKey(prop), slot);
    }
    if (slot >= 0 && slot < 32) g_snapshotOwed &= ~(1u << slot);
}
}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    Register();
}

void Tick() {
    Register();
    if (!ue_wrap::cord::EnsureResolved()) return;
    auto* session = g_session.load(std::memory_order_acquire);
    if (!session) return;
    if (!IndexCurrent()) { g_props.clear(); g_sockets.clear(); g_lastSent.clear(); return; }
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextPoll) {
        for (int slot = 0; slot < 32; ++slot) if (g_snapshotOwed & (1u << slot)) SendSnapshot(slot);
        return;
    }
    g_nextPoll = now + std::chrono::milliseconds(250);
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        if (now >= it->second.deadline) { it = g_pending.erase(it); continue; }
        const auto payload = it->second.payload;
        ++it;
        Apply(payload);
    }
    for (const auto& [key, prop] : g_props) {
        if (!R::IsLive(prop)) continue;
        const std::wstring socketKey = SocketKey(prop);
        auto it = g_lastSent.find(key);
        if (it == g_lastSent.end()) {
            g_lastSent.emplace(key, socketKey);
            if (!socketKey.empty() && socketKey != L"None") Send(key, socketKey);
            continue;
        }
        if (it->second == socketKey) continue;
        it->second = socketKey;
        if (!g_applying) Send(key, socketKey);
    }
    for (auto it = g_lastSent.begin(); it != g_lastSent.end();) {
        if (g_props.find(it->first) == g_props.end()) it = g_lastSent.erase(it);
        else ++it;
    }
    for (int slot = 0; slot < 32; ++slot) if (g_snapshotOwed & (1u << slot)) SendSnapshot(slot);
}

void OnReliable(const coop::net::CordLinkPayload& payload, uint8_t senderSlot) {
    auto* session = g_session.load(std::memory_order_acquire);
    if (!session) return;
    if (session->role() == coop::net::Role::Host) {
        if (senderSlot == 0 || senderSlot >= coop::net::kMaxPeers) return;
    } else if (senderSlot != 0) {
        return;
    }
    if (!IndexCurrent()) { Park(coop::net::StringFromWireKey(payload.propKey), payload); return; }
    if (session->role() == coop::net::Role::Host) {
        Apply(payload);
    } else {
        Apply(payload);
    }
}

void QueueConnectBroadcastForSlot(int peerSlot) { SendSnapshot(peerSlot); }

void OnDisconnect() {
    g_session.store(nullptr, std::memory_order_release);
    g_props.clear(); g_sockets.clear();
    g_foundProps.clear(); g_foundSockets.clear();
    g_lastSent.clear(); g_pending.clear(); g_snapshotOwed = 0; g_indexGen = 0;
    g_nextPoll = {};
}
}  // namespace coop::cord_sync
