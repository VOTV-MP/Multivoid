// coop/world/dug_hole_sync.cpp -- see coop/world/dug_hole_sync.h.

#include "coop/world/dug_hole_sync.h"

#include "coop/element/intent_authority.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/intent_queue.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster_ledger.h"  // PerSlotState
#include "coop/props/active_drive.h"
#include "coop/requester_feedback/requester_feedback.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/world/ambient_births.h"
#include "coop/world/spawn_authority.h"

#include "ue_wrap/actors/dig_tool.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop::dug_hole_sync {
namespace {

namespace AB = coop::ambient_births;
namespace DT = ue_wrap::dig_tool;
namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;
namespace SG = ue_wrap::script_gate;
namespace dh = coop::net::dug_hole;
using State = coop::net::DugHoleStatePayload;
using Intent = coop::net::DugHoleIntentPayload;

// The hole's begin play: the dig-up of a buried item it overlaps. A native AActor::BeginPlay dispatches it by
// ProcessEvent, which the script-body gate sees.
UE_FACT_FN(kRowHoleBeginPlay, "dug_hole_sync", L"dirthole_C", L"ReceiveBeginPlay", Declared);
constexpr int kTag = 0x44484F4C;  // 'DHOL'

// useShovel traces from the holder's arm; the bound is the pile dig's, which leaves room for the pose's latency.
constexpr float kReachUU = 400.0f;
// One press is one hole; an honest sender stays far inside a burst of three and three a second.
constexpr coop::net::IntentBudget kBudget{3.0f, 3.0f};
constexpr size_t kMaxPending = 8;
constexpr int kSendsPerTick = 32;
constexpr uint64_t kSayEveryMs = 10000;

std::atomic<coop::net::Session*> g_session{nullptr};
std::vector<AB::Event> g_events;  // reused by every drain
Counters g_counters;
bool g_gateTaken = false;
bool g_gateLiveSaid = false;

bool Finite(float v) { return std::isfinite(v) && std::fabs(v) <= coop::net::kMaxCoord; }

bool IsClient() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Client;
}

// ---- HOST ----

struct Known {
    int32_t  index = -1;
    State    wire{};
    bool     placed = false;
    bool     sentAny = false;
    bool     pendingAll = true;   // goes to every client
    uint32_t pendingSlots = 0;    // goes to these joiners alone (bit = slot)
};
std::unordered_map<void*, Known> g_known;
uint32_t g_nextId = 1;
std::vector<State> g_ends;
bool g_work = false;

struct Slot {
    coop::net::IntentQueue<Intent> queue;
    uint64_t nextSayMs = 0;
};
coop::roster_ledger::PerSlotState<Slot> g_slots;

bool ReadPlace(void* obj, Known& k) {
    ue_wrap::FVector at{};
    ue_wrap::FRotator rot{};
    uint8_t ground = 0;
    if (!E::TryGetActorLocation(obj, at) || !E::TryGetActorRotation(obj, rot) || !DT::ReadHoleGround(obj, ground))
        return false;
    k.wire.x = at.X;
    k.wire.y = at.Y;
    k.wire.z = at.Z;
    k.wire.pitch = rot.Pitch;
    k.wire.yaw = rot.Yaw;
    k.wire.roll = rot.Roll;
    k.wire.matType = ground;
    k.placed = true;
    return true;
}

void DrainHost() {
    for (const AB::Event& e : g_events) {
        if (e.kind == AB::Kind::Death) {
            const auto it = g_known.find(e.obj);
            if (it == g_known.end() || it->second.index != e.index) continue;
            if (it->second.sentAny) {
                State end = it->second.wire;
                end.alive = 0;
                g_ends.push_back(end);
            }
            UE_LOGI("[DUG-HOLE] hole %u ended", it->second.wire.id);
            g_known.erase(it);
            g_work = true;
            continue;
        }
        if (!R::IsLiveByIndex(e.obj, e.index) || !DT::IsDugHole(e.obj)) continue;  // a sus hole's child is not one
        auto it = g_known.find(e.obj);
        if (it != g_known.end() && it->second.index == e.index) continue;  // a repeat of the seed
        if (it != g_known.end()) g_known.erase(it);
        Known k;
        k.index = e.index;
        k.wire.id = g_nextId++;
        k.wire.alive = 1;
        g_known.emplace(e.obj, k);
        g_work = true;
    }
}

uint32_t ConnectedSlots(const coop::net::Session& s) {
    uint32_t mask = 0;
    for (int slot = 1; slot < coop::players::kMaxPeers; ++slot)
        if (s.IsSlotConnected(slot)) mask |= 1u << slot;
    return mask;
}

