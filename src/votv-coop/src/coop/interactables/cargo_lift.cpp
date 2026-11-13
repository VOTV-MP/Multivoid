// coop/interactables/cargo_lift.cpp -- see coop/interactables/cargo_lift.h.

#include "coop/interactables/cargo_lift.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/intent_authority.h"
#include "coop/interactables/interactable_sync.h"  // the cargo lift channel's key, resolve and edge
#include "coop/net/intent_bucket.h"
#include "coop/net/intent_queue.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"  // LifeOf
#include "coop/player/roster_ledger.h"  // PerSlotState
#include "coop/session/net_pump.h"      // IsInAnnouncedWorld: a client's own world load runs natively

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/cargo_lift.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

namespace coop::cargo_lift {
namespace {

namespace L  = ue_wrap::cargo_lift;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

// The press, declared on the lift's class.
UE_FACT_FN(kRowPress, "cargo_lift", L"cargoLift_C", L"actionOptionIndex", Declared);
constexpr int kTagPress = 0x434C4950;  // 'CLIP'
// The lift's own reversal when its platform is blocked: its body is called by the move loop, never by a press.
UE_FACT_FN(kRowIgnore, "cargo_lift", L"cargoLift_C", L"ignore", Declared);
constexpr int kTagIgnore = 0x434C4947;  // 'CLIG'

// A press reaches as far as the look-at trace, 200 uu by default; twice that is the reach, to which the authority adds
// the lift's own bounds (its shaft) and its allowance for the sender's pose lagging.
constexpr float kReachUU = 400.0f;

// A press is a click and the lift takes seconds, so the bound is only felt by a flooding client.
constexpr coop::net::IntentBudget kBudget{4.0f, 4.0f};
constexpr size_t kMaxPending = 8;

std::atomic<coop::net::Session*> g_session{nullptr};
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg g_reg = Reg::Pending;
bool g_settled = false;

R::ParamOffset g_action{L"action"};
bool g_actionSaid = false;

// The lift this peer is pressing for the channel's apply: its own press runs through the client's gate.
void* g_applying = nullptr;

uint64_t g_sent = 0, g_cancelled = 0, g_reversed = 0, g_ran = 0, g_already = 0, g_moving = 0, g_refused = 0, g_dead = 0;
uint64_t g_applied = 0, g_turned = 0, g_heldAgainst = 0;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Slot {
    coop::net::IntentQueue<coop::net::KeyedTogglePayload> queue;
};
coop::roster_ledger::PerSlotState<Slot> g_slots;

// The host's press for a client: false keeps it queued while the host has no body to measure the sender's reach from.
bool Execute(coop::net::Session& s, const coop::net::KeyedTogglePayload& p, uint8_t slot) {
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    void* lift = key.empty() || !s.IsSlotWorldReady(slot) ? nullptr : coop::interactable_sync::ResolveCargoLift(key);
    if (!lift || !L::IsCargoLift(lift)) {
        ++g_refused;
        UE_LOGI("[CARGO-LIFT] DENY slot=%u key='%ls' -- no lift holds the key, or the sender's world is not ready",
                static_cast<unsigned>(slot), key.c_str());
        return true;
    }
    // A player who is not alive presses nothing; judged before reach, as the coffee, woodchipper and cord lanes judge it.
    if (coop::players::Registry::Get().LifeOf(slot) != coop::players::PuppetLife::Alive) {
        ++g_dead;
        UE_LOGI("[CARGO-LIFT] DENY slot=%u key='%ls' -- the sender is not alive", static_cast<unsigned>(slot),
                key.c_str());
        coop::interactable_sync::ResendCargoLiftsTo(slot);
        return true;
    }
    const coop::element::IntentSubject subject =
        coop::element::IntentTarget::ForClientIntent(s, slot, kReachUU).Authorize(lift);
    if (subject.outcome == coop::element::IntentOutcome::NoBody) return false;
    if (!subject) {
        ++g_refused;
        UE_LOGI("[CARGO-LIFT] DENY slot=%u key='%ls' -- %s", static_cast<unsigned>(slot), key.c_str(),
                coop::element::OutcomeName(subject.outcome));
        coop::interactable_sync::ResendCargoLiftsTo(slot);  // the refusal is answered: the sender's copy hears the host's
        return true;
    }
    // The press names the direction its sender wants. A lift already sent there, or still on its way, is left as it
    // is; the sender's copy converges on the channel's state.
    const bool wantUp = p.action != 0;
    bool up = false, moving = false;
    if (!L::ReadDir(lift, up) || !L::ReadMoving(lift, moving)) {
        ++g_refused;
        return true;
    }
    if (up == wantUp) {
        ++g_already;
        coop::interactable_sync::ResendCargoLiftsTo(slot);
        return true;
    }
    if (moving) {
        ++g_moving;
        return true;
    }
    if (L::Press(lift)) {
        ++g_ran;
        UE_LOGI("[CARGO-LIFT] host pressed '%ls' for slot %u (sent %s)", key.c_str(), static_cast<unsigned>(slot),
                wantUp ? "up" : "down");
    }
    return true;
}

// The cargo lift drill's red: a client's press runs the lift's own body on its copy alone.
bool ClientRuns() {
    static const bool on = coop::config::ResolveFlag(::coop::config_registry::rows::cargo_lift_client_runs);
    return on;
}

void Cancelled(const char* why) {
    ++g_cancelled;
    if (g_cancelled <= 3 || g_cancelled % 20 == 0)
        UE_LOGW("[CARGO-LIFT] a press was cancelled and not sent: %s (#%llu)", why,
                static_cast<unsigned long long>(g_cancelled));
}

// Before the press's body. A client's own press is refused and sent to the host; the channel's apply runs.
sg::Verdict OnPressPre(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != coop::net::Role::Client) return sg::Verdict::Run;
    if (ClientRuns()) return sg::Verdict::Run;
    if (!call.object || !L::IsCargoLift(call.object) || call.object == g_applying) return sg::Verdict::Run;
    const int32_t off = g_action.Of(call.function);
    if (off < 0) {
        if (!g_actionSaid) {
            g_actionSaid = true;
            UE_LOGW("[CARGO-LIFT] the press's action parameter did not resolve -- a lift press runs here alone");
        }
        return sg::Verdict::Run;
    }
    if (*(call.locals + off) != L::kPressAction) return sg::Verdict::Run;
    if (!coop::net_pump::IsInAnnouncedWorld(call.object)) return sg::Verdict::Run;
    // Every press that is cancelled and not sent is said (rate-limited) and counted, since to the player it is a dead
    // button: the lift not indexed yet here, a direction that does not read, or a send the session refused.
    const std::wstring key = coop::interactable_sync::CargoLiftKey(call.object);
    bool up = false;
    if (key.empty() || !L::ReadDir(call.object, up)) {
        Cancelled("the lift is not indexed here yet or its direction does not read");
        return sg::Verdict::Cancel;
    }
    coop::net::KeyedTogglePayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.action = up ? 0 : 1;
    if (!s->SendReliable(coop::net::ReliableKind::CargoLiftIntent, &p, sizeof(p))) {
        Cancelled("the session refused the send");
        return sg::Verdict::Cancel;
    }
    ++g_sent;
    UE_LOGI("[CARGO-LIFT] CLIENT SENT a press on key='%ls' (wants %s)", key.c_str(), up ? "down" : "up");
    return sg::Verdict::Cancel;
}

// After the press's body, on the host: the direction it left goes to every client.
void OnPressPost(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != coop::net::Role::Host) return;
    if (call.object && L::IsCargoLift(call.object)) coop::interactable_sync::OnCargoLiftVerb(call.object);
}

