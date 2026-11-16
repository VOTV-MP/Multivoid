// coop/props/container_break.cpp -- see coop/props/container_break.h.

#include "coop/props/container_break.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/intent_bucket.h"   // a sender's budget of breaks, and of replies
#include "coop/net/net_clock.h"       // NowMs: the budgets' clock
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/hand_item.h"    // a hacksaw in the sender's hand, as the hand lane has applied it
#include "coop/player/hit_judge.h"    // kReachSlackCm
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"  // PerSlotState
#include "coop/props/hacksaw_cut.h"   // the client's saw learns of its grant or refusal
#include "coop/props/prop_drive_host.h"  // ClientSyncerOf: the client whose row stands on a prop
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/remote_prop.h"      // IsDrivenBy: whether a slot's held-pose stream moves a prop
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::props::container_break {
namespace {

namespace R  = ue_wrap::reflection;
namespace ci = ue_wrap::container_inventory;
using Payload = coop::net::ContainerBreakPayload;

constexpr float kReachCm = 1000.f;     // a break's sender from its container, on the host (a throw carries it away)
// A cut's reach. The saw takes its target from the player's own `hitResult`, a trace along the camera as long as
// mainPlayer's `armLength`, which is 200 cm when the hand holds no weapon row (a saw is none: mainPlayer.offsets.txt
// :11130). The surface point it hits lies within that, plus the slack the host allows a pose it holds a moment behind
// (kReachSlackCm, which a swing's judge allows the same way). The host measures from the puppet's eye (MeasureSawReach).
constexpr float kSawArmCm = 200.f;
constexpr float kSawReachCm = kSawArmCm + coop::hit_judge::kReachSlackCm;
constexpr float kLethal = 1000000.f;   // a damage past any container's health and resistance
// A client's requests wait in a queue of its own: one sender's flood cannot drop another's (the nametag lane's shape).
constexpr size_t kSlotQueue = 4;
constexpr size_t kOutboxCap = 16;
// A sender's budget is our addition: MTA bounds only a join's flood (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:163).
constexpr coop::net::IntentBudget kBreakBudget{2.0f, 1.0f};
// The replies a sender is owed: a hostile flood loses its replies (counted), an honest client asks once per cut.
constexpr coop::net::IntentBudget kReplyBudget{4.0f, 2.0f};
constexpr const wchar_t* kSawClass = L"prop_hacksaw_C";

std::atomic<coop::net::Session*> g_session{nullptr};
std::vector<Payload> g_outbox;                                         // CLIENT
coop::roster_ledger::PerSlotState<std::vector<Payload>> g_inbox;       // HOST: each client's requests, in arrival order
coop::roster_ledger::PerSlotState<coop::net::IntentBucket> g_budget;   // HOST: spent by every request judged
coop::roster_ledger::PerSlotState<coop::net::IntentBucket> g_replyBudget;   // HOST: spent by every reply sent
// HOST: the cuts it granted, by the prop's key, until the sender's destroy of the prop arrives (OnClientDestroy breaks the
// host's copy then), the sender withdraws the grant (op 4: its cut ended without the destroy), the sender leaves, the prop dies
// another way or the sender is refused a later request for it. A
// second slot's request for a granted prop is refused, so two saws cannot both be granted one prop.
struct Granted {
    int slot = 0;
    ue_wrap::CachedObjRef prop;
};
std::unordered_map<std::wstring, Granted> g_granted;
uint32_t g_asked = 0, g_broken = 0, g_granted_n = 0, g_refused = 0, g_malformed = 0, g_refusedAnswers = 0;
uint32_t g_grantedAnswers = 0, g_droppedImpacts = 0, g_repliesDropped = 0, g_outboxFull = 0;
uint32_t g_withdrawn = 0, g_strayWithdrawals = 0;

std::wstring KeyOf(void* a) {
    std::wstring key = ue_wrap::prop::GetInteractableKeyString(a);
    if (key.empty() || key == L"None" || key.size() >= sizeof(coop::net::WireKey::data)) return {};
    return key;
}

coop::net::Session* Live() {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    return s && s->connected() ? s : nullptr;
}

bool SameRequest(const Payload& a, const Payload& b) {
    return a.op == b.op && a.key.len == b.key.len && a.fire == b.fire && a.sawn == b.sawn &&
           std::memcmp(a.key.data, b.key.data, a.key.len) == 0;
}

// The prop's own physicsImpact component.
void* ImpactOf(void* prop) {
    static int32_t s_off = -2;
    if (s_off == -2) {
        void* cls = R::FindClass(L"prop_C");
        if (!cls) return nullptr;
        s_off = R::FindPropertyOffset(cls, L"physicsImpact");
    }
    if (s_off < 0) return nullptr;
    void* comp = *reinterpret_cast<void**>(static_cast<uint8_t*>(prop) + s_off);
    return comp && R::IsLive(comp) ? comp : nullptr;
}

// The host's own copy breaks by the same lethal call: its `broken` spills, its component destroys it.
bool Break(void* container, bool fire) {
    void* comp = ImpactOf(container);
    void* fn = comp ? R::FindFunction(R::ClassOf(comp), fire ? L"fireDamage" : L"impactDamage") : nullptr;
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid()) return false;
    f.Set<float>(L"damage", kLethal);
    return ue_wrap::Call(comp, f);
}

// The cut's part that is the host's, run when the sender's destroy of the granted prop arrives: the prop's own `broken` on
// the host's copy (a container spills what it holds there, so the spill is the host's). The destroy is the client's saw's,
// which relays it, and the scrap is the client's player's own births.
bool BreakForCut(void* prop) { return ue_wrap::component_calls::CallParamlessNamed(prop, L"broken"); }

// A cut's reach as the saw measures it: its trace runs from the player's CAMERA to the prop's SURFACE, so the host measures
// from the puppet's eye to the nearest point of the host copy's colliding bounds, never origin to origin (a desk is wide
// and its origin is not on the surface the trace hits). The numbers are kept for the log.
struct SawReach {
    bool ok = false;
    float dist = 0;
    ue_wrap::FVector body{}, eye{}, origin{}, extent{}, nearest{};
};

SawReach MeasureSawReach(int slot, void* prop) {
    SawReach r;
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
    void* body = rp && rp->valid() ? rp->GetActor() : nullptr;
    if (!body || !ue_wrap::engine::TryGetActorLocation(body, r.body) ||
        !ue_wrap::engine::GetActorBounds(prop, true, r.origin, r.extent))
        return r;
    if (!rp->TryGetEyePosition(r.eye)) r.eye = r.body;
    auto clampTo = [](float v, float c, float e) { return v < c - e ? c - e : v > c + e ? c + e : v; };
    r.nearest = {clampTo(r.eye.X, r.origin.X, r.extent.X), clampTo(r.eye.Y, r.origin.Y, r.extent.Y),
                 clampTo(r.eye.Z, r.origin.Z, r.extent.Z)};
    const float dx = r.nearest.X - r.eye.X, dy = r.nearest.Y - r.eye.Y, dz = r.nearest.Z - r.eye.Z;
    r.dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    r.ok = true;
    return r;
}

const char* SawReachLine(const SawReach& r, char* buf, size_t n) {
    std::snprintf(buf, n,
                  "body %.0f,%.0f,%.0f eye %.0f,%.0f,%.0f prop origin %.0f,%.0f,%.0f extent %.0f,%.0f,%.0f nearest bounds point "
                  "%.0f,%.0f,%.0f -> %.0f cm from the eye, the bound is %.0f",
                  r.body.X, r.body.Y, r.body.Z, r.eye.X, r.eye.Y, r.eye.Z, r.origin.X, r.origin.Y, r.origin.Z, r.extent.X,
                  r.extent.Y, r.extent.Z, r.nearest.X, r.nearest.Y, r.nearest.Z, r.dist, kSawReachCm);
    return buf;
}

bool InReach(int slot, void* prop, float reachCm) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
    void* body = rp && rp->valid() ? rp->GetActor() : nullptr;
    ue_wrap::FVector at{}, by{};
    if (!body || !ue_wrap::engine::TryGetActorLocation(prop, at) || !ue_wrap::engine::TryGetActorLocation(body, by))
        return false;
    const float dx = at.X - by.X, dy = at.Y - by.Y, dz = at.Z - by.Z;
    return dx * dx + dy * dy + dz * dz <= reachCm * reachCm;
}

