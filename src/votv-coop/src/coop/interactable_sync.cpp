// coop/interactable_sync.cpp -- see coop/interactable_sync.h. One generic Channel
// drives door / light-switch / container-lid state sync.
//
// The Channel holds all the SHARED machinery (key->actor index with IsLiveByIndex
// self-heal, per-key dedup, deferred-apply + throttled retry, echo-suppress,
// connect-snapshot). Each feature is just an Adapter (a vtable over its ue_wrap
// engine wrapper) + a file-static Channel instance + a POST-observer thunk. The
// only per-feature engine specifics (which class, which Key offset, which
// open/close UFunction) live behind the Adapter, so adding a fourth interactable
// later is an adapter + a few lines, not another 350-line copy.

#include "coop/interactable_sync.h"

#include "coop/ini_config.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/players_registry.h"  // coop::players::kMaxPeers

#include "ue_wrap/door.h"
#include "ue_wrap/engine.h"        // ReadMainPlayerLookAtActor (the E-press door target)
#include "ue_wrap/game_thread.h"
#include "ue_wrap/lightswitch.h"
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"          // GetKeyString for swinger (it is an Aprop_C)
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"   // MainPlayerClass + the InpActEvt_use input-action fn
#include "ue_wrap/swinger.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop::interactable_sync {
namespace {

namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace P = ue_wrap::profile;

constexpr auto kRetryRebuildThrottle = std::chrono::seconds(2);
constexpr auto kPendingTTL = std::chrono::seconds(25);
// After the host commands a door close, isOpened flips ~0.5s later (the door animates).
// The poll skips the door for this bridge window so the mid-animation transient isn't
// re-broadcast as a flicker; once it expires the poll resumes and broadcasts the real
// settled state (so a host-driven re-open during the window still propagates -- no desync).
constexpr auto kDoorSettleBridge = std::chrono::milliseconds(1500);

bool ProbeLog() {
    static const bool s_enabled = ::coop::ini_config::IsIniKeyTrue("interactable_log");
    return s_enabled;
}

// ---- WireKey <-> wstring (interactable Keys are ASCII FNames) -------------
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
    std::wstring out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i)
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(k.data[i])));
    return out;
}
uint64_t FnvKey(const std::wstring& s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (wchar_t c : s) { h ^= static_cast<uint8_t>(c & 0xFF); h *= 0x100000001b3ULL; }
    return h;
}

// ---- Per-feature engine vtable -------------------------------------------
struct Adapter {
    const char* name;                          // "door" / "light" / "container"
    coop::net::ReliableKind kind;
    bool (*EnsureResolved)();
    bool (*IsInstance)(void* obj);             // class-descendant check
    std::wstring (*GetKey)(void* actor);       // cross-peer-stable Key string
    bool (*ReadState)(void* actor, bool& on);  // current open/on state
    bool (*ApplyState)(void* actor, bool on);  // drive to target (channel echo-suppresses)
    // --- HostAuth-mode only (doors); null/no-op for Symmetric channels ----------
    void (*SuppressAutonomy)(void* actor);     // CLIENT: mute local auto-revert so applied state sticks
    void (*RestoreAutonomy)(void* actor);      // restore authored autonomy at disconnect
    bool (*RequestApply)(void* actor, bool on);// HOST applies a client REQUEST honoring real guards (no bypass)
    void (*SuppressHeld)(void* actor);         // HOST: mute its OWN autoclose while a client holds this door open
    void (*ReleaseHeld)(void* actor);          // HOST: restore autoclose + close when the client releases the hold
    bool (*CanOpen)(void* actor);              // HostAuth: true if a manual E-press would open it now (Active && !jammed && !superClosed); null = always openable
};

// ---- The generic replication engine --------------------------------------
class Channel {
public:
    // Symmetric: every peer is authoritative over the changes it causes (lights /
    // containers / keypads -- no local auto-revert, so no fight). HostAuth: only the
    // HOST broadcasts state; the CLIENT renders it (autonomy suppressed) and routes its
    // own interactions to the host as DoorOpenRequest (doors -- their sensor/autoclose
    // re-drives the state each tick, so a symmetric poll oscillates). MTA single-syncer.
    enum class Mode { Symmetric, HostAuth };

    explicit Channel(const Adapter& a, Mode mode = Mode::Symmetric) : a_(a), mode_(mode) {}

    void SetSession(coop::net::Session* s) { session_.store(s, std::memory_order_release); }
    coop::net::Session* GetSession() const { return session_.load(std::memory_order_acquire); }
    // Prime the poll baseline for `key`.
    void PreUpdateLastKnown(const std::wstring& key, bool val) {
        std::lock_guard<std::mutex> lk(stateMutex_); lastKnown_[key] = val;
    }

