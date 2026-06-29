// coop/keypad_sync.cpp -- see coop/keypad_sync.h. Password-keypad (ApasswordLock_C) INPUT
// mirror: poll inPassword -> broadcast on a buffer change; receiver replays inputNumber for
// the digit delta (which drives the keypad's own native validator -- MTA input-replication).
// No isAcc/isDeny mirror (removed 2026-06-06: hover flags, the old PURPLE).
//
// HOST AUTHORITY for keypad POWER + the gated door's LOCK (2026-06-17, the "some keypads are dead
// from the host's perspective" fix). keypad.active is BUILDING POWER (keypad BP RE 2026-06-06):
// setActive propagates it to the gated door (door.active = keypad.active) and the door's E-press
// opens iff Active && !jammed && !superClosed -- so it is host-authoritative world state, like the
// HostAuth door channel. The two axes are split:
//   - DIGIT BUFFER (inPassword): bidirectional INPUT mirror. A client typing replays onto the host's
//     keypad, running the HOST's own native validator (a correct code unlocks the shared door
//     host-authoritatively; a wrong 5-digit code auto-submits + denies on the host's own validator).
//   - active / door POWER + LED: HOST-AUTHORITATIVE. The host NEVER takes its keypad power / door lock
//     from a CLIENT packet (ApplyState's active/door write is client-only; ApplyIncoming drops a
//     client's DENY before open(false) on the host). A client's cancel / wrong short code / a
//     save-transfer transient carrying active=0 can no longer de-power the host's door. Clients mirror
//     the host's authoritative active. (Known edge: a client's deliberate cancel still reddens its OWN
//     LED + may briefly diverge until the host's next authoritative broadcast / the connect-snapshot;
//     the SP-faithful re-lock is now host-driven. A full single-syncer refactor -- client sends input
//     requests, host is the sole KeypadState producer -- would close that edge; deferred as it is the
//     reported bug's secondary symptom, not its cause.)
//
// v59 SUBMIT MIRROR (2026-06-11, replaces the deleted HostAcceptPoll): the BP auto-submits
// at Len>=5 (uber @2398), so long codes validate NATIVELY on every peer from the digit
// replay alone. A SHORT code's accept press (open(password==inPassword)) and the explicit
// cancel (open(false)) change no digit -- the typing peer detects its own native submit
// EDGE in the poll (active flip + buffer cleared, lastKnown buffer 0<len<5, not reset
// mode), stamps KeypadEvent::Accept/Deny on the state packet, and every receiver runs the
// keypad's OWN native Open(Active) chain (accept/deny sound, LED, buffer clear, and the
// LOCK-state propagation to the PAIR keypad + the gated door via setActive). A native
// accept UNLOCKS the door -- it does NOT open it (the door's 4s-doorOpen chain belongs to
// a scripted trigger entry, not the player accept); opening the unlocked door is a normal
// E press, already synced by the door channel. The old HostAcceptPoll accepted on
// buffer==password at ANY length WITHOUT the accept press, auto-ForceOpened the door
// (non-native), latched the LED green for the session (g_unlocked re-assert), and
// permanently muted the door's autoclose (SuppressHostHeldDoor with no release): the
// 2026-06-11 "door opens on the last digit + stuck green/open forever" bug. All deleted
// (RULE 2).
//
// REPLAYED-CHAIN SETTLING (2026-06-12, the red/green echo-storm fix): the BP open() body
// defers its state writes through latent sub-chains (the CallOpen contract: "never assume
// synchronous state"; proven live -- the poll read the PRE-chain {code,0} for ~0.3s after
// ProcessEvent(Open) returned). Priming lastKnown to the pre-chain {'',false} (the original
// v59 echo-break) therefore let the poll (1) BROADCAST the stale mid-chain buffer (the
// "poison" packet that retyped the code + forced the LED red on every peer) and then
// (2) CLASSIFY the chain's landing ({code,0}->{'',1} with a short lastKnown buffer) as a
// fresh local Accept (the "phantom"). Both peers did both -> a self-sustaining ~3Hz
// cross-peer CallOpen loop: LED popping red/green, door power thrash, PE=224k/s, RAM
// balloon (2026-06-12 hands-on). The chain's settled endpoint is DETERMINISTIC ({'',
// Active}: the Open param IS the verdict, no internal re-validation), so ApplyIncoming now
// primes lastKnown to the ENDPOINT and marks the key SETTLING; the poll neither broadcasts
// nor classifies that key until the keypad reads the endpoint (failsafe TTL re-baselines
// silently). A delta surfacing at settle-erase is an interleaved apply, not a local press
// -- it is broadcast as plain state (convergence) but never event-classified.
//
// Structure borrows the proven interactable_sync Channel patterns (key->actor index
// with IsLiveByIndex self-heal, throttled rebuild, deferred-apply retry, silent first-
// sight prime, echo-suppress via priming lastKnown_ to the applied value) but with a
// keypad-shaped state (a typed buffer + 3 bools) and an input-replay apply, which is why
// it is a separate module rather than another toggle Adapter (RULE 2: forcing it into the
// toggle Channel is what produced the v31 fail-cycle).