// Whether `slot` authors the container: a row of its own stands on it (it threw it, or syncs it), or its held-pose stream
// moves it. The two are separate tables (prop_drive_host's rows, remote_prop's drives), and an elected throw's row ends
// at rest, so a break that arrives after the host took the throw back is refused (the client logs it).
bool Authors(void* container, int slot) {
    const coop::element::ElementId eid = coop::prop_element_tracker::GetPropElementIdForActor(container);
    if (eid != coop::element::kInvalidId && coop::prop_drive_host::ClientSyncerOf(eid) == slot) return true;
    return coop::remote_prop::IsDrivenBy(container, slot);
}

const char* ReasonName(uint8_t reason) {
    switch (reason) {
    case coop::net::kContainerBreakNotFound:    return "the host holds no live prop of that key";
    case coop::net::kContainerBreakNotAuthor:   return "the sender neither syncs nor holds it";
    case coop::net::kContainerBreakNoSaw:       return "no hacksaw in the sender's hand";
    case coop::net::kContainerBreakOutOfReach:  return "the prop is out of the sender's reach";
    case coop::net::kContainerBreakTooFast:     return "the sender is past its budget";
    case coop::net::kContainerBreakNotReady:    return "the sender's world is not ready";
    case coop::net::kContainerBreakNotTaken:    return "the host's call did not run";
    default:                                    return "a reason this build does not know";
    }
}

