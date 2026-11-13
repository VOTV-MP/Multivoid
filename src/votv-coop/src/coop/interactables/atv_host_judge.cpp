// coop/interactables/atv_host_judge.cpp -- see coop/interactables/atv_host_judge.h.

#include "coop/interactables/atv_host_judge.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/interactables/atv_drill_events.h"
#include "coop/interactables/atv_host_sync.h"
#include "coop/interactables/atv_sync.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/intent_queue.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/refusal_tally.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"  // PerSlotState

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/desk/phys_mods.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

namespace coop::atv_host_judge {
namespace {

namespace A = ue_wrap::atv;
namespace H = coop::atv_host_sync;
namespace N = coop::net;
namespace op = coop::net::atv_host_op;
namespace DE = coop::atv_drill_events;
namespace why = coop::net::atv_host_reason;
using Payload = N::AtvHostPayload;
using Clock = std::chrono::steady_clock;

// The ATV drill's red arms: each is read here only, by the rule it turns off (config row atv_gate_drill).
bool Arm(const char* value) { return coop::config::ResolveEnum(::coop::config_registry::rows::atv_gate_drill) == value; }
bool JudgeOff() { static const bool on = Arm("atv_judge_off"); return on; }       // every request is judged ok
bool DupOff() { static const bool on = Arm("atv_dup_module"); return on; }        // a duplicate fit is allowed
bool RefundOff() { static const bool on = Arm("atv_refund_off"); return on; }     // a refused fit is not refunded
bool ParkOff() { static const bool on = Arm("atv_park_off"); return on; }         // a request is judged before its world is ready

constexpr float kMax = 100.f;      // a full tank, full health
constexpr float kReachCm = 800.f;  // a sender's puppet from its ATV, when it is not seated in it
// A sender's item acts are served at {4, 4} a second (the chipper's pour and the cremator lever's bound), from a queue of 8.
constexpr N::IntentBudget kBudget{4.0f, 4.0f};
constexpr size_t kMaxPending = 8;
constexpr int kMaxModules = 16;
constexpr const wchar_t* kCanClass = L"prop_gascan_C";
constexpr const wchar_t* kToolboxClass = L"prop_toolbox_C";
constexpr const wchar_t* kUpgradeClass = L"prop_atvUpgrade_C";
constexpr uint32_t kToolboxUses = 10;   // prop_toolbox_C's `fixes` default; its hold record carries none, so the class's bound
constexpr uint32_t kModuleUses = 1;     // a module is consumed by its fit

struct Slot {
    N::IntentQueue<Payload> queue;
    // The head waits for an event: the slot's world-ready edge (OnSlotReady), its hand state applied (OnHandApplied), or a
    // state of the host's own (OnStatePublished, the drill's fixture). Tick serves no parked slot; an event ends the park.
    bool parked = false;
    bool waitSaid = false;   // a park, said once a streak
    bool parkedForWorld = false;   // [dev] the park was for the slot's world, which the drill's event says once
    // The hand generation (hand_item::HoldGeneration) of the last item a request spent, and the uses of it counted (SpendHold).
    uint32_t spentGen = 0, spentUses = 0;
};
coop::roster_ledger::PerSlotState<Slot> g_slots;

N::RefusalTally g_refusals{"atv_host"};
std::wstring g_holdKey;   // [dev] see Execute
uint32_t g_holdSeq = 0;

constexpr uint8_t kReasons = 11;   // atv_host_reason::kOk .. kFull
struct Counts {
    uint32_t accepted[7] = {};   // by request op
    uint32_t refused[kReasons] = {};   // by atv_host_reason
    uint32_t parked = 0, noKey = 0, refunded = 0;
};
Counts g_n;

const char* ReasonName(uint8_t r) {
    static const char* const kNames[kReasons] = {"ok",   "not_ready", "gone",       "reach",      "no_item",   "rate",
                                                 "duplicate", "not_held", "not_syncer", "malformed", "full"};
    return r < kReasons ? kNames[r] : "unknown";
}

// ---- the answer ----

// The verdict to the request's sender alone, with the host copy's state as it is now (none for an ATV the host does not
// hold: the answer then says so in its flags). MTA answers every failed vehicle request to its sender (CGame.cpp:3042,
// :3098, :3358).
void Answer(N::Session& s, uint8_t slot, const Payload& req, uint8_t reason, void* atv) {
    Payload a{};
    const std::wstring key = N::StringFromWireKey(req.key);
    H::Vitals v;
    const bool carries = atv && reason != why::kGone && H::ReadVitals(atv, v);
    if (carries) H::FillState(key, v, op::kAnswer, a);
    else a.flags = N::kAtvHostNoState;
    a.key = req.key;
    a.op = op::kAnswer;
    a.reqId = req.reqId;
    a.module = reason;
    s.SendReliableToSlot(slot, N::ReliableKind::AtvHostState, &a, sizeof(a));
    if (reason != why::kOk) {
        ++g_n.refused[reason < kReasons ? reason : kReasons - 1];
        UE_LOGI("atv_host: slot %u's op %u on '%ls' refused -- %s", static_cast<unsigned>(slot),
                static_cast<unsigned>(req.op), key.c_str(), ReasonName(reason));
    }
}

// ---- the judge ----

bool InReach(uint8_t slot, void* atv) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
    void* body = rp && rp->valid() ? rp->GetActor() : nullptr;
    ue_wrap::FVector at{}, by{};
    if (!body || !ue_wrap::engine::TryGetActorLocation(atv, at) || !ue_wrap::engine::TryGetActorLocation(body, by))
        return false;
    const float dx = at.X - by.X, dy = at.Y - by.Y, dz = at.Z - by.Z;
    return dx * dx + dy * dy + dz * dz <= kReachCm * kReachCm;
}

// A fit the host would not make is given back: the module's prop at the ATV, as the take-off hands one over.
void Refund(void* atv, uint8_t module) {
    if (RefundOff()) {
        DE::Note(DE::kRefund, 0);
        return;
    }
    void* cls = ue_wrap::phys_mods::ClassForByte(module);
    ue_wrap::FVector at{};
    if (!cls || !ue_wrap::engine::TryGetActorLocation(atv, at)) {
        UE_LOGW("atv_host: no prop class for module %u -- the refused fit is not refunded", static_cast<unsigned>(module));
        DE::Note(DE::kRefund, 0);
        return;
    }
    ue_wrap::engine::SpawnActor(cls, {at.X, at.Y, at.Z + 120.f});
    ++g_n.refunded;
    DE::Note(DE::kRefund, 1);
}

// A verb that consumes the held item (a toolbox use, a module): the sender's last held class must be `cls`, and the hold,
// as hand_item numbers it, must have a use left. The number moves on each hold the client's own hand lane announces, so it
// is a rate bound on that announce (kHandBudget), not the identity of an item: a client that empties its hand and
// announces a hold again starts a new one. The item is spent on its copy by then, so
// a use is counted whatever the verdict after it. A hold announced after the last spend starts over; one that never was
// (a forged request with an old item) finds none left. A gas can is not spent: its fuel comes out of the tank's room.
bool SpendHold(uint8_t slot, const wchar_t* cls, uint32_t uses) {
    if (!coop::hand_item::LastHeldClassIs(slot, cls)) return false;
    Slot& sl = g_slots[slot];
    const uint32_t gen = coop::hand_item::HoldGeneration(slot);
    if (gen != sl.spentGen) {
        sl.spentGen = gen;
        sl.spentUses = 0;
    }
    if (sl.spentUses >= uses) return false;
    ++sl.spentUses;
    return true;
}

bool WellFormed(const Payload& p) {
    switch (p.op) {
    case op::kRefuel: return p.reqId != 0 && std::isfinite(p.amount) && p.amount > 0.f && p.amount <= kMax;
    case op::kRepair:
    case op::kFit:
    case op::kTakeoff:
    case op::kSync: return p.reqId != 0;
    default: return false;
    }
}

// An applied request: the host copy is as the verb's own body would leave it, the change published (the sender's gate
// holds its copy of it until the answer), the sender answered with the result.
void Applied(N::Session& s, uint8_t slot, const Payload& p, const std::wstring& key, void* atv,
             const H::Vitals& before, const char* what) {
    H::PublishIfChanged(key, atv, before);
    if (DE::On()) {
        H::Vitals after;
        if (H::ReadVitals(atv, after))
            DE::Note(DE::kAccepted, p.op, slot, static_cast<int32_t>(H::CurrentStateSeq(key)), after.fuel, after.health, after.count);
    }
    ++g_n.accepted[p.op < 7 ? p.op : 0];
    UE_LOGI("atv_host: slot %u %s '%ls' on the host", static_cast<unsigned>(slot), what, key.c_str());
    Answer(s, slot, p, why::kOk, atv);
}

// Fuelling: fuelUp's own body is fuel += the gain, and the empty mark cleared when the tank rose.
uint8_t Refuel(N::Session& s, uint8_t slot, const Payload& p, const std::wstring& key, void* atv, const H::Vitals& v) {
    if (!JudgeOff() && !coop::hand_item::HeldClassIs(slot, kCanClass)) return why::kNoItem;
    // The client's copy ran the pour as the game wrote it, so the amount is the fuel its own can gave; the host's copy
    // of that can is the hand mirror, which carries no amount, so the bound is the tank's room, as the chipper's.
    const float room = kMax - v.fuel;
    if (!(room > 0.f)) return why::kFull;
    {
        H::ApplyScope applying;
        A::SetFuel(atv, v.fuel + std::fmin(p.amount, room));
        A::SetEmpty(atv, false);
    }
    Applied(s, slot, p, key, atv, v, "refuelled");
    return why::kOk;
}

// Repairing: toolboxFix's own body is health := 100 and the broken mark cleared, then the game's updHealth.
uint8_t Repair(N::Session& s, uint8_t slot, const Payload& p, const std::wstring& key, void* atv, const H::Vitals& v) {
    // The toolbox spends itself in the verb, so the hand may be empty again by now: the last class held counts, once.
    if (!JudgeOff() && !SpendHold(slot, kToolboxClass, kToolboxUses)) return why::kNoItem;
    if (!(std::floor(v.health) < kMax)) return why::kFull;
    {
        H::ApplyScope applying;
        A::SetHealth(atv, kMax);
        A::SetBroken(atv, false);
        A::CallUpdHealth(atv);
    }
    Applied(s, slot, p, key, atv, v, "repaired");
    return why::kOk;
}

uint8_t Fit(N::Session& s, uint8_t slot, const Payload& p, const std::wstring& key, void* atv, const H::Vitals& v) {
    // The module's byte is bound to the item the sender held: the class the game gives that byte (physModToActor) is the
    // class its hand last announced, and an ATV upgrade's. A module's value is read from no hand state (the announce
    // carries a class, a name and an item), so the class is the binding; a byte of another class is no fit of this item.
    void* const cls = ue_wrap::phys_mods::ClassForByte(p.module);
    void* const base = ue_wrap::object_index::ClassByName(kUpgradeClass);
    if (!cls || !base || !ue_wrap::prop::WalksToBase(cls, base)) return why::kMalformed;
    if (!JudgeOff() && !SpendHold(slot, ue_wrap::reflection::ToString(ue_wrap::reflection::NameOf(cls)).c_str(), kModuleUses))
        return why::kNoItem;
    uint8_t mods[kMaxModules + 1] = {};
    std::copy(v.modules, v.modules + v.count, mods);
    if ((!DupOff() && std::find(mods, mods + v.count, p.module) != mods + v.count) || v.count >= kMaxModules) {
        // The hand judge passed: the module was this sender's to fit, and it is spent on its copy, so it comes back.
        Refund(atv, p.module);
        return why::kDuplicate;
    }
    mods[v.count] = p.module;
    {
        H::ApplyScope applying;
        A::WriteModules(atv, mods, v.count + 1);
    }
    Applied(s, slot, p, key, atv, v, "fitted a module on");
    return why::kOk;
}

uint8_t Takeoff(N::Session& s, uint8_t slot, const Payload& p, const std::wstring& key, void* atv,
                const H::Vitals& v) {
    uint8_t mods[kMaxModules] = {};
    int n = 0;
    bool held = false;
    for (int i = 0; i < v.count; ++i) {
        if (!held && v.modules[i] == p.module) {
            held = true;
            continue;
        }
        mods[n++] = v.modules[i];
    }
    if (!held) return why::kNotHeld;
    {
        H::ApplyScope applying;
        A::WriteModules(atv, mods, n);
    }
    Applied(s, slot, p, key, atv, v, "took a module off");
    return why::kOk;
}

// One head of a sender's queue: Wait leaves it where it is, with no token spent, parked until an event of the host's own
// lets it be judged (the slot's world-ready edge, its hand state applied); Done answers it, whatever the verdict.
N::Served Execute(N::Session& s, const Payload& p, uint8_t slot) {
    const std::wstring key = N::StringFromWireKey(p.key);
    Slot& sl = g_slots[slot];
    // [dev] The drill's fixture: a request for this ATV waits until the host has published a state of its own since it was
    // set (OnStatePublished ends the park).
    if (!g_holdKey.empty() && key == g_holdKey) {
        if (H::CurrentStateSeq(key) == g_holdSeq) {
            sl.parked = true;
            return N::Served::Wait;
        }
        g_holdKey.clear();
    }
    const bool handJudged = p.op == op::kRefuel || p.op == op::kRepair || p.op == op::kFit;
    const bool ready = s.IsSlotWorldReady(slot);
    if ((!ready && !ParkOff()) || (handJudged && coop::hand_item::HandStateOwed(slot))) {
        sl.parked = true;
        if (!sl.waitSaid) {
            sl.waitSaid = true;
            ++g_n.parked;
            if (!ready) {
                sl.parkedForWorld = true;
                DE::Note(DE::kPark, slot, 0);
            }
            UE_LOGI("atv_host: slot %u's op %u on '%ls' waits: %s", static_cast<unsigned>(slot),
                    static_cast<unsigned>(p.op), key.c_str(),
                    ready ? "its hand state is still owed" : "its world is not ready on the host");
        }
        return N::Served::Wait;
    }
    sl.waitSaid = false;
    if (sl.parkedForWorld || !ready) DE::Note(DE::kPark, slot, ready ? 1 : 2);
    sl.parkedForWorld = false;
    coop::atv_sync::TableView t;
    void* atv = coop::atv_sync::ReadTable(key, t) ? t.actor : nullptr;
    uint8_t verdict = why::kOk;
    H::Vitals v;
    // A sync asks for the host's state, nothing of this sender's body or reach: it is answered, and nothing is applied.
    const bool sync = p.op == op::kSync;
    if (!atv) verdict = why::kGone;
    else if (!WellFormed(p)) verdict = why::kMalformed;
    else if (!sync && coop::players::Registry::Get().LifeOf(slot) != coop::players::PuppetLife::Alive) verdict = why::kNotReady;
    else if (!sync && t.occupantSlot != slot && !JudgeOff() && !InReach(slot, atv)) verdict = why::kReach;
    else if (!H::ReadVitals(atv, v)) verdict = why::kNotReady;
    if (verdict == why::kOk) {
        switch (p.op) {
        case op::kRefuel: verdict = Refuel(s, slot, p, key, atv, v); break;
        case op::kRepair: verdict = Repair(s, slot, p, key, atv, v); break;
        case op::kFit: verdict = Fit(s, slot, p, key, atv, v); break;
        case op::kSync:
            ++g_n.accepted[op::kSync];
            Answer(s, slot, p, why::kOk, atv);
            return N::Served::Done;
        default: verdict = Takeoff(s, slot, p, key, atv, v); break;
        }
        if (verdict == why::kOk) return N::Served::Done;   // Applied() answered
    }
    Answer(s, slot, p, verdict, atv);
    return N::Served::Done;
}

// The hand lane applied a hold state for `slot`: a request parked for the hand state it was owed is judged again.
void OnHandApplied(uint8_t slot) {
    if (slot >= 1 && slot < N::kMaxPeers) g_slots[slot].parked = false;
}

}  // namespace

