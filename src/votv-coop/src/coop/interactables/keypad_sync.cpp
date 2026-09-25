// coop/interactables/keypad_sync.cpp -- see coop/interactables/keypad_sync.h.

#include "coop/interactables/keypad_sync.h"

#include "coop/element/object_scan_hub.h"   // the shared sliced scan pass
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"          // WireKeyFromString / StringFromWireKey / FnvKey
#include "coop/player/players_registry.h"    // coop::players::kMaxPeers

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/passwordlock.h"
#include "ue_wrap/engine/world_identity.h"   // the world generation the index is stamped with

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop::keypad_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace PL = ue_wrap::passwordlock;
using Clock = std::chrono::steady_clock;

constexpr auto kRetryThrottle = std::chrono::seconds(1);
constexpr auto kPendingTTL = std::chrono::seconds(25);

std::atomic<coop::net::Session*> g_session{nullptr};

// The index, game thread only: every keyed keypad of the current world, found by the scan hub.
struct Entry { std::wstring key; void* actor; int32_t idx; };
std::vector<Entry> g_index;
uint32_t g_indexGen = 0;   // the world generation of the last completed pass
size_t   g_lastLogCount = SIZE_MAX;
uint64_t g_lastLogHash = 0;
std::vector<Entry> g_scanFound;   // one pass's matches

bool IndexCurrent() { return g_indexGen == ue_wrap::world_identity::Generation(); }

// A state that arrived before its keypad was indexed: retried until the TTL.
struct Pending { coop::net::KeypadSyncPayload p; Clock::time_point deadline; };
std::unordered_map<std::wstring, Pending> g_pending;
Clock::time_point g_nextRetry{};

// A state that arrived while this copy's own replayed open was in its 0.2 s wait: the host sends
// its state after its tail, which can land before this copy's tail has read `isReset`. It is written
// at this copy's own chain end instead, the tail's setActive (OnClientChainEnd).
std::unordered_map<std::wstring, coop::net::KeypadSyncPayload> g_parked;

// Joiners whose snapshot waited for the index to be current, one bit a slot.
uint32_t g_snapshotOwed = 0;

// The verb the lane is running on a keypad right now.
struct Mark { void* lock = nullptr; Verb verb = Verb::SetActive; };
Mark g_mark;
struct MarkScope {
    Mark saved;
    MarkScope(void* lock, Verb verb) : saved(g_mark) { g_mark = Mark{lock, verb}; }
    ~MarkScope() { g_mark = saved; }
};

uint64_t g_sentEvents = 0, g_sentStates = 0, g_applied = 0, g_dropped = 0;

const char* EventName(uint8_t ev) {
    switch (static_cast<coop::net::KeypadEvent>(ev)) {
    case coop::net::KeypadEvent::State:      return "state";
    case coop::net::KeypadEvent::Digit:      return "digit";
    case coop::net::KeypadEvent::Open:       return "open";
    case coop::net::KeypadEvent::Guesser:    return "guesser";
    case coop::net::KeypadEvent::Reset:      return "reset";
    case coop::net::KeypadEvent::FalseEntry: return "false entry";
    }
    return "?";
}

uint8_t PackDigits(const std::wstring& s, uint8_t* out, size_t cap) {
    uint8_t n = 0;
    for (wchar_t c : s) {
        if (n >= cap) break;
        if (c >= L'0' && c <= L'9') out[n++] = static_cast<uint8_t>(c - L'0');
    }
    return n;
}

std::wstring UnpackDigits(const uint8_t* in, uint8_t n, size_t cap) {
    std::wstring s;
    for (uint8_t i = 0; i < n && i < cap; ++i) s.push_back(static_cast<wchar_t>(L'0' + in[i]));
    return s;
}

void StateToPayload(const std::wstring& key, const PL::State& st, coop::net::KeypadSyncPayload& p) {
    coop::net::WireKeyFromString(key, p.key);
    p.bufLen  = PackDigits(st.buffer, p.buf, sizeof(p.buf));
    p.pwLen   = PackDigits(st.password, p.pw, sizeof(p.pw));
    p.active  = st.active ? 1 : 0;
    p.isReset = st.isReset ? 1 : 0;
}

