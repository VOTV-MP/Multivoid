// coop/interactables/drone_call_intent.cpp -- see coop/interactables/drone_call_intent.h.

#include "coop/interactables/drone_call_intent.h"

#include "coop/element/element.h"
#include "coop/element/intent_authority.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/intent_queue.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/roster_ledger.h"  // PerSlotState
#include "coop/props/active_drive.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/drone_console.h"
#include "ue_wrap/devices/drone.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <iterator>

namespace coop::drone_call_intent {
namespace {

namespace D  = ue_wrap::drone_console;
namespace GT = ue_wrap::game_thread;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

// The verb the console's buttons arrive on, watched on the console's own class by name, so the watch
// matches whichever class the running world loads under that name: a level class is never kept by
// pointer across worlds. The drone's own option menu arrives on the drone's.
UE_FACT_FN(kRowActionOptionIndex, "drone_call_intent", L"droneConsole_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowDroneActionOptionIndex, "drone_call_intent", L"drone_C", L"actionOptionIndex", Declared);
constexpr int kTagDroneCall = 0x44524f4e;  // 'DRON'
constexpr int kTagDroneSack = 0x44525350;  // 'DRSP'

// The action the console's own switch routes to its faces. Its other cases, 10 and 11, are the
// lid, which is already a synced door and must keep running locally.
constexpr uint8_t kActionUse = 4;

// The drone's own action options: 4 opens the cargo (local: it reads the cargo the host sent), 7 drops the sack.
constexpr uint8_t kActionDropSack = 7;

// A console press is an arm's-length interaction, so the sender's reach is the desk's rather than
// the pile grab's: the console is a small box a player stands at.
constexpr float kConsoleReachUU = 400.0f;
// The parked drone is a body some metres across, looked at from beside it.
constexpr float kDroneReachUU = 600.0f;

// One press is one call. The drone's own body ignores a press it cannot honour, so a burst costs
// nothing but the dispatch; the bound is here so a stalled client's backlog neither stalls a frame
// nor arrives refused.
constexpr coop::net::IntentBudget kPressBudget{2.0f, 1.0f};
constexpr size_t   kMaxPending     = 4;
constexpr uint64_t kRefusalSayMs   = 10000;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watchInstalled = false;
bool g_watchLive = false;
bool g_sackWatchInstalled = false;

uint64_t g_sent = 0, g_flown = 0, g_denied = 0;

// One sender: its queued presses with their bucket, and the latches that clear with its occupant.
struct Slot {
    coop::net::IntentQueue<coop::net::DroneFlyIntentPayload> queue;
    uint64_t nextSayMs = 0;      // the full queue's refusal, said once in kRefusalSayMs
    bool     waitSaid  = false;  // a wait for the body, said once a streak
};
coop::roster_ledger::PerSlotState<Slot> g_slots;

// ---- host side -------------------------------------------------------------------------------
// The sack option: the drone itself is within the sender's reach, parked, with its sack aboard. The
// host's own copy of the drone is tested, never the press, and dropSack runs on it: the sack and the
// cargo it takes leave on the prop and container lanes, and the drone's flags on DroneState.
void ExecuteDropSack(coop::net::Session& s, uint8_t slot) {
    void* const drone = ue_wrap::drone::Find();
    const auto token = coop::element::IntentTarget::ForClientIntent(s, slot, kDroneReachUU);
    if (!drone || token.Authorize(drone).outcome != coop::element::IntentOutcome::Ok) {
        ++g_denied;
        UE_LOGI("[DRONE-CALL] DENY slot=%u -- the drone is not within reach", static_cast<unsigned>(slot));
        return;
    }
    const uint8_t bits = ue_wrap::drone::ReadFxBits(drone);
    if (!(bits & ue_wrap::drone::kFxArrived) || !(bits & ue_wrap::drone::kFxHasSack)) {
        ++g_denied;
        UE_LOGI("[DRONE-CALL] DENY slot=%u -- the drone is not parked with its sack aboard",
                static_cast<unsigned>(slot));
        return;
    }
    if (!ue_wrap::drone::DropSack(drone)) {
        ++g_denied;
        UE_LOGW("[DRONE-CALL] slot=%u -- dropSack did not dispatch", static_cast<unsigned>(slot));
        return;
    }
    ++g_flown;
    UE_LOGI("[DRONE-CALL] the sack dropped for slot=%u", static_cast<unsigned>(slot));
}

// One press, run or refused and then consumed, or left at the head of its queue: false while the host
// has no body for the sender, the seconds between a joiner's curtain and its first pose. That is a
// mid-join window rather than a verdict, which the door-verb, keypad and container lanes wait out the
// same way (principle 8): a press made in it is not lost.
bool Execute(coop::net::Session& s, const coop::net::DroneFlyIntentPayload& p, uint8_t slot) {
    if (p.verb == coop::net::drone_verb::kDropSack) {
        if (!coop::element::IntentTarget::ForClientIntent(s, slot, kDroneReachUU).HasBody()) {
            if (!g_slots[slot].waitSaid) {
                g_slots[slot].waitSaid = true;
                UE_LOGI("[DRONE-CALL] slot %u's presses wait: the host has no body for it yet",
                        static_cast<unsigned>(slot));
            }
            return false;
        }
        ExecuteDropSack(s, slot);
        return true;
    }
    // The console carries no identity to name, so the sender's reach IS the resolve: of this
    // world's consoles, the one the sender is standing at. Any of them calls the same drone, so
    // the first within reach is the press.
    const auto token = coop::element::IntentTarget::ForClientIntent(s, slot, kConsoleReachUU);
    if (!token.HasBody()) {
        if (!g_slots[slot].waitSaid) {
            g_slots[slot].waitSaid = true;
            UE_LOGI("[DRONE-CALL] slot %u's presses wait: the host has no body for it yet",
                    static_cast<unsigned>(slot));
        }
        return false;
    }
    void* consoles[4];
    const int32_t nc = D::LiveConsoles(consoles, static_cast<int32_t>(std::size(consoles)));
    void* console = nullptr;
    for (int32_t i = 0; i < nc && !console; ++i)
        if (token.Authorize(consoles[i]).outcome == coop::element::IntentOutcome::Ok) console = consoles[i];
    if (!console) {
        ++g_denied;
        UE_LOGI("[DRONE-CALL] DENY slot=%u -- none of the %d live console(s) is within reach",
                static_cast<unsigned>(slot), nc);
        return true;
    }
    // The lid is the console's own gate on its keyboard, and it is shared state the box lane
    // (door_box) already carries, so the host reads its own copy rather than trusting the press.
    if (!D::IsLidOpen(console)) {
        ++g_denied;
        UE_LOGI("[DRONE-CALL] DENY slot=%u -- the console's lid is shut here",
                static_cast<unsigned>(slot));
        return true;
    }
    if (p.verb == coop::net::drone_verb::kLeaveTimer) {
        void* drone = ue_wrap::drone::Find();
        bool leaveAfter5min = false;
        if (!drone || !ue_wrap::drone::ReadLeaveAfter5min(drone, leaveAfter5min) ||
            !ue_wrap::drone::WriteLeaveAfter5min(drone, !leaveAfter5min)) {
            ++g_denied;
            UE_LOGW("[DRONE-CALL] slot=%u -- leave timer could not be written on the host drone",
                    static_cast<unsigned>(slot));
            return true;
        }
        // The face's press is the toggle and then the console's click; the host's own copy of the
        // console plays it, and the presser's machine plays its own (OnActionPre).
        if (!D::PlayChangeCue(console))
            UE_LOGW("[DRONE-CALL] slot=%u -- the console's change cue did not play on the host",
                    static_cast<unsigned>(slot));
        ++g_flown;
        UE_LOGI("[DRONE-CALL] leave timer set by slot=%u to %s",
                static_cast<unsigned>(slot), leaveAfter5min ? "indefinitely" : "five minutes");
        return true;
    }
    if (!D::TriggerFly(console)) {
        ++g_denied;
        UE_LOGW("[DRONE-CALL] slot=%u -- triggerFly did not dispatch (no drone reference or the "
                "verb did not resolve)", static_cast<unsigned>(slot));
        return true;
    }
    ++g_flown;
    UE_LOGI("[DRONE-CALL] PRESSED console %p for slot=%u (#%llu) -- the drone's own body owns the "
            "outcome", console, static_cast<unsigned>(slot),
            static_cast<unsigned long long>(g_flown));
    return true;
}

// ---- the client's gate -----------------------------------------------------------------------
sg::Verdict OnActionPre(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Client || !call.object) return sg::Verdict::Run;

    static void*   sFn = nullptr;
    static int32_t sActionOff = -1;
    if (call.function != sFn) {
        sFn = call.function;
        sActionOff = R::FindParamOffset(call.function, L"action");
    }
    if (sActionOff < 0) return sg::Verdict::Run;
    if (*reinterpret_cast<uint8_t*>(call.locals + sActionOff) != kActionUse)
        return sg::Verdict::Run;  // the lid's cases stay the game's, on every peer

    // Which face this press is belongs to the presser: the cursor fields are written by this
    // machine's own look-at. Each timer-face press remains a toggle, which the host applies to its
    // latest value so two quick presses still select the original setting.
    const bool keyboard = D::IsCursorOnKeyboard(call.object);
    const bool leaveTimer = D::IsCursorOnLeaveTimer(call.object);
    if (!keyboard && !leaveTimer) return sg::Verdict::Run;

    coop::net::DroneFlyIntentPayload p{};
    p.verb = keyboard ? coop::net::drone_verb::kCall : coop::net::drone_verb::kLeaveTimer;
    if (!s->SendReliable(coop::net::ReliableKind::DroneFlyIntent, &p, sizeof(p))) {
        UE_LOGW("[DRONE-CALL] the console press was not sent (the session refused it); the drone stays "
                "as the host has it");
        return sg::Verdict::Cancel;
    }
    ++g_sent;
    if (g_sent <= 3 || g_sent % 20 == 0)
        UE_LOGI("[DRONE-CALL] CLIENT SENT a %s press (#%llu)", keyboard ? "keyboard" : "leave-timer",
                static_cast<unsigned long long>(g_sent));
    // The timer face's own body would have clicked here, and the body it cancels is the only thing
    // that plays it on this machine, so the presser hears its press from this copy of the console.
    // The gate is inside the script VM's loop, so the cue is posted and plays after it returns.
    if (!keyboard) {
        ue_wrap::CachedObjRef console;
        console.Set(call.object);
        GT::Post([console]() {
            void* c = console.Get();
            if (!c || !D::PlayChangeCue(c))
                UE_LOGW("[DRONE-CALL] the console's change cue did not play on this client");
        });
    }
    return sg::Verdict::Cancel;
}

// The drone's own option menu: the sack option is the host's to run, since dropSack moves the cargo into
// a new sack, and a client's copy of it would be a second sack and a second cargo. The open option reads
// the cargo this client holds and stays local.
sg::Verdict OnDroneActionPre(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Client || !call.object) return sg::Verdict::Run;

