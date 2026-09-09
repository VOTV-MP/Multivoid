// coop/interactables/keypad_sync.cpp -- the password keypad (passwordLock_C) mirror, on two axes.
// The digit buffer is a bidirectional input mirror: the poll broadcasts on a change, and the
// receiver replays the digit delta through inputNumber, which runs the keypad's own validator, so a
// client typing the correct code unlocks the shared door through the host's validation. Power
// (active, propagated to the gated door's lock) is host-authoritative for state packets and
// input-replayed for press events: a plain packet never drives the host's power, while a
// stamped Accept or Deny is a deliberate press every peer replays through the keypad's own
// Open chain. The BP auto-submits at five digits, so long codes validate from the replay
// alone; a short code's accept and the cancel change no digit, so the typing peer detects its
// own submit edge in the poll and stamps the event. Open's writes land through latent
// sub-chains, so a replayed chain marks its key settling and primes lastKnown to the endpoint
// {'', Active}, and the poll neither broadcasts nor classifies the key until the keypad reads
// it. The index, retry and echo-suppression shape is interactable_channel's, with a
// keypad-shaped state. See the header.

#include "coop/interactables/keypad_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"  // WireKeyFromString / StringFromWireKey / FnvKey (shared)
#include "coop/player/players_registry.h"  // coop::players::kMaxPeers

#include "ue_wrap/devices/door.h"          // the active mirror keeps the gated door's LOCK state in step
#include "ue_wrap/core/log.h"
#include "ue_wrap/devices/passwordlock.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/world_identity.h"     // the world generation the index is stamped with
#include "coop/element/object_scan_hub.h"      // the shared sliced scan pass

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
std::unordered_map<std::wstring, State> g_lastKnown;  // key -> last broadcast or applied state
struct Pending { State want; coop::net::KeypadEvent ev; std::chrono::steady_clock::time_point deadline; };
std::unordered_map<std::wstring, Pending> g_pending;  // key -> deferred incoming apply

// A native chain this peer dispatched (ApplyIncoming's CallOpen) whose writes have not landed.
// Until the keypad reads the endpoint {'', Active} the poll neither broadcasts nor classifies
// the key; the deadline is a failsafe, and a healthy chain erases itself within a second.
struct Settling { State endpoint; std::chrono::steady_clock::time_point deadline; };
std::unordered_map<std::wstring, Settling> g_settling;  // key -> replayed chain in flight
constexpr auto kSettleTTL = std::chrono::seconds(2);

std::chrono::steady_clock::time_point g_lastRetry{};
size_t g_lastLogCount = SIZE_MAX;
uint64_t g_lastLogHash = 0;
std::vector<std::pair<std::wstring, Ref>> g_pollScratch;  // GT-only: the reused poll snapshot
// The world generation of the last completed hub pass; a stale-generation index reads as empty.
uint32_t g_indexGen = 0;
bool IndexCurrent() { return g_indexGen == ue_wrap::world_identity::Generation(); }

// The wire-key helpers, pulled into this namespace.
using coop::net::WireKeyFromString;
using coop::net::StringFromWireKey;
using coop::net::FnvKey;

bool SameState(const State& a, const State& b) {
    return a.buffer == b.buffer && a.active == b.active;
}

// State to payload and back. The buffer is digits only; a non-digit is dropped.
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
    p.active = st.active ? 1 : 0;  // the LED selector and door power
    p.event  = static_cast<uint8_t>(ev);  // the short-code submit event
}
State PayloadToState(const coop::net::KeypadSyncPayload& p) {
    State st;
    uint8_t n = p.bufLen; if (n > sizeof(p.buf)) n = sizeof(p.buf);
    st.buffer.reserve(n);
    for (uint8_t i = 0; i < n; ++i) {
        uint8_t d = p.buf[i]; if (d > 9) d = 9;
        st.buffer.push_back(static_cast<wchar_t>(L'0' + d));
    }
    st.active = (p.active != 0);
    return st;
}