std::wstring BufferOf(const coop::net::KeypadSyncPayload& p) { return UnpackDigits(p.buf, p.bufLen, sizeof(p.buf)); }

// ---- the index ---------------------------------------------------------------------------------
void HubPassBegin(void*, bool /*isFull*/) { g_scanFound.clear(); }

void HubMatch(void*, void* obj) {
    if (R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
    if (!R::IsLive(obj)) return;
    std::wstring key = PL::GetKeyString(obj);
    if (key.empty() || key == L"None") return;  // an unkeyed template is no placed keypad
    g_scanFound.push_back(Entry{std::move(key), obj, R::InternalIndexOf(obj)});
}

size_t HubPassComplete(void*, bool isFull, uint32_t worldGen) {
    const size_t added = g_scanFound.size();
    if (isFull) {
        g_index.swap(g_scanFound);
    } else {
        // A tail pass adds the births and drops the dead.
        for (Entry& e : g_scanFound) {
            bool known = false;
            for (Entry& have : g_index)
                if (have.key == e.key) { have = e; known = true; break; }
            if (!known) g_index.push_back(std::move(e));
        }
        for (size_t i = 0; i < g_index.size();) {
            if (R::IsLiveByIndex(g_index[i].actor, g_index[i].idx)) { ++i; continue; }
            g_index[i] = std::move(g_index.back());
            g_index.pop_back();
        }
    }
    g_scanFound.clear();
    g_indexGen = worldGen;
    uint64_t keysHash = 0;
    for (const Entry& e : g_index) keysHash ^= coop::net::FnvKey(e.key);
    if (g_index.size() != g_lastLogCount || keysHash != g_lastLogHash) {
        g_lastLogCount = g_index.size();
        g_lastLogHash = keysHash;
        UE_LOGI("keypad: index rebuilt -- %zu live keyed keypad(s), keysHash=0x%016llX (%s pass, +%zu new) "
                "(compare host vs client for cross-peer Key stability)",
                g_index.size(), static_cast<unsigned long long>(keysHash), isFull ? "full" : "tail", added);
    }
    return g_index.size();
}

void RegisterWithScanHub() {
    static bool s_done = false;
    if (s_done) return;
    s_done = true;
    coop::element::scan_hub::Register(coop::element::scan_hub::Consumer{
        "keypad", nullptr, &PL::EnsureResolved, &PL::IsPasswordLock,
        &HubPassBegin, &HubMatch, &HubPassComplete, /*settleScans*/ 15});
}

// ---- the client's apply ------------------------------------------------------------------------
// The settled state, written whole -- the buffer, the password, the verdict and the mode -- then
// setActive(false), which repaints and hands the power on to the pair and the gated door.
void ApplyStateNow(void* lock, const coop::net::KeypadSyncPayload& p) {
    PL::State cur;
    if (!PL::ReadState(lock, cur)) return;
    const std::wstring buffer = BufferOf(p);
    const std::wstring password = UnpackDigits(p.pw, p.pwLen, sizeof(p.pw));
    if (cur.buffer != buffer) PL::WriteBuffer(lock, buffer);
    if (cur.password != password) PL::WritePassword(lock, password);
    PL::WriteActive(lock, p.active != 0);
    PL::WriteResetMode(lock, p.isReset != 0);
    MarkScope mark(lock, Verb::SetActive);
    PL::CallSetActive(lock, false);
}

void ApplyState(void* lock, const std::wstring& key, const coop::net::KeypadSyncPayload& p) {
    if (PL::IsEntering(lock)) {
        g_parked[key] = p;
        return;
    }
    g_parked.erase(key);
    ApplyStateNow(lock, p);
}

void Apply(void* lock, const std::wstring& key, const coop::net::KeypadSyncPayload& p) {
    bool ok = false;
    switch (static_cast<coop::net::KeypadEvent>(p.event)) {
    case coop::net::KeypadEvent::State:
        ApplyState(lock, key, p);
        ok = true;
        break;
    case coop::net::KeypadEvent::Digit: {
        MarkScope mark(lock, Verb::InputNumber);
        ok = PL::CallInputNumber(lock, p.arg);
        break;
    }
    case coop::net::KeypadEvent::Open: {
        MarkScope mark(lock, Verb::Open);
        ok = PL::CallOpen(lock, p.arg != 0);
        break;
    }
    case coop::net::KeypadEvent::Guesser: {
        MarkScope mark(lock, Verb::Open2);
        ok = PL::CallOpen2(lock);
        break;
    }
    case coop::net::KeypadEvent::Reset: {
        MarkScope mark(lock, Verb::Reset);
        ok = PL::CallReset(lock);
        break;
    }
    case coop::net::KeypadEvent::FalseEntry: {
        MarkScope mark(lock, Verb::FalseEnter);
        ok = PL::CallFalseEnter(lock);
        break;
    }
    }
    ++g_applied;
    // Digits are said for the first few keypads' worth; everything else, and every failure, is said.
    if (!ok || p.event != static_cast<uint8_t>(coop::net::KeypadEvent::Digit) || g_applied <= 20)
        UE_LOGI("keypad: applied the host's %s on key='%ls' (arg=%u; the host's buf='%ls' active=%u reset=%u)%s",
                EventName(p.event), key.c_str(), static_cast<unsigned>(p.arg), BufferOf(p).c_str(),
                static_cast<unsigned>(p.active), static_cast<unsigned>(p.isReset), ok ? "" : " -- FAILED to dispatch");
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    RegisterWithScanHub();
}

void Tick() {
    if (!PL::EnsureResolved()) return;
    RegisterWithScanHub();
    if (g_snapshotOwed && IndexCurrent()) {
        const uint32_t owed = g_snapshotOwed;
        g_snapshotOwed = 0;
        for (int slot = 0; slot < static_cast<int>(coop::players::kMaxPeers); ++slot)
            if (owed & (1u << slot)) QueueConnectBroadcastForSlot(slot);
    }
    if (g_pending.empty() || !IndexCurrent()) return;
    const Clock::time_point now = Clock::now();
    if (now < g_nextRetry) return;
    g_nextRetry = now + kRetryThrottle;
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        if (void* lock = ResolveKeypad(it->first)) {
            Apply(lock, it->first, it->second.p);
            it = g_pending.erase(it);
        } else if (now >= it->second.deadline) {
            ++g_dropped;
            UE_LOGW("keypad: the host's state for key='%ls' expired -- no keypad by that key here",
                    it->first.c_str());
            it = g_pending.erase(it);
        } else {
            ++it;
        }
    }
}

