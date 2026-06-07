// coop/keypad_sync.cpp -- see coop/keypad_sync.h. Password-keypad (ApasswordLock_C) INPUT
// mirror: poll inPassword -> broadcast on a buffer change; receiver replays inputNumber for
// the digit delta (which drives the keypad's own native validator -- MTA input-replication).
// No isAcc/isDeny mirror (removed 2026-06-06: hover flags, the old PURPLE).
//
// Structure borrows the proven interactable_sync Channel patterns (key->actor index
// with IsLiveByIndex self-heal, throttled rebuild, deferred-apply retry, silent first-
// sight prime, echo-suppress via priming lastKnown_ to the applied value) but with a
// keypad-shaped state (a typed buffer + 3 bools) and an input-replay apply, which is why
// it is a separate module rather than another toggle Adapter (RULE 2: forcing it into the
// toggle Channel is what produced the v31 fail-cycle).

#include "coop/keypad_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/players_registry.h"  // coop::players::kMaxPeers

#include "ue_wrap/door.h"          // host-authoritative accept drives the gated door open
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/passwordlock.h"
#include "ue_wrap/reflection.h"

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
std::unordered_map<std::wstring, bool>  g_accepted;   // GT-only (HOST): key -> accept latched this code-entry (un-latched when the buffer no longer matches)
struct Pending { State want; std::chrono::steady_clock::time_point deadline; };
std::unordered_map<std::wstring, Pending> g_pending;  // key -> deferred incoming apply

std::chrono::steady_clock::time_point g_lastRetry{};
size_t g_lastLogCount = SIZE_MAX;
uint64_t g_lastLogHash = 0;
bool g_indexed = false;
std::vector<std::pair<std::wstring, Ref>> g_pollScratch;  // GT-only: reused per-tick poll snapshot (no per-tick heap alloc)

// ---- WireKey <-> wstring (keypad Keys are ASCII FNames) -------------------------
void WireKeyFromString(const std::wstring& key, coop::net::WireKey& out) {
    std::memset(&out, 0, sizeof(out));
    size_t n = key.size();
    if (n > sizeof(out.data)) n = sizeof(out.data);  // 31
    for (size_t i = 0; i < n; ++i) out.data[i] = static_cast<char>(key[i] & 0xFF);
    out.len = static_cast<uint8_t>(n);
}
std::wstring StringFromWireKey(const coop::net::WireKey& k) {
    size_t n = k.len;
    if (n > sizeof(k.data)) n = sizeof(k.data);
    std::wstring out; out.reserve(n);
    for (size_t i = 0; i < n; ++i) out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(k.data[i])));
    return out;
}
uint64_t FnvKey(const std::wstring& s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (wchar_t c : s) { h ^= static_cast<uint8_t>(c & 0xFF); h *= 0x100000001b3ULL; }
    return h;
}

bool SameState(const State& a, const State& b) {
    return a.buffer == b.buffer && a.active == b.active;
}

// State <-> payload. The buffer is digits only (keypad input is 0..9); a non-digit (never
// expected) is dropped so the wire stays digit-clean.
void StateToPayload(const std::wstring& key, const State& st, coop::net::KeypadSyncPayload& p) {
    std::memset(&p, 0, sizeof(p));
    WireKeyFromString(key, p.key);
    uint8_t n = 0;
    for (wchar_t c : st.buffer) {
        if (n >= sizeof(p.buf)) break;
        if (c >= L'0' && c <= L'9') p.buf[n++] = static_cast<uint8_t>(c - L'0');
    }
    p.bufLen = n;
    p.active = st.active ? 1 : 0;  // v38: LED selector / door power (cancel -> red)
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
    std::vector<std::pair<std::wstring, Ref>> found;
    found.reserve(32);
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
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
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_index.clear();
        for (auto& f : found) { keysHash ^= FnvKey(f.first); g_index[f.first] = f.second; }
    }
    if (found.size() != g_lastLogCount || keysHash != g_lastLogHash) {
        g_lastLogCount = found.size();
        g_lastLogHash = keysHash;
        UE_LOGI("keypad: index rebuilt -- %zu live keyed keypad(s), keysHash=0x%016llX "
                "(compare host vs client for cross-peer Key stability)",
                found.size(), static_cast<unsigned long long>(keysHash));
    }
    return found.size();
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
            // diverged / shrank (post-submit clear, backspace) -> clear then retype
            PL::CallReset(actor);
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
        PL::WriteActive(actor, want.active);
        if (void* door = PL::GatedDoor(actor)) ue_wrap::door::SetActive(door, want.active);
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
        if (!R::IsLiveByIndex(r.second.actor, r.second.idx)) continue;
        State cur;
        if (!PL::ReadState(r.second.actor, cur)) continue;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            auto it = g_lastKnown.find(r.first);
            if (it == g_lastKnown.end()) { g_lastKnown[r.first] = cur; continue; }  // prime silently
            if (SameState(it->second, cur)) continue;                                // no change
        }
        coop::net::KeypadSyncPayload p{};
        StateToPayload(r.first, cur, p);
        if (s->SendReliable(coop::net::ReliableKind::KeypadState, &p, sizeof(p))) {
            { std::lock_guard<std::mutex> lk(g_mutex); g_lastKnown[r.first] = cur; }
            UE_LOGI("keypad: sent key='%ls' buf='%ls'", r.first.c_str(), cur.buffer.c_str());
        } else {
            UE_LOGW("keypad: SendReliable failed key='%ls'", r.first.c_str());
        }
    }
}

