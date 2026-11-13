// coop/interactables/atv_host_sync.cpp -- see coop/interactables/atv_host_sync.h.

#include "coop/interactables/atv_host_sync.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/interactables/atv_drill_events.h"
#include "coop/interactables/atv_gate.h"
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
namespace DE = coop::atv_drill_events;

constexpr int kMaxModules = 16;   // what a state carries: Payload::modules

std::atomic<coop::net::Session*> g_session{nullptr};
uint32_t g_world = 0;
int g_applying = 0;   // our own writes run inside an ApplyScope

// The state sequences (atv_host_sync.h): the host's per ATV, and the newest this client applied. Both are the session's,
// never reset by a world change.
std::unordered_map<std::wstring, uint32_t> g_stateSeq, g_appliedSeq;

struct Counts {
    uint32_t published = 0, replayed = 0, applied = 0, gated = 0, dropped = 0, kept = 0, asked = 0, answered = 0,
             refusedAsks = 0, stray = 0, unanswered = 0;
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
void ApplyState(const std::wstring& key, void* atv, const Payload& p, int source) {
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
    DE::Note(DE::kStateApplied, static_cast<int32_t>(p.stateSeq), source, p.moduleCount, A::GetFuel(atv), A::GetHealth(atv));
}

// [dev] The drill's fixture: the next state for this ATV is held, not applied, until the copy builds its next condition
// block, so the block carries the ack of the state before it (atv_host_sync.h).
std::wstring g_holdKey;
Payload g_held{};
bool g_haveHeld = false;

void ReleaseHeld() {
    if (!g_haveHeld) return;
    g_haveHeld = false;
    void* atv = Find(g_holdKey);
    if (atv) ApplyState(g_holdKey, atv, g_held, 0);
}

// ---- CLIENT: the open-request gate ----

constexpr size_t kMaxOpen = 4;        // requests open on one ATV
constexpr size_t kMaxOpenTotal = 8;   // and over all of them: the host's per-sender queue (atv_host_judge)
struct Gate {
    std::vector<uint32_t> open;
    bool fullSaid = false;
};
std::unordered_map<std::wstring, Gate> g_gate;
uint32_t g_nextReq = 0;
bool g_totalFullSaid = false;

size_t OpenTotal() {
    size_t n = 0;
    for (const auto& kv : g_gate) n += kv.second.open.size();
    return n;
}

// Every set cleared by an exit that is no answer; each request still unanswered is counted and said once.
void ClearGate(const char* why) {
    const size_t n = OpenTotal();
    if (n) {
        g_n.unanswered += static_cast<uint32_t>(n);
        UE_LOGI("atv_host: %zu request(s) unanswered at %s -- the open-request gate is cleared", n, why);
    }
    g_gate.clear();
    g_totalFullSaid = false;
}

// ---- CLIENT: the unadopted rows ----

// The newest state per ATV this client holds no copy of yet, 8 rows, the oldest replaced (walker_state_client's shape),
// applied at the copy's adoption (NoteAtvIndexed) and dropped at the session's end.
constexpr size_t kMaxUnadopted = 8;
std::vector<Payload> g_unadopted;
std::wstring g_droppedForDrill;   // [dev] the key whose state the red atv_joiner_rows_off dropped (UnadoptedDecidedForDrill)

void KeepUnadopted(const std::wstring& key, const Payload& p) {
    for (Payload& row : g_unadopted)
        if (coop::net::StringFromWireKey(row.key) == key) {
            row = p;
            return;
        }
    if (g_unadopted.size() >= kMaxUnadopted) g_unadopted.erase(g_unadopted.begin());
    g_unadopted.push_back(p);
    ++g_n.kept;
}

}  // namespace

bool Vitals::operator==(const Vitals& o) const {
    return fuel == o.fuel && health == o.health && flags == o.flags && count == o.count &&
           std::memcmp(modules, o.modules, count) == 0;
}

ApplyScope::ApplyScope() { ++g_applying; }
ApplyScope::~ApplyScope() { --g_applying; }
bool Applying() { return g_applying > 0; }

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    coop::atv_host_judge::Install();
}

coop::net::Session* LiveSession() { return Live(); }