// The lift's own obstruction reversal. The Blueprint's move loop (ubergraph offsets 565-839) flips `dir` (offset 826)
// and then calls `ignore()` (offset 839) when the platform lags its attach point by 10 uu, with no call to the press, so
// the press's watch never sees it; `ignore` restarts the speed timeline, plays the click and sets `ignoreCheck` for a
// second (offsets 1698-1719), so the loop does not flip again at once. Only the host's reversal exists: after its body
// the host sends the direction it left, and every client's moving copy turns with it.
//
// A client's copy refuses `ignore`. The flip before the call has already happened, so the gate undoes it: the copy keeps
// its direction. With `ignoreCheck` never set the loop meets the same lag the next frame, flips, and is undone again, and
// the lag also keeps `alpha` from advancing (the flip branch skips offset 617), so an obstructed copy holds in place
// against the obstruction until the lag clears, going on in the direction it has, or until the host's state turns it.
sg::Verdict OnIgnorePre(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != coop::net::Role::Client) return sg::Verdict::Run;
    if (!call.object || !L::IsCargoLift(call.object)) return sg::Verdict::Run;
    bool flipped = false;
    if (!L::ReadDir(call.object, flipped) || !L::WriteDir(call.object, !flipped)) return sg::Verdict::Run;
    ++g_heldAgainst;
    return sg::Verdict::Cancel;
}

