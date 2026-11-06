// coop/interactables/cremator_intent.cpp -- see coop/interactables/cremator_intent.h.

#include "coop/interactables/cremator_intent.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/intent_authority.h"
#include "coop/interactables/interactable_sync.h"  // the cremator lane's key, and its resolve
#include "coop/net/intent_bucket.h"
#include "coop/net/intent_queue.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/roster_ledger.h"  // PerSlotState
#include "coop/session/net_pump.h"      // IsInAnnouncedWorld: a client's own world load runs natively

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/cremator.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

namespace coop::cremator_intent {
namespace {

namespace C  = ue_wrap::cremator;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

// The press, declared on the cremator's class: another class's actionOptionIndex is never asked.
UE_FACT_FN(kRowPress, "cremator_intent", L"cremator_C", L"actionOptionIndex", Declared);
constexpr int kTagPress = 0x43524950;  // 'CRIP'

// The reach a sender's press is judged against. A player's use reaches as far as the look-at trace, whose length is
// mainPlayer.armLength: 200 by default. Twice the default is the reach; the authority adds the machine's own bounds
// radius and a 600 uu allowance for the puppet's pose lagging the sender (coop/element/intent_authority). The
// handle sits on the cremator's front within the actor's bounds, as a door's does.
constexpr float kReachUU = 400.0f;

// A sender's presses run from a bounded queue at a bounded rate: a press is a click and the handle takes seconds, so
// the bound is only felt by a stalled or flooding client.
constexpr coop::net::IntentBudget kBudget{4.0f, 4.0f};
constexpr size_t   kMaxPending   = 8;
constexpr uint64_t kRefusalSayMs = 10000;

std::atomic<coop::net::Session*> g_session{nullptr};
// The watch registers once: a refusal (a full gate table) is final, and said once.
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg = Reg::Pending;
bool g_settled = false;

R::ParamOffset g_action{L"action"};
bool g_actionSaid = false;
bool g_activeUnreadSaid = false;

// Why a press was refused or went nowhere, counted by reason and said at most once in kRefusalSayMs a reason.
enum Reason : uint8_t {
    kNoDevice, kWrongClass, kNotWorldReady, kNotAuthorized, kHandleBusy, kAlready, kDoorOpen, kNotRun, kQueueFull,
    kReasonCount
};
const char* const kReasonName[kReasonCount] = {
    "no cremator holds the key", "the key names no cremator", "the sender's world is not ready",
    "the authorisation refused the sender (no row, dead, wrong type, no target, out of reach)",
    "the host's handle is mid-travel or its animation unread",
    "the machine is already in the state the press wanted", "the host's door is open",
    "the host's copy did not take the call",
    "the sender's queue is full",
};
uint64_t g_refused[kReasonCount] = {};
struct Said { uint64_t nextMs = 0; uint64_t since = 0; };
Said g_said[kReasonCount];

uint64_t g_sent = 0, g_ran = 0, g_unkeyed = 0, g_dropSize = 0, g_dropKey = 0, g_dropWanted = 0, g_loadNative = 0;

// The dev red arms of the lever drill: the judge's check of its own handle, and of the sender's reach, off.
bool AnimCheckOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::cremator_intent_anim_off);
    return off;
}
bool ReachCheckOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::cremator_intent_reach_off);
    return off;
}

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// One sender: its queued presses with their bucket, and the latch that clears with its occupant.
struct Slot {
    coop::net::IntentQueue<coop::net::KeyedTogglePayload> queue;
    bool waitSaid = false;  // a wait for the body, said once a streak
};
coop::roster_ledger::PerSlotState<Slot> g_slots;

// ---- host side -------------------------------------------------------------------------------
void Refuse(Reason why, uint8_t slot, const std::wstring& key, const char* more) {
    ++g_refused[why];
    Said& sd = g_said[why];
    const uint64_t now = NowMs();
    if (now < sd.nextMs) { ++sd.since; return; }
    UE_LOGI("[CREMATOR-INTENT] DENY slot=%u key='%ls' -- %s%s%s (%llu more since the last line)",
            static_cast<unsigned>(slot), key.c_str(), kReasonName[why], more ? ": " : "", more ? more : "",
            static_cast<unsigned long long>(sd.since));
    sd.nextMs = now + kRefusalSayMs;
    sd.since = 0;
}

