// coop/interactables/atv_host_sync.cpp -- see coop/interactables/atv_host_sync.h.

#include "coop/interactables/atv_host_sync.h"

#include "coop/interactables/atv_host_judge.h"
#include "coop/interactables/atv_sync.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/world_identity.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::atv_host_sync {
namespace {

namespace A = ue_wrap::atv;
using Payload = coop::net::AtvHostPayload;
namespace op = coop::net::atv_host_op;

constexpr int kMaxModules = 16;   // what a state carries: Payload::modules

std::atomic<coop::net::Session*> g_session{nullptr};
uint32_t g_world = 0;
int g_applying = 0;   // our own writes run inside an ApplyScope
uint32_t g_hornsPlayed = 0;

// The state sequences (atv_host_sync.h): the host's per ATV, and the newest this client applied. Both are the session's,
// never reset by a world change.
std::unordered_map<std::wstring, uint32_t> g_stateSeq, g_appliedSeq;

struct Counts {
    uint32_t published = 0, replayed = 0, applied = 0, gated = 0, dropped = 0;
};
Counts g_n;

coop::net::Session* Live() {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    return s && s->connected() ? s : nullptr;
}

std::string ModulesText(const Vitals& v) {
    std::string s;
    for (int i = 0; i < v.count; ++i) s += (s.empty() ? "" : ",") + std::to_string(v.modules[i]);
    return "[" + s + "]";
}

struct Indexed {
    std::wstring key;
    void* actor;
};

void Collect(void* ctx, const std::wstring& key, void* actor) {
    static_cast<std::vector<Indexed>*>(ctx)->push_back({key, actor});
}

void* Find(const std::wstring& key) {
    coop::atv_sync::TableView t;
    return coop::atv_sync::ReadTable(key, t) ? t.actor : nullptr;
}

// CLIENT: one host state written onto this peer's copy, then its sequence recorded as the newest this client applied.
// Fuel and health as the host holds them, even over the syncer's own lower values (the burn of one round trip is lost,
// which the design states); the marks; the modules through the ATV's own updUpgrades, only when they differ.
void ApplyState(const std::wstring& key, void* atv, const Payload& p) {
    ApplyScope applying;
    A::SetFuel(atv, p.fuel);
    if (A::GetHealth(atv) != p.health) {
        A::SetHealth(atv, p.health);
        A::CallUpdHealth(atv);
    }
    A::SetEmpty(atv, (p.flags & coop::net::kAtvHostEmpty) != 0);
    A::SetBroken(atv, (p.flags & coop::net::kAtvHostBroken) != 0);
    Vitals have;
    if (ReadVitals(atv, have) &&
        (have.count != p.moduleCount || std::memcmp(have.modules, p.modules, p.moduleCount) != 0)) {
        A::WriteModules(atv, p.modules, p.moduleCount);
        UE_LOGI("atv_host: the host's '%ls' has modules %hs here", key.c_str(), ModulesText(have).c_str());
    }
    NoteAppliedStateSeq(key, p.stateSeq);
    ++g_n.applied;
}

}  // namespace

bool Vitals::operator==(const Vitals& o) const {
    return fuel == o.fuel && health == o.health && flags == o.flags && count == o.count &&
           std::memcmp(modules, o.modules, count) == 0;
}

ApplyScope::ApplyScope() { ++g_applying; }
ApplyScope::~ApplyScope() { --g_applying; }

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

coop::net::Session* LiveSession() { return Live(); }

void Tick() {
    coop::net::Session* s = Live();
    if (!s || !A::EnsureResolved()) return;
    const uint32_t world = ue_wrap::world_identity::Generation();
    if (world != g_world) {
        g_world = world;
        coop::atv_host_judge::OnWorldChange();
    }
    if (s->role() == coop::net::Role::Host) coop::atv_host_judge::Tick(*s);
}

bool ReadVitals(void* atv, Vitals& out) {
    uint8_t mods[kMaxModules] = {};
    const int n = A::ReadModules(atv, mods, kMaxModules);
    if (n < 0 || n > kMaxModules) return false;
    out.fuel = A::GetFuel(atv);
    out.health = A::GetHealth(atv);
    out.flags = static_cast<uint8_t>((A::GetEmpty(atv) ? coop::net::kAtvHostEmpty : 0) |
                                     (A::GetBroken(atv) ? coop::net::kAtvHostBroken : 0));
    out.count = static_cast<uint8_t>(n);
    std::memcpy(out.modules, mods, sizeof(out.modules));
    return true;
}