    // SENDER: poll every indexed instance for a state change and broadcast deltas.
    // This REPLACES the old InpActEvt_use observer (RULE 2 -- no parallel senders).
    // Polling is the root-cause fix: it catches EVERY writer of the state -- player
    // E-press, NPC-proximity auto-open, keypad-unlock, scripts -- and can never miss a
    // BP-internal verb the way a POST observer does (the doors `sent=0` bug, IDA-proven
    // 2026-06-04: doorOpen / SetActive / Open all dispatch via CallFunction ->
    // ProcessInternal, bypassing our ProcessEvent detour). It is the weather-style
    // host-authoritative flag poll, generalized + made SYMMETRIC (each peer is
    // authoritative over the changes it causes; the host relays client edges). The first
    // sighting of a key primes the baseline SILENTLY -- initial divergence is the
    // connect-snapshot's job, so a fresh peer does not broadcast its whole world; only
    // genuine LIVE changes hit the wire. Echo is impossible: ApplyResolved updates
    // lastKnown_ to the applied value, so the next poll sees no delta. Game thread (the
    // net-pump Tick asserts GT) -- reading the bool fields + SendReliable are GT-safe.
    void PollAndBroadcast() {
        if (echo_.load(std::memory_order_acquire)) return;  // mid-apply -- belt-and-suspenders
        auto* s = session_.load(std::memory_order_acquire);
        // Gate on connected(): while a host is solo (no client yet, or its last client
        // left) there is nobody to send to. We deliberately do NOT advance lastKnown_
        // here -- OnDisconnect clears the whole map when all peers leave, and the
        // connect-snapshot re-primes every key on the next join, so no stale baseline can
        // survive to cause a spurious reconnect edge. Returning early (vs advancing the
        // baseline) is also the safer choice against a transient blip: the change is
        // re-detected and sent once connected, never silently dropped.
        if (!s || !s->connected()) return;
        // HostAuth (doors): the CLIENT NEVER poll-broadcasts. A client door is purely render-
        // only (sensor off + autoclose off); its isOpened only changes when WE apply a host
        // DoorState, so a client poll would just re-report the host's own commands back as
        // "requests" -- the exact feedback storm that made doors oscillate. The MTA shape is
        // that the non-authority sends INPUT EDGES, never entity state: the client's sole
        // door->host signal is the explicit player_use E-press (OnDoorPlayerUse observer).
        // So for a HostAuth client the poll does nothing. The HOST polls + broadcasts
        // DoorState normally (it is the single authority + simulator).
        if (mode_ == Mode::HostAuth && s->role() != coop::net::Role::Host) return;
        // Snapshot the index refs so we don't hold indexMutex_ across ReadState/Send.
        // Reuse a Channel-member buffer (GT-only: Tick -> PollAndBroadcast is game-thread
        // serial) to avoid a per-tick heap allocation on this 60-125 Hz x N-channel path.
        auto& refs = pollScratch_;
        refs.clear();
        {
            std::lock_guard<std::mutex> lk(indexMutex_);
            if (byKey_.empty()) return;
            refs.reserve(byKey_.size());
            for (auto& kv : byKey_) refs.emplace_back(kv.first, kv.second);
        }
        for (auto& r : refs) {
            if (!R::IsLiveByIndex(r.second.actor, r.second.idx)) continue;
            // HostAuth doors whose state is currently DICTATED by a client hold are not polled
            // (their state is the hold's, not the engine's async isOpened) -- skip them.
            if (!holdOpen_.empty() && holdOpen_.count(r.first)) continue;
            bool cur = false;
            if (!a_.ReadState(r.second.actor, cur)) continue;
            // "Settling" doors: we just commanded a close, but isOpened flips ~0.5s LATER (the
            // door animates). Skip the poll for a bounded bridge window so the mid-animation
            // transient isn't re-broadcast as a flicker (the open/close cycle). When the window
            // expires the poll resumes and broadcasts whatever the door has actually settled to
            // -- so a host-driven re-open during the window still propagates (no permanent
            // desync from an indefinite wait-for-value).
            if (!settling_.empty()) {
                auto sit = settling_.find(r.first);
                if (sit != settling_.end()) {
                    if (std::chrono::steady_clock::now() < sit->second) continue;  // still bridging -- skip
                    settling_.erase(sit);                                          // expired -> resume normal poll
                }
            }
            {
                std::lock_guard<std::mutex> lk(stateMutex_);
                auto it = lastKnown_.find(r.first);
                if (it == lastKnown_.end()) { lastKnown_[r.first] = cur; continue; }  // prime silently
                if (it->second == cur) continue;                                       // no change
            }
            coop::net::KeyedTogglePayload p{};
            WireKeyFromString(r.first, p.key);
            p.action = cur ? 1 : 0;
            if (s->SendReliable(a_.kind, &p, sizeof(p))) {
                { std::lock_guard<std::mutex> lk(stateMutex_); lastKnown_[r.first] = cur; }
                UE_LOGI("%s: sent %s key='%ls'", a_.name, cur ? "ON" : "OFF", r.first.c_str());
            } else {
                UE_LOGW("%s: SendReliable failed key='%ls'", a_.name, r.first.c_str());
            }
        }
    }

    // RECEIVER: from event_feed (payload already memcpy'd + range-checked). Applies
    // SYNCHRONOUSLY -- the reliable drain (event_feed::Update) runs on the game thread inside
    // net_pump::Tick, so the old GT::Post deferred the mirror by a whole pump tick for no
    // reason (the same wasted tick removed from OnRequest). The engine reads + UFunction
    // calls below are game-thread-only, which this caller already satisfies. The genuine
    // deferral (instance not streamed in yet) is preserved via pending_ + the retry tick.
    void OnReliable(const coop::net::KeyedTogglePayload& p, unsigned senderSlot) {
        std::wstring key = StringFromWireKey(p.key);
        if (key.empty()) { UE_LOGW("%s: OnReliable empty key -- dropping", a_.name); return; }
        const bool want = (p.action != 0);
        if (!a_.EnsureResolved()) {
            UE_LOGW("%s: apply -- class not resolved, dropping key='%ls'", a_.name, key.c_str());
            return;
        }
        void* actor = ResolveFast(key);
        if (actor) { ApplyResolved(actor, key, want, senderSlot); return; }
        // Not streamed in yet -- defer + retry on the throttled tick.
        pending_[key] = Pending{ want, std::chrono::steady_clock::now() + kPendingTTL };
        if (ProbeLog())
            UE_LOGI("%s: '%ls' not present yet -- deferring %s (slot %u)",
                    a_.name, key.c_str(), want ? "ON" : "OFF", senderSlot);
    }

