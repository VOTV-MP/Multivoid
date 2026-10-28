// coop/interactables/coffee_intent.cpp -- see coop/interactables/coffee_intent.h.

#include "coop/interactables/coffee_intent.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/intent_authority.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/intent_queue.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"  // PerSlotState
#include "coop/props/prop_key_index.h"  // the device a key names

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/appliance.h"  // CallAction: a player's interaction dispatch, on any actor
#include "ue_wrap/devices/coffee.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

namespace coop::coffee_intent {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace CF = ue_wrap::coffee;

// The press, declared on each device's class: another class's actionOptionIndex is never asked.
UE_FACT_FN(kRowVending, "coffee_intent", L"prop_coffeevending_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowMaker, "coffee_intent", L"prop_coffeeMaker_C", L"actionOptionIndex", Declared);

struct Watch { const ue_wrap::facts::Function* row; int tag; };
const Watch kWatches[] = {
    { &kRowVending, 0x43465650 /*'CFVP'*/ },
    { &kRowMaker,   0x43464D50 /*'CFMP'*/ },
};
constexpr int kWatchCount = static_cast<int>(sizeof(kWatches) / sizeof(kWatches[0]));

// The reach a sender's press is judged against. A player's use reaches as far as the look-at trace, whose length
// is mainPlayer.armLength: 200 by default. Twice the default is the reach; the authority adds the device's own
// bounds radius and a 600 uu allowance for the puppet's pose lagging the sender (coop/element/intent_authority),
// so the bound refuses a device the sender cannot be near, never a reach the game itself allows.
constexpr float kReachUU = 400.0f;

// A sender's presses run from a bounded queue at a bounded rate: a press is a click and the machines hold their
// own gate for seconds, so the bound is only felt by a stalled or flooding client.
constexpr coop::net::IntentBudget kBudget{4.0f, 4.0f};
constexpr size_t   kMaxPending   = 8;
constexpr uint64_t kRefusalSayMs = 10000;

std::atomic<coop::net::Session*> g_session{nullptr};
// Each watch registers once: a refusal (a full gate table) is final, and said once. The set settles once every
// watch is live, or once no name is left to resolve and the rest are dead for good.
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg[kWatchCount] = {};
bool g_settled = false;

// The press's parameter, per watch: each device's body is its own function, so each keeps its own offset.
R::ParamOffset g_action[kWatchCount] = {R::ParamOffset{L"action"}, R::ParamOffset{L"action"}};
bool g_actionSaid = false;

// Why a press was refused or went nowhere, counted by reason and said at most once in kRefusalSayMs a reason.
enum Reason : uint8_t { kNoDevice, kWrongClass, kOutOfReach, kNotRun, kQueueFull, kSenderDead, kReasonCount };
const char* const kReasonName[kReasonCount] = {
    "no device holds the key", "the key names no coffee device", "the sender is not within reach",
    "the host's copy did not take the call", "the sender's queue is full",
    "the sender's player is not alive",
};
uint64_t g_refused[kReasonCount] = {};
struct Said { uint64_t nextMs = 0; uint64_t since = 0; };
Said g_said[kReasonCount];

uint64_t g_sent = 0, g_ran = 0, g_unkeyed = 0, g_claims = 0, g_dropSize = 0, g_dropKey = 0;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// One sender: its queued presses with their bucket, and the latch that clears with its occupant.
struct Slot {
    coop::net::IntentQueue<coop::net::CoffeeActionIntentPayload> queue;
    bool waitSaid = false;  // a wait for the body, said once a streak
};
coop::roster_ledger::PerSlotState<Slot> g_slots;

// ---- host side -------------------------------------------------------------------------------
void Refuse(Reason why, uint8_t slot, const std::wstring& key, const char* more) {
    ++g_refused[why];
    Said& sd = g_said[why];
    const uint64_t now = NowMs();
    if (now < sd.nextMs) { ++sd.since; return; }
    UE_LOGI("[COFFEE-INTENT] DENY slot=%u key='%ls' -- %s%s%s (%llu more since the last line)",
            static_cast<unsigned>(slot), key.c_str(), kReasonName[why], more ? ": " : "", more ? more : "",
            static_cast<unsigned long long>(sd.since));
    sd.nextMs = now + kRefusalSayMs;
    sd.since = 0;
}

// False keeps the press at its sender's queue head: the host has no body for the sender yet (its first pose has
// not arrived), so its reach cannot be measured, and the press waits for the body rather than being lost.
// Everything else is run or refused, and consumed.
bool Execute(coop::net::Session& s, const coop::net::CoffeeActionIntentPayload& p, uint8_t slot) {
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    void* device = key.empty() ? nullptr : coop::prop_key_index::Find(key);
    if (!device) {
        Refuse(kNoDevice, slot, key, nullptr);
        return true;
    }
    // A player who is not alive presses nothing; judged before reach and the rest, as the cord lane judges a claim.
    if (coop::players::Registry::Get().LifeOf(slot) != coop::players::PuppetLife::Alive) {
        Refuse(kSenderDead, slot, key, nullptr);
        return true;
    }
    const coop::element::IntentSubject subject =
        coop::element::IntentTarget::ForClientIntent(s, slot, kReachUU).Authorize(device);
    if (subject.outcome == coop::element::IntentOutcome::NoBody) {
        if (!g_slots[slot].waitSaid) {
            g_slots[slot].waitSaid = true;
            UE_LOGI("[COFFEE-INTENT] slot %u's presses wait: the host has no body for it yet",
                    static_cast<unsigned>(slot));
        }
        return false;
    }
    if (!subject) {
        Refuse(kOutOfReach, slot, key, coop::element::OutcomeName(subject.outcome));
        return true;
    }
    // Authorize answers reach only; the key is the sender's word, so the actor it names must be one of the two.
    const CF::Device kind = CF::DeviceOf(device);
    if (kind == CF::Device::None) {
        Refuse(kWrongClass, slot, key, nullptr);
        return true;
    }
    // The body Authorize just measured: the sender's puppet, passed as the player of the use.
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
    void* body = (rp && rp->valid()) ? rp->GetActor() : nullptr;
    if (!ue_wrap::appliance::CallAction(device, body, CF::kBrewAction)) {
        Refuse(kNotRun, slot, key, kind == CF::Device::Vending ? "vending machine" : "coffee maker");
        return true;
    }
    ++g_ran;
    UE_LOGI("[COFFEE-INTENT] host ran slot %u's press on the %s key='%ls'", static_cast<unsigned>(slot),
            kind == CF::Device::Vending ? "vending machine" : "coffee maker", key.c_str());
    return true;
}

// ---- the client's gate -----------------------------------------------------------------------
// The client of a running session; no load exemption, since no world load calls a device's action 4, and
// after the session the press runs natively (coop/interactables/garbage_sync, the same predicate).
sg::Verdict OnPressPre(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != coop::net::Role::Client || !call.object) return sg::Verdict::Run;
    // The coffee drill's red arm: the press runs its own body here, as before the lane. Resolved once.
    static const bool s_redNoIntent =
        coop::config::ResolveEnum(coop::config_registry::rows::coffee_red) == "no_intent";
    if (s_redNoIntent) return sg::Verdict::Run;
    int wi = -1;
    for (int i = 0; i < kWatchCount; ++i)
        if (kWatches[i].tag == call.tag) { wi = i; break; }
    if (wi < 0) return sg::Verdict::Run;
    const int32_t off = g_action[wi].Of(call.function);
    if (off < 0) {
        if (!g_actionSaid) {
            g_actionSaid = true;
            UE_LOGW("[COFFEE-INTENT] the press's action parameter did not resolve -- a coffee press runs here "
                    "alone (said once)");
        }
        return sg::Verdict::Run;
    }
    if (*(call.locals + off) != CF::kBrewAction) return sg::Verdict::Run;
    const std::wstring key = ue_wrap::prop::GetKeyString(call.object);
    if (key.empty() || key == L"None" || key.size() > sizeof(coop::net::WireKey::data)) {
        // Said for the first three and every twentieth, since to the player it is a dead button.
        if (++g_unkeyed <= 3 || g_unkeyed % 20 == 0)
            UE_LOGW("[COFFEE-INTENT] a press on coffee device %p, which holds no key the host could resolve -- "
                    "nothing sent (#%llu)", call.object, static_cast<unsigned long long>(g_unkeyed));
        return sg::Verdict::Cancel;
    }
    coop::net::CoffeeActionIntentPayload p{};
    coop::net::WireKeyFromString(key, p.key);
    if (!s->SendReliable(coop::net::ReliableKind::CoffeeActionIntent, &p, sizeof(p))) {
        UE_LOGW("[COFFEE-INTENT] the press on key='%ls' was not sent (the session refused it); the device stays "
                "as the host has it", key.c_str());
        return sg::Verdict::Cancel;
    }
    ++g_sent;
    UE_LOGI("[COFFEE-INTENT] CLIENT SENT a press on key='%ls' (#%llu)", key.c_str(),
            static_cast<unsigned long long>(g_sent));
    return sg::Verdict::Cancel;
}

void RegisterWatches() {
    for (int i = 0; i < kWatchCount; ++i) {
        if (g_reg[i] != Reg::Pending) continue;
        if (sg::WatchName(*kWatches[i].row, kWatches[i].tag, &OnPressPre, nullptr)) {
            g_reg[i] = Reg::Registered;
            continue;
        }
        g_reg[i] = Reg::Refused;
        UE_LOGE("[COFFEE-INTENT] the script-body gate refused the watch on '%ls' of '%ls' -- for the rest of "
                "this process that press runs only where it is made", kWatches[i].row->Name(),
                kWatches[i].row->Class());
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    RegisterWatches();
}

void Tick(coop::net::Session& session) {
    sg::ResolvePendingNames();
    if (!g_settled) {
        int live = 0, pending = 0;
        for (int i = 0; i < kWatchCount; ++i) {
            if (g_reg[i] == Reg::Pending) ++pending;
            else if (g_reg[i] == Reg::Registered && sg::NameWatchLive(*kWatches[i].row, kWatches[i].tag)) ++live;
        }
        if (live == kWatchCount) {
            g_settled = true;
            UE_LOGI("[COFFEE-INTENT] the coffee press gates are live: vending machine and coffee maker");
        } else if (pending == 0 && sg::PendingNameCount() == 0) {
            g_settled = true;
            UE_LOGE("[COFFEE-INTENT] %d of %d coffee press gates are dead (refused, or resolved into a full table) "
                    "-- those presses run only where they are made", kWatchCount - live, kWatchCount);
        }
    }
    if (!session.running() || session.role() != coop::net::Role::Host) return;
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        Slot& sl = g_slots[slot];
        sl.queue.Serve(kBudget, NowMs, [&](const coop::net::CoffeeActionIntentPayload& p) {
            if (!Execute(session, p, slot)) return coop::net::Served::Wait;  // a wait runs nothing: no token spent
            sl.waitSaid = false;  // a consumed press ends the wait's streak
            return coop::net::Served::Done;
        });
    }
}

void OnAction(coop::net::Session& session, const coop::net::CoffeeActionIntentPayload& payload,
              uint8_t senderSlot) {
    if (session.role() != coop::net::Role::Host) return;
    if (senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    ++g_claims;
    Slot& sl = g_slots[senderSlot];
    auto payloadCopy = payload;
    if (!sl.queue.Push(std::move(payloadCopy), kMaxPending))
        Refuse(kQueueFull, senderSlot, coop::net::StringFromWireKey(payload.key), nullptr);
}

void CountDropped(Drop why) {
    ++(why == Drop::Size ? g_dropSize : g_dropKey);
}

DrillView ReadForDrill() {
    DrillView v;
    v.claims = g_claims;
    v.ran = g_ran;
    v.noDevice = g_refused[kNoDevice];
    v.wrongClass = g_refused[kWrongClass];
    v.outOfReach = g_refused[kOutOfReach];
    v.notRun = g_refused[kNotRun];
    v.queueFull = g_refused[kQueueFull];
    v.senderDead = g_refused[kSenderDead];
    v.dropSize = g_dropSize;
    v.dropKey = g_dropKey;
    return v;
}

void OnDisconnect() {
    uint64_t refused = 0;
    for (const uint64_t n : g_refused) refused += n;
    if (g_sent || g_ran || g_unkeyed || refused || g_dropSize || g_dropKey)
        UE_LOGI("[COFFEE-INTENT] session end -- sent=%llu ran=%llu unkeyed=%llu denied=%llu (no device %llu, wrong "
                "class %llu, out of reach %llu, not run %llu, queue full %llu, sender dead %llu) dropped at the "
                "edge: size %llu, key %llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_ran),
                static_cast<unsigned long long>(g_unkeyed), static_cast<unsigned long long>(refused),
                static_cast<unsigned long long>(g_refused[kNoDevice]),
                static_cast<unsigned long long>(g_refused[kWrongClass]),
                static_cast<unsigned long long>(g_refused[kOutOfReach]),
                static_cast<unsigned long long>(g_refused[kNotRun]),
                static_cast<unsigned long long>(g_refused[kQueueFull]),
                static_cast<unsigned long long>(g_refused[kSenderDead]), static_cast<unsigned long long>(g_dropSize),
                static_cast<unsigned long long>(g_dropKey));
    g_sent = g_ran = g_unkeyed = g_claims = g_dropSize = g_dropKey = 0;
    for (uint64_t& n : g_refused) n = 0;
    for (Said& sd : g_said) sd = Said{};
    g_actionSaid = false;
}

}  // namespace coop::coffee_intent