void OnRequest(int senderSlot, const Payload& p) {
    N::Session* s = H::LiveSession();
    if (!s || s->role() != N::Role::Host || senderSlot < 1 || senderSlot >= static_cast<int>(N::kMaxPeers)) return;
    const uint8_t slot = static_cast<uint8_t>(senderSlot);
    const std::wstring key = N::StringFromWireKey(p.key);
    if (key.empty()) {
        ++g_n.noKey;
        g_refusals.Note(slot, "a request with no readable key", Clock::now());
        return;
    }
    Payload copy = p;
    if (g_slots[slot].queue.Push(std::move(copy), kMaxPending)) return;
    // Past the queue the sender is answered at once, its gate's request closing: only a sender that ignores the client's own
    // cap of 8 open requests gets here (atv_host_sync::Ask). One whose world is not ready on the host would be answered into
    // the pre-world gate, which drops it; its own leave clears its gate. An ATV the host does not hold is `gone`.
    if (!s->IsSlotWorldReady(slot)) return;
    coop::atv_sync::TableView t;
    void* const atv = coop::atv_sync::ReadTable(key, t) ? t.actor : nullptr;
    Answer(*s, slot, p, atv ? why::kRate : why::kGone, atv);
}

void Tick(N::Session& s) {
    for (uint8_t slot = 1; slot < N::kMaxPeers; ++slot) {
        Slot& sl = g_slots[slot];
        if (sl.parked || sl.queue.empty()) continue;   // a parked head waits for its event, not for a look each frame
        sl.queue.Serve(kBudget, N::NowMs, [&](const Payload& p) { return Execute(s, p, slot); });
    }
    g_refusals.Flush(Clock::now());
}