#include "coop/interactables/keypad_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"  // WireKeyFromString / StringFromWireKey / FnvKey (shared)
#include "coop/player/players_registry.h"  // coop::players::kMaxPeers

#include "ue_wrap/door.h"          // the active mirror keeps the gated door's LOCK state in step
#include "ue_wrap/log.h"
#include "ue_wrap/passwordlock.h"
#include "ue_wrap/reflection.h"
#include "coop/util/incremental_object_scan.h"  // L5: scan only NEW objects (no 237k walk)
#include "ue_wrap/walk_timer.h"           // L5: [WALK-TIME] profiling

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop::keypad_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace PL = ue_wrap::passwordlock;

constexpr auto kRetryRebuildThrottle = std::chrono::seconds(2);
constexpr auto kPendingTTL = std::chrono::seconds(25);

std::atomic<coop::net::Session*> g_session{nullptr};

struct Ref { void* actor; int32_t idx; };
using State = PL::State;

std::mutex g_mutex;  // guards the maps below (all access is game-thread-serial; defensive)
std::unordered_map<std::wstring, Ref>   g_index;      // key -> live keypad
std::unordered_map<std::wstring, State> g_lastKnown;  // key -> last broadcast/applied state (change-detect + echo-suppress)
struct Pending { State want; coop::net::KeypadEvent ev; std::chrono::steady_clock::time_point deadline; };
std::unordered_map<std::wstring, Pending> g_pending;  // key -> deferred incoming apply

// A native chain WE dispatched (ApplyIncoming's CallOpen) whose state writes have not yet
// landed (the BP defers them through latent sub-chains -- see the header note). Until the
// keypad settles on the chain's deterministic endpoint {'', Active}, the poll must neither
// broadcast nor event-classify this key. The deadline is a failsafe only (a chain that
// never reaches its endpoint stops suppressing); a healthy chain erases itself the moment
// the endpoint reads back (observed landings are sub-second).
struct Settling { State endpoint; std::chrono::steady_clock::time_point deadline; };
std::unordered_map<std::wstring, Settling> g_settling;  // key -> replayed chain in flight
constexpr auto kSettleTTL = std::chrono::seconds(2);

std::chrono::steady_clock::time_point g_lastRetry{};
size_t g_lastLogCount = SIZE_MAX;
uint64_t g_lastLogHash = 0;
bool g_indexed = false;
std::vector<std::pair<std::wstring, Ref>> g_pollScratch;  // GT-only: reused per-tick poll snapshot (no per-tick heap alloc)

// ---- WireKey <-> wstring + FNV key hash: shared coop::net helpers (RULE 2:
// extracted to coop/net/wire_key_util.h). Pulled into this anonymous namespace so
// the existing unqualified call sites resolve unchanged. -------------------------
using coop::net::WireKeyFromString;
using coop::net::StringFromWireKey;
using coop::net::FnvKey;

bool SameState(const State& a, const State& b) {
    return a.buffer == b.buffer && a.active == b.active;
}