void OnIgnorePost(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != coop::net::Role::Host) return;
    if (call.object && L::IsCargoLift(call.object)) {
        ++g_reversed;
        coop::interactable_sync::OnCargoLiftVerb(call.object);
    }
}

void RegisterWatch() {
    if (g_reg != Reg::Pending) return;
    if (sg::WatchName(kRowPress, kTagPress, &OnPressPre, &OnPressPost) &&
        sg::WatchName(kRowIgnore, kTagIgnore, &OnIgnorePre, &OnIgnorePost)) {
        g_reg = Reg::Registered;
        return;
    }
    g_reg = Reg::Refused;
    UE_LOGE("[CARGO-LIFT] the script-body gate refused the watch on '%ls' of '%ls' -- the lift's press runs only "
            "where it is made", kRowPress.Name(), kRowPress.Class());
}

// The client's own press for the channel, through its gate.
bool PressForApply(void* lift) {
    g_applying = lift;
    const bool ok = L::Press(lift);
    g_applying = nullptr;
    if (ok) ++g_applied;
    return ok;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    RegisterWatch();
}

void Tick(coop::net::Session& session) {
    sg::ResolvePendingNames();
    if (!g_settled) {
        if (g_reg == Reg::Registered && sg::NameWatchLive(kRowPress, kTagPress) &&
            sg::NameWatchLive(kRowIgnore, kTagIgnore)) {
            g_settled = true;
            UE_LOGI("[CARGO-LIFT] the cargo lift press gate is live");
        } else if (g_reg != Reg::Pending && sg::PendingNameCount() == 0) {
            g_settled = true;
            UE_LOGE("[CARGO-LIFT] the cargo lift press gate is dead -- the lift's press runs only where it is made");
        }
    }
    if (!session.running() || session.role() != coop::net::Role::Host) return;
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        g_slots[slot].queue.Serve(kBudget, NowMs, [&](const coop::net::KeyedTogglePayload& p) {
            return Execute(session, p, slot) ? coop::net::Served::Done : coop::net::Served::Wait;
        });
    }
}

void OnIntent(coop::net::Session& session, const coop::net::KeyedTogglePayload& payload, uint8_t senderSlot) {
    if (session.role() != coop::net::Role::Host || senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    auto copy = payload;
    if (!g_slots[senderSlot].queue.Push(std::move(copy), kMaxPending)) ++g_refused;  // a flood: not answered, so no amplifier
}

bool ReadState(void* lift, bool& up) { return L::ReadDir(lift, up); }

bool ApplyState(void* lift, bool up) {
    bool cur = false, moving = false;
    if (!L::ReadDir(lift, cur) || !L::ReadMoving(lift, moving)) return false;
    if (cur == up) return true;
    // A copy that moves turns the way the host's lift turned (its own obstruction reversal, or a press whose state
    // reached here late): the move loop reads `dir` every frame, so writing it is the reversal the Blueprint makes
    // itself. A press is no use here, the Blueprint ignores one while `moving`.
    if (moving) {
        if (!L::WriteDir(lift, up)) return false;
        ++g_turned;
        return true;
    }
    return PressForApply(lift);
}

uint64_t PressesSent() { return g_sent; }
uint64_t PressesCancelled() { return g_cancelled; }

void OnDisconnect() {
    if (g_sent || g_cancelled || g_reversed || g_ran || g_already || g_moving || g_refused || g_dead || g_applied ||
        g_turned || g_heldAgainst)
        UE_LOGI("[CARGO-LIFT] session end -- client presses sent=%llu, cancelled unsent=%llu; host presses run=%llu, "
                "already so=%llu, lift moving=%llu, refused=%llu, sender dead=%llu; client applies pressed=%llu, "
                "turned while moving=%llu, own reversals refused=%llu; host reversals sent=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_cancelled),
                static_cast<unsigned long long>(g_ran), static_cast<unsigned long long>(g_already),
                static_cast<unsigned long long>(g_moving), static_cast<unsigned long long>(g_refused),
                static_cast<unsigned long long>(g_dead), static_cast<unsigned long long>(g_applied),
                static_cast<unsigned long long>(g_turned), static_cast<unsigned long long>(g_heldAgainst),
                static_cast<unsigned long long>(g_reversed));
    g_sent = g_cancelled = g_reversed = g_ran = g_already = g_moving = g_refused = g_dead = 0;
    g_applied = g_turned = g_heldAgainst = 0;
    g_actionSaid = false;
}

}  // namespace coop::cargo_lift