void FillState(const std::wstring& key, const Vitals& v, uint8_t opcode, Payload& out) {
    coop::net::WireKeyFromString(key, out.key);
    out.op = opcode;
    out.fuel = v.fuel;
    out.health = v.health;
    out.flags = v.flags;
    out.moduleCount = v.count;
    std::memcpy(out.modules, v.modules, sizeof(out.modules));
    out.stateSeq = CurrentStateSeq(key);
}

bool PublishIfChanged(const std::wstring& key, void* atv, const Vitals& before) {
    coop::net::Session* s = Live();
    Vitals now;
    if (!s || s->role() != coop::net::Role::Host || !ReadVitals(atv, now) || now == before) return false;
    ++g_stateSeq[key];
    Payload p{};
    FillState(key, now, op::kState, p);
    s->SendReliable(coop::net::ReliableKind::AtvHostState, &p, sizeof(p));
    ++g_n.published;
    UE_LOGI("atv_host: '%ls' state #%u published -- fuel %.1f health %.1f modules %hs%s%s", key.c_str(), p.stateSeq,
            now.fuel, now.health, ModulesText(now).c_str(), (now.flags & coop::net::kAtvHostEmpty) ? " empty" : "",
            (now.flags & coop::net::kAtvHostBroken) ? " broken" : "");
    return true;
}

void SendHorn(const std::wstring& key, bool pressed, int skipSlot) {
    coop::net::Session* s = Live();
    if (!s || s->role() != coop::net::Role::Host) return;
    Payload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.op = op::kHornEdge;
    p.flags = pressed ? coop::net::kAtvHostHornDown : 0;
    s->SendReliable(coop::net::ReliableKind::AtvHostState, &p, sizeof(p), skipSlot);
}

void OnState(const Payload& p) {
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    if (key.empty() || !A::EnsureResolved()) return;
    if (p.op != op::kState || p.moduleCount > kMaxModules) {
        ++g_n.dropped;
        return;
    }
    void* atv = Find(key);
    if (!atv) {
        ++g_n.dropped;
        return;
    }
    if (VitalsGated(key)) {
        ++g_n.gated;
        return;
    }
    ApplyState(key, atv, p);
}

void QueueConnectBroadcastForSlot(int slot) {
    coop::net::Session* s = Live();
    if (!s || s->role() != coop::net::Role::Host || !A::EnsureResolved()) return;
    std::vector<Indexed> atvs;
    coop::atv_sync::ForEachIndexed(&Collect, &atvs);
    for (const Indexed& x : atvs) {
        Vitals v;
        if (!ReadVitals(x.actor, v)) continue;
        Payload p{};
        FillState(x.key, v, op::kState, p);
        s->SendReliableToSlot(slot, coop::net::ReliableKind::AtvHostState, &p, sizeof(p));
        ++g_n.replayed;
    }
}

void OnDisconnect() {
    if (g_n.published || g_n.replayed || g_n.applied || g_n.gated || g_n.dropped)
        UE_LOGI("atv_host: session end -- states published=%u replayed=%u; applied=%u gated=%u dropped=%u", g_n.published,
                g_n.replayed, g_n.applied, g_n.gated, g_n.dropped);
    g_n = Counts{};
    g_stateSeq.clear();
    g_appliedSeq.clear();
    coop::atv_host_judge::OnDisconnect();
    g_session.store(nullptr);
}

uint32_t CurrentStateSeq(const std::wstring& key) {
    const auto it = g_stateSeq.find(key);
    return it == g_stateSeq.end() ? 0u : it->second;
}

uint32_t AppliedStateSeq(const std::wstring& key) {
    const auto it = g_appliedSeq.find(key);
    return it == g_appliedSeq.end() ? 0u : it->second;
}

void NoteAppliedStateSeq(const std::wstring& key, uint32_t seq) {
    uint32_t& held = g_appliedSeq[key];
    if (static_cast<int32_t>(seq - held) > 0) held = seq;
}

bool VitalsGated(const std::wstring& /*key*/) { return false; }

void NoteHornPlayed() { ++g_hornsPlayed; }
uint32_t HornsPlayed() { return g_hornsPlayed; }

}  // namespace coop::atv_host_sync