// HOST: a word to one sender, under its budget of replies. A slot whose world is not up cannot be sent to (the pre-world
// gate), so it is counted only.
bool Reply(coop::net::Session& s, int slot, const std::wstring& key, Payload p, uint8_t op, uint8_t reason) {
    if (!s.IsSlotWorldReady(slot)) return false;
    if (!g_replyBudget[slot].Take(kReplyBudget, coop::net::NowMs())) {
        ++g_repliesDropped;
        return false;
    }
    p.op = op;
    p.reason = reason;
    if (s.SendReliableToSlot(slot, coop::net::ReliableKind::ContainerBreakRequest, &p, sizeof(p))) return true;
    UE_LOGW("container_break: the reply to slot %d about '%ls' did not send", slot, key.c_str());
    return false;
}

// HOST: an answer to a cut's request that could not be sent (the reply budget, the lane) is owed: kept, and sent at the next
// Tick, so the client's saw never waits on a reply that was dropped. A grant is recorded only once it is sent: a record
// stands for a grant the client holds. An impact is not owed an answer.
struct Owed {
    Payload ask;
    uint8_t op = 0, reason = 0;
    ue_wrap::CachedObjRef prop;   // a grant's prop
};
coop::roster_ledger::PerSlotState<std::vector<Owed>> g_owed;   // HOST: each client's unsent answers, in order

void RecordGrant(int slot, const std::wstring& key, void* prop) {
    Granted& g = g_granted[key];
    g.slot = slot;
    g.prop.Set(prop);
    ++g_granted_n;
}

// HOST: send an answer; a grant is recorded when it went. False when it did not send.
bool Send(coop::net::Session& s, int slot, const std::wstring& key, const Payload& ask, uint8_t op, uint8_t reason, void* prop) {
    if (!Reply(s, slot, key, ask, op, reason)) return false;
    if (op == coop::net::kContainerBreakGranted) RecordGrant(slot, key, prop);
    return true;
}

void Answer(coop::net::Session& s, int slot, const std::wstring& key, const Payload& ask, uint8_t op, uint8_t reason, void* prop) {
    if (Send(s, slot, key, ask, op, reason, prop) || !ask.sawn) return;
    std::vector<Owed>& owed = g_owed[slot];
    if (owed.size() >= kOutboxCap) {
        ++g_repliesDropped;
        UE_LOGW("container_break: slot %d is owed %zu answers -- the one about '%ls' is dropped", slot, owed.size(), key.c_str());
        return;
    }
    Owed o;
    o.ask = ask;
    o.op = op;
    o.reason = reason;
    o.prop.Set(prop);
    owed.push_back(o);
}

