// coop/interactables/drone_call_intent.cpp -- see coop/interactables/drone_call_intent.h.

#include "coop/interactables/drone_call_intent.h"

#include "coop/element/element.h"
#include "coop/element/intent_authority.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/props/active_drive.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/drone_console.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <iterator>

namespace coop::drone_call_intent {
namespace {

namespace D  = ue_wrap::drone_console;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

// The verb the console's buttons arrive on, watched on the console's own class by name, so the watch
// matches whichever class the running world loads under that name (poll arc 2.7: a level class is
// never kept by pointer across worlds).
const wchar_t* const kConsoleClass = L"droneConsole_C";
const wchar_t* const kActionVerb   = L"actionOptionIndex";
constexpr int kTagDroneCall = 0x44524f4e;  // 'DRON'

// The action the console's own switch routes to its faces. Its other cases, 10 and 11, are the
// lid, which is already a synced door and must keep running locally.
constexpr uint8_t kActionUse = 4;

// A console press is an arm's-length interaction, so the sender's reach is the desk's rather than
// the pile grab's: the console is a small box a player stands at.
constexpr float kConsoleReachUU = 400.0f;

// One press is one call. The drone's own body ignores a press it cannot honour, so a burst costs
// nothing but the dispatch; the bound is here so a stalled client's backlog neither stalls a frame
// nor arrives refused.
constexpr float    kPressBurst     = 2.0f;
constexpr float    kPressPerSecond = 1.0f;
constexpr size_t   kMaxPending     = 4;
constexpr uint64_t kRefusalSayMs   = 10000;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watchInstalled = false;
bool g_watchLive = false;

uint64_t g_sent = 0, g_flown = 0, g_denied = 0;
bool g_waitSaid[coop::net::kMaxPeers] = {};  // a sender's wait for its body, said once a streak

struct Bucket {
    float    tokens = kPressBurst;
    uint64_t lastMs = 0;
    uint64_t nextSayMs = 0;
};
Bucket g_rate[coop::net::kMaxPeers];
std::deque<coop::net::DroneFlyIntentPayload> g_pending[coop::net::kMaxPeers];

bool TakeToken(uint8_t slot) {
    Bucket& b = g_rate[slot];
    const uint64_t now = coop::active_drive::NowMs();
    if (b.lastMs != 0 && now > b.lastMs) {
        b.tokens += kPressPerSecond * static_cast<float>(now - b.lastMs) / 1000.0f;
        if (b.tokens > kPressBurst) b.tokens = kPressBurst;
    }
    b.lastMs = now;
    if (b.tokens < 1.0f) return false;
    b.tokens -= 1.0f;
    return true;
}

// ---- host side -------------------------------------------------------------------------------
// One press, run or refused and then consumed, or left at the head of its queue: false while the host
// has no body for the sender, the seconds between a joiner's curtain and its first pose. That is a
// mid-join window rather than a verdict, which the door-verb, keypad and container lanes wait out the
// same way (principle 8): a press made in it is not lost.
bool Execute(coop::net::Session& s, const coop::net::DroneFlyIntentPayload& p, uint8_t slot) {
    (void)p;  // verb 0 is the only face with a lane; the dispatcher refuses the rest
    // The console carries no identity to name, so the sender's reach IS the resolve: of this
    // world's consoles, the one the sender is standing at. Any of them calls the same drone, so
    // the first within reach is the press.
    const auto token = coop::element::IntentTarget::ForClientIntent(s, slot, kConsoleReachUU);
    if (!token.HasBody()) {
        if (!g_waitSaid[slot]) {
            g_waitSaid[slot] = true;
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
    // machine's own look-at. The other face only toggles leaveAfter5min, which has no lane and
    // stays local.
    if (!D::IsCursorOnKeyboard(call.object)) return sg::Verdict::Run;

    coop::net::DroneFlyIntentPayload p{};
    p.verb = 0;  // the keyboard; the leave-timer face still runs locally and has no lane
    if (!s->SendReliable(coop::net::ReliableKind::DroneFlyIntent, &p, sizeof(p))) {
        UE_LOGW("[DRONE-CALL] a keyboard press was not sent (the session refused it); the drone stays as "
                "the host has it");
        return sg::Verdict::Cancel;
    }
    ++g_sent;
    if (g_sent <= 3 || g_sent % 20 == 0)
        UE_LOGI("[DRONE-CALL] CLIENT SENT a keyboard press (#%llu)",
                static_cast<unsigned long long>(g_sent));
    return sg::Verdict::Cancel;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!g_watchInstalled)
        g_watchInstalled = sg::WatchClassName(kConsoleClass, kActionVerb, kTagDroneCall, &OnActionPre, nullptr);
}

void Tick(coop::net::Session& session) {
    sg::ResolvePendingNames();
    if (!g_watchInstalled) {
        g_watchInstalled = sg::WatchClassName(kConsoleClass, kActionVerb, kTagDroneCall, &OnActionPre, nullptr);
        return;
    }
    if (!g_watchLive && sg::ClassNameWatchLive(kConsoleClass, kActionVerb, kTagDroneCall)) {
        g_watchLive = true;
        UE_LOGI("[DRONE-CALL] the console action gate is live");
    }
    if (!session.running()) return;

    if (session.role() != coop::net::Role::Host) return;
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        if (g_pending[slot].empty() || !TakeToken(slot)) continue;
        const coop::net::DroneFlyIntentPayload p = g_pending[slot].front();
        if (Execute(session, p, slot)) {
            g_pending[slot].pop_front();
            g_waitSaid[slot] = false;        // a consumed press ends the wait's streak
        } else {
            g_rate[slot].tokens += 1.0f;     // a wait runs nothing, so it spends no token
        }
    }
}

void OnDroneFlyIntent(coop::net::Session& session, const coop::net::DroneFlyIntentPayload& payload,
                      uint8_t senderSlot) {
    if (session.role() != coop::net::Role::Host) return;
    if (senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    auto& q = g_pending[senderSlot];
    if (q.size() >= kMaxPending) {
        Bucket& b = g_rate[senderSlot];
        const uint64_t now = coop::active_drive::NowMs();
        if (now >= b.nextSayMs) {
            b.nextSayMs = now + kRefusalSayMs;
            UE_LOGW("[DRONE-CALL] slot %u queue full (%zu) -- refusing presses until it drains",
                    static_cast<unsigned>(senderSlot), q.size());
        }
        ++g_denied;
        return;
    }
    q.push_back(payload);
}

void OnPeerLeft(uint8_t slot) {
    if (slot >= coop::net::kMaxPeers) return;
    g_pending[slot].clear();
    g_rate[slot] = Bucket{};
    g_waitSaid[slot] = false;
}

uint64_t SentCount() { return g_sent; }

void OnDisconnect() {
    if (g_sent || g_flown || g_denied)
        UE_LOGI("[DRONE-CALL] session end -- sent=%llu pressed=%llu denied=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_flown),
                static_cast<unsigned long long>(g_denied));
    for (uint8_t slot = 0; slot < coop::net::kMaxPeers; ++slot) {
        g_pending[slot].clear();
        g_rate[slot] = Bucket{};
        g_waitSaid[slot] = false;
    }
    g_sent = g_flown = g_denied = 0;
}

}  // namespace coop::drone_call_intent