    static void*   sFn = nullptr;
    static int32_t sActionOff = -1;
    if (call.function != sFn) {
        sFn = call.function;
        sActionOff = R::FindParamOffset(call.function, L"action");
    }
    if (sActionOff < 0) return sg::Verdict::Run;
    if (*reinterpret_cast<uint8_t*>(call.locals + sActionOff) != kActionDropSack) return sg::Verdict::Run;

    coop::net::DroneFlyIntentPayload p{};
    p.verb = coop::net::drone_verb::kDropSack;
    if (!s->SendReliable(coop::net::ReliableKind::DroneFlyIntent, &p, sizeof(p))) {
        UE_LOGW("[DRONE-CALL] the drop-sack press was not sent (the session refused it); the drone stays as the "
                "host has it");
        return sg::Verdict::Cancel;
    }
    ++g_sent;
    UE_LOGI("[DRONE-CALL] CLIENT SENT a drop-sack press (#%llu)", static_cast<unsigned long long>(g_sent));
    return sg::Verdict::Cancel;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!g_watchInstalled)
        g_watchInstalled = sg::WatchName(kRowActionOptionIndex, kTagDroneCall, &OnActionPre, nullptr);
    if (!g_sackWatchInstalled)
        g_sackWatchInstalled = sg::WatchName(kRowDroneActionOptionIndex, kTagDroneSack, &OnDroneActionPre, nullptr);
}