void Install() {
    static bool subscribed = false;
    if (subscribed) return;
    subscribed = true;
    if (!coop::hand_item::SubscribeHandApplied(&OnHandApplied))
        UE_LOGE("atv_host: the hand lane's subscriber table is full -- a request parked for its hand state is not released");
}

void OnSlotReady(int slot) {
    if (slot < 1 || slot >= static_cast<int>(N::kMaxPeers)) return;
    g_slots[slot].parked = false;
}

void OnStatePublished(const std::wstring& key) {
    if (g_holdKey.empty() || key != g_holdKey) return;
    for (uint8_t slot = 1; slot < N::kMaxPeers; ++slot) g_slots[slot].parked = false;
}

void HoldRequestsForDrill(const std::wstring& key) {
    g_holdKey = key;
    g_holdSeq = H::CurrentStateSeq(key);
}

void OnDisconnect() {
    uint32_t accepted = 0, refused = 0;
    for (uint32_t n : g_n.accepted) accepted += n;
    for (uint32_t n : g_n.refused) refused += n;
    if (accepted || refused || g_n.parked || g_n.noKey)
        UE_LOGI("atv_host: session end -- host requests accepted=%u (refuel %u repair %u fit %u takeoff %u) refused=%u "
                "parked=%u refunded=%u; no key=%u", accepted, g_n.accepted[op::kRefuel],
                g_n.accepted[op::kRepair], g_n.accepted[op::kFit], g_n.accepted[op::kTakeoff], refused, g_n.parked,
                g_n.refunded, g_n.noKey);
    g_n = Counts{};
    g_refusals.Clear();
    g_holdKey.clear();
}

}  // namespace coop::atv_host_judge