// HOST: the owed answers of one slot, sent in order. A grant whose prop is gone or was granted to another slot meanwhile is
// answered as a refusal, never recorded.
void SendOwed(coop::net::Session& s, int slot) {
    std::vector<Owed>& owed = g_owed[slot];
    while (!owed.empty()) {
        Owed o = owed.front();
        const std::wstring key = coop::net::StringFromWireKey(o.ask.key);
        void* prop = o.prop.Get();
        if (o.op == coop::net::kContainerBreakGranted) {
            const auto it = g_granted.find(key);
            if (!prop || !R::IsLive(prop) || (it != g_granted.end() && it->second.slot != slot)) {
                o.op = coop::net::kContainerBreakRefused;
                o.reason = coop::net::kContainerBreakNotFound;
            }
        }
        if (!Send(s, slot, key, o.ask, o.op, o.reason, prop)) return;
        owed.erase(owed.begin());
    }
}

// HOST: a request that is not taken is answered to its sender alone, with the reason, so the hit is not lost without a
// word. A refused cut changes nothing on the client (its prop is whole and its saw waits for this word), and the saw
// ends the cut itself on it.
// MTA answers a refused request with the server's own correction (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:
// 3363-3366); a cancelled hit or a waiting saw left this copy as it was, so the reason is all a client is owed.
void Refuse(coop::net::Session& s, int slot, const std::wstring& key, const Payload& ask, uint8_t reason) {
    ++g_refused;
    if (ask.sawn) {   // a refused cut ends the sender's grant of it
        const auto it = g_granted.find(key);
        if (it != g_granted.end() && it->second.slot == slot) g_granted.erase(it);
    }
    if (g_refused <= 20 || g_refused % 50 == 0)
        UE_LOGW("container_break: slot %d's %s of '%ls' is refused (%s)", slot, ask.sawn ? "cut" : "break", key.c_str(),
                ReasonName(reason));
    Answer(s, slot, key, ask, coop::net::kContainerBreakRefused, reason, nullptr);
}

// HOST: one request, judged in the order the checks cost. An impact is taken when the sender authors the container; a
// cut when a hacksaw is in the sender's hand as the hand lane applied it (its target is the saw's, which need not be
// authored, and any keyed prop may be sawn), and the prop is not granted to another slot. Both stand within reach. A grant
// only records the cut: the host's copy breaks when the sender's destroy of it arrives, so a cut the player never finishes
// changes nothing on either peer.
// A budget or a queue spent by an impact is dropped without a word (the bucket's contract: a sender past its budget
// costs no answer); a cut's request is answered, since its saw waits for the word, and a reply has a budget of its own.
void Judge(coop::net::Session& s, int slot, const Payload& p) {
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    if (!g_budget[slot].Take(kBreakBudget, coop::net::NowMs())) {
        if (p.sawn) Refuse(s, slot, key, p, coop::net::kContainerBreakTooFast);
        else ++g_droppedImpacts;
        return;
    }
    if (!s.IsSlotWorldReady(slot)) { Refuse(s, slot, key, p, coop::net::kContainerBreakNotReady); return; }
    // The hacksaw drill's refused arm: every cut a client asks is refused.
    static const bool s_refuseAll = coop::config::ResolveFlag(coop::config_registry::rows::sawn_refuse_all);
    if (p.sawn && s_refuseAll) { Refuse(s, slot, key, p, coop::net::kContainerBreakNotTaken); return; }
    void* c = coop::prop_key_index::Find(key);
    const auto already = g_granted.find(key);
    if (!c || !R::IsLive(c) || (!p.sawn && !ci::IsContainer(c)) ||
        (already != g_granted.end() && already->second.slot != slot && already->second.prop.Is(c))) {
        Refuse(s, slot, key, p, coop::net::kContainerBreakNotFound);
        return;
    }
    if (p.sawn) {
        // The saw was held for the seconds it took to cut; the owed hand state, announced ahead of this request on the one
        // ordered lane, is applied now, so a request that rests on the hand it was sent from is judged against it.
        coop::hand_item::ApplyOwedNow(s, static_cast<uint8_t>(slot));
        if (!coop::hand_item::HeldClassIs(static_cast<uint8_t>(slot), kSawClass)) {
            Refuse(s, slot, key, p, coop::net::kContainerBreakNoSaw);
            return;
        }
    } else if (!Authors(c, slot)) {
        Refuse(s, slot, key, p, coop::net::kContainerBreakNotAuthor);
        return;
    }
    SawReach reach;
    if (p.sawn) {
        reach = MeasureSawReach(slot, c);
        if (!reach.ok || reach.dist > kSawReachCm) {
            char line[400];
            UE_LOGW("container_break: slot %d's cut of '%ls' is out of reach: %s", slot, key.c_str(),
                    reach.ok ? SawReachLine(reach, line, sizeof(line)) : "the puppet's or the prop's position did not read");
            Refuse(s, slot, key, p, coop::net::kContainerBreakOutOfReach);
            return;
        }
    } else if (!InReach(slot, c, kReachCm)) {
        Refuse(s, slot, key, p, coop::net::kContainerBreakOutOfReach);
        return;
    }
    if (p.sawn) {
        char line[400];
        UE_LOGI("container_break: slot %d's cut of '%ls' is granted -- the host's copy breaks when the saw's destroy of it "
                "arrives, and the grant ends with the cut (#%u)%s%s", slot, key.c_str(), g_granted_n + 1, g_granted_n < 5 ? ": " : "",
                g_granted_n < 5 ? SawReachLine(reach, line, sizeof(line)) : "");
        Answer(s, slot, key, p, coop::net::kContainerBreakGranted, 0, c);
        return;
    }
    if (!Break(c, p.fire != 0)) { Refuse(s, slot, key, p, coop::net::kContainerBreakNotTaken); return; }
    {
        ++g_broken;
        UE_LOGI("container_break: slot %d broke '%ls' -- the host's copy breaks and spills (%s, #%u)", slot, key.c_str(),
                p.fire ? "fire" : "impact", g_broken);
    }
}