void OnReliable(const coop::net::KeypadSyncPayload& payload) {
    if (!PL::EnsureResolved()) return;
    const std::wstring key = coop::net::StringFromWireKey(payload.key);
    if (key.empty()) return;
    if (void* lock = ResolveKeypad(key)) {
        g_pending.erase(key);  // a state parked before the index is older than this record
        Apply(lock, key, payload);
        return;
    }
    // A verb for a keypad not indexed yet is dropped: the state its chain settles on follows it,
    // and a state waits here until the keypad is indexed.
    if (payload.event == static_cast<uint8_t>(coop::net::KeypadEvent::State))
        g_pending[key] = Pending{payload, Clock::now() + kPendingTTL};
    else
        ++g_dropped;
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    if (peerSlot < 0 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    if (!IndexCurrent()) {
        // Mid-transition: owed, and sent by Tick once the hub has a current pass.
        g_snapshotOwed |= 1u << peerSlot;
        UE_LOGI("keypad: connect-snapshot for slot %d waits for a current index", peerSlot);
        return;
    }
    int sent = 0;
    for (const Entry& e : g_index) {
        if (!R::IsLiveByIndex(e.actor, e.idx)) continue;
        PL::State st;
        if (!PL::ReadState(e.actor, st)) continue;
        coop::net::KeypadSyncPayload p{};
        StateToPayload(e.key, st, p);
        p.event = static_cast<uint8_t>(coop::net::KeypadEvent::State);
        if (s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::KeypadState, &p, sizeof(p))) ++sent;
    }
    UE_LOGI("keypad: connect-snapshot -- sent %d state(s) to slot %d (of %zu indexed)", sent, peerSlot,
            g_index.size());
}