    // HOST-only (HostAuth channels): a client asked to open/close this door. This is the
    // cycle-free core (2026-06-04). The cycle's root cause: the host opens its copy of a
    // door for the client, but no host player is at that door (the client's render puppet
    // does NOT hold the host's sensor), so the host's native checkSensor autocloses the
    // door it just opened -> broadcasts OFF -> the client re-requests -> ~1 Hz fight. The
    // fix (MTA single-syncer: disable local sim on the entity while a remote intent owns it):
    // a per-door HOLD REGISTER. On a client OPEN, the host opens the door (real guards),
    // SUPPRESSES its own autoclose for that door (a_.SuppressHeld -- lazy, once per door),
    // and records the holding slot; native autonomy can no longer close it. On the client's
    // explicit CLOSE (or disconnect), the hold is cleared, autoclose is restored, and the
    // door closes once. Exactly one authority edge per state change -> no cycle.
    void OnRequest(const coop::net::KeyedTogglePayload& p, unsigned senderSlot) {
        if (mode_ != Mode::HostAuth || !a_.RequestApply) return;
        auto* s = session_.load(std::memory_order_acquire);
        if (!s || s->role() != coop::net::Role::Host) return;  // host applies requests
        std::wstring key = StringFromWireKey(p.key);
        if (key.empty() || !a_.EnsureResolved()) return;
        void* actor = ResolveFast(key);
        if (!actor) return;  // the door must exist host-side (it is authoritative there)
        // holdOpen_[key] is a BITMASK of the peer slots currently holding this door open
        // (refcount across peers). The door stays open while ANY bit is set and only closes
        // when the LAST holder releases -- so two+ peers opening the same door, and one of
        // them later closing it, never shuts the door out from under the others.
        const uint8_t bit = (senderSlot < 8) ? static_cast<uint8_t>(1u << senderSlot) : 0;
        // A DoorOpenRequest is a pure TOGGLE, NOT an explicit state. The client cannot read the
        // door's intended state (isOpened is the animation-COMPLETED flag and lags the swing, so
        // at E-press time it reports the PRE-toggle value -> the wrong intent: pressing to OPEN a
        // closed door read "closed" and sent CLOSE, and the host obeyed -> "tried to open and
        // closed in 0.5s"). So the host derives the intent from its OWN authoritative hold record
        // (set synchronously here, never animation-gated): if this sender already holds the door
        // open -> they want to CLOSE it; otherwise -> OPEN. Robust regardless of animation timing.
        const auto holdIt = holdOpen_.find(key);
        const uint8_t curMask = (holdIt != holdOpen_.end()) ? holdIt->second : uint8_t{0};
        const bool want = ((curMask & bit) == 0);  // toggle: not-yet-held -> open; held -> close

        if (want) {
            // --- POWER/LOCK GATE (BYTE-EXACT, door BP disassembly 2026-06-06) ----------------
            // The request path force-opens (bypassCheck=true), so it skips the door's own gate --
            // without this check the host would open a door single-player keeps shut. CanOpen
            // applies the door's REAL E-press condition: Active(power) && !jammed && !superClosed
            // (the disassembled gate at ubergraph offset 3533 + the doorOpen open-condition). This
            // REPLACES the old IsLocked, which read the passlock's isAcc (a crosshair-HOVER flag,
            // disassembly-proven) and wrongly DENIED powered doors with isAcc=0 -- the user's
            // "client's first E-press doesn't work, host doesn't see the open". A keypad-locked
            // door is held Active=false (or superClosed) and is correctly refused until the
            // gamemode/keypad powers it.
            if (a_.CanOpen && !a_.CanOpen(actor)) {
                UE_LOGI("%s: client open DENIED key='%ls' slot=%u -- not openable (unpowered/jammed/superClosed)",
                        a_.name, key.c_str(), senderSlot);
                BroadcastAndPrime(key, false, s);  // correct the requester's optimistic local open
                return;
            }
            // --- OPEN intent ---------------------------------------------------------------
            // Trust the client's validated open (MTA trust-the-edge). isOpened is ASYNC --
            // doorOpen animates and the bool flips ~0.5s LATER -- so we DO NOT re-read it to
            // "verify". The old re-read saw the stale CLOSED value, logged a false DENIED, and
            // bailed WITHOUT suppressing the host autoclose, so the door opened then autoclosed
            // in 0.5s (exactly "opens on client, opens+closes on host"). Instead: drive it open
            // (bypass guards -- the host has no local player to satisfy them), mute the host's
            // OWN autoclose for the hold so it can't re-close, register the holder, broadcast
            // authoritative ON. While held the poll SKIPS this door, so the async-opening
            // transient is never polled.
            a_.RequestApply(actor, true);
            if (a_.SuppressHeld) a_.SuppressHeld(actor);
            holdOpen_[key] |= bit;
            settling_.erase(key);                 // a fresh open supersedes any pending close-settle
            BroadcastAndPrime(key, true, s);
            UE_LOGI("%s: host opened+held key='%ls' slot=%u holders=0x%02X",
                    a_.name, key.c_str(), senderSlot, holdOpen_[key]);
        } else {
            // --- CLOSE intent / release ----------------------------------------------------
            auto it = holdOpen_.find(key);
            if (it != holdOpen_.end() && (it->second & bit)) {
                it->second &= static_cast<uint8_t>(~bit);      // this peer releases its hold
                if (it->second == 0) {
                    // Last holder released -> the door genuinely closes. ReleaseHeld restores
                    // autoclose + re-enables the sensor + closes (async). Broadcast the
                    // commanded OFF immediately and mark the door "settling(false)" so the poll
                    // does not re-broadcast the mid-animation isOpened transient.
                    holdOpen_.erase(it);
                    if (a_.ReleaseHeld) a_.ReleaseHeld(actor);
                    BroadcastAndPrime(key, false, s);
                    settling_[key] = std::chrono::steady_clock::now() + kDoorSettleBridge;
                    UE_LOGI("%s: host closed key='%ls' (last holder slot=%u released)",
                            a_.name, key.c_str(), senderSlot);
                } else {
                    // Other peers still hold it open -> the door stays open (still in holdOpen_,
                    // poll keeps skipping it). Re-assert ON so the releasing peer's optimistic
                    // local close is corrected back to open.
                    BroadcastAndPrime(key, true, s);
                    UE_LOGI("%s: held key='%ls' stays open (slot=%u released, remaining=0x%02X)",
                            a_.name, key.c_str(), senderSlot, it->second);
                }
            } else if (it != holdOpen_.end() && it->second != 0) {
                // Sender isn't a holder but others hold it -> keep open, re-assert ON.
                BroadcastAndPrime(key, true, s);
                UE_LOGI("%s: held key='%ls' stays open (non-holder slot=%u close ignored, holders=0x%02X)",
                        a_.name, key.c_str(), senderSlot, it->second);
            } else {
                // Nobody holds it -> a plain client-initiated close of an unheld door (async).
                a_.RequestApply(actor, false);
                BroadcastAndPrime(key, false, s);
                settling_[key] = std::chrono::steady_clock::now() + kDoorSettleBridge;
                UE_LOGI("%s: host closed key='%ls' (unheld, slot=%u)", a_.name, key.c_str(), senderSlot);
            }
        }
    }