// State <-> payload. The buffer is digits only (keypad input is 0..9); a non-digit (never
// expected) is dropped so the wire stays digit-clean.
void StateToPayload(const std::wstring& key, const State& st, coop::net::KeypadEvent ev,
                    coop::net::KeypadSyncPayload& p) {
    std::memset(&p, 0, sizeof(p));
    WireKeyFromString(key, p.key);
    uint8_t n = 0;
    for (wchar_t c : st.buffer) {
        if (n >= sizeof(p.buf)) break;
        if (c >= L'0' && c <= L'9') p.buf[n++] = static_cast<uint8_t>(c - L'0');
    }
    p.bufLen = n;
    p.active = st.active ? 1 : 0;  // v38: LED selector / door power (cancel -> red)
    p.event  = static_cast<uint8_t>(ev);  // v59: short-code submit mirror
}
State PayloadToState(const coop::net::KeypadSyncPayload& p) {
    State st;
    uint8_t n = p.bufLen; if (n > sizeof(p.buf)) n = sizeof(p.buf);
    st.buffer.reserve(n);
    for (uint8_t i = 0; i < n; ++i) {
        uint8_t d = p.buf[i]; if (d > 9) d = 9;
        st.buffer.push_back(static_cast<wchar_t>(L'0' + d));
    }
    st.active = (p.active != 0);  // v38
    return st;
}

void* ResolveFast(const std::wstring& key) {
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_index.find(key);
    if (it != g_index.end() && R::IsLiveByIndex(it->second.actor, it->second.idx)) return it->second.actor;
    return nullptr;
}

// Full GUObjectArray walk -> rebuild the key->actor index. Game thread. Logs a keys-hash
// (cross-peer Key stability signal) only on change.
size_t RebuildIndex() {
    if (!PL::EnsureResolved()) return 0;
    // L5: scan only the range NextRange gives -- the array TAIL (new objects) every call, a FULL
    // re-scan every ~5min (the slot-reuse backstop). A keypad is a level-loaded ApasswordLock_C, so
    // the tail-scan catches it at load; static actors rarely reuse slots, so the rare full safety
    // covers any drift.
    static coop::util::IncrementalObjectScan sScan;
    const auto r = coop::util::NextRange(sScan);
    std::vector<std::pair<std::wstring, Ref>> found;
    found.reserve(32);
    for (int32_t i = r.begin; i < r.end; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj || !PL::IsPasswordLock(obj)) continue;
        const std::wstring nm = R::ToString(R::NameOf(obj));
        if (nm.rfind(L"Default__", 0) == 0) continue;  // skip CDO
        if (!R::IsLive(obj)) continue;
        std::wstring key = PL::GetKeyString(obj);
        if (key.empty() || key == L"None") continue;  // unkeyed template -- not a placed keypad
        found.emplace_back(std::move(key), Ref{ obj, R::InternalIndexOf(obj) });
    }
    uint64_t keysHash = 0;
    size_t   total;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (r.isFull) g_index.clear();                         // full re-scan: rebuild from scratch
        for (auto& f : found) g_index[f.first] = f.second;     // add the new (or all, on a full scan)
        if (!r.isFull) {                                       // tail scan: prune dead entries (cheap, O(index))
            for (auto it = g_index.begin(); it != g_index.end(); ) {
                if (R::IsLiveByIndex(it->second.actor, it->second.idx)) ++it;
                else it = g_index.erase(it);
            }
        }
        for (auto& kv : g_index) keysHash ^= FnvKey(kv.first);  // recompute over the index (cheap, O(index))
        total = g_index.size();
    }
    if (total != g_lastLogCount || keysHash != g_lastLogHash) {
        g_lastLogCount = total;
        g_lastLogHash = keysHash;
        UE_LOGI("keypad: index rebuilt -- %zu live keyed keypad(s), keysHash=0x%016llX (%s scan, +%zu new) "
                "(compare host vs client for cross-peer Key stability)",
                total, static_cast<unsigned long long>(keysHash), r.isFull ? "full" : "tail", found.size());
    }
    return total;
}