void Tick() {
    coop::net::Session* s = Live();
    if (!s || !A::EnsureResolved()) return;
    const uint32_t world = ue_wrap::world_identity::Generation();
    if (world != g_world) {
        g_world = world;
        ClearGate("a world change");
    }
    coop::atv_gate::SettleWatches();
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
    coop::atv_host_judge::OnStatePublished(key);
    DE::Note(DE::kPublished, static_cast<int32_t>(p.stateSeq), now.count, 0, now.fuel, now.health);
    UE_LOGI("atv_host: '%ls' state #%u published -- fuel %.1f health %.1f modules %hs%s%s", key.c_str(), p.stateSeq,
            now.fuel, now.health, ModulesText(now).c_str(), (now.flags & coop::net::kAtvHostEmpty) ? " empty" : "",
            (now.flags & coop::net::kAtvHostBroken) ? " broken" : "");
    return true;
}

void OnState(const Payload& p) {
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    if (key.empty() || !A::EnsureResolved()) return;
    if (p.moduleCount > kMaxModules) {
        ++g_n.dropped;
        return;
    }
    void* atv = Find(key);
    if (p.op == op::kAnswer) {
        const auto it = g_gate.find(key);
        if (it == g_gate.end() ||
            std::find(it->second.open.begin(), it->second.open.end(), p.reqId) == it->second.open.end()) {
            ++g_n.stray;
            return;
        }
        std::vector<uint32_t>& ids = it->second.open;
        ids.erase(std::find(ids.begin(), ids.end(), p.reqId));
        ++g_n.answered;
        coop::atv_gate::OnAnswer(p.reqId, p.module);
        const bool closed = ids.empty();
        if (closed) {
            g_gate.erase(it);
            // An answer that carries no state (the ATV is gone on the host, or it could not read it) writes nothing.
            if (atv && p.module != coop::net::atv_host_reason::kGone && !(p.flags & coop::net::kAtvHostNoState))
                ApplyState(key, atv, p, 1);
        }   // else the state this answer carries is not applied: a request of this client's is still out
        DE::Note(DE::kAnswer, static_cast<int32_t>(p.reqId), p.module, closed ? 1 : 0, p.fuel, p.health, p.moduleCount);
        return;
    }
    if (p.op != op::kState) {
        ++g_n.dropped;
        return;
    }
    if (!atv) {
        // Red atv_joiner_rows_off: a state for an ATV with no copy is dropped, not kept for the copy's adoption.
        static const bool s_rowsOff = coop::config::ResolveEnum(::coop::config_registry::rows::atv_gate_drill) == "atv_joiner_rows_off";
        if (!s_rowsOff) KeepUnadopted(key, p);
        else g_droppedForDrill = key;
        DE::Note(DE::kUnadopted, static_cast<int32_t>(p.stateSeq), s_rowsOff ? 0 : 1, DE::KeyTag(key));
        return;
    }
    if (VitalsGated(key)) {
        ++g_n.gated;
        // Red atv_gate_hold_off: the state applies while the open-request gate is shut.
        static const bool s_holdOff = coop::config::ResolveEnum(::coop::config_registry::rows::atv_gate_drill) == "atv_gate_hold_off";
        if (s_holdOff) ApplyState(key, atv, p, 0);
        DE::Note(DE::kStateGated, static_cast<int32_t>(p.stateSeq), s_holdOff ? 1 : 0, 0, p.fuel, A::GetFuel(atv));
        return;
    }
    if (g_holdKey == key) {
        g_held = p;
        g_haveHeld = true;
        return;
    }
    ApplyState(key, atv, p, 0);
}

bool UnadoptedDecidedForDrill(const std::wstring& key) {
    if (key == g_droppedForDrill) return true;
    for (const Payload& row : g_unadopted)
        if (coop::net::StringFromWireKey(row.key) == key) return true;
    return false;
}

void HoldNextStateForDrill(const std::wstring& key) {
    ReleaseHeld();
    g_holdKey = key;
}

void NoteBlockBuilt(const std::wstring& key) {
    if (g_haveHeld && key == g_holdKey) ReleaseHeld();
}

bool RequestCapReached(void* atv) {
    coop::net::Session* s = Live();
    if (!s || s->role() != coop::net::Role::Client || g_gate.empty()) return false;   // nothing open: no key is built
    if (OpenTotal() >= kMaxOpenTotal) return true;
    const auto it = g_gate.find(A::GetKeyString(atv));
    return it != g_gate.end() && it->second.open.size() >= kMaxOpen;
}