void Tick(coop::net::Session& session) {
    sg::ResolvePendingNames();
    if (!g_sackWatchInstalled)
        g_sackWatchInstalled = sg::WatchName(kRowDroneActionOptionIndex, kTagDroneSack, &OnDroneActionPre, nullptr);
    if (!g_watchInstalled) {
        g_watchInstalled = sg::WatchName(kRowActionOptionIndex, kTagDroneCall, &OnActionPre, nullptr);
        return;
    }
    if (!g_watchLive && sg::NameWatchLive(kRowActionOptionIndex, kTagDroneCall)) {
        g_watchLive = true;
        UE_LOGI("[DRONE-CALL] the console action gate is live");
    }
    if (!session.running()) return;

    if (session.role() != coop::net::Role::Host) return;
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        Slot& sl = g_slots[slot];
        sl.queue.Serve(kPressBudget, coop::active_drive::NowMs, [&](const coop::net::DroneFlyIntentPayload& p) {
            if (!Execute(session, p, slot)) return coop::net::Served::Wait;  // a wait runs nothing: no token spent
            sl.waitSaid = false;  // a consumed press ends the wait's streak
            return coop::net::Served::Done;
        });
    }
}

void OnDroneFlyIntent(coop::net::Session& session, const coop::net::DroneFlyIntentPayload& payload,
                      uint8_t senderSlot) {
    if (session.role() != coop::net::Role::Host) return;
    if (senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    Slot& sl = g_slots[senderSlot];
    coop::net::DroneFlyIntentPayload copy = payload;
    if (!sl.queue.Push(std::move(copy), kMaxPending)) {
        const uint64_t now = coop::active_drive::NowMs();
        if (now >= sl.nextSayMs) {
            sl.nextSayMs = now + kRefusalSayMs;
            UE_LOGW("[DRONE-CALL] slot %u queue full (%zu) -- refusing presses until it drains",
                    static_cast<unsigned>(senderSlot), sl.queue.size());
        }
        ++g_denied;
    }
}

uint64_t SentCount() { return g_sent; }

void OnDisconnect() {
    if (g_sent || g_flown || g_denied)
        UE_LOGI("[DRONE-CALL] session end -- sent=%llu pressed=%llu denied=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_flown),
                static_cast<unsigned long long>(g_denied));
    g_sent = g_flown = g_denied = 0;
}

}  // namespace coop::drone_call_intent