// RECEIVER apply: drive `actor`'s typed buffer to `want` by replaying the digit delta via
// inputNumber (native display + beep) -- which also runs the keypad's OWN native validator,
// so on the HOST replaying a client's correct code makes the host accept it (MTA input-
// replication). NEVER a submit verb, NEVER an isAcc/isDeny write (those are crosshair-hover
// flags -> the old PURPLE). Updates g_lastKnown[key] so this peer's poll never echoes it.
void ApplyState(void* actor, const std::wstring& key, const State& want, unsigned fromSlot) {
    State cur;
    if (!PL::ReadState(actor, cur)) return;

    // --- typed-buffer reconcile (digits only) ---
    if (cur.buffer != want.buffer) {
        const bool append = want.buffer.size() >= cur.buffer.size() &&
                            want.buffer.compare(0, cur.buffer.size(), cur.buffer) == 0;
        if (!append) {
            // diverged / shrank (CANCEL clear, post-submit clear, backspace) -> clear the typed
            // buffer then retype. ClearBuffer is a direct inPassword length-zero with NO side
            // effects -- NOT the BP Reset() verb, which is the keypad's "set a new code" mode
            // (isReset=true -> BLUE LED): mirroring the client's CANCEL (a buffer shrink) through
            // Reset() turned the HOST blue forever (the 2026-06-08 bug). The red LED is mirrored
            // separately by the `active` write below; the panel repaint is the CallUpd at the end.
            PL::ClearBuffer(actor);
            for (wchar_t c : want.buffer)
                if (c >= L'0' && c <= L'9') PL::CallInputNumber(actor, static_cast<int32_t>(c - L'0'));
        } else {
            // pure append -> replay only the new digits
            for (size_t i = cur.buffer.size(); i < want.buffer.size(); ++i) {
                wchar_t c = want.buffer[i];
                if (c >= L'0' && c <= L'9') PL::CallInputNumber(actor, static_cast<int32_t>(c - L'0'));
            }
        }
    }
    // active (LED selector @0x0330 + door power) -- mirrors the cancel->red the user reported.
    // Re-read AFTER the buffer reconcile: replaying a correct/wrong code can fire the keypad's OWN
    // native validator (open()), which sets `active` itself -- so a wrong-code red the digit replay
    // already reproduced is NOT double-written here (after.active already == want.active). Only the
    // EXPLICIT cancel button (no digit typed -> buffer mirror misses it) still diverges, and we close
    // it with a DIRECT field-write (never the setActive verb -> no powerChanged "purple") + propagate
    // the same value to the gated door's power so keypad.active == door.active like SP (coop=SP).
    State after;
    if (PL::ReadState(actor, after) && after.active != want.active) {
        // HOST AUTHORITY (2026-06-17, "keypads dead from the host's view" fix). keypad.active is
        // BUILDING POWER (keypad BP RE 2026-06-06): setActive propagates it to the gated door
        // (door.active = keypad.active) and the door's E-press opens iff Active && !jammed &&
        // !superClosed. It is host-authoritative world state -- exactly why the door channel is
        // HostAuth. So the HOST must NEVER take its keypad power / door lock from a CLIENT packet: a
        // client's cancel / wrong-code / save-transfer transient carrying active=0 would otherwise
        // de-power the host's door, making the host's own E-press fail = the reported "some keypads
        // are dead from the host's perspective." Only a CLIENT mirrors the host's authoritative active
        // (+ door). The host's keypad power changes solely from its OWN native open() -- driven by the
        // replayed digit input running the host's validator -- and the gamemode power system; the host
        // then broadcasts that authoritative result. The digit BUFFER reconcile above stays
        // bidirectional (the input mirror), so a client typing the correct code still unlocks the
        // shared door via the host's own validation.
        auto* s = g_session.load(std::memory_order_acquire);
        if (s && s->role() == coop::net::Role::Client) {
            PL::WriteActive(actor, want.active);
            if (void* door = PL::GatedDoor(actor)) ue_wrap::door::SetActive(door, want.active);
        }
    }
    // Repaint the digit DISPLAY + the LED (so a native clear wipes the panel and upd() re-selects the
    // particle template from the freshly-written `active`: eff_glow_red when !active).
    PL::CallUpd(actor);

    { std::lock_guard<std::mutex> lk(g_mutex); g_lastKnown[key] = want; }
    UE_LOGI("keypad: applied key='%ls' buf='%ls' active=%d (from slot %u)",
            key.c_str(), want.buffer.c_str(), want.active ? 1 : 0, fromSlot);
}