void OnDisconnect() {
    if (g_sentEvents || g_sentStates || g_applied || g_dropped)
        UE_LOGI("keypad: session end -- host sent %llu verb(s) and %llu state(s); applied %llu, dropped %llu",
                static_cast<unsigned long long>(g_sentEvents), static_cast<unsigned long long>(g_sentStates),
                static_cast<unsigned long long>(g_applied), static_cast<unsigned long long>(g_dropped));
    g_sentEvents = g_sentStates = g_applied = g_dropped = 0;
    g_pending.clear();
    g_parked.clear();
    g_snapshotOwed = 0;
    g_mark = Mark{};
}

std::wstring KeypadKey(void* lock) {
    if (!lock || !IndexCurrent()) return std::wstring();
    for (const Entry& e : g_index)
        if (e.actor == lock && R::IsLiveByIndex(e.actor, e.idx)) return e.key;
    return std::wstring();
}

void* ResolveKeypad(const std::wstring& key) {
    if (key.empty() || !IndexCurrent()) return nullptr;
    for (const Entry& e : g_index)
        if (e.key == key && R::IsLiveByIndex(e.actor, e.idx)) return e.actor;
    return nullptr;
}

void OnClientChainEnd(void* lock) {
    if (g_parked.empty()) return;
    const std::wstring key = KeypadKey(lock);
    auto it = g_parked.find(key);
    if (it == g_parked.end()) return;
    const coop::net::KeypadSyncPayload p = it->second;
    g_parked.erase(it);
    ApplyStateNow(lock, p);
}

bool Applying(void* lock, Verb verb) { return lock && g_mark.lock == lock && g_mark.verb == verb; }
bool ApplyingAny(void* lock) { return lock && g_mark.lock == lock; }

void SendEvent(void* lock, coop::net::KeypadEvent event, uint8_t arg) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    const std::wstring key = KeypadKey(lock);
    if (key.empty()) return;
    PL::State st;
    if (!PL::ReadState(lock, st)) return;
    coop::net::KeypadSyncPayload p{};
    StateToPayload(key, st, p);
    p.event = static_cast<uint8_t>(event);
    p.arg = arg;
    if (!s->SendReliable(coop::net::ReliableKind::KeypadState, &p, sizeof(p))) {
        UE_LOGW("keypad: the %s on key='%ls' was not sent (the session refused it)", EventName(p.event), key.c_str());
        return;
    }
    ++g_sentEvents;
    if (event != coop::net::KeypadEvent::Digit || g_sentEvents <= 20)
        UE_LOGI("keypad: host sent its %s on key='%ls' (arg=%u)", EventName(p.event), key.c_str(),
                static_cast<unsigned>(arg));
}

void SendState(void* lock) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    const std::wstring key = KeypadKey(lock);
    if (key.empty()) return;
    PL::State st;
    if (!PL::ReadState(lock, st)) return;
    coop::net::KeypadSyncPayload p{};
    StateToPayload(key, st, p);
    p.event = static_cast<uint8_t>(coop::net::KeypadEvent::State);
    if (!s->SendReliable(coop::net::ReliableKind::KeypadState, &p, sizeof(p))) {
        UE_LOGW("keypad: the state of key='%ls' was not sent (the session refused it)", key.c_str());
        return;
    }
    ++g_sentStates;
    UE_LOGI("keypad: host sent the state key='%ls' settled on: buf='%ls' active=%d reset=%d", key.c_str(),
            st.buffer.c_str(), st.active ? 1 : 0, st.isReset ? 1 : 0);
}

}  // namespace coop::keypad_sync