    // HOST: a single peer left -- drop its bit from every door it was holding open; any door
    // whose holder set hits zero genuinely closes (restore autoclose + broadcast OFF). The
    // doors still held by OTHER peers stay open. Robust per-peer cleanup for N-peer sessions
    // (OnDisconnect's full clear only fires when ALL peers are gone). Game thread.
    void OnPeerLeft(uint8_t slot) {
        if (mode_ != Mode::HostAuth) return;
        auto* s = session_.load(std::memory_order_acquire);
        if (!s || s->role() != coop::net::Role::Host) return;
        const uint8_t bit = (slot < 8) ? static_cast<uint8_t>(1u << slot) : 0;
        if (!bit) return;
        for (auto it = holdOpen_.begin(); it != holdOpen_.end();) {
            if (!(it->second & bit)) { ++it; continue; }
            it->second &= static_cast<uint8_t>(~bit);
            if (it->second != 0) { ++it; continue; }            // still held by others
            std::wstring key = it->first;
            it = holdOpen_.erase(it);
            if (void* actor = ResolveFast(key)) {
                if (a_.ReleaseHeld) a_.ReleaseHeld(actor);
                BroadcastAndPrime(key, false, s);
                settling_[key] = std::chrono::steady_clock::now() + kDoorSettleBridge;  // async close: bridge the animation
                UE_LOGI("%s: peer slot=%u left -- released held door key='%ls'",
                        a_.name, slot, key.c_str());
            }
        }
    }

    // Broadcast the authoritative state for `key` to all peers and PRIME lastKnown_ to it.
    // The prime is load-bearing: it makes the host poll's baseline already match, so the very
    // next poll sees no delta and does NOT re-broadcast the same edge (the unprimed immediate
    // broadcast double-fired with the poll = the old oscillation). HostAuth/host only.
    void BroadcastAndPrime(const std::wstring& key, bool val, coop::net::Session* s) {
        coop::net::KeyedTogglePayload bp{};
        WireKeyFromString(key, bp.key);
        bp.action = val ? 1 : 0;
        if (s->SendReliable(a_.kind, &bp, sizeof(bp))) {
            std::lock_guard<std::mutex> lk(stateMutex_);
            lastKnown_[key] = val;
        }
    }