// False keeps the press at its sender's queue head: the host has no body for the sender yet (its first pose has not
// arrived), so its reach cannot be measured, and the press waits for the body rather than being lost. Everything else
// is run or refused, and consumed.
bool Execute(coop::net::Session& s, const coop::net::KeyedTogglePayload& p, uint8_t slot) {
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    if (!s.IsSlotWorldReady(slot)) {
        Refuse(kNotWorldReady, slot, key, nullptr);
        return true;
    }
    void* cremator = key.empty() ? nullptr : coop::interactable_sync::ResolveCremator(key);
    if (!cremator) {
        Refuse(kNoDevice, slot, key, nullptr);
        return true;
    }
    const coop::element::IntentSubject subject =
        coop::element::IntentTarget::ForClientIntent(s, slot, kReachUU).Authorize(cremator);
    if (subject.outcome == coop::element::IntentOutcome::NoBody) {
        if (!g_slots[slot].waitSaid) {
            g_slots[slot].waitSaid = true;
            UE_LOGI("[CREMATOR-INTENT] slot %u's presses wait: the host has no body for it yet",
                    static_cast<unsigned>(slot));
        }
        return false;
    }
    if (!subject && !(ReachCheckOff() && subject.outcome == coop::element::IntentOutcome::OutOfReach)) {
        Refuse(kNotAuthorized, slot, key, coop::element::OutcomeName(subject.outcome));
        return true;
    }
    // Authorize answers reach only; the key is the sender's word, so the actor it names must be a cremator.
    if (!C::IsCremator(cremator)) {
        Refuse(kWrongClass, slot, key, nullptr);
        return true;
    }
    // The machine's own guard: a press while the handle is moving does nothing (it sits at the press, not in
    // useLever, which the judge calls directly).
    bool anim = false;
    if (!AnimCheckOff() && (!C::ReadAnim(cremator, anim) || anim)) {
        Refuse(kHandleBusy, slot, key, nullptr);
        return true;
    }
    // The press names the state its sender wants, the opposite of what its copy showed. The host's lever only
    // toggles, so a press whose wanted state already holds (the copy trails the host's by a handle travel) must not
    // invert the machine.
    const bool wantOn = p.action != 0;
    bool active = false;
    if (!C::ReadActive(cremator, active)) {
        Refuse(kNotRun, slot, key, nullptr);
        return true;
    }
    if (active == wantOn) {
        Refuse(kAlready, slot, key, wantOn ? "on" : "off");
        return true;
    }
    // A start with the host's door open would be turned back by the machine at the forward finish, with its "door is
    // open" hint on the host's screen for a client's press: refused here, before useLever runs.
    bool doorOpen = false;
    if (wantOn && C::ReadDoorOpened(cremator, doorOpen) && doorOpen) {
        Refuse(kDoorOpen, slot, key, nullptr);
        return true;
    }
    if (!C::CallUseLever(cremator)) {
        Refuse(kNotRun, slot, key, nullptr);
        return true;
    }
    ++g_ran;
    UE_LOGI("[CREMATOR-INTENT] host pulled the lever for slot %u on key='%ls' (wanted %s)", static_cast<unsigned>(slot),
            key.c_str(), wantOn ? "on" : "off");
    return true;
}

// ---- the client's gate -----------------------------------------------------------------------
// Before the body, on a client: the handle's press is sent to the host and its body refused. The valve's press is
// refused by coop/interactables/cremator_client_guard; every other action runs natively.
sg::Verdict OnPressPre(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != coop::net::Role::Client) return sg::Verdict::Run;
    if (!call.object || !C::IsCremator(call.object)) return sg::Verdict::Run;
    const int32_t off = g_action.Of(call.function);
    if (off < 0) {
        if (!g_actionSaid) {
            g_actionSaid = true;
            UE_LOGW("[CREMATOR-INTENT] the press's action parameter did not resolve -- a cremator press runs here "
                    "alone (said once)");
        }
        return sg::Verdict::Run;
    }
    if (*(call.locals + off) != C::kPressAction) return sg::Verdict::Run;
    bool handle = false;
    if (!C::ReadLookingAtHandle(call.object, handle) || !handle) return sg::Verdict::Run;
    // A call on an object of a world this client has not announced ready is that world's load, the host's save: it
    // runs natively, and the host's own state for what the lane owns follows the announce.
    if (!coop::net_pump::IsInAnnouncedWorld(call.object)) {
        ++g_loadNative;
        return sg::Verdict::Run;
    }
    // A cremator the lane does not index has no name the host could resolve: the press runs here, where the lane's
    // useLever gate refuses its lever, and the line says so, rate-limited, since to the player it is a dead handle.
    const std::wstring key = coop::interactable_sync::CrematorKey(call.object);
    if (key.empty()) {
        if (++g_unkeyed <= 3 || g_unkeyed % 20 == 0)
            UE_LOGW("[CREMATOR-INTENT] a handle press on cremator %p, which the cremator lane does not index -- "
                    "nothing sent (#%llu)", call.object, static_cast<unsigned long long>(g_unkeyed));
        return sg::Verdict::Run;
    }
    // The press asks for the opposite of what this copy shows.
    bool active = false;
    if (!C::ReadActive(call.object, active)) {
        if (!g_activeUnreadSaid) {
            g_activeUnreadSaid = true;
            UE_LOGW("[CREMATOR-INTENT] the copy's isActive did not read -- a handle press here is not sent (said once)");
        }
        return sg::Verdict::Cancel;
    }
    coop::net::KeyedTogglePayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.action = active ? 0 : 1;
    if (!s->SendReliable(coop::net::ReliableKind::CrematorIntent, &p, sizeof(p))) {
        UE_LOGW("[CREMATOR-INTENT] the press on key='%ls' was not sent (the session refused it); the machine stays as "
                "the host has it", key.c_str());
        return sg::Verdict::Cancel;
    }
    ++g_sent;
    UE_LOGI("[CREMATOR-INTENT] CLIENT SENT a handle press on key='%ls' (#%llu)", key.c_str(),
            static_cast<unsigned long long>(g_sent));
    return sg::Verdict::Cancel;
}