void* ResolveFast(const std::wstring& key) {
    if (!IndexCurrent()) return nullptr;  // a stale-generation index is another world's actors
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_index.find(key);
    if (it != g_index.end() && R::IsLiveByIndex(it->second.actor, it->second.idx)) return it->second.actor;
    return nullptr;
}

// The scan-hub consumer: the hub's shared pass drives these three callbacks. The index is
// world-stamped, and a stale generation reads as empty, since slot-and-serial liveness cannot
// see world death.
std::vector<std::pair<std::wstring, Ref>> g_scanFound;  // pass scratch (GT-only)

void HubPassBegin(void*, bool /*isFull*/) { g_scanFound.clear(); }

void HubMatch(void*, void* obj) {
    const std::wstring nm = R::ToString(R::NameOf(obj));
    if (nm.rfind(L"Default__", 0) == 0) return;  // skip CDO
    if (!R::IsLive(obj)) return;
    std::wstring key = PL::GetKeyString(obj);
    if (key.empty() || key == L"None") return;  // unkeyed template -- not a placed keypad
    g_scanFound.emplace_back(std::move(key), Ref{ obj, R::InternalIndexOf(obj) });
}

size_t HubPassComplete(void*, bool isFull, uint32_t worldGen) {
    const size_t added = g_scanFound.size();
    uint64_t keysHash = 0;
    size_t   total;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (isFull) g_index.clear();                           // full pass: rebuild from scratch
        for (auto& f : g_scanFound) g_index[f.first] = f.second;
        if (!isFull) {                                         // tail pass: prune dead entries (cheap, O(index))
            for (auto it = g_index.begin(); it != g_index.end(); ) {
                if (R::IsLiveByIndex(it->second.actor, it->second.idx)) ++it;
                else it = g_index.erase(it);
            }
        }
        for (auto& kv : g_index) keysHash ^= FnvKey(kv.first);  // recompute over the index (cheap, O(index))
        total = g_index.size();
        g_indexGen = worldGen;
    }
    g_scanFound.clear();
    if (total != g_lastLogCount || keysHash != g_lastLogHash) {
        g_lastLogCount = total;
        g_lastLogHash = keysHash;
        UE_LOGI("keypad: index rebuilt -- %zu live keyed keypad(s), keysHash=0x%016llX (%s pass, +%zu new) "
                "(compare host vs client for cross-peer Key stability)",
                total, static_cast<unsigned long long>(keysHash), isFull ? "full" : "tail", added);
    }
    return total;
}

void RegisterWithScanHub() {
    static bool sDone = false;
    if (sDone) return;
    sDone = true;
    coop::element::scan_hub::Register(coop::element::scan_hub::Consumer{
        "keypad", nullptr, &PL::EnsureResolved, &PL::IsPasswordLock,
        &HubPassBegin, &HubMatch, &HubPassComplete, /*settleScans*/ 15});
}