void HostServe(coop::net::Session& s) {
    for (auto it = g_granted.begin(); it != g_granted.end();)
        it = it->second.prop.Alive() && s.IsSlotWorldReady(it->second.slot) ? std::next(it) : g_granted.erase(it);
    for (int slot = 1; slot < g_inbox.size(); ++slot) {
        SendOwed(s, slot);
        if (g_inbox[slot].empty()) continue;
        std::vector<Payload> queued;
        queued.swap(g_inbox[slot]);
        for (const Payload& p : queued) Judge(s, slot, p);
    }
}

// A message this peer does not act on, at the first twenty and each fiftieth after: no honest peer sends one. MTA
// disconnects a peer whose packet is impossible (reference/mtasa-blue/Server/mods/deathmatch/logic/CRPCFunctions.cpp:98-103);
// a client of ours is bounded by the budgets and the checks and the host may relay, so a malformed message is counted and
// dropped, never a disconnect.
void Drop(const char* why, int slot) {
    ++g_malformed;
    if (g_malformed <= 20 || g_malformed % 50 == 0)
        UE_LOGW("container_break: a message from slot %d dropped (%s); %u so far", slot, why, g_malformed);
}

// A request already waiting stands for the hit that repeats (a bouncing container takes several in a second).
enum class Put { Full, Fresh, Repeat };

Put Enqueue(const Payload& p) {
    for (const Payload& q : g_outbox)
        if (SameRequest(q, p)) return Put::Repeat;
    if (g_outbox.size() >= kOutboxCap) return Put::Full;
    g_outbox.push_back(p);
    if (p.op == coop::net::kContainerBreakAsk) ++g_asked;
    return Put::Fresh;
}