uint32_t Ask(const std::wstring& key, uint8_t opcode, float amount, uint8_t module) {
    coop::net::Session* s = Live();
    if (!s || s->role() != coop::net::Role::Client) return 0;
    // The adoption's sync is exempt from the total: one per ATV, asked only while its gate is open for nothing else.
    if (opcode != op::kSync && OpenTotal() >= kMaxOpenTotal) {
        ++g_n.refusedAsks;
        if (!g_totalFullSaid) {
            g_totalFullSaid = true;
            UE_LOGI("atv_host: %zu requests are open over all ATVs -- this request is not sent (said once)",
                    kMaxOpenTotal);
        }
        return 0;
    }
    Gate& g = g_gate[key];
    if (g.open.size() >= kMaxOpen) {
        ++g_n.refusedAsks;
        if (!g.fullSaid) {
            g.fullSaid = true;
            UE_LOGI("atv_host: '%ls' already has %zu requests open -- this request is not sent (said once)",
                    key.c_str(), kMaxOpen);
        }
        return 0;
    }
    if (++g_nextReq == 0) g_nextReq = 1;
    Payload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.op = opcode;
    p.amount = amount;
    p.module = module;
    p.reqId = g_nextReq;
    g.open.push_back(p.reqId);   // registered before the send: the answer cannot beat it
    if (!s->SendReliable(coop::net::ReliableKind::AtvRequest, &p, sizeof(p))) {
        g.open.pop_back();
        if (g.open.empty()) g_gate.erase(key);
        return 0;
    }
    ++g_n.asked;
    return p.reqId;
}


void NoteAtvGone(const std::wstring& key) {
    const auto it = g_gate.find(key);
    if (it == g_gate.end()) return;
    g_n.unanswered += static_cast<uint32_t>(it->second.open.size());
    UE_LOGI("atv_host: '%ls' is gone with %zu request(s) unanswered -- its gate is cleared", key.c_str(),
            it->second.open.size());
    g_gate.erase(it);
}

void NoteAtvIndexed(const std::wstring& key, void* atv) {
    bool applied = false;
    uint32_t seq = 0;
    for (auto it = g_unadopted.begin(); it != g_unadopted.end(); ++it) {
        if (coop::net::StringFromWireKey(it->key) != key) continue;
        const Payload p = *it;
        g_unadopted.erase(it);
        seq = p.stateSeq;
        if (!VitalsGated(key)) {
            ApplyState(key, atv, p, 2);
            applied = true;
        }
        break;
    }
    if (DE::On()) {
        Vitals now;
        if (ReadVitals(atv, now))
            DE::Note(DE::kAdopted, applied ? 1 : 0, DE::KeyTag(key), static_cast<int32_t>(seq), now.fuel, now.health, now.count);
    }
    // A copy that took no state of the host's asks for it. MTA binds a vehicle's state to the entity's add packet
    // (reference/mtasa-blue/Server/mods/deathmatch/logic/packets/CEntityAddPacket.cpp:208, :454, :531-552), so a joiner never
    // lacks it. Here the copy comes from this client's own save, adopted by its key, and no host entity-add exists to carry
    // the state: the pull at adoption is our add-time. The host's replay may also have been lost to the unadopted rows' cap,
    // and the copy's ack would stay behind the host's sequence for good, so a syncer's vitals would never be taken. A request
    // of this client's already open on the ATV holds the gate, and its answer carries the state.
    if (!applied && !VitalsGated(key)) Ask(key, op::kSync, 0.f, 0);
}

void QueueConnectBroadcastForSlot(int slot) {
    coop::net::Session* s = Live();
    if (!s || s->role() != coop::net::Role::Host) return;
    if (A::EnsureResolved()) {
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
    // The slot is world-ready: its parked requests are served.
    coop::atv_host_judge::OnSlotReady(slot);
}

void OnDisconnect() {
    ClearGate("the session's end");
    if (g_n.published || g_n.replayed || g_n.applied || g_n.gated || g_n.dropped || g_n.asked || g_n.kept)
        UE_LOGI("atv_host: session end -- states published=%u replayed=%u; applied=%u gated=%u kept=%u dropped=%u; asked=%u "
                "answered=%u refused=%u stray=%u unanswered=%u", g_n.published, g_n.replayed, g_n.applied, g_n.gated,
                g_n.kept, g_n.dropped, g_n.asked, g_n.answered, g_n.refusedAsks, g_n.stray, g_n.unanswered);
    g_n = Counts{};
    g_unadopted.clear();
    g_droppedForDrill.clear();
    g_nextReq = 0;
    coop::atv_gate::OnDisconnect();
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

bool VitalsGated(const std::wstring& key) {
    const auto it = g_gate.find(key);
    return it != g_gate.end() && !it->second.open.empty();
}

}  // namespace coop::atv_host_sync