// The receiver apply: the typed buffer is driven to `want` by replaying the digit delta through
// inputNumber (native display and beep), which also runs the keypad's own validator, so the
// host accepts a client's correct code. Never a submit verb, never a hover-flag write. Primes
// lastKnown, so this peer's poll never echoes it.
void ApplyState(void* actor, const std::wstring& key, const State& want, unsigned fromSlot) {
    State cur;
    if (!PL::ReadState(actor, cur)) return;

    // The buffer reconcile.
    if (cur.buffer != want.buffer) {
        const bool append = want.buffer.size() >= cur.buffer.size() &&
                            want.buffer.compare(0, cur.buffer.size(), cur.buffer) == 0;
        if (!append) {
            // Diverged or shrank (a cancel, a post-submit clear, a backspace): the buffer is
            // cleared and retyped. ClearBuffer is a direct length-zero write, not the BP's Reset
            // verb, which is the set-a-new-code mode (a blue LED).
            PL::ClearBuffer(actor);
            for (wchar_t c : want.buffer)
                if (c >= L'0' && c <= L'9') PL::CallInputNumber(actor, static_cast<int32_t>(c - L'0'));
        } else {
            // A pure append: only the new digits are replayed.
            for (size_t i = cur.buffer.size(); i < want.buffer.size(); ++i) {
                wchar_t c = want.buffer[i];
                if (c >= L'0' && c <= L'9') PL::CallInputNumber(actor, static_cast<int32_t>(c - L'0'));
            }
        }
    }
    // `active` (the LED selector and door power), re-read after the buffer reconcile: replaying a
    // code can fire the keypad's own validator, which sets it itself, so only the explicit cancel
    // (no digit typed) still diverges. Closed with a direct field write (never the setActive verb)
    // plus the same value on the gated door's power, so keypad.active equals door.active as in
    // single-player.
    State after;
    if (PL::ReadState(actor, after) && after.active != want.active) {
        // Host authority: keypad.active is building power, propagated to the gated door (whose
        // E-press opens iff Active and neither jammed nor superClosed), so the host never takes its
        // power or door lock from a client packet; a client's cancel, wrong code or save-transfer
        // transient carrying active=0 would de-power the host's door. Only a client mirrors the
        // host's value. The host's power changes solely from its own Open, driven by the replayed
        // digits or a replayed press event, and it then broadcasts the result.
        auto* s = g_session.load(std::memory_order_acquire);
        if (s && s->role() == coop::net::Role::Client) {
            PL::WriteActive(actor, want.active);
            if (void* door = PL::GatedDoor(actor)) ue_wrap::door::SetActive(door, want.active);
        }
    }
    // Repaint the digit display and the LED: upd re-selects the particle template from the freshly
    // written `active`.
    PL::CallUpd(actor);

    { std::lock_guard<std::mutex> lk(g_mutex); g_lastKnown[key] = want; }
    UE_LOGI("keypad: applied key='%ls' buf='%ls' active=%d (from slot %u)",
            key.c_str(), want.buffer.c_str(), want.active ? 1 : 0, fromSlot);
}

