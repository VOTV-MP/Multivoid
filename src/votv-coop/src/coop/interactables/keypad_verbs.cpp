// coop/interactables/keypad_verbs.cpp -- see coop/interactables/keypad_verbs.h.

#include "coop/interactables/keypad_verbs.h"

#include "coop/element/intent_authority.h"
#include "coop/interactables/keypad_sync.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/hand_item.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/passwordlock.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <string>

namespace coop::keypad_verbs {
namespace {

namespace KS = coop::keypad_sync;
namespace PL = ue_wrap::passwordlock;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace KI = coop::net::keypad_intent;
using KeypadEvent = coop::net::KeypadEvent;

// The verbs, watched by name: every class's function of that name fires, so each callback's first
// test is the keypad class. `open`, `reset` and `setActive` are common names. playerAnykey is the
// numpad's entry, watched for which key it got: its accept calls open(password == buffer), which a
// copy whose digits are still crossing judges false, the same call its cancel makes.
struct VerbWatch { const wchar_t* name; int tag; KS::Verb verb; bool post; bool anyKey; };
constexpr VerbWatch kWatches[] = {
    { L"inputNumber",     0x4b50494e /*'KPIN'*/, KS::Verb::InputNumber, false, false },
    { L"open",            0x4b504f50 /*'KPOP'*/, KS::Verb::Open,        false, false },
    { L"open2",           0x4b504f32 /*'KPO2'*/, KS::Verb::Open2,       false, false },
    { L"reset",           0x4b505253 /*'KPRS'*/, KS::Verb::Reset,       false, false },
    { L"falseEnterEvent", 0x4b504645 /*'KPFE'*/, KS::Verb::FalseEnter,  false, false },
    { L"setActive",       0x4b505341 /*'KPSA'*/, KS::Verb::SetActive,   true,  false },
    { L"playerAnykey",    0x4b50414b /*'KPAK'*/, KS::Verb::InputNumber, false, true },
};
constexpr int kWatchCount = static_cast<int>(sizeof(kWatches) / sizeof(kWatches[0]));

// The reach a sender's intent is judged against: a keypad is used through the look-at trace, as a
// door is, so the door intent's reach (coop/interactables/door_verb_intent) holds for it.
constexpr float kKeypadReachUU = 400.0f;

// Typing is several digits a second, so a sender's intents run at a rate a fast typist does not
// feel and a flooding client does.
constexpr float    kIntentBurst     = 8.0f;
constexpr float    kIntentPerSecond = 10.0f;
constexpr size_t   kMaxPending      = 16;
constexpr uint64_t kRefusalSayMs    = 10000;

std::atomic<coop::net::Session*> g_session{nullptr};
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg[kWatchCount] = {};
bool g_settled = false;

uint64_t g_sent = 0, g_ran = 0, g_denied = 0, g_worldRefused = 0;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Bucket {
    float    tokens = kIntentBurst;
    uint64_t lastMs = 0;
    uint64_t nextSayMs = 0;
};
Bucket g_rate[coop::net::kMaxPeers];
std::deque<coop::net::KeypadIntentPayload> g_pending[coop::net::kMaxPeers];

bool TakeToken(uint8_t slot) {
    Bucket& b = g_rate[slot];
    const uint64_t now = NowMs();
    if (b.lastMs != 0 && now > b.lastMs) {
        b.tokens += kIntentPerSecond * static_cast<float>(now - b.lastMs) / 1000.0f;
        if (b.tokens > kIntentBurst) b.tokens = kIntentBurst;
    }
    b.lastMs = now;
    if (b.tokens < 1.0f) return false;
    b.tokens -= 1.0f;
    return true;
}

const char* IntentName(uint8_t verb) {
    switch (verb) {
    case KI::kDigit:   return "digit";
    case KI::kSubmit:  return "submit";
    case KI::kCancel:  return "cancel";
    case KI::kKeycard: return "keycard";
    case KI::kReset:   return "reset";
    }
    return "?";
}

// One parameter's offset in the function that fired, resolved per function (a keypad subclass may
// declare its own), and said once when it does not resolve.
struct ParamSlot { void* fn = nullptr; int32_t off = -1; };
ParamSlot g_numParam, g_openParam, g_pairParam, g_keyParam, g_pressedParam;

int32_t ParamOffset(ParamSlot& slot, const sg::Call& call, const wchar_t* name) {
    if (slot.fn == call.function) return slot.off;
    slot.fn = call.function;
    slot.off = R::FindParamOffset(call.function, name);
    if (slot.off < 0) UE_LOGW("[KEYPAD-VERB] the parameter '%ls' did not resolve -- that verb is read as unknown", name);
    return slot.off;
}

bool CallerIs(const sg::Call& call, const wchar_t* className) {
    void* caller = call.callerObject;
    if (!caller) return false;
    void* cls = R::ClassOf(caller);
    return cls && R::NameEquals(R::NameOf(cls), className);
}

// ---- the host ----------------------------------------------------------------------------------
void Execute(coop::net::Session& s, const coop::net::KeypadIntentPayload& p, uint8_t slot) {
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    void* lock = KS::ResolveKeypad(key);
    if (!lock) {
        ++g_denied;
        UE_LOGI("[KEYPAD-VERB] DENY slot=%u %s key='%ls' -- the keypad lane indexes no keypad by that key",
                static_cast<unsigned>(slot), IntentName(p.verb), key.c_str());
        return;
    }
    const coop::element::IntentSubject subject =
        coop::element::IntentTarget::ForClientIntent(s, slot, kKeypadReachUU).Authorize(lock);
    if (!subject) {
        ++g_denied;
        UE_LOGI("[KEYPAD-VERB] DENY slot=%u %s key='%ls' -- %s (%.0f uu of %.0f)", static_cast<unsigned>(slot),
                IntentName(p.verb), key.c_str(), coop::element::OutcomeName(subject.outcome), subject.distUU,
                subject.reachUU);
        return;
    }
    // A keycard's swipe and a pass changer's use are the held item's verbs.
    const wchar_t* needs = p.verb == KI::kKeycard ? L"prop_keycard_C"
                         : p.verb == KI::kReset   ? L"prop_passchanger_C" : nullptr;
    if (needs && !coop::hand_item::HeldClassIs(slot, needs)) {
        ++g_denied;
        UE_LOGI("[KEYPAD-VERB] DENY slot=%u %s key='%ls' -- the sender does not hold a %ls",
                static_cast<unsigned>(slot), IntentName(p.verb), key.c_str(), needs);
        return;
    }
    bool dispatched = false;
    bool verdict = false;
    switch (p.verb) {
    case KI::kDigit:   dispatched = PL::CallInputNumber(lock, p.arg); break;
    case KI::kSubmit:  verdict = PL::BufferMatchesPassword(lock); dispatched = PL::CallOpen(lock, verdict); break;
    case KI::kCancel:  dispatched = PL::CallOpen(lock, false); break;
    case KI::kKeycard: verdict = p.arg != 0; dispatched = PL::CallOpen(lock, verdict); break;
    case KI::kReset:   dispatched = PL::CallReset(lock); break;
    default: break;  // the dispatcher refuses any other verb
    }
    if (dispatched) ++g_ran; else ++g_denied;
    if (p.verb != KI::kDigit || !dispatched || g_ran <= 20)
        UE_LOGI("[KEYPAD-VERB] host ran slot %u's %s on key='%ls': dispatched=%d%s", static_cast<unsigned>(slot),
                IntentName(p.verb), key.c_str(), dispatched ? 1 : 0,
                (p.verb == KI::kSubmit || p.verb == KI::kKeycard) ? (verdict ? ", verdict accept" : ", verdict deny") : "");
}

void HostPre(const sg::Call& call, KS::Verb verb) {
    switch (verb) {
    case KS::Verb::InputNumber: {
        // A press off the digits sends nothing: the open it makes is sent itself.
        const int32_t off = ParamOffset(g_numParam, call, L"num");
        if (off < 0) return;
        const int32_t num = *reinterpret_cast<const int32_t*>(call.locals + off);
        if (num >= 0 && num <= 9) KS::SendEvent(call.object, KeypadEvent::Digit, static_cast<uint8_t>(num));
        return;
    }
    case KS::Verb::Open: {
        const int32_t off = ParamOffset(g_openParam, call, L"active");
        if (off < 0) return;
        const bool accept = *reinterpret_cast<const bool*>(call.locals + off);
        KS::SendEvent(call.object, KeypadEvent::Open, accept ? 1 : 0);
        return;
    }
    case KS::Verb::Open2:      KS::SendEvent(call.object, KeypadEvent::Guesser, 0); return;
    case KS::Verb::Reset:
        // A protected keypad's reset changes nothing but its own 2D sound, which every peer would hear.
        if (!PL::IsProtected(call.object)) KS::SendEvent(call.object, KeypadEvent::Reset, 0);
        return;
    case KS::Verb::FalseEnter: KS::SendEvent(call.object, KeypadEvent::FalseEntry, 0); return;
    case KS::Verb::SetActive:  return;  // its state goes after the body
    }
}

// ---- the client --------------------------------------------------------------------------------
void SendIntent(coop::net::Session& s, const std::wstring& key, uint8_t verb, uint8_t arg) {
    coop::net::KeypadIntentPayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.verb = verb;
    p.arg = arg;
    if (!s.SendReliable(coop::net::ReliableKind::KeypadIntent, &p, sizeof(p))) {
        UE_LOGW("[KEYPAD-VERB] the %s on key='%ls' was not sent (the session refused it); the keypad stays "
                "as the host has it", IntentName(verb), key.c_str());
        return;
    }
    ++g_sent;
    if (verb != KI::kDigit || g_sent <= 20)
        UE_LOGI("[KEYPAD-VERB] CLIENT SENT a %s on key='%ls' (arg=%u, #%llu)", IntentName(verb), key.c_str(),
                static_cast<unsigned>(arg), static_cast<unsigned long long>(g_sent));
}

// CLIENT, before playerAnykey: the numpad's accept and cancel are told apart by the key, the engine's
// own name for it (the keypad's graph reads its display name: "Num +" accepts, "Num -" and "-" cancel);
// every other key runs, its digit reaching inputNumber.
sg::Verdict ClientAnyKey(coop::net::Session& s, const sg::Call& call) {
    void* lock = call.object;
    if (KS::ApplyingAny(lock)) return sg::Verdict::Cancel;
    const std::wstring key = KS::KeypadKey(lock);
    if (key.empty()) return sg::Verdict::Run;
    const int32_t keyOff = ParamOffset(g_keyParam, call, L"key");
    const int32_t pressedOff = ParamOffset(g_pressedParam, call, L"pressed");
    if (keyOff < 0 || pressedOff < 0) return sg::Verdict::Cancel;
    if (!*reinterpret_cast<const bool*>(call.locals + pressedOff)) return sg::Verdict::Run;  // a release does nothing
    const R::FName& name = *reinterpret_cast<const R::FName*>(call.locals + keyOff);  // an FKey is its name first
    if (R::NameEquals(name, L"Add")) {
        SendIntent(s, key, KI::kSubmit, 0);
        return sg::Verdict::Cancel;
    }
    if (R::NameEquals(name, L"Subtract") || R::NameEquals(name, L"Hyphen")) {
        SendIntent(s, key, KI::kCancel, 0);
        return sg::Verdict::Cancel;
    }
    return sg::Verdict::Run;
}

sg::Verdict ClientPre(coop::net::Session& s, const sg::Call& call, KS::Verb verb) {
    void* lock = call.object;
    if (KS::Applying(lock, verb)) return sg::Verdict::Run;       // the lane's own replay
    if (KS::ApplyingAny(lock)) return sg::Verdict::Cancel;       // inside the replay: the host's record follows
    // A keypad's own chain hands its state on through setActive: a replayed open's latent tail, a
    // pair's setActive(true), begin play, processKeys.
    const bool keypadCaller = call.callerObject && PL::IsPasswordLock(call.callerObject);
    if (verb == KS::Verb::SetActive && keypadCaller) return sg::Verdict::Run;
    const std::wstring key = KS::KeypadKey(lock);
    if (key.empty()) return sg::Verdict::Run;                    // no lane owns it: native
    switch (verb) {
    case KS::Verb::InputNumber: {
        const int32_t off = ParamOffset(g_numParam, call, L"num");
        if (off < 0) return sg::Verdict::Cancel;
        const int32_t num = *reinterpret_cast<const int32_t*>(call.locals + off);
        if (num >= 0 && num <= 9) {
            SendIntent(s, key, KI::kDigit, static_cast<uint8_t>(num));
        } else {
            bool onAccept = false, onCancel = false;
            PL::ReadHover(lock, onAccept, onCancel);
            SendIntent(s, key, (!onAccept && onCancel) ? KI::kCancel : KI::kSubmit, 0);
        }
        return sg::Verdict::Cancel;
    }
    case KS::Verb::Open: {
        // The keypad's own opens come from inputNumber and the numpad, both refused at their entry, so
        // one here reaches no host; a keycard's swipe is sent, anything else is the world's.
        if (!keypadCaller && CallerIs(call, L"prop_keycard_C")) {
            const int32_t off = ParamOffset(g_openParam, call, L"active");
            const bool accept = off >= 0 && *reinterpret_cast<const bool*>(call.locals + off);
            SendIntent(s, key, KI::kKeycard, accept ? 1 : 0);
        } else {
            ++g_worldRefused;
        }
        return sg::Verdict::Cancel;
    }
    case KS::Verb::Reset:
        if (CallerIs(call, L"prop_passchanger_C")) SendIntent(s, key, KI::kReset, 0);
        else ++g_worldRefused;
        return sg::Verdict::Cancel;
    case KS::Verb::Open2:
    case KS::Verb::FalseEnter:
    case KS::Verb::SetActive:
        ++g_worldRefused;
        return sg::Verdict::Cancel;
    }
    return sg::Verdict::Run;
}

const VerbWatch* WatchOf(int tag) {
    for (const VerbWatch& w : kWatches)
        if (w.tag == tag) return &w;
    return nullptr;
}

sg::Verdict OnVerbPre(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return sg::Verdict::Run;
    if (!call.object || !PL::IsPasswordLock(call.object)) return sg::Verdict::Run;
    const VerbWatch* w = WatchOf(call.tag);
    if (!w) return sg::Verdict::Run;
    if (s->role() == coop::net::Role::Host) {
        if (!w->anyKey) HostPre(call, w->verb);  // the host's numpad reaches its own inputNumber and open
        return sg::Verdict::Run;
    }
    return w->anyKey ? ClientAnyKey(*s, call) : ClientPre(*s, call, w->verb);
}

// After setActive's propagating call (isPairCall false), which ends every chain that moved a keypad.
// HOST: the settled state of the keypad and of the pair it handed its state to. CLIENT: this copy's
// own chain ended, so a state that waited for it is written.
void OnSetActivePost(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    if (!call.object || !PL::IsPasswordLock(call.object)) return;
    const int32_t off = ParamOffset(g_pairParam, call, L"isPairCall");
    if (off < 0 || *reinterpret_cast<const bool*>(call.locals + off)) return;
    if (s->role() == coop::net::Role::Host) {
        KS::SendState(call.object);
        if (void* pair = PL::PairOf(call.object)) KS::SendState(pair);
        return;
    }
    if (call.callerObject && PL::IsPasswordLock(call.callerObject)) KS::OnClientChainEnd(call.object);
}

void RegisterWatches() {
    for (int i = 0; i < kWatchCount; ++i) {
        if (g_reg[i] != Reg::Pending) continue;
        if (sg::WatchName(kWatches[i].name, kWatches[i].tag, &OnVerbPre, kWatches[i].post ? &OnSetActivePost : nullptr)) {
            g_reg[i] = Reg::Registered;
            continue;
        }
        g_reg[i] = Reg::Refused;
        UE_LOGE("[KEYPAD-VERB] the script-body gate refused the watch on '%ls' -- for the rest of this process "
                "the keypad lane cannot see that verb", kWatches[i].name);
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
            else if (g_reg[i] == Reg::Registered && sg::NameWatchLive(kWatches[i].name, kWatches[i].tag)) ++live;
        }
        if (live == kWatchCount) {
            g_settled = true;
            UE_LOGI("[KEYPAD-VERB] the keypad verb gates are live: inputNumber, open, open2, reset, falseEnterEvent, "
                    "setActive, playerAnykey");
        } else if (pending == 0 && sg::PendingNameCount() == 0) {
            g_settled = true;
            UE_LOGE("[KEYPAD-VERB] %d of %d keypad verb gates are dead (refused, or resolved into a full table) -- "
                    "the keypad lane cannot see those verbs", kWatchCount - live, kWatchCount);
        }
    }
    if (!session.running() || session.role() != coop::net::Role::Host) return;
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        if (g_pending[slot].empty() || !TakeToken(slot)) continue;
        const coop::net::KeypadIntentPayload p = g_pending[slot].front();
        g_pending[slot].pop_front();
        Execute(session, p, slot);
    }
}

void OnKeypadIntent(coop::net::Session& session, const coop::net::KeypadIntentPayload& payload,
                    uint8_t senderSlot) {
    if (session.role() != coop::net::Role::Host) return;
    if (senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    auto& q = g_pending[senderSlot];
    if (q.size() >= kMaxPending) {
        Bucket& b = g_rate[senderSlot];
        const uint64_t now = NowMs();
        if (now >= b.nextSayMs) {
            b.nextSayMs = now + kRefusalSayMs;
            UE_LOGW("[KEYPAD-VERB] slot %u queue full (%zu) -- refusing its keypad intents until it drains",
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
}

void OnDisconnect() {
    if (g_sent || g_ran || g_denied || g_worldRefused)
        UE_LOGI("[KEYPAD-VERB] session end -- sent=%llu ran=%llu denied=%llu world verbs refused=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_ran),
                static_cast<unsigned long long>(g_denied), static_cast<unsigned long long>(g_worldRefused));
    for (uint8_t slot = 0; slot < coop::net::kMaxPeers; ++slot) {
        g_pending[slot].clear();
        g_rate[slot] = Bucket{};
    }
    g_sent = g_ran = g_denied = g_worldRefused = 0;
}

}  // namespace coop::keypad_verbs