// SENDER: poll every indexed keypad for a state change and broadcast deltas. First sight
// of a key primes the baseline SILENTLY (initial divergence is the connect-snapshot's
// job). Echo is impossible: ApplyState primes g_lastKnown to the applied value, so the
// next poll sees no delta. Game thread.
//
// v59: the delta is also CLASSIFIED into a KeypadEvent (the short-code submit mirror):
//   Accept -- active flipped false->true AND lastKnown buffer was a SHORT code (0<len<5):
//             the native Open(true) chain just completed here from a local accept press.
//             (len>=5 stays None: the digit replay already ran the BP auto-submit on every
//             peer -- stamping it would double-run the chain. A receiver's own CallOpen
//             never reaches classification at all: ApplyIncoming marks the key SETTLING
//             and the poll skips it until the chain lands on the primed endpoint -- the
//             echo-break, see the header note.)
//   Deny   -- buffer shrank to empty with active false AND lastKnown buffer was a short
//             code: a wrong-code accept press or the explicit cancel.
//   Both stamps are gated on !IsResetMode (entering set-new-code mode shrinks the buffer
//   too -- a stamped Deny would deny-blink every peer on a password change).
// NO door drive here: a native accept UNLOCKS the door (open(Active) propagates
// active to pair + door via setActive) -- it never opens it (the @2061 4s-doorOpen
// chain is a scripted trigger entry, NOT the player accept; 2026-06-06 doc + audit
// 2026-06-11). Opening the now-unlocked door is a normal player E press, already
// synced by the door channel. The old auto-ForceOpen here was half of the
// "door always open" bug.
void PollAndBroadcast() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;

    auto& refs = g_pollScratch;  // reused buffer (GT-serial) -- no per-tick heap alloc
    refs.clear();
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (g_index.empty()) return;
        refs.reserve(g_index.size());
        for (auto& kv : g_index) refs.emplace_back(kv.first, kv.second);
    }
    for (auto& r : refs) {
        if (!R::IsLiveByIndex(r.second.actor, r.second.idx)) {
            // A dead/streamed-out keypad can never land its chain: drop any settling
            // entry so a re-streamed actor isn't suppressed by the stale endpoint
            // for the TTL remainder (audit 2026-06-12 item 9a). lastKnown keeps the
            // endpoint -- the re-streamed actor converges via the normal delta path
            // (no event possible: the endpoint buffer is empty).
            std::lock_guard<std::mutex> lk(g_mutex);
            g_settling.erase(r.first);
            continue;
        }
        State cur;
        if (!PL::ReadState(r.second.actor, cur)) continue;
        State last;
        bool classify = true;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            // A replayed Open chain in flight on this key: its writes land frames after the
            // CallOpen (latent BP sub-chains), so any state read before the endpoint is a
            // TRANSIENT -- broadcasting one is the poison packet and classifying the landing
            // is the phantom Accept (the 2026-06-12 echo storm; header note). Suppress the
            // key until it settles; a delta that remains AT settle-erase came from an
            // interleaved apply, not a local press -- converge it, never classify it.
            auto sIt = g_settling.find(r.first);
            if (sIt != g_settling.end()) {
                if (SameState(cur, sIt->second.endpoint)) {
                    g_settling.erase(sIt);  // chain landed (lastKnown already == endpoint)
                    classify = false;
                } else if (std::chrono::steady_clock::now() < sIt->second.deadline) {
                    continue;               // mid-chain -- no broadcast, no classification
                } else {
                    g_settling.erase(sIt);  // failsafe: chain never landed -- converge below
                    classify = false;
                }
            }
            auto it = g_lastKnown.find(r.first);
            if (it == g_lastKnown.end()) { g_lastKnown[r.first] = cur; continue; }  // prime silently
            if (SameState(it->second, cur)) continue;                                // no change
            last = it->second;
        }
        // Classify the delta (see the function comment). Reads on the live actor are
        // outside g_mutex by design (engine access never under our lock).
        coop::net::KeypadEvent ev = coop::net::KeypadEvent::None;
        const bool shortCode = !last.buffer.empty() && last.buffer.size() < 5;
        if (classify && shortCode && !PL::IsResetMode(r.second.actor)) {
            if (!last.active && cur.active) {
                ev = coop::net::KeypadEvent::Accept;
            } else if (cur.buffer.empty() && !cur.active) {
                ev = coop::net::KeypadEvent::Deny;
            }
        }
        coop::net::KeypadSyncPayload p{};
        StateToPayload(r.first, cur, ev, p);
        if (s->SendReliable(coop::net::ReliableKind::KeypadState, &p, sizeof(p))) {
            { std::lock_guard<std::mutex> lk(g_mutex); g_lastKnown[r.first] = cur; }
            UE_LOGI("keypad: sent key='%ls' buf='%ls' active=%d ev=%u", r.first.c_str(),
                    cur.buffer.c_str(), cur.active ? 1 : 0, static_cast<unsigned>(ev));
        } else {
            UE_LOGW("keypad: SendReliable failed key='%ls'", r.first.c_str());
        }
    }
}