// HOST-only Phase B: the authoritative keypad ACCEPT. The disassembly proved a correct code runs
// `open(password==inPassword)` (when Len>=5) whose chain ends at `door.doorOpen(false)` -- but that
// native open needs the door near the host player to animate (it freezes far away), and runs through
// fragile latent delays. So the host evaluates the SAME condition against its OWN `password` (never
// replicated) and drives the gated door itself: SetActive(true) (the unlock CanOpen gates on) +
// ForceOpen (snaps isOpened reliably at any distance). The door channel's host poll then broadcasts
// the open to every peer. The keypad's native chain (fired by the replayed inputNumber for display)
// does the same idempotently (ForceOpen no-ops an already-open door). Latched per keypad until the
// buffer no longer matches, so it fires once per code-entry. Runs every tick over the (~14) indexed
// keypads -- a handful of bool/FString reads; only the host does the door writes.
void HostAcceptPoll() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    if (!ue_wrap::door::EnsureResolved()) return;
    // Snapshot the index refs (reuse the GT-serial scratch -- PollAndBroadcast already finished with
    // it this tick) so we never hold g_mutex across the engine reads / door UFunction calls below.
    auto& refs = g_pollScratch;
    refs.clear();
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (g_index.empty()) return;
        refs.reserve(g_index.size());
        for (auto& kv : g_index) refs.emplace_back(kv.first, kv.second);
    }
    // Un-latch ONLY a currently-latched key -- never insert/rewrite an entry for an idle keypad
    // (operator[]=false would write all ~14 keys every tick). Keeps g_accepted to the few keys
    // that actually accepted.
    auto unlatch = [](const std::wstring& key) {
        auto it = g_accepted.find(key);
        if (it != g_accepted.end() && it->second) it->second = false;
    };
    for (auto& r : refs) {
        void* lock = r.second.actor;
        if (!R::IsLiveByIndex(lock, r.second.idx)) continue;
        State cur;
        if (!PL::ReadState(lock, cur)) continue;
        // Short-circuit on the BP's own length gate (Len(inPassword)>=5) BEFORE the password /
        // isReset reads -- on the common idle keypad (empty buffer) this is the only work done.
        if (cur.buffer.size() < 5) { unlatch(r.first); continue; }  // too short -> un-latch, skip
        const std::wstring pw = PL::ReadPassword(lock);
        // accept = a full buffer equal to the password, and NOT in set-new-code mode (isReset
        // writes the password instead of opening -- skip the auto-open for it).
        const bool accept = !pw.empty() && cur.buffer == pw && !PL::IsResetMode(lock);
        if (!accept) { unlatch(r.first); continue; }  // wrong/partial code -> un-latch
        if (g_accepted[r.first]) continue;            // already handled this code-entry
        g_accepted[r.first] = true;
        void* door = PL::GatedDoor(lock);
        if (!door) { UE_LOGW("keypad: ACCEPT key='%ls' but no gated door resolved", r.first.c_str()); continue; }
        ue_wrap::door::SetActive(door, true);        // unlock (CanOpen) -- authoritative, independent of the native chain
        ue_wrap::door::ForceOpen(door);              // open reliably regardless of host-player distance (idempotent if already open)
        // Mute the host's autoclose + sensor for this door (same proven recipe as a client-held door,
        // door.h SuppressHostHeldDoor): the host has NO player at the keypad's door (only the typing
        // client's puppet), so its native checkSensor would find an empty sensor + autoclose and shut
        // the door it just opened -> open/close oscillation. The door stays open (the keypad unlock
        // persists, like SP's door.Active=true). NOTE (Phase B scope): the keypad door is not yet
        // registered in the door channel's hold register, so it is not E-closable and won't auto-close
        // after walk-through -- a follow-up integrates it with the hold register for those nuances.
        ue_wrap::door::SuppressHostHeldDoor(door);
        UE_LOGI("keypad: ACCEPT key='%ls' -- correct code, unlocked + opened the gated door (host-authoritative)", r.first.c_str());
    }
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
    if (void* actor = ResolveFast(key)) { ApplyState(actor, key, want, senderPeerSlot); return; }
    // Not streamed in yet -- defer + retry on the throttled tick.
    std::lock_guard<std::mutex> lk(g_mutex);
    g_pending[key] = Pending{ std::move(want), std::chrono::steady_clock::now() + kPendingTTL };
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
        coop::net::KeypadSyncPayload p{};
        StateToPayload(d.first, cur, p);
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
        RebuildIndex();
        // RECEIVER: retry deferred applies for keypads that have now streamed in.
        std::vector<std::pair<std::wstring, State>> ready;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            for (auto it = g_pending.begin(); it != g_pending.end();) {
                auto idxIt = g_index.find(it->first);
                if (idxIt != g_index.end() && R::IsLiveByIndex(idxIt->second.actor, idxIt->second.idx)) {
                    ready.emplace_back(it->first, it->second.want);
                    it = g_pending.erase(it);
                } else if (now >= it->second.deadline) {
                    it = g_pending.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& rdy : ready)
            if (void* actor = ResolveFast(rdy.first)) ApplyState(actor, rdy.first, rdy.second, 0xFF);
    }
    PollAndBroadcast();
    HostAcceptPoll();   // HOST: drive the gated door open on a correct code (Phase B)
}

void OnDisconnect() {
    std::lock_guard<std::mutex> lk(g_mutex);
    const size_t n = g_lastKnown.size();
    g_lastKnown.clear();
    g_pending.clear();
    g_accepted.clear();
    if (n > 0) UE_LOGI("keypad: OnDisconnect cleared %zu last-known", n);
}

}  // namespace coop::keypad_sync