// The sender: polls every indexed keypad and broadcasts deltas; the first sighting primes
// silently (initial divergence is the connect snapshot's job), and ApplyState primes lastKnown
// to the applied value, so an echo never shows. The delta is classified into a KeypadEvent:
// Accept when active flipped on and the last buffer was a short code (under five digits, so
// the native Open(true) chain just completed from a local accept press; at five the replay
// already ran the auto-submit on every peer), Deny when the buffer shrank to empty with active
// off and the last buffer was a short code (a wrong-code press or the cancel). Both gated on
// not being in set-new-code mode. No door drive: a native accept unlocks the door and never
// opens it; opening it is an E-press the door channel syncs.
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
            // A dead or streamed-out keypad can never land its chain, so its settling entry is
            // dropped; lastKnown keeps the endpoint, and a re-streamed actor converges through the
            // normal delta path.
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
            // A replayed Open chain in flight on this key: its writes land frames later, so any
            // state read before the endpoint is a transient (broadcast, it is the poison packet;
            // classified, the phantom Accept). The key is suppressed until it settles; a delta that
            // remains at settle-erase came from an interleaved apply, so it converges but is never
            // classified.
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
        // Classify the delta. Reads on the live actor are outside the mutex; engine access is never
        // under our lock.
        coop::net::KeypadEvent ev = coop::net::KeypadEvent::None;
        const bool shortCode = !last.buffer.empty() && last.buffer.size() < 5;
        if (classify && !PL::IsResetMode(r.second.actor)) {
            if (shortCode) {
                if (!last.active && cur.active) {
                    ev = coop::net::KeypadEvent::Accept;
                } else if (cur.buffer.empty() && !cur.active) {
                    ev = coop::net::KeypadEvent::Deny;
                }
            } else if (last.buffer.empty() && cur.buffer.empty() &&
                       last.active && !cur.active && PL::IsPressHover(r.second.actor)) {
                // The empty-buffer cancel press: the red button with nothing typed runs open(false)
                // natively, and active 1 to 0 is the only delta, invisible to the short-code arms
                // above. The discriminator is the look-at hover: active flipped off while the
                // crosshair sits on a submit button is a press (the BP's own press routing keys on
                // the same flags). The flags stick after the crosshair leaves the keypad, which
                // suits Open's latent landing; they clear only when the crosshair moves to a
                // non-button part of the same keypad, a narrow miss that falls back to the plain
                // state packet. The inverse edge (an ambient power loss with a stale hover flag)
                // mis-stamps a Deny that replays open(false) on an already inactive keypad: a beep,
                // and the state converges.
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

// The incoming dispatch: a stamped Accept or Deny runs the keypad's own submit chain
// (CallOpen); a plain packet takes ApplyState. The echo-break is the endpoint prime and the
// settle mark: Open's writes land frames later, but the settled state is deterministic ({'',
// Active}: the param is the verdict), so lastKnown is primed to it and the poll skips the key
// until the keypad reads it, after which cur equals lastKnown and nothing is sent. The chain
// does the LED, the buffer clear and the pair and door lock propagation natively.
void ApplyIncoming(void* actor, const std::wstring& key, const State& want,
                   coop::net::KeypadEvent ev, unsigned fromSlot) {
    if (ev == coop::net::KeypadEvent::None) { ApplyState(actor, key, want, fromSlot); return; }
    if (PL::IsResetMode(actor)) {
        UE_LOGW("keypad: dropping ev=%u for key='%ls' -- keypad is in set-new-code mode",
                static_cast<unsigned>(ev), key.c_str());
        return;
    }
    const bool accept = (ev == coop::net::KeypadEvent::Accept);
    // Every peer, the host included, replays a stamped event natively: an event is a deliberate
    // press, an input like the digits, so the host runs its own Open chain (a client's red button
    // locks the shared door; a wrong-code deny re-locks it). The other clients converge from the
    // host's relay of the event packet, each replaying the same chain, not from a poll rebroadcast,
    // since a chain landing on its endpoint sends nothing. A save-transfer transient carrying
    // active=0 is a plain packet, and ApplyState's power write stays host-skipped.
    if (!PL::CallOpen(actor, accept)) {
        // The degraded fallback (Open unresolved): the end state mirrored directly.
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
    RegisterWithScanHub();  // the hub builds the index on its own cadence
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
    // Not streamed in yet: deferred, retried on the throttled tick.
    std::lock_guard<std::mutex> lk(g_mutex);
    g_pending[key] = Pending{ std::move(want), ev, std::chrono::steady_clock::now() + kPendingTTL };
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;  // host-only snapshot
    if (peerSlot < 0 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    // The hub keeps the index within one pass; keypads are static level actors on the host's
    // long-loaded world at join time.
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
            // A key mid-settle snapshots the chain's endpoint, not the live keypad: the live read
            // is a transient the joiner would mirror, and writing it to lastKnown would clobber the
            // prime.
            std::lock_guard<std::mutex> lk(g_mutex);
            auto sIt = g_settling.find(d.first);
            if (sIt != g_settling.end()) cur = sIt->second.endpoint;
        }
        coop::net::KeypadSyncPayload p{};
        // The snapshot is plain state: the joiner mirrors the result (the LED, the typed digits);
        // the door's own snapshot carries the door.
        StateToPayload(d.first, cur, coop::net::KeypadEvent::None, p);
        s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::KeypadState, &p, sizeof(p));
        { std::lock_guard<std::mutex> lk(g_mutex); g_lastKnown[d.first] = cur; }
        ++sent;
    }
    UE_LOGI("keypad: connect-snapshot -- sent %d state(s) to slot %d (of %zu indexed)", sent, peerSlot, items.size());
}

void Tick() {
    if (!PL::EnsureResolved()) return;
    RegisterWithScanHub();  // safety net for any order where Tick precedes Install
    if (!IndexCurrent()) return;  // index belongs to a dead world -- wait for the hub's next pass

    const auto now = std::chrono::steady_clock::now();
    if (now - g_lastRetry >= kRetryRebuildThrottle) {
        g_lastRetry = now;
        // Retry deferred applies for keypads that have streamed in since.
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