// Incoming-packet dispatch: a stamped Accept/Deny runs the keypad's NATIVE submit chain
// (CallOpen) -- the receiver-side replication of a short-code accept/cancel press; a plain
// None packet takes the existing state-mirror ApplyState. The echo-break is the ENDPOINT
// prime + settle mark: open(Active)'s writes land frames later (latent BP sub-chains), but
// its settled state is deterministic ({'', Active} -- the param IS the verdict), so
// lastKnown is primed to that endpoint and the key is marked settling. The poll skips the
// key until the keypad reads the endpoint (then cur == lastKnown -> nothing is sent at
// all), so neither the mid-chain transient nor the landing can be broadcast or classified
// -- the original {'',false} pre-chain prime allowed both, which was the 2026-06-12
// cross-peer echo storm (header note). The chain does the LED + buffer clear + pair/door
// LOCK propagation natively.
void ApplyIncoming(void* actor, const std::wstring& key, const State& want,
                   coop::net::KeypadEvent ev, unsigned fromSlot) {
    if (ev == coop::net::KeypadEvent::None) { ApplyState(actor, key, want, fromSlot); return; }
    if (PL::IsResetMode(actor)) {
        UE_LOGW("keypad: dropping ev=%u for key='%ls' -- keypad is in set-new-code mode",
                static_cast<unsigned>(ev), key.c_str());
        return;
    }
    const bool accept = (ev == coop::net::KeypadEvent::Accept);
    // HOST AUTHORITY: a CLIENT's DENY (a wrong short code or the explicit cancel) must NOT run
    // open(false) on the host -- open(false) sets active=false and propagates it to the gated door,
    // de-powering the host's door (the "dead keypad from the host's view" bug). The host applies only a
    // client's ACCEPT (a legitimate shared unlock: the client entered the correct code on the shared
    // keypad, which powers the door for everyone). A Deny on the host instead falls through to a
    // buffer-only reconcile (ApplyState, whose active/door write is host-skipped) so the host's keypad
    // power stays its own. A CLIENT still replays BOTH events (it mirrors the host's authoritative
    // accept/deny result). The host's own keypad presses + the gamemode remain its only power source.
    {
        auto* s = g_session.load(std::memory_order_acquire);
        if (s && s->role() == coop::net::Role::Host && !accept) {
            ApplyState(actor, key, want, fromSlot);
            return;
        }
    }
    if (!PL::CallOpen(actor, accept)) {
        // Degraded fallback (Open UFunction unresolved): mirror the END state directly so
        // the LED/door at least converge -- the plain state apply.
        ApplyState(actor, key, want, fromSlot);
        return;
    }
    State endpoint;
    endpoint.active = accept;  // the chain settles on {'', Active} -- see the comment above
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_lastKnown[key] = endpoint;
        g_settling[key] = Settling{ endpoint, std::chrono::steady_clock::now() + kSettleTTL };
    }
    UE_LOGI("keypad: native Open(%d) replayed key='%ls' (from slot %u)",
            accept ? 1 : 0, key.c_str(), fromSlot);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!g_indexed && PL::EnsureResolved()) {
        UE_LOGI("keypad: indexed %zu keypad(s)", RebuildIndex());
        g_indexed = true;
    }
}