void HostPass(coop::net::Session& s) {
    const uint32_t connected = ConnectedSlots(s);
    bool again = false;
    int sends = 0;
    for (auto& kv : g_known) {
        Known& k = kv.second;
        k.pendingSlots &= connected;
        if (!k.placed && !ReadPlace(kv.first, k)) {
            again = true;
            continue;
        }
        if (!k.pendingAll && !k.pendingSlots) continue;
        if (sends >= kSendsPerTick) {
            again = true;
            continue;
        }
        if (k.pendingAll) {
            if (s.SendReliable(coop::net::ReliableKind::DugHoleState, &k.wire, sizeof(State))) {
                k.pendingAll = false;
                k.pendingSlots = 0;
                k.sentAny = true;
                ++sends;
                ++g_counters.sent;
            } else {
                again = true;
            }
            continue;
        }
        for (int slot = 1; slot < coop::players::kMaxPeers; ++slot) {
            if (!(k.pendingSlots & (1u << slot))) continue;
            if (s.SendReliableToSlot(slot, coop::net::ReliableKind::DugHoleState, &k.wire, sizeof(State))) {
                k.pendingSlots &= ~(1u << slot);
                ++sends;
                ++g_counters.sent;
            } else {
                again = true;
            }
        }
    }
    for (size_t i = 0; i < g_ends.size();) {
        if (s.SendReliable(coop::net::ReliableKind::DugHoleState, &g_ends[i], sizeof(State))) {
            ++g_counters.sent;
            g_ends.erase(g_ends.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
            again = true;
        }
    }
    g_work = again;
}

void Deny(uint8_t slot, const Intent& p, const char* why) {
    ++g_counters.denied;
    UE_LOGI("[DUG-HOLE] DENY op=%u slot=%u at (%.0f %.0f %.0f) -- %s", static_cast<unsigned>(p.op),
            static_cast<unsigned>(slot), p.x, p.y, p.z, why);
}

coop::net::Served ExecuteDig(coop::net::Session& s, const Intent& p, uint8_t slot) {
    if (!coop::requester_feedback::Ready() && !coop::requester_feedback::Refused()) return coop::net::Served::Wait;
    if (!DT::IsShovel(coop::hand_item::MirrorActorForSlot(slot))) {
        Deny(slot, p, "the sender holds no shovel here");
        return coop::net::Served::Done;
    }
    const ue_wrap::FVector at{p.x, p.y, p.z};
    const coop::element::IntentSubject sub =
        coop::element::IntentTarget::ForClientIntent(s, slot, kReachUU).AuthorizeSegment(at, at);
    if (sub.outcome == coop::element::IntentOutcome::NoBody) return coop::net::Served::Wait;
    if (!sub) {
        Deny(slot, p, coop::element::OutcomeName(sub.outcome));
        return coop::net::Served::Done;
    }
    void* hole = nullptr;
    {
        // The hole's begin play may dig up a buried item and play its sound in 2D: the digger hears it.
        coop::requester_feedback::Scope feedback(slot, 0);
        hole = DT::SpawnDugHole(at, ue_wrap::FRotator{p.pitch, p.yaw, p.roll}, p.matType);
    }
    if (!hole) {
        Deny(slot, p, "the hole did not spawn");
        return coop::net::Served::Done;
    }
    ++g_counters.made;
    UE_LOGI("[DUG-HOLE] DUG a hole for slot=%u at (%.0f %.0f %.0f) ground=%u", static_cast<unsigned>(slot), p.x, p.y,
            p.z, static_cast<unsigned>(p.matType));
    return coop::net::Served::Done;
}

coop::net::Served ExecuteEnd(coop::net::Session& s, const Intent& p, uint8_t slot) {
    void* hole = nullptr;
    Known* known = nullptr;
    for (auto& kv : g_known) {
        if (kv.second.wire.id != p.id) continue;
        hole = kv.first;
        known = &kv.second;
        break;
    }
    if (!hole || !R::IsLiveByIndex(hole, known->index)) {
        UE_LOGI("[DUG-HOLE] slot=%u ended hole %u, which no longer stands here", static_cast<unsigned>(slot), p.id);
        return coop::net::Served::Done;
    }
    const ue_wrap::FVector at{known->wire.x, known->wire.y, known->wire.z};
    const coop::element::IntentSubject sub =
        coop::element::IntentTarget::ForClientIntent(s, slot, kReachUU).AuthorizeSegment(at, at);
    if (sub.outcome == coop::element::IntentOutcome::NoBody) return coop::net::Served::Wait;
    if (!sub) {
        Deny(slot, p, coop::element::OutcomeName(sub.outcome));
        return coop::net::Served::Done;
    }
    if (!DT::DestroyHole(hole)) {
        Deny(slot, p, "the hole's destroy did not dispatch");
        return coop::net::Served::Done;
    }
    ++g_counters.endedFor;
    UE_LOGI("[DUG-HOLE] ended hole %u for slot=%u", p.id, static_cast<unsigned>(slot));
    return coop::net::Served::Done;
}

void HostTick(coop::net::Session& s) {
    DrainHost();
    if (g_work && s.AnyWorldReadyPeer()) HostPass(s);
    for (uint8_t slot = 1; slot < coop::players::kMaxPeers; ++slot) {
        g_slots[slot].queue.Serve(kBudget, coop::active_drive::NowMs, [&](const Intent& p) {
            return p.op == dh::kDig ? ExecuteDig(s, p, slot) : ExecuteEnd(s, p, slot);
        });
    }
}

// ---- CLIENT ----

struct Ref {
    void*   obj = nullptr;
    int32_t index = -1;
};
std::unordered_map<uint32_t, Ref> g_copies;   // by the host's number
std::unordered_map<void*, uint32_t> g_byObj;   // a copy's actor to its number, until it ends
std::vector<State> g_inbox;
std::vector<ue_wrap::CachedObjRef> g_stamped;  // this player's own holes, refused at begin play
std::vector<Intent> g_outbox;
bool g_spawningCopy = false;
uint32_t g_world = 0;

// The gate on a client: a copy keeps the host's dig-up; any other dug hole is this player's own, the host's to make.
SG::Verdict OnBeginPlayPre(const SG::Call& call) {
    if (!call.object || !coop::spawn_authority::IsActiveClientSession() || !DT::IsDugHole(call.object))
        return SG::Verdict::Run;
    if (g_spawningCopy) return SG::Verdict::Cancel;
    ue_wrap::CachedObjRef ref;
    ref.Set(call.object);
    g_stamped.push_back(ref);
    ++g_counters.stamped;
    return SG::Verdict::Cancel;
}

void DrainClient() {
    for (const AB::Event& e : g_events) {
        if (e.kind != AB::Kind::Death) continue;
        const auto it = g_byObj.find(e.obj);
        if (it == g_byObj.end()) continue;
        const uint32_t id = it->second;
        g_byObj.erase(it);
        const auto c = g_copies.find(id);
        if (c == g_copies.end() || c->second.index != e.index) continue;
        g_copies.erase(c);
        // Ended here, not by the host's word: this player buried, planted or filled it.
        Intent p{};
        p.id = id;
        p.op = dh::kEnd;
        g_outbox.push_back(p);
        UE_LOGI("[DUG-HOLE] CLIENT the copy of hole %u ended here -- the host ends its hole", id);
    }
}

void SpawnCopy(const State& p) {
    g_spawningCopy = true;
    void* hole = DT::SpawnDugHole({p.x, p.y, p.z}, {p.pitch, p.yaw, p.roll}, p.matType);
    g_spawningCopy = false;
    if (!hole) {
        UE_LOGW("[DUG-HOLE] CLIENT hole %u did not spawn", p.id);
        return;
    }
    g_copies[p.id] = Ref{hole, R::InternalIndexOf(hole)};
    g_byObj[hole] = p.id;
    ++g_counters.copies;
}

void ApplyInbox() {
    std::vector<State> inbox;
    inbox.swap(g_inbox);
    for (const State& p : inbox) {
        const auto it = g_copies.find(p.id);
        if (p.alive) {
            if (it == g_copies.end()) SpawnCopy(p);
            continue;
        }
        if (it == g_copies.end()) continue;
        const Ref r = it->second;
        g_copies.erase(it);
        g_byObj.erase(r.obj);  // first: its death is the host's word, not this player's use
        if (R::IsLiveByIndex(r.obj, r.index)) DT::DestroyHole(r.obj);
        ++g_counters.wireEnds;
    }
}

void ClientTick(coop::net::Session& s) {
    const uint32_t world = ue_wrap::world_identity::Generation();
    if (world != g_world) {  // the copies and holes of a world the load discarded are gone with it
        g_world = world;
        g_copies.clear();
        g_byObj.clear();
        g_stamped.clear();
        g_outbox.clear();
    }
    DrainClient();
    if (!coop::net_pump::HasAnnouncedWorldReady() || coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle ||
        !s.connected())
        return;
    for (ue_wrap::CachedObjRef& ref : g_stamped) {
        void* hole = ref.Get();
        if (!hole) continue;
        ue_wrap::FVector at{};
        ue_wrap::FRotator rot{};
        uint8_t ground = 0;
        const bool read = E::TryGetActorLocation(hole, at) && E::TryGetActorRotation(hole, rot) &&
                          DT::ReadHoleGround(hole, ground);
        DT::DestroyHole(hole);  // the host makes the one hole; this player's dig is only its word
        if (!read) continue;
        Intent p{};
        p.op = dh::kDig;
        p.x = at.X;
        p.y = at.Y;
        p.z = at.Z;
        p.pitch = rot.Pitch;
        p.yaw = rot.Yaw;
        p.roll = rot.Roll;
        p.matType = ground;
        g_outbox.push_back(p);
        UE_LOGI("[DUG-HOLE] CLIENT this player dug a hole at (%.0f %.0f %.0f) -- the host makes it", at.X, at.Y, at.Z);
    }
    g_stamped.clear();
    for (auto it = g_outbox.begin(); it != g_outbox.end();) {
        if (!s.SendReliable(coop::net::ReliableKind::DugHoleIntent, &*it, sizeof(Intent))) break;
        ++g_counters.intents;
        it = g_outbox.erase(it);
    }
    ApplyInbox();
}

void TakeGate() {
    if (!g_gateTaken) {
        g_gateTaken = SG::WatchName(kRowHoleBeginPlay, kTag, &OnBeginPlayPre, nullptr);
        return;
    }
    if (g_gateLiveSaid) return;
    SG::ResolvePendingNames();
    if (SG::NameWatchLive(kRowHoleBeginPlay, kTag)) {
        g_gateLiveSaid = true;
        UE_LOGI("[DUG-HOLE] the hole's begin-play gate is live");
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    TakeGate();
}

void Tick(coop::net::Session& session) {
    if (!g_session.load(std::memory_order_acquire) || !session.running()) return;
    TakeGate();
    g_events.clear();
    AB::Drain(AB::Bit(AB::Class::Dirt), g_events);
    if (session.role() == coop::net::Role::Host) HostTick(session);
    else if (session.role() == coop::net::Role::Client) ClientTick(session);
}

void OnState(const coop::net::DugHoleStatePayload& p) {
    if (IsClient()) g_inbox.push_back(p);
}

void OnIntent(coop::net::Session& session, int senderSlot, const coop::net::DugHoleIntentPayload& p) {
    if (session.role() != coop::net::Role::Host) return;
    if (senderSlot < 1 || senderSlot >= coop::players::kMaxPeers) return;
    const uint8_t slot = static_cast<uint8_t>(senderSlot);
    if (p.op != dh::kDig && p.op != dh::kEnd) {
        Deny(slot, p, "an unknown op");
        return;
    }
    if (p.op == dh::kDig && (!Finite(p.x) || !Finite(p.y) || !Finite(p.z) || !std::isfinite(p.pitch) ||
                             !std::isfinite(p.yaw) || !std::isfinite(p.roll) || p.matType > 2)) {
        Deny(slot, p, "the place is not a finite point in the world");
        return;
    }
    Slot& sl = g_slots[slot];
    Intent copy = p;
    if (!sl.queue.Push(std::move(copy), kMaxPending)) {
        ++g_counters.denied;
        const uint64_t now = coop::active_drive::NowMs();
        if (now >= sl.nextSayMs) {
            sl.nextSayMs = now + kSayEveryMs;
            UE_LOGW("[DUG-HOLE] slot %u queue full (%zu) -- refusing until it drains", static_cast<unsigned>(slot),
                    sl.queue.size());
        }
    }
}

void QueueConnectBroadcastForSlot(int slot) {
    if (slot <= 0 || slot >= coop::players::kMaxPeers) return;
    for (auto& kv : g_known) kv.second.pendingSlots |= 1u << slot;
    g_work = true;
}

void OnDisconnect() {
    if (g_counters.sent || g_counters.made || g_counters.denied || g_counters.stamped || g_counters.copies)
        UE_LOGI("[DUG-HOLE] session end -- sent=%llu made=%llu ended=%llu denied=%llu stamped=%llu intents=%llu "
                "copies=%llu",
                static_cast<unsigned long long>(g_counters.sent), static_cast<unsigned long long>(g_counters.made),
                static_cast<unsigned long long>(g_counters.endedFor), static_cast<unsigned long long>(g_counters.denied),
                static_cast<unsigned long long>(g_counters.stamped), static_cast<unsigned long long>(g_counters.intents),
                static_cast<unsigned long long>(g_counters.copies));
    if (IsClient()) {
        std::unordered_map<uint32_t, Ref> copies;
        copies.swap(g_copies);
        g_byObj.clear();
        for (const auto& kv : copies)
            if (R::IsLiveByIndex(kv.second.obj, kv.second.index)) DT::DestroyHole(kv.second.obj);
    }
    g_counters = Counters{};
    g_known.clear();
    g_nextId = 1;
    g_ends.clear();
    g_work = false;
    for (int slot = 1; slot < coop::players::kMaxPeers; ++slot) g_slots[slot] = Slot{};
    g_copies.clear();
    g_byObj.clear();
    g_inbox.clear();
    g_stamped.clear();
    g_outbox.clear();
    g_events.clear();
    g_session.store(nullptr, std::memory_order_release);
}

Counters Read() { return g_counters; }

}  // namespace coop::dug_hole_sync
