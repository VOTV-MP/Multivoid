// coop/interactables/atv_host_judge.cpp -- see coop/interactables/atv_host_judge.h.

#include "coop/interactables/atv_host_judge.h"

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

#include "ue_wrap/core/log.h"
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
#include <unordered_map>
#include <utility>

namespace coop::atv_host_judge {
namespace {

namespace A = ue_wrap::atv;
namespace H = coop::atv_host_sync;
namespace N = coop::net;
namespace op = coop::net::atv_host_op;
namespace why = coop::net::atv_host_reason;
using Payload = N::AtvHostPayload;
using Clock = std::chrono::steady_clock;

constexpr float kMax = 100.f;      // a full tank, full health
constexpr float kReachCm = 800.f;  // a sender's puppet from its ATV, when it is not seated in it
// A sender's item acts are served at {4, 4} a second (the chipper's pour and the cremator lever's bound), from a queue of
// 8; the horn, a stream of short edges, at {8, 8}.
constexpr N::IntentBudget kBudget{4.0f, 4.0f};
constexpr N::IntentBudget kHornBudget{8.0f, 8.0f};
constexpr size_t kMaxPending = 8;
constexpr int kMaxModules = 16;
constexpr const wchar_t* kCanClass = L"prop_gascan_C";
constexpr const wchar_t* kToolboxClass = L"prop_toolbox_C";
constexpr const wchar_t* kUpgradeClass = L"prop_atvUpgrade_C";

struct Slot {
    N::IntentQueue<Payload> queue;
    N::IntentBucket horn;
    bool waitSaid = false;   // a park, said once a streak
};
coop::roster_ledger::PerSlotState<Slot> g_slots;

// The ATVs whose horn a client's press holds down, with the slot that pressed it.
struct HornHeld { void* atv; uint8_t slot; };
std::unordered_map<std::wstring, HornHeld> g_hornDown;
N::RefusalTally g_refusals{"atv_host"};

struct Counts {
    uint32_t accepted[6] = {};   // by request op
    uint32_t refused[10] = {};   // by atv_host_reason
    uint32_t parked = 0, hornsDown = 0, hornsRefused = 0, noKey = 0, refunded = 0;
};
Counts g_n;

const char* ReasonName(uint8_t r) {
    static const char* const kNames[10] = {"ok",   "not_ready", "gone",      "reach",  "no_item",
                                           "rate", "duplicate", "not_held",  "not_syncer", "malformed"};
    return r < 10 ? kNames[r] : "unknown";
}

// ---- the answer ----

// The verdict to the request's sender alone, with the host copy's state as it is now (none for an ATV the host does not
// hold). MTA answers every failed vehicle request to its sender (CGame.cpp:3042, :3098, :3358).
void Answer(N::Session& s, uint8_t slot, const Payload& req, uint8_t reason, void* atv) {
    Payload a{};
    const std::wstring key = N::StringFromWireKey(req.key);
    H::Vitals v;
    if (atv && reason != why::kGone && H::ReadVitals(atv, v)) H::FillState(key, v, op::kAnswer, a);
    a.key = req.key;
    a.op = op::kAnswer;
    a.reqId = req.reqId;
    a.module = reason;
    s.SendReliableToSlot(slot, N::ReliableKind::AtvHostState, &a, sizeof(a));
    if (reason != why::kOk) {
        ++g_n.refused[reason < 10 ? reason : 9];
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
    void* cls = ue_wrap::phys_mods::ClassForByte(module);
    ue_wrap::FVector at{};
    if (!cls || !ue_wrap::engine::TryGetActorLocation(atv, at)) {
        UE_LOGW("atv_host: no prop class for module %u -- the refused fit is not refunded", static_cast<unsigned>(module));
        return;
    }
    ue_wrap::engine::SpawnActor(cls, {at.X, at.Y, at.Z + 120.f});
    ++g_n.refunded;
}

bool WellFormed(const Payload& p) {
    switch (p.op) {
    case op::kRefuel: return p.reqId != 0 && std::isfinite(p.amount) && p.amount > 0.f && p.amount <= kMax;
    case op::kRepair:
    case op::kFit:
    case op::kTakeoff: return p.reqId != 0;
    default: return false;
    }
}

// An applied request: the host copy is as the verb's own body would leave it, the change published (the sender's gate
// holds its copy of it until the answer), the sender answered with the result.
void Applied(N::Session& s, uint8_t slot, const Payload& p, const std::wstring& key, void* atv,
             const H::Vitals& before, const char* what) {
    H::PublishIfChanged(key, atv, before);
    ++g_n.accepted[p.op < 6 ? p.op : 0];
    UE_LOGI("atv_host: slot %u %s '%ls' on the host", static_cast<unsigned>(slot), what, key.c_str());
    Answer(s, slot, p, why::kOk, atv);
}

// Fuelling: fuelUp's own body is fuel += the gain, and the empty mark cleared when the tank rose.
uint8_t Refuel(N::Session& s, uint8_t slot, const Payload& p, const std::wstring& key, void* atv, const H::Vitals& v) {
    if (!coop::hand_item::HeldClassIs(slot, kCanClass)) return why::kNoItem;
    // The client's copy ran the pour as the game wrote it, so the amount is the fuel its own can gave; the host's copy
    // of that can is the hand mirror, which carries no amount, so the bound is the tank's room, as the chipper's.
    const float room = kMax - v.fuel;
    if (!(room > 0.f)) return why::kDuplicate;
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
    // The toolbox spends itself in the verb, so the hand may be empty again by now: the last class held counts.
    if (!coop::hand_item::LastHeldClassIs(slot, kToolboxClass)) return why::kNoItem;
    if (!(std::floor(v.health) < kMax)) return why::kDuplicate;
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
    if (!coop::hand_item::LastHeldClassIs(slot, kUpgradeClass)) return why::kNoItem;
    if (!ue_wrap::phys_mods::ClassForByte(p.module)) return why::kMalformed;
    uint8_t mods[kMaxModules + 1] = {};
    std::copy(v.modules, v.modules + v.count, mods);
    if (std::find(mods, mods + v.count, p.module) != mods + v.count || v.count >= kMaxModules) {
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

// One head of a sender's queue: Wait leaves it where it is, with no token spent, until the host's own state lets it be
// judged; Done answers it, whatever the verdict.
N::Served Execute(N::Session& s, const Payload& p, uint8_t slot) {
    const std::wstring key = N::StringFromWireKey(p.key);
    Slot& sl = g_slots[slot];
    const bool handJudged = p.op == op::kRefuel || p.op == op::kRepair || p.op == op::kFit;
    if (!s.IsSlotWorldReady(slot) || (handJudged && coop::hand_item::HandStateOwed(slot))) {
        if (!sl.waitSaid) {
            sl.waitSaid = true;
            ++g_n.parked;
            UE_LOGI("atv_host: slot %u's op %u on '%ls' waits: %s", static_cast<unsigned>(slot),
                    static_cast<unsigned>(p.op), key.c_str(),
                    s.IsSlotWorldReady(slot) ? "its hand state is still owed" : "its world is not ready on the host");
        }
        return N::Served::Wait;
    }
    sl.waitSaid = false;
    coop::atv_sync::TableView t;
    void* atv = coop::atv_sync::ReadTable(key, t) ? t.actor : nullptr;
    uint8_t verdict = why::kOk;
    H::Vitals v;
    if (!atv) verdict = why::kGone;
    else if (!WellFormed(p)) verdict = why::kMalformed;
    else if (coop::players::Registry::Get().LifeOf(slot) != coop::players::PuppetLife::Alive) verdict = why::kNotReady;
    else if (t.occupantSlot != slot && !InReach(slot, atv)) verdict = why::kReach;
    else if (!H::ReadVitals(atv, v)) verdict = why::kNotReady;
    if (verdict == why::kOk) {
        switch (p.op) {
        case op::kRefuel: verdict = Refuel(s, slot, p, key, atv, v); break;
        case op::kRepair: verdict = Repair(s, slot, p, key, atv, v); break;
        case op::kFit: verdict = Fit(s, slot, p, key, atv, v); break;
        default: verdict = Takeoff(s, slot, p, key, atv, v); break;
        }
        if (verdict == why::kOk) return N::Served::Done;   // Applied() answered
    }
    Answer(s, slot, p, verdict, atv);
    return N::Served::Done;
}

// ---- the horn ----

// The host copy sounds the edge and the others are told.
void Sound(const std::wstring& key, void* atv, bool down, int skipSlot) {
    {
        H::ApplyScope applying;
        A::ReplayHorn(atv, down);
    }
    if (down) H::NoteHornPlayed();
    H::SendHorn(key, down, skipSlot);
}

void OnHorn(N::Session& s, uint8_t slot, const std::wstring& key, const Payload& p) {
    const Clock::time_point now = Clock::now();
    coop::atv_sync::TableView t;
    if (!s.IsSlotWorldReady(slot) || !coop::atv_sync::ReadTable(key, t)) {
        ++g_n.hornsRefused;
        g_refusals.Note(slot, "a horn of an ATV the host does not hold, or from a world not ready", now);
        return;
    }
    const bool down = (p.flags & N::kAtvHostHornDown) != 0;
    const auto held = g_hornDown.find(key);
    if (down) {
        if (t.syncerSlot != slot) {
            ++g_n.hornsRefused;
            g_refusals.Note(slot, "a horn from a slot that does not sync the ATV", now);
            return;
        }
        if (held != g_hornDown.end()) return;   // already down
        if (!g_slots[slot].horn.Take(kHornBudget, N::NowMs())) {
            ++g_n.hornsRefused;
            g_refusals.Note(slot, "a horn past its budget", now);
            return;
        }
        g_hornDown[key] = HornHeld{t.actor, slot};
        ++g_n.hornsDown;
    } else {
        // A release is the press's own end: no token, and not refused because the syncer changed since.
        if (held == g_hornDown.end() || held->second.slot != slot) return;
        g_hornDown.erase(held);
    }
    Sound(key, t.actor, down, slot);
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
    if (p.op == op::kHorn) {
        OnHorn(*s, slot, key, p);
        return;
    }
    Payload copy = p;
    if (g_slots[slot].queue.Push(std::move(copy), kMaxPending)) return;
    // Past the queue the sender is answered at once, its gate's request closing. A sender whose world is not ready on the
    // host would be answered into the pre-world gate, which drops it; its own leave clears its gate.
    if (!s->IsSlotWorldReady(slot)) return;
    coop::atv_sync::TableView t;
    Answer(*s, slot, p, why::kRate, coop::atv_sync::ReadTable(key, t) ? t.actor : nullptr);
}

void Tick(N::Session& s) {
    for (uint8_t slot = 1; slot < N::kMaxPeers; ++slot) {
        Slot& sl = g_slots[slot];
        if (sl.queue.empty()) continue;
        sl.queue.Serve(kBudget, N::NowMs, [&](const Payload& p) { return Execute(s, p, slot); });
    }
    g_refusals.Flush(Clock::now());
}

void ReleaseHorn(void* atv) {
    N::Session* s = H::LiveSession();
    if (!s || s->role() != N::Role::Host || g_hornDown.empty() || !atv) return;
    // Found by the actor, not read from it: a table clear can come with the actor already gone.
    for (auto it = g_hornDown.begin(); it != g_hornDown.end(); ++it) {
        if (it->second.atv != atv) continue;
        const std::wstring key = it->first;
        g_hornDown.erase(it);
        UE_LOGI("atv_host: the horn of '%ls' is released: its press is over", key.c_str());
        if (ue_wrap::reflection::IsLive(atv)) Sound(key, atv, false, -1);
        else H::SendHorn(key, false, -1);
        return;
    }
}

void OnWorldChange() { g_hornDown.clear(); }

void OnDisconnect() {
    uint32_t accepted = 0, refused = 0;
    for (uint32_t n : g_n.accepted) accepted += n;
    for (uint32_t n : g_n.refused) refused += n;
    if (accepted || refused || g_n.parked || g_n.hornsDown || g_n.hornsRefused || g_n.noKey)
        UE_LOGI("atv_host: session end -- host requests accepted=%u (refuel %u repair %u fit %u takeoff %u) refused=%u "
                "parked=%u refunded=%u; horns down=%u refused=%u; no key=%u", accepted, g_n.accepted[op::kRefuel],
                g_n.accepted[op::kRepair], g_n.accepted[op::kFit], g_n.accepted[op::kTakeoff], refused, g_n.parked,
                g_n.refunded, g_n.hornsDown, g_n.hornsRefused, g_n.noKey);
    g_n = Counts{};
    g_hornDown.clear();
    g_refusals.Clear();
}

}  // namespace coop::atv_host_judge