void OnReliable(const coop::net::KeypadSyncPayload& payload, uint8_t senderPeerSlot) {
    std::wstring key = StringFromWireKey(payload.key);
    if (key.empty()) { UE_LOGW("keypad: OnReliable empty key -- dropping"); return; }
    if (!PL::EnsureResolved()) { UE_LOGW("keypad: apply -- class not resolved, dropping key='%ls'", key.c_str()); return; }
    State want = PayloadToState(payload);
    auto ev = static_cast<coop::net::KeypadEvent>(payload.event);
    if (ev != coop::net::KeypadEvent::None && ev != coop::net::KeypadEvent::Accept &&
        ev != coop::net::KeypadEvent::Deny) {
        ev = coop::net::KeypadEvent::None;  // unknown future value -> degrade to state mirror
    }
    if (void* actor = ResolveFast(key)) { ApplyIncoming(actor, key, want, ev, senderPeerSlot); return; }
    // Not streamed in yet -- defer + retry on the throttled tick.
    std::lock_guard<std::mutex> lk(g_mutex);
    g_pending[key] = Pending{ std::move(want), ev, std::chrono::steady_clock::now() + kPendingTTL };
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;  // host-only snapshot
    if (peerSlot < 0 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    RebuildIndex();
    std::vector<std::pair<std::wstring, Ref>> items;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        items.reserve(g_index.size());
        for (auto& kv : g_index) items.emplace_back(kv.first, kv.second);
    }
    int sent = 0;
    for (auto& d : items) {
        if (!R::IsLiveByIndex(d.second.actor, d.second.idx)) continue;
        State cur;
        if (!PL::ReadState(d.second.actor, cur)) continue;
        {
            // A key mid-settle snapshots the chain's ENDPOINT, not the live keypad:
            // the live read is a mid-chain transient (the joiner would mirror the
            // poison state), and writing it to lastKnown below would clobber the
            // endpoint prime (audit 2026-06-12 item 7).
            std::lock_guard<std::mutex> lk(g_mutex);
            auto sIt = g_settling.find(d.first);
            if (sIt != g_settling.end()) cur = sIt->second.endpoint;
        }
        coop::net::KeypadSyncPayload p{};
        // Snapshot = plain state (event None): the joiner mirrors the RESULT (green/red LED,
        // typed digits); door state arrives via the door channel's own snapshot.
        StateToPayload(d.first, cur, coop::net::KeypadEvent::None, p);
        s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::KeypadState, &p, sizeof(p));
        { std::lock_guard<std::mutex> lk(g_mutex); g_lastKnown[d.first] = cur; }
        ++sent;
    }
    UE_LOGI("keypad: connect-snapshot -- sent %d state(s) to slot %d (of %zu indexed)", sent, peerSlot, items.size());
}

void Tick() {
    if (!PL::EnsureResolved()) return;
    if (!g_indexed) { UE_LOGI("keypad: indexed %zu keypad(s)", RebuildIndex()); g_indexed = true; }

    const auto now = std::chrono::steady_clock::now();
    if (now - g_lastRetry >= kRetryRebuildThrottle) {
        g_lastRetry = now;
        ue_wrap::ScopedWalkTimer _wt("keypad:RebuildIndex");  // logs only the rare ~5min full safety
        RebuildIndex();   // L5: INCREMENTAL -- tail-scan + a rare full safety (no 237k walk)
        // RECEIVER: retry deferred applies for keypads that have now streamed in.
        std::vector<std::pair<std::wstring, Pending>> ready;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            for (auto it = g_pending.begin(); it != g_pending.end();) {
                auto idxIt = g_index.find(it->first);
                if (idxIt != g_index.end() && R::IsLiveByIndex(idxIt->second.actor, idxIt->second.idx)) {
                    ready.emplace_back(it->first, it->second);
                    it = g_pending.erase(it);
                } else if (now >= it->second.deadline) {
                    it = g_pending.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& rdy : ready)
            if (void* actor = ResolveFast(rdy.first))
                ApplyIncoming(actor, rdy.first, rdy.second.want, rdy.second.ev, 0xFF);
    }
    PollAndBroadcast();
}

void OnDisconnect() {
    std::lock_guard<std::mutex> lk(g_mutex);
    const size_t n = g_lastKnown.size();
    g_lastKnown.clear();
    g_pending.clear();
    g_settling.clear();
    if (n > 0) UE_LOGI("keypad: OnDisconnect cleared %zu last-known", n);
}

}  // namespace coop::keypad_sync