void RegisterWatch() {
    if (g_reg != Reg::Pending) return;
    if (sg::WatchName(kRowPress, kTagPress, &OnPressPre, nullptr)) {
        g_reg = Reg::Registered;
        return;
    }
    g_reg = Reg::Refused;
    UE_LOGE("[CREMATOR-INTENT] the script-body gate refused the watch on '%ls' of '%ls' -- for the rest of this process "
            "the handle press runs only where it is made", kRowPress.Name(), kRowPress.Class());
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    RegisterWatch();
}

void Tick(coop::net::Session& session) {
    sg::ResolvePendingNames();
    if (!g_settled) {
        if (g_reg == Reg::Registered && sg::NameWatchLive(kRowPress, kTagPress)) {
            g_settled = true;
            UE_LOGI("[CREMATOR-INTENT] the cremator press gate is live");
        } else if (g_reg != Reg::Pending && sg::PendingNameCount() == 0) {
            g_settled = true;
            UE_LOGE("[CREMATOR-INTENT] the cremator press gate is dead (refused, or resolved into a full table) -- "
                    "the handle press runs only where it is made");
        }
    }
    if (!session.running() || session.role() != coop::net::Role::Host) return;
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        Slot& sl = g_slots[slot];
        sl.queue.Serve(kBudget, NowMs, [&](const coop::net::KeyedTogglePayload& p) {
            if (!Execute(session, p, slot)) return coop::net::Served::Wait;  // a wait runs nothing: no token spent
            sl.waitSaid = false;  // a consumed press ends the wait's streak
            return coop::net::Served::Done;
        });
    }
}

void OnIntent(coop::net::Session& session, const coop::net::KeyedTogglePayload& payload, uint8_t senderSlot) {
    if (session.role() != coop::net::Role::Host) return;
    if (senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    Slot& sl = g_slots[senderSlot];
    auto payloadCopy = payload;
    if (!sl.queue.Push(std::move(payloadCopy), kMaxPending))
        Refuse(kQueueFull, senderSlot, coop::net::StringFromWireKey(payload.key), nullptr);
}

void CountDropped(Drop why) {
    ++(why == Drop::Size ? g_dropSize : why == Drop::Key ? g_dropKey : g_dropWanted);
}

uint64_t DeniedCount(Denied why) {
    return g_refused[why == Denied::HandleBusyOrUnread ? kHandleBusy : kNotAuthorized];
}

void OnDisconnect() {
    uint64_t refused = 0;
    for (const uint64_t n : g_refused) refused += n;
    if (g_sent || g_ran || g_unkeyed || g_loadNative || refused || g_dropSize || g_dropKey || g_dropWanted)
        UE_LOGI("[CREMATOR-INTENT] session end -- sent=%llu ran=%llu unkeyed=%llu own load run natively=%llu "
                "denied=%llu (no cremator %llu, wrong class %llu, world not ready %llu, not authorised %llu, handle "
                "busy or unread %llu, already so %llu, door open %llu, not run %llu, queue full %llu) dropped at the edge: size "
                "%llu, key %llu, wanted state %llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_ran),
                static_cast<unsigned long long>(g_unkeyed), static_cast<unsigned long long>(g_loadNative),
                static_cast<unsigned long long>(refused), static_cast<unsigned long long>(g_refused[kNoDevice]),
                static_cast<unsigned long long>(g_refused[kWrongClass]),
                static_cast<unsigned long long>(g_refused[kNotWorldReady]),
                static_cast<unsigned long long>(g_refused[kNotAuthorized]),
                static_cast<unsigned long long>(g_refused[kHandleBusy]), static_cast<unsigned long long>(g_refused[kAlready]),
                static_cast<unsigned long long>(g_refused[kDoorOpen]), static_cast<unsigned long long>(g_refused[kNotRun]),
                static_cast<unsigned long long>(g_refused[kQueueFull]), static_cast<unsigned long long>(g_dropSize),
                static_cast<unsigned long long>(g_dropKey), static_cast<unsigned long long>(g_dropWanted));
    g_sent = g_ran = g_unkeyed = g_dropSize = g_dropKey = g_dropWanted = g_loadNative = 0;
    for (uint64_t& n : g_refused) n = 0;
    for (Said& sd : g_said) sd = Said{};
    g_actionSaid = false;
    g_activeUnreadSaid = false;
}

}  // namespace coop::cremator_intent