// HOST: a client handed back the grant of a cut it did not finish (op 4): the record ends, so another saw can be granted the
// prop and a later destroy of it is not broken first. Only the slot the record belongs to ends it.
void Withdrawn(int slot, const std::wstring& key) {
    const auto it = g_granted.find(key);
    if (it == g_granted.end() || it->second.slot != slot) {
        ++g_strayWithdrawals;
        UE_LOGW("container_break: slot %d withdrew the cut of '%ls', which the host holds no grant of for it (%u so far)", slot,
                key.c_str(), g_strayWithdrawals);
        return;
    }
    g_granted.erase(it);
    ++g_withdrawn;
    UE_LOGI("container_break: slot %d withdrew the grant of its cut of '%ls' -- the cut ended without its destroy (#%u)", slot,
            key.c_str(), g_withdrawn);
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

// An unkeyed container is not shared state (no peer holds a copy to agree with), so its local break stays. A keyed one
// never breaks on this copy: with the queue full the hit is dropped and logged, not run (a client-first break would put
// its spill on this client alone while its destroy took the host's copy). MTA's vehicle damage takes no client verdict
// either (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:2430-2431: applied only for the element's syncer).
bool RequestFromClient(void* owner, bool fire) {
    // The drill's red: the hit runs on this copy and asks the host nothing.
    static const bool s_breakOff = coop::config::ResolveFlag(coop::config_registry::rows::break_request_off);
    if (s_breakOff) return false;
    if (!owner || !R::IsLive(owner) || !ci::IsContainer(owner)) return false;
    const std::wstring key = KeyOf(owner);
    if (key.empty()) return false;
    Payload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.op = coop::net::kContainerBreakAsk;
    p.fire = fire ? 1 : 0;
    switch (Enqueue(p)) {
    case Put::Full:
        ++g_outboxFull;
        if (g_outboxFull <= 20 || g_outboxFull % 50 == 0)
            UE_LOGW("container_break: this client's %zu requests are waiting for the host -- the %s of '%ls' is dropped (%u)",
                    kOutboxCap, fire ? "fire" : "hit", key.c_str(), g_outboxFull);
        break;
    case Put::Fresh:
        UE_LOGI("container_break: this player broke '%ls' (%s) -- the host breaks it (#%u)", key.c_str(),
                fire ? "fire" : "impact", g_asked);
        break;
    case Put::Repeat: break;
    }
    return true;
}

bool RequestSaw(const std::wstring& propKey) {
    if (propKey.empty() || propKey.size() >= sizeof(coop::net::WireKey::data)) return false;
    Payload p{};
    coop::net::WireKeyFromString(propKey, p.key);
    p.op = coop::net::kContainerBreakAsk;
    p.sawn = 1;
    const Put put = Enqueue(p);
    if (put == Put::Full) return false;
    if (put == Put::Fresh)
        UE_LOGI("container_break: this player's saw asks the host to grant the cut of '%ls' (#%u)", propKey.c_str(), g_asked);
    return true;
}

bool WithdrawSaw(const std::wstring& propKey) {
    if (propKey.empty() || propKey.size() >= sizeof(coop::net::WireKey::data)) return false;
    Payload p{};
    coop::net::WireKeyFromString(propKey, p.key);
    p.op = coop::net::kContainerBreakWithdrawn;
    p.sawn = 1;
    return Enqueue(p) != Put::Full;
}

void OnClientDestroy(int senderSlot, const std::wstring& key) {
    if (g_granted.empty()) return;
    const auto it = g_granted.find(key);
    if (it == g_granted.end() || it->second.slot != senderSlot) return;
    void* prop = it->second.prop.Get();
    g_granted.erase(it);
    if (!prop || !R::IsLive(prop) || !BreakForCut(prop)) {
        UE_LOGW("container_break: slot %d's destroy of the granted '%ls' arrived and the host's copy did not break",
                senderSlot, key.c_str());
        return;
    }
    ++g_broken;
    UE_LOGI("container_break: slot %d's saw destroyed the granted '%ls' -- the host's copy breaks and spills first (#%u)",
            senderSlot, key.c_str(), g_broken);
}

uint32_t AskedCount() { return g_asked; }

uint32_t RefusedAnswerCount() { return g_refusedAnswers; }

void Tick() {
    coop::net::Session* s = Live();
    if (!s) return;
    if (s->role() == coop::net::Role::Host) {
        HostServe(*s);
        return;
    }
    if (g_outbox.empty() || !coop::net_pump::HasAnnouncedWorldReady()) return;
    for (auto it = g_outbox.begin(); it != g_outbox.end();) {
        if (!s->SendReliable(coop::net::ReliableKind::ContainerBreakRequest, &*it, sizeof(*it))) break;
        it = g_outbox.erase(it);
    }
}

void OnMessage(coop::net::Session& s, int senderSlot, const void* payload, size_t payloadLen) {
    if (senderSlot < 0 || senderSlot >= coop::players::kMaxPeers) { Drop("a slot outside the table", senderSlot); return; }
    if (!payload || payloadLen != sizeof(Payload)) { Drop("a payload that is not a break request's size", senderSlot); return; }
    Payload p{};
    std::memcpy(&p, payload, sizeof(p));
    if (p.key.len == 0 || p.key.len > sizeof(p.key.data)) { Drop("a key length the wire does not carry", senderSlot); return; }
    if (p.fire > 1 || p.sawn > 1 || (p.fire && p.sawn)) {
        Drop("a fire or sawn flag the wire does not carry", senderSlot);
        return;
    }
    if (s.role() == coop::net::Role::Host) {
        if (senderSlot == 0 || p.reason != 0 ||
            (p.op != coop::net::kContainerBreakAsk && p.op != coop::net::kContainerBreakWithdrawn)) {
            Drop("a client may only ask or withdraw a grant, with no reason", senderSlot);
            return;
        }
        if (p.op == coop::net::kContainerBreakWithdrawn) {
            if (p.sawn != 1 || p.fire != 0) { Drop("a withdrawal that is not a cut's", senderSlot); return; }
            // Not queued: it only ends a record, and a full queue must not hold it back.
            Withdrawn(senderSlot, coop::net::StringFromWireKey(p.key));
            return;
        }
        std::vector<Payload>& queue = g_inbox[senderSlot];
        for (const Payload& q : queue)
            if (SameRequest(q, p)) return;   // the request that waits answers this one too
        for (const Owed& o : g_owed[senderSlot])
            if (SameRequest(o.ask, p)) return;   // so does the answer that is owed
        if (queue.size() < kSlotQueue) { queue.push_back(p); return; }
        // A queue past its size: an impact is dropped without a word, a cut is answered (its saw waits for it).
        if (p.sawn) Refuse(s, senderSlot, coop::net::StringFromWireKey(p.key), p, coop::net::kContainerBreakTooFast);
        else ++g_droppedImpacts;
        return;
    }
    const bool refusal = p.op == coop::net::kContainerBreakRefused && p.reason >= coop::net::kContainerBreakNotFound &&
                         p.reason <= coop::net::kContainerBreakNotTaken;
    const bool grant = p.op == coop::net::kContainerBreakGranted && p.reason == 0 && p.sawn == 1 && p.fire == 0;
    if (senderSlot != 0 || !(refusal || grant)) {
        Drop("the host's word is a refusal with a known reason or the grant of a cut", senderSlot);
        return;
    }
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    if (grant) {
        ++g_grantedAnswers;
        coop::props::hacksaw_cut::OnGranted(key);
        return;
    }
    ++g_refusedAnswers;
    UE_LOGW("container_break: the host refused this client's %s of '%ls' (%s) -- nothing was broken there",
            p.sawn ? "cut" : p.fire ? "fire" : "hit", key.c_str(), ReasonName(p.reason));
    if (p.sawn) coop::props::hacksaw_cut::OnRefused(key);
}

void OnDisconnect() {
    if (g_asked || g_broken || g_granted_n || g_refused || g_malformed || g_refusedAnswers || g_grantedAnswers || g_withdrawn)
        UE_LOGI("container_break: session end -- asked=%u, host broke=%u granted=%u refused=%u (impacts dropped=%u, replies "
                "dropped=%u), withdrawn=%u (stray %u), answers seen: granted=%u refused=%u, queue full=%u, malformed=%u", g_asked,
                g_broken, g_granted_n,
                g_refused, g_droppedImpacts, g_repliesDropped, g_withdrawn, g_strayWithdrawals, g_grantedAnswers, g_refusedAnswers,
                g_outboxFull, g_malformed);
    g_asked = g_broken = g_granted_n = g_refused = g_malformed = g_refusedAnswers = 0;
    g_grantedAnswers = g_droppedImpacts = g_repliesDropped = g_outboxFull = 0;
    g_withdrawn = g_strayWithdrawals = 0;
    g_outbox.clear();
    g_granted.clear();
    for (int i = 0; i < g_inbox.size(); ++i) g_inbox[i].clear();
    for (int i = 0; i < g_owed.size(); ++i) g_owed[i].clear();
    for (int i = 0; i < g_budget.size(); ++i) g_budget[i].Reset();
    for (int i = 0; i < g_replyBudget.size(); ++i) g_replyBudget[i].Reset();
    g_session.store(nullptr);
}

}  // namespace coop::props::container_break