    void QueueConnectBroadcastForSlot(int peerSlot) {
        auto* s = session_.load(std::memory_order_acquire);
        if (!s) return;
        if (s->role() != coop::net::Role::Host) return;  // host-only snapshot
        if (peerSlot < 0 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
        RebuildIndex();
        std::vector<std::pair<std::wstring, Ref>> items;
        {
            std::lock_guard<std::mutex> lk(indexMutex_);
            items.reserve(byKey_.size());
            for (auto& kv : byKey_) items.emplace_back(kv.first, kv.second);
        }
        // Send the host's CURRENT state for EVERY indexed instance (ON *and* OFF), not
        // just ON. A joiner loads its OWN save, so a switch/door the host turned OFF/closed
        // (from a saved or default ON/open state) would otherwise stay ON/open on the
        // joiner -- the old "off is the joiner's default" assumption is wrong for
        // save-persistent lights (and saved-open doors). The receiver idempotently SKIPS
        // instances already matching, so OFF sends are no-ops where the joiner agrees; the
        // cost is a few dozen small reliable packets, once, on connect.
        int sent = 0;
        for (auto& d : items) {
            if (!R::IsLiveByIndex(d.second.actor, d.second.idx)) continue;
            bool on = false;
            if (!a_.ReadState(d.second.actor, on)) continue;
            coop::net::KeyedTogglePayload p{};
            WireKeyFromString(d.first, p.key);
            p.action = on ? 1 : 0;
            s->SendReliableToSlot(peerSlot, a_.kind, &p, sizeof(p));
            { std::lock_guard<std::mutex> lk(stateMutex_); lastKnown_[d.first] = on; }
            ++sent;
        }
        UE_LOGI("%s: connect-snapshot -- sent %d full state(s) to slot %d (of %zu indexed)",
                a_.name, sent, peerSlot, items.size());
    }

    void Tick() {
        if (!a_.EnsureResolved()) return;
        if (mode_ == Mode::HostAuth) ue_wrap::door::TickSmartApply();  // finish/snap doors mid-animate
        const auto now = std::chrono::steady_clock::now();
        // Refresh the index on a throttle so the poll set picks up newly-streamed
        // instances (doors stream in progressively: 26 at connect -> 57 later) and drops
        // ones that streamed out. A full GUObjectArray walk is NOT a per-frame cost; the
        // per-tick poll below iterates only the already-built index (cheap bool reads).
        // The same refresh re-resolves deferred receiver applies.
        if (now - lastRetry_ >= kRetryRebuildThrottle) {
            lastRetry_ = now;
            RebuildIndex();
            // RECEIVER: retry deferred applies for instances that have now streamed in.
            if (!pending_.empty()) {
                int applied = 0, expired = 0, still = 0;
                for (auto it = pending_.begin(); it != pending_.end();) {
                    void* actor = ResolveFast(it->first);
                    if (actor) {
                        ApplyResolved(actor, it->first, it->second.want, 0xFF);
                        it = pending_.erase(it);
                        ++applied;
                    } else if (now >= it->second.deadline) {
                        if (ProbeLog())
                            UE_LOGI("%s: deferred '%ls' expired (not present on this peer)", a_.name, it->first.c_str());
                        it = pending_.erase(it);
                        ++expired;
                    } else {
                        ++it;
                        ++still;
                    }
                }
                if (applied || expired)
                    UE_LOGI("%s: retry tick -- applied %d deferred, dropped %d expired, %d still pending",
                            a_.name, applied, expired, still);
            }
        }
        // SENDER: poll for live state changes every tick (cheap -- the expensive rebuild
        // is throttled above; this only reads bools over the current index).
        PollAndBroadcast();
    }

    void OnDisconnect() {
        // HostAuth: restore each door's authored autonomy (we suppressed autoclose on the
        // client). Safe to call unconditionally -- RestoreAutonomy is a no-op for any door
        // we never suppressed (host side / never applied), and for Symmetric channels.
        if (mode_ == Mode::HostAuth && a_.RestoreAutonomy) {
            std::vector<Ref> live;
            { std::lock_guard<std::mutex> lk(indexMutex_); live.reserve(byKey_.size());
              for (auto& kv : byKey_) live.push_back(kv.second); }
            for (auto& r : live)
                if (R::IsLiveByIndex(r.actor, r.idx)) a_.RestoreAutonomy(r.actor);
        }
        // HostAuth + HOST: restore autoclose on any door a departed client was holding open
        // (ReleaseHeld also closes it). No-op on the client (holdOpen_ is host-populated).
        if (mode_ == Mode::HostAuth && a_.ReleaseHeld && !holdOpen_.empty()) {
            for (auto& kv : holdOpen_)
                if (void* actor = ResolveFast(kv.first)) a_.ReleaseHeld(actor);
            holdOpen_.clear();
        }
        settling_.clear();
        size_t nP = pending_.size();
        pending_.clear();
        std::lock_guard<std::mutex> lk(stateMutex_);
        const size_t n = lastKnown_.size();
        lastKnown_.clear();
        if (n > 0 || nP > 0)
            UE_LOGI("%s: OnDisconnect cleared %zu last-known + %zu pending", a_.name, n, nP);
    }

    // Full GUObjectArray walk -> rebuild the key->actor index. Game thread.
    // Returns the instance count. Logs an order-independent hash of all Keys so
    // host vs client logs reveal cross-peer Key stability (critical for the
    // swinger/container channel whose child-actor Keys may be per-peer GUIDs).
    size_t RebuildIndex() {
        if (!a_.EnsureResolved()) return 0;
        std::vector<std::pair<std::wstring, Ref>> found;
        found.reserve(64);
        const int32_t n = R::NumObjects();
        for (int32_t i = 0; i < n; ++i) {
            void* obj = R::ObjectAt(i);
            if (!obj) continue;
            if (!a_.IsInstance(obj)) continue;  // cheap filter (no alloc)
            const std::wstring nm = R::ToString(R::NameOf(obj));
            if (nm.rfind(L"Default__", 0) == 0) continue;  // skip CDO
            if (!R::IsLive(obj)) continue;
            std::wstring key = a_.GetKey(obj);
            if (key.empty() || key == L"None") continue;
            found.emplace_back(std::move(key), Ref{ obj, R::InternalIndexOf(obj) });
        }
        uint64_t keysHash = 0;
        {
            std::lock_guard<std::mutex> lk(indexMutex_);
            byKey_.clear();
            for (auto& f : found) { keysHash ^= FnvKey(f.first); byKey_[f.first] = f.second; }
        }
        // Log the (count, keysHash) only when it CHANGES -- throttled retry-tick
        // rebuilds otherwise spam the same line. The hash is the cross-peer Key-
        // stability signal (compare host vs client).
        if (found.size() != lastLogCount_ || keysHash != lastLogHash_) {
            lastLogCount_ = found.size();
            lastLogHash_ = keysHash;
            UE_LOGI("%s: index rebuilt -- %zu live keyed instance(s), keysHash=0x%016llX "
                    "(compare host vs client for cross-peer Key stability)",
                    a_.name, found.size(), static_cast<unsigned long long>(keysHash));
        }
        if (ProbeLog())
            for (auto& f : found)
                UE_LOGI("%s[probe]: key='%ls' idx=%d actor=%p", a_.name, f.first.c_str(), f.second.idx, f.second.actor);
        return found.size();
    }

private:
    struct Ref { void* actor; int32_t idx; };
    struct Pending { bool want; std::chrono::steady_clock::time_point deadline; };

    void* ResolveFast(const std::wstring& key) {
        std::lock_guard<std::mutex> lk(indexMutex_);
        auto it = byKey_.find(key);
        if (it != byKey_.end() && R::IsLiveByIndex(it->second.actor, it->second.idx))
            return it->second.actor;
        return nullptr;
    }

    void ApplyResolved(void* actor, const std::wstring& key, bool want, unsigned fromSlot) {
        bool cur = false;
        if (a_.ReadState(actor, cur) && cur == want) {
            { std::lock_guard<std::mutex> lk(stateMutex_); lastKnown_[key] = want; }
            MaybeSuppressClientAutonomy(actor);  // a connect-snapshot door we already agreed on still needs muting
            if (ProbeLog()) UE_LOGI("%s: apply key='%ls' already %s -- idempotent skip", a_.name, key.c_str(), want ? "ON" : "OFF");
            return;
        }
        echo_.store(true, std::memory_order_release);
        const bool ok = a_.ApplyState(actor, want);
        echo_.store(false, std::memory_order_release);
        { std::lock_guard<std::mutex> lk(stateMutex_); lastKnown_[key] = want; }
        MaybeSuppressClientAutonomy(actor);  // HostAuth client: mute auto-revert so the applied state holds
        UE_LOGI("%s: applied %s key='%ls' ok=%d (from slot %u)",
                a_.name, want ? "ON" : "OFF", key.c_str(), ok ? 1 : 0, fromSlot);
    }

    // HostAuth + CLIENT role only: render-only doors must not auto-revert the host's
    // state. No-op for Symmetric channels and on the host (which keeps full door logic).
    void MaybeSuppressClientAutonomy(void* actor) {
        if (mode_ != Mode::HostAuth || !a_.SuppressAutonomy) return;
        auto* s = session_.load(std::memory_order_acquire);
        if (s && s->role() != coop::net::Role::Host) a_.SuppressAutonomy(actor);
    }

    const Adapter& a_;
    const Mode mode_;
    std::atomic<coop::net::Session*> session_{nullptr};
    std::atomic<bool> echo_{false};

    std::mutex indexMutex_;
    std::unordered_map<std::wstring, Ref> byKey_;

    std::mutex stateMutex_;
    std::unordered_map<std::wstring, bool> lastKnown_;

    std::unordered_map<std::wstring, Pending> pending_;            // GT-only
    std::unordered_map<std::wstring, uint8_t> holdOpen_;          // GT-only (HostAuth/host): door key -> bitmask of peer slots holding it open
    std::unordered_map<std::wstring, std::chrono::steady_clock::time_point> settling_;  // GT-only (HostAuth/host): door key -> deadline until which the poll skips it (bridges the async close animation)
    std::vector<std::pair<std::wstring, Ref>> pollScratch_;       // GT-only: reused poll snapshot buffer
    std::chrono::steady_clock::time_point lastRetry_{};           // GT-only
    size_t lastLogCount_ = SIZE_MAX;                              // GT-only: dedup the rebuilt log
    uint64_t lastLogHash_ = 0;                                   // GT-only
};

// ---- Adapters (file-static; must precede the Channel instances) ----------
const Adapter g_doorAdapter = {
    "door", coop::net::ReliableKind::DoorState,
    &ue_wrap::door::EnsureResolved,
    &ue_wrap::door::IsDoor,
    &ue_wrap::door::GetKeyString,
    &ue_wrap::door::TryReadOpen,
    // Receiver-apply (host state on this peer): FORCE-SNAP, not doorOpen/doorClose. The open is
    // a tick-gated animation that FREEZES when this peer's player is far from the door (probe-
    // proven 2026-06-04: isOpened never sets), so doorOpen() can't reliably mirror a door the
    // local player isn't standing at. ForceOpen/ForceClose complete the state via the move
    // timeline + move__FinishedFunc regardless of proximity. (If the local player IS near it
    // snaps rather than animates -- correctness over a swing; visual polish is a later pass.)
    [](void* a, bool on) -> bool { ue_wrap::door::SmartApply(a, on); return true; },
    // HostAuth hooks (doors only):
    &ue_wrap::door::SuppressClientAutonomy,
    &ue_wrap::door::RestoreClientAutonomy,
    // Host applies a CLIENT request: FORCE-SNAP too. The host has no local player at the door
    // (only the client's puppet), so BOTH doorOpen(bypassCheck=false) [denied -- needs a local
    // interactor] AND doorOpen(bypassCheck=true) [animation freezes when the host player is far]
    // fail to open it ("the door is closed on host"). Force-snap is the only thing that opens a
    // door the host player isn't next to. The client already enforced the lock locally, so
    // trusting its validated edge and snapping the host door is correct + authoritative.
    [](void* a, bool on) -> bool { ue_wrap::door::SmartApply(a, on); return true; },
    // HOST held-door suppression (cycle fix): mute the host's own autoclose while a client
    // holds this door open; restore + close on release.
    &ue_wrap::door::SuppressHostHeldDoor,
    &ue_wrap::door::ReleaseHostHeldDoor,
    // Open gate: the host applies a client's open only if the door's own engine would (power on,
    // not jammed, not superClosed) -- so coop never opens a door SP keeps shut.
    &ue_wrap::door::CanOpen,
};
const Adapter g_lightAdapter = {
    // Re-keyed to the SWITCH (was the lightRoot) so the receiver replays use() -> the
    // switch FLIPS VISUALLY on the peer AND its lights fan out, in one BP call. use()
    // toggles; the channel only applies when cur != want (ApplyResolved's cur==want
    // idempotent guard), so for a 2-state bool "toggle when different" == an absolute set,
    // and double-delivery is safe (applies are GT-serialized + use() updates A
    // synchronously -- lightswitch_probe proved A flips 0->1 right after the call).
    // (IDA 2026-06-04: the lightRoot.SetActive observer never fired -- BP-internal.)
    // HANDS-ON TO VERIFY: that use() toggles BOTH directions (1->0, not just 0->1); if a
    // switch's use() is one-way, want=OFF would never land -> needs a switch-level set verb.
    "light", coop::net::ReliableKind::LightState,
    &ue_wrap::lightswitch::EnsureSwitchResolved,
    &ue_wrap::lightswitch::IsLightSwitch,
    &ue_wrap::lightswitch::GetSwitchKeyString,
    &ue_wrap::lightswitch::TryReadSwitchA,
    [](void* a, bool /*on*/) -> bool { return ue_wrap::lightswitch::CallUse(a); },
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,  // Symmetric channel -- no HostAuth hooks (last = CanOpen)
};
const Adapter g_containerAdapter = {
    "container", coop::net::ReliableKind::ContainerState,
    &ue_wrap::swinger::EnsureResolved,
    &ue_wrap::swinger::IsSwinger,
    &ue_wrap::prop::GetKeyString,  // a swinger is an Aprop_C
    &ue_wrap::swinger::TryReadOpen,
    [](void* a, bool on) -> bool { return on ? ue_wrap::swinger::CallOpen(a, false) : ue_wrap::swinger::CallClose(a); },
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,  // Symmetric channel -- no HostAuth hooks (last = CanOpen)
};
Channel g_door{g_doorAdapter, Channel::Mode::HostAuth};  // doors auto-revert -> host-authoritative
Channel g_light{g_lightAdapter};
Channel g_container{g_containerAdapter};
// Keypads (ApasswordLock_C) are NOT a toggle -- they carry a typed buffer + 3 state bools
// and their accept verb is unreachable (proven), so forcing them into this Channel was the
// v31 fail-cycle. They now live in their own coop::keypad_sync module (RULE 2: the broken
// adapter + g_keypad Channel are GONE, not disabled-in-place). KeypadState routes there
// from event_feed, not through ChannelForKind below.

Channel* ChannelForKind(coop::net::ReliableKind k) {
    switch (k) {
    case coop::net::ReliableKind::DoorState:      return &g_door;
    case coop::net::ReliableKind::LightState:     return &g_light;
    case coop::net::ReliableKind::ContainerState: return &g_container;
    default:                                      return nullptr;
    }
}

// ---- The SENDER is per-tick STATE POLLING (Channel::PollAndBroadcast, driven by
// Tick()). There is no UFunction observer: IDA-PROVEN 2026-06-04 the per-verb edges
// (Adoor_C::doorOpen, Atrigger_lightRoot_C::SetActive, Aprop_swinger_C::Open -- and even
// the switch's player_use/use) dispatch via CallFunction -> ProcessInternal (@0x141302dc0)
// and bypass our ProcessEvent detour (@0x141465930), so a POST observer never fires (the
// doors `sent=0` bug). Polling the resulting STATE field instead catches every writer --
// player E-press, NPC-proximity auto-open, keypad-unlock, scripts -- uniformly. -------

// ---- CLIENT door request on E-press (HostAuth doors) ----------------------------------
// The door's own verbs (player_use / doorOpen) dispatch BP-internally (CallFunction ->
// ProcessInternal), so a POST observer on them NEVER fires -- the client's door open would
// never reach the host (the bug: "client opening door never mirrors"). The ONE ProcessEvent-
// observable use-edge is AmainPlayer_C::InpActEvt_use (the E-press input action; grab_observer
// proves it fires). We observe THAT (POST) on the local player, read the actor the player was
// aiming at (mainPlayer.lookAtActor @0x0AA0), and -- if it is a door -- send a DoorOpenRequest
// with the door's post-press isOpened (POST = the new toggled state). The host applies it
// (real guards) + broadcasts authoritative DoorState. CLIENT-only; the host runs the real
// door logic + polls, so it needs no such hook. (Puppets are unpossessed -> they never
// process input, so this only ever fires for the local player.)
bool g_useInputObserverInstalled = false;

void OnUseInput(void* self, void*, void*) {
    if (!self) return;
    auto* s = g_door.GetSession();
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return;  // CLIENT-only
    if (!ue_wrap::door::EnsureResolved()) return;
    void* door = ue_wrap::engine::ReadMainPlayerLookAtActor(self);  // the actor under the cursor at press
    const bool isDoor = (door && ue_wrap::door::IsDoor(door));
    // DIAGNOSTIC (gated behind interactable_log -- fires on EVERY E-press, door or not, so it
    // must NOT be unconditional: log spam on a per-input path costs FPS). Enable it when a door
    // open misbehaves: no line = observer didn't fire; lookAtActor=0 = the interaction trace had
    // not populated the aimed actor yet; non-null + isDoor=0 = aiming at a non-door. The
    // meaningful edges (toggle request sent; host opened/DENIED) are logged unconditionally below
    // + in OnRequest, so the normal path is visible without this.
    if (ProbeLog())
        UE_LOGI("door: use-input fired -- lookAtActor=%p isDoor=%d (role=client, connected)", door, isDoor ? 1 : 0);
    if (!isDoor) return;             // not aiming at a door -> not ours
    std::wstring key = ue_wrap::door::GetKeyString(door);
    if (key.empty() || key == L"None") return;
    // DEBOUNCE: AmainPlayer_C::InpActEvt_use dispatches on BOTH the press AND the release of one
    // tap (~0.3s apart), so a single "use" fires this observer TWICE -> two toggles -> the host
    // opens then immediately closes ("open-closed in 0.3s, nothing changed") and the release's
    // force-close interrupts the client's mid-open swing -> ajar. Collapse rapid repeats for the
    // same door into ONE toggle. 300ms < any deliberate re-toggle, > a tap's press-release gap.
    static std::unordered_map<std::wstring, std::chrono::steady_clock::time_point> s_lastUse;  // GT-only
    const auto nowTs = std::chrono::steady_clock::now();
    if (auto it = s_lastUse.find(key); it != s_lastUse.end() && nowTs - it->second < std::chrono::milliseconds(300)) {
        UE_LOGI("door: use-input hook -> debounced repeat (press+release) key='%ls'", key.c_str());
        return;
    }
    s_lastUse[key] = nowTs;
    // Send a pure TOGGLE -- do NOT read isOpened here. isOpened is the animation-COMPLETED flag
    // and lags the swing, so at POST-player_use it holds the PRE-toggle value and reports the
    // WRONG intent (press-to-open read "closed" -> sent close -> host closed the door). The host
    // derives open-vs-close from its own authoritative hold record (OnRequest), which is timing-
    // robust. p.action is unused by OnRequest (every DoorOpenRequest is a toggle).
    coop::net::KeyedTogglePayload p{};
    WireKeyFromString(key, p.key);
    p.action = 0;
    if (s->SendReliable(coop::net::ReliableKind::DoorOpenRequest, &p, sizeof(p)))
        UE_LOGI("door: use-input hook -> toggle request key='%ls'", key.c_str());
}

void InstallUseInputObserver() {
    if (g_useInputObserverInstalled) return;
    void* playerCls = R::FindClass(P::name::MainPlayerClass);
    if (!playerCls) return;  // retry until mainPlayer_C loads
    void* fn = R::FindFunction(playerCls, P::name::MainPlayerUseInputEventFn);
    if (!fn) {
        UE_LOGW("door: InpActEvt_use UFunction not found -- client door opens cannot be signalled");
        g_useInputObserverInstalled = true;  // don't retry forever
        return;
    }
    if (!GT::RegisterPostObserver(fn, &OnUseInput)) {
        UE_LOGW("door: InpActEvt_use observer register failed");
        return;
    }
    g_useInputObserverInstalled = true;
    UE_LOGI("door: InpActEvt_use POST observer installed (client E-press -> DoorOpenRequest via lookAtActor)");
}

// ---- Receiver index (one-shot per channel, latched). The receiver resolves by Key;
// late-loaded instances are caught by the retry-tick + connect-snapshot rebuilds. ------
bool g_doorIndexed = false, g_lightIndexed = false, g_containerIndexed = false;
void IndexChannels() {
    if (!g_doorIndexed && ue_wrap::door::EnsureResolved()) {
        UE_LOGI("door: indexed %zu door(s)", g_door.RebuildIndex()); g_doorIndexed = true;
    }
    if (!g_lightIndexed && ue_wrap::lightswitch::EnsureSwitchResolved()) {
        UE_LOGI("light: indexed %zu switch(es)", g_light.RebuildIndex()); g_lightIndexed = true;
    }
    if (!g_containerIndexed && ue_wrap::swinger::EnsureResolved()) {
        UE_LOGI("container: indexed %zu lid(s)", g_container.RebuildIndex()); g_containerIndexed = true;
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_door.SetSession(session);
    g_light.SetSession(session);
    g_container.SetSession(session);
    IndexChannels();              // build the key->actor index (sender polls it; receiver resolves by it)
    InstallUseInputObserver();   // client E-press (InpActEvt_use + lookAtActor) -> DoorOpenRequest
}

void OnReliable(uint8_t kind, const coop::net::KeyedTogglePayload& payload, uint8_t senderPeerSlot) {
    if (Channel* ch = ChannelForKind(static_cast<coop::net::ReliableKind>(kind)))
        ch->OnReliable(payload, senderPeerSlot);
}

void OnDoorOpenRequest(const coop::net::KeyedTogglePayload& payload, uint8_t senderPeerSlot) {
    // HOST-only: a client asked to open/close a door. event_feed already trust-gates
    // senderPeerSlot != 0; OnRequest re-checks the host role. The host applies it (real
    // guards) and its poll broadcasts the authoritative DoorState back to everyone.
    if (senderPeerSlot == 0) return;  // the host never sends this to itself
    g_door.OnRequest(payload, senderPeerSlot);
}

void OnPeerLeft(int peerSlot) {
    if (peerSlot <= 0 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    g_door.OnPeerLeft(static_cast<uint8_t>(peerSlot));  // door is the only HostAuth channel
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    g_door.QueueConnectBroadcastForSlot(peerSlot);
    g_light.QueueConnectBroadcastForSlot(peerSlot);
    g_container.QueueConnectBroadcastForSlot(peerSlot);
}

void Tick() {
    g_door.Tick();
    g_light.Tick();
    g_container.Tick();
}

void OnDisconnect() {
    g_door.OnDisconnect();
    g_light.OnDisconnect();
    g_container.OnDisconnect();
}

}  // namespace coop::interactable_sync
