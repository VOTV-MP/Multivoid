// coop/interactables/interactable_channel.h -- the keyed-interactable replication engine shared by
// every keyed channel (doors, lights, light groups, containers, the garage, appliances, door
// boxes): the key-to-actor index fed by the shared scan hub, per-key dedup, deferred apply with a
// throttled retry, echo suppression and the connect snapshot. A
// feature is an Adapter (a vtable over its ue_wrap wrapper) plus a Channel instance in
// interactable_sync.cpp, this header's one includer. Nothing per-class lives here.

#pragma once

#include "coop/config/config.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"  // WireKeyFromString, StringFromWireKey, FnvKey
#include "coop/player/players_registry.h"   // coop::players::kMaxPeers
#include "coop/element/object_scan_hub.h"      // the shared sliced scan pass
#include "coop/element/portable_identity.h"  // the cross-peer name, when one exists
#include "ue_wrap/engine/world_identity.h"     // the world generation the index is stamped with
#include "ue_wrap/engine/engine.h"            // TryGetActorLocation, for the probe

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/walk_timer.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/actors/prop.h"          // the parent prop's own key, for the probe
#include "ue_wrap/core/sdk_profile.h"   // UObject_ObjectFlags

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace coop::interactable_sync {

namespace R = ue_wrap::reflection;

// The wire-key helpers from coop/net/wire_key_util.h, pulled into this namespace so the Channel's
// unqualified calls resolve.
using coop::net::WireKeyFromString;
using coop::net::StringFromWireKey;
using coop::net::FnvKey;

inline constexpr auto kRetryRebuildThrottle = std::chrono::seconds(2);
// The scan cadence and backstop live with the shared pass in coop/element/object_scan_hub. The
// mid-join row: a deferred state expires only when this channel's live count has been unchanged
// for kSettlePassesForExpiry consecutive hub passes and the key still does not resolve. A wall
// clock alone bounds the wrong quantity (a field join streams the world far slower than the
// lab), so kPendingTTL is the outer backstop against a map leak, and it logs a different line.
inline constexpr auto kPendingTTL = std::chrono::minutes(10);
inline constexpr int  kSettlePassesForExpiry = 5;

inline bool ProbeLog() {
    static const bool s_enabled = ::coop::config::ResolveFlag(::coop::config_registry::rows::interactable_log);
    return s_enabled;
}

// The per-feature engine vtable.
struct Adapter {
    const char* name;                          // "door" / "light" / "container"
    coop::net::ReliableKind kind;
    bool (*EnsureResolved)();
    bool (*IsInstance)(void* obj);             // class-descendant check
    std::wstring (*GetKey)(void* actor);       // cross-peer-stable Key string
    bool (*ReadState)(void* actor, bool& on);  // current open/on state
    bool (*ApplyState)(void* actor, bool on);  // drive to target (channel echo-suppresses)
    // HostAuth channels only; null for symmetric ones.
    void (*SuppressAutonomy)(void* actor);     // CLIENT: mute local auto-revert so applied state sticks
    void (*RestoreAutonomy)(void* actor);      // restore authored autonomy at disconnect
    void (*TickApply)();                       // per-tick completion of an async apply (doors); null = none
};

// The engine.
class Channel {
public:
    // Symmetric: every peer is authoritative over the changes it causes (no local auto-revert, so
    // no fight). HostAuth: only the host broadcasts state, and the client renders it with autonomy
    // suppressed (a door's autoclose re-drives its state, so a symmetric poll would oscillate). A
    // client's own use of a HostAuth device reaches the host outside the channel: a door's as a
    // door verb intent (coop/interactables/door_verb_intent), a switch's through the light lane.
    enum class Mode { Symmetric, HostAuth };

    explicit Channel(const Adapter& a, Mode mode = Mode::Symmetric) : a_(a), mode_(mode) {}

    // Registers this channel as a scan-hub consumer. Called from Install, not the constructor: the
    // channel instances are namespace-scope statics in another TU, and cross-TU static-init order
    // against the hub's state is undefined.
    void RegisterWithScanHub() {
        if (scanRegistered_) return;
        scanRegistered_ = true;
        coop::element::scan_hub::Register(coop::element::scan_hub::Consumer{
            a_.name, this, a_.EnsureResolved, a_.IsInstance,
            &Channel::HubPassBeginThunk, &Channel::HubMatchThunk, &Channel::HubPassCompleteThunk,
            /*settleScans*/ 15});
    }

    void SetSession(coop::net::Session* s) { session_.store(s, std::memory_order_release); }
    coop::net::Session* GetSession() const { return session_.load(std::memory_order_acquire); }
    // Prime the poll baseline for `key`.
    void PreUpdateLastKnown(const std::wstring& key, bool val) {
        std::lock_guard<std::mutex> lk(stateMutex_); lastKnown_[key] = val;
    }

    // The sender: polls every indexed instance for a state change and broadcasts deltas. The poll
    // catches every writer of the state (a press, an NPC's open, a keypad unlock, a script) without
    // watching each one. The first sighting of a key primes the baseline silently, since initial
    // divergence is the connect snapshot's job; ApplyResolved primes lastKnown_ to the applied
    // value, so an echo never shows as a delta. Game thread.
    void PollAndBroadcast() {
        if (echo_.load(std::memory_order_acquire)) return;  // mid-apply
        if (!IndexCurrent()) return;  // a dead world's index: wait for the hub
        auto* s = session_.load(std::memory_order_acquire);
        // Solo host: nobody to send to. lastKnown_ is not advanced here; OnDisconnect clears it and
        // the connect snapshot re-primes every key on the next join, so a change made while solo
        // reaches the joiner through the snapshot rather than as a stale edge.
        if (!s || !s->connected()) return;
        // A HostAuth client never poll-broadcasts: its door is render-only, its isOpened moves only
        // when a host state is applied, and a client poll would report the host's own commands back
        // as its own, the feedback storm that oscillated doors. The non-authority sends its player's
        // verbs only (coop/interactables/door_verb_intent).
        if (mode_ == Mode::HostAuth && s->role() != coop::net::Role::Host) return;
        // The index refs are snapshotted so indexMutex_ is not held across ReadState and Send; the
        // buffer is a member (game-thread serial) so the per-tick path does not allocate.
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
            bool cur = false;
            if (!a_.ReadState(r.second.actor, cur)) continue;
            {
                std::lock_guard<std::mutex> lk(stateMutex_);
                auto it = lastKnown_.find(r.first);
                if (it == lastKnown_.end()) { lastKnown_[r.first] = cur; continue; }  // prime silently
                if (it->second == cur) {                                               // no change
                    // A refused change that reverted before the channel freed: the streak is over.
                    if (!sendRefused_.empty()) sendRefused_.erase(r.first);
                    continue;
                }
            }
            coop::net::KeyedTogglePayload p{};
            WireKeyFromString(r.first, p.key);
            p.action = cur ? 1 : 0;
            if (s->SendReliable(a_.kind, &p, sizeof(p))) {
                { std::lock_guard<std::mutex> lk(stateMutex_); lastKnown_[r.first] = cur; }
                sendRefused_.erase(r.first);
                UE_LOGI("%s: sent %s key='%ls'", a_.name, cur ? "ON" : "OFF", r.first.c_str());
            } else if (sendRefused_.insert(r.first).second) {
                // The change stays unsent and this poll retries it every tick until the channel
                // takes it (a joiner's save stream holds it for the length of the transfer), so the
                // refusal is said once per streak: measured unthrottled, 1196 lines in 20 s.
                UE_LOGW("%s: SendReliable refused key='%ls' -- retrying every poll until it is sent",
                        a_.name, r.first.c_str());
            }
        }
    }

    // The receiver, from event_feed (payload already copied and range-checked). Applies
    // synchronously: the reliable drain runs on the game thread inside net_pump::Tick, which the
    // engine reads and UFunction calls below require. An instance not streamed in yet is deferred
    // to pending_ and retried on the throttled tick.
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
        // Not streamed in yet: deferred, retried on the throttled tick.
        pending_[key] = Pending{ want, std::chrono::steady_clock::now() + kPendingTTL };
        if (ProbeLog())
            UE_LOGI("%s: '%ls' not present yet -- deferring %s (slot %u)",
                    a_.name, key.c_str(), want ? "ON" : "OFF", senderSlot);
    }

    // Broadcasts the authoritative state for the key and primes lastKnown_ to it, so the next poll
    // sees no delta and does not send the same edge twice. Host only.
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
        // No forced rebuild: the hub's 2 s pass keeps the index fresh, and a swinger spawned inside
        // that window reaches the joiner on its next state change.
        std::vector<std::pair<std::wstring, Ref>> items;
        {
            std::lock_guard<std::mutex> lk(indexMutex_);
            items.reserve(byKey_.size());
            for (auto& kv : byKey_) items.emplace_back(kv.first, kv.second);
        }
        // The host's current state for every indexed instance, OFF included: the joiner loads its
        // own save, so a switch the host turned off would otherwise stay on there. A symmetric
        // receiver skips instances already matching, so an agreeing OFF costs one small packet.
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
        RegisterWithScanHub();  // safety net for any order where Tick precedes Install
        if (a_.TickApply) a_.TickApply();  // finish an async apply (doors)
        if (!IndexCurrent()) return;  // a dead world's index: wait for the hub
        const auto now = std::chrono::steady_clock::now();
        if (now - lastRetry_ >= kRetryRebuildThrottle) {
            lastRetry_ = now;
            // Retry deferred applies for instances that have streamed in since; the throttle paces
            // only the retries.
            if (!pending_.empty()) {
                int applied = 0, expired = 0, still = 0, backstopped = 0;
                for (auto it = pending_.begin(); it != pending_.end();) {
                    void* actor = ResolveFast(it->first);
                    if (actor) {
                        ApplyResolved(actor, it->first, it->second.want, 0xFF);
                        it = pending_.erase(it);
                        ++applied;
                    } else if (stablePasses_ >= kSettlePassesForExpiry) {
                        // The world settled and it is still not here: the count that grades this
                        // lane.
                        if (ProbeLog())
                            UE_LOGI("%s: deferred '%ls' expired (index settled %d passes, still "
                                    "not present on this peer)", a_.name, it->first.c_str(), stablePasses_);
                        it = pending_.erase(it);
                        ++expired;
                    } else if (now >= it->second.deadline) {
                        // The backstop: the index never settled in ten minutes, a hub or world
                        // signal rather than a missing instance, counted apart from the settled
                        // expiry.
                        UE_LOGW("%s: deferred '%ls' hit the %lld-minute BACKSTOP -- the index "
                                "never settled (stablePasses=%d); this is a hub/world signal, "
                                "not a missing instance",
                                a_.name, it->first.c_str(),
                                static_cast<long long>(std::chrono::duration_cast<std::chrono::minutes>(kPendingTTL).count()),
                                stablePasses_);
                        it = pending_.erase(it);
                        ++backstopped;
                    } else {
                        ++it;
                        ++still;
                    }
                }
                if (applied || expired || backstopped)
                    UE_LOGI("%s: retry tick -- applied %d deferred, dropped %d expired, %d backstopped, "
                            "%d still pending", a_.name, applied, expired, backstopped, still);
            }
        }
        // The poll, every tick: bool reads over the current index.
        PollAndBroadcast();
    }

    void OnDisconnect() {
        // HostAuth: each door's authored autonomy restored (the client suppressed autoclose).
        // RestoreAutonomy is a no-op for a door never suppressed.
        if (mode_ == Mode::HostAuth && a_.RestoreAutonomy && IndexCurrent()) {
            // The IndexCurrent gate: on a quit-to-menu disconnect the index holds a dead world's
            // doors, which slot-and-serial liveness cannot see, and a dead world needs no restore.
            std::vector<Ref> live;
            { std::lock_guard<std::mutex> lk(indexMutex_); live.reserve(byKey_.size());
              for (auto& kv : byKey_) live.push_back(kv.second); }
            for (auto& r : live)
                if (R::IsLiveByIndex(r.actor, r.idx)) a_.RestoreAutonomy(r.actor);
        }
        size_t nP = pending_.size();
        pending_.clear();
        std::lock_guard<std::mutex> lk(stateMutex_);
        const size_t n = lastKnown_.size();
        lastKnown_.clear();
        sendRefused_.clear();
        if (n > 0 || nP > 0)
            UE_LOGI("%s: OnDisconnect cleared %zu last-known + %zu pending", a_.name, n, nP);
    }

    // The scan-hub consumer: one shared GUObjectArray pass drives the callbacks below for every
    // channel. The index is world-stamped: indexGen_ records the generation it was built in, and
    // every read path treats a stale generation as empty, since slot-and-serial liveness is blind
    // to world death and the generation compare is not.
    bool IndexCurrent() const { return indexGen_ == ue_wrap::world_identity::Generation(); }

    // The key this channel indexes the actor under, or "" when not indexed yet. The send side uses
    // this and never re-derives: HubMatch is the one derivation site, and PortableWireKey reads a
    // weak pointer that can legitimately resolve to nothing during the join's GC churn, so a second
    // derivation could name a key the far peer's index does not hold. "" degrades to "not indexed
    // yet", which the deferred retry handles.
    std::wstring KeyForActor(void* actor) const {
        if (!actor || !IndexCurrent()) return std::wstring();
        std::lock_guard<std::mutex> lk(indexMutex_);
        for (const auto& kv : byKey_)
            if (kv.second.actor == actor) return kv.first;
        return std::wstring();
    }

    // The live actor this channel indexes under `key` in the current world, or null. Game thread.
    void* ActorForKey(const std::wstring& key) { return ResolveFast(key); }

    void HubPassBegin(bool /*isFull*/) { scanFound_.clear(); }

    void HubMatch(void* obj) {
        const std::wstring nm = R::ToString(R::NameOf(obj));
        if (nm.rfind(L"Default__", 0) == 0) return;  // skip CDO
        if (!R::IsLive(obj)) return;
        std::wstring key = a_.GetKey(obj);
        if (key.empty() || key == L"None") return;
        // The game's Key is a local name: lib_C::assignKey mints a random one per process for
        // anything the save does not persist. Where a portable identity exists the actor is indexed
        // and addressed by that instead; otherwise the game key stands. See
        // coop/element/portable_identity.h.
        std::wstring portable = coop::element::PortableWireKey(obj);
        if (!portable.empty()) {
            if (ProbeLog())
                UE_LOGI("%s[ident]: '%ls' -> %ls (%ls)", a_.name, key.c_str(), portable.c_str(),
                        coop::element::PortableIdentity(obj).c_str());
            key = std::move(portable);
        }
        scanFound_.emplace_back(std::move(key), Ref{ obj, R::InternalIndexOf(obj) });
    }

    size_t HubPassComplete(bool isFull, uint32_t worldGen) {
        size_t liveCount = 0;
        uint64_t keysHash = 0;
        {
            std::lock_guard<std::mutex> lk(indexMutex_);
            if (isFull) byKey_.clear();                        // full pass rebuilds from scratch
            for (auto& f : scanFound_) byKey_[f.first] = f.second;
            if (!isFull) {                                     // tail pass: prune entries whose actor died
                for (auto it = byKey_.begin(); it != byKey_.end();) {
                    if (!R::IsLiveByIndex(it->second.actor, it->second.idx)) it = byKey_.erase(it);
                    else ++it;
                }
            }
            for (auto& kv : byKey_) keysHash ^= FnvKey(kv.first);  // over the full current set
            liveCount = byKey_.size();
            indexGen_ = worldGen;
        }
        // The settle term behind kSettlePassesForExpiry, kept local: the hub exposes its own settle
        // signal only through a Debug accessor, and a count delta is all this needs.
        if (liveCount == lastCount_) { if (stablePasses_ < 1000000) ++stablePasses_; }
        else                         { stablePasses_ = 0; lastCount_ = liveCount; }
        scanFound_.clear();
        // Logged only when the count or the hash changes. The hash is the cross-peer key-stability
        // signal (compare host and client), and it is also what says whether this census differs
        // from the one already reported -- which the per-instance dump below reads too.
        const bool censusChanged = (liveCount != lastLogCount_ || keysHash != lastLogHash_);
        if (censusChanged) {
            lastLogCount_ = liveCount;
            lastLogHash_ = keysHash;
            UE_LOGI("%s: index rebuilt -- %zu live keyed instance(s), keysHash=0x%016llX "
                    "(compare host vs client for cross-peer Key stability)",
                    a_.name, liveCount, static_cast<unsigned long long>(keysHash));
        }
        // The dump rides the SAME gate as the summary above. A census that has not changed has
        // nothing new to say about identity, and re-printing it every pass costs a dozen reflection
        // reads per instance and buries the one pass that did change: one key's line was printed 73
        // times in a single join, and six channels together put about 20,000 of these in one run.
        if (censusChanged && ProbeLog()) {
            // The identity probe: for each indexed instance, every candidate for a cross-peer
            // identity alongside the key: the UObject name (baked into the level package for a
            // placed actor, counter-suffixed for a spawned one), the Outer, the class, the object
            // flags (RF_WasLoaded, 0x00080000, separates loaded from runtime-made) and the world
            // location. One run per peer answers by diff.
            std::vector<std::pair<std::wstring, Ref>> snap;
            { std::lock_guard<std::mutex> lk(indexMutex_); snap.assign(byKey_.begin(), byKey_.end()); }
            for (auto& kv : snap) {
                void* const obj = kv.second.actor;
                if (!R::IsLiveByIndex(obj, kv.second.idx)) continue;
                void* const outer = R::OuterOf(obj);
                const uint32_t objFlags = *reinterpret_cast<const uint32_t*>(
                    reinterpret_cast<const char*>(obj) + ue_wrap::profile::off::UObject_ObjectFlags);
                ue_wrap::FVector loc{};
                const bool haveLoc = ue_wrap::engine::TryGetActorLocation(obj, loc);
                // The child-actor chain: a child actor's own name carries a per-process counter, so
                // its identity is the parent's plus the owning component's authored name.
                std::wstring compName;
                void* const parent = ue_wrap::engine::ParentActorOf(obj, &compName);
                std::wstring parentName = L"<none>", parentClass = L"<none>", parentKey = L"<none>";
                uint32_t parentFlags = 0;
                if (parent) {
                    parentName = R::ToString(R::NameOf(parent));
                    parentClass = R::ClassNameOf(parent);
                    parentFlags = *reinterpret_cast<const uint32_t*>(
                        reinterpret_cast<const char*>(parent) + ue_wrap::profile::off::UObject_ObjectFlags);
                    // The parent's own key, the only portable candidate for a counter-named child.
                    parentKey = ue_wrap::prop::GetInteractableKeyString(parent);
                    if (parentKey.empty() || parentKey == L"None")
                        parentKey = ue_wrap::prop::GetActorSaveKeyString(parent);
                    if (parentKey.empty()) parentKey = L"<empty>";
                }
                UE_LOGI("%s[probe]: key='%ls' idx=%d actor=%p name='%ls' outer='%ls' class='%ls' "
                        "flags=0x%08X loc=%.1f,%.1f,%.1f comp='%ls' parent='%ls' pclass='%ls' "
                        "pflags=0x%08X pkey='%ls'%s",
                        a_.name, kv.first.c_str(), kv.second.idx, obj,
                        R::ToString(R::NameOf(obj)).c_str(),
                        outer ? R::ToString(R::NameOf(outer)).c_str() : L"<none>",
                        R::ClassNameOf(obj).c_str(), objFlags,
                        loc.X, loc.Y, loc.Z,
                        compName.empty() ? L"<none>" : compName.c_str(),
                        parentName.c_str(), parentClass.c_str(), parentFlags, parentKey.c_str(),
                        haveLoc ? "" : " LOC-FAILED");
            }
        }
        return liveCount;
    }

private:
    struct Ref { void* actor; int32_t idx; };
    struct Pending { bool want; std::chrono::steady_clock::time_point deadline; };

    void* ResolveFast(const std::wstring& key) {
        if (!IndexCurrent()) return nullptr;  // stale-gen index = another world's actors
        std::lock_guard<std::mutex> lk(indexMutex_);
        auto it = byKey_.find(key);
        if (it != byKey_.end() && R::IsLiveByIndex(it->second.actor, it->second.idx))
            return it->second.actor;
        return nullptr;
    }

    void ApplyResolved(void* actor, const std::wstring& key, bool want, unsigned fromSlot) {
        // The idempotent skip is symmetric-only, where the local field is moved only by us or by
        // the peer being echoed. On a HostAuth channel a client-local writer the lane does not
        // refuse (a creature's open, the local blackout, a keypad's latent open) can move the field
        // without the host, so a match is not proof the copy holds the host's state; each HostAuth
        // adapter's apply is idempotent itself (a door already at or swinging toward the state is
        // left alone, a light group already there is skipped), and it always runs.
        if (mode_ == Mode::Symmetric) {
            bool cur = false;
            if (a_.ReadState(actor, cur) && cur == want) {
                { std::lock_guard<std::mutex> lk(stateMutex_); lastKnown_[key] = want; }
                MaybeSuppressClientAutonomy(actor);
                if (ProbeLog()) UE_LOGI("%s: apply key='%ls' already %s -- idempotent skip", a_.name, key.c_str(), want ? "ON" : "OFF");
                return;
            }
        }
        echo_.store(true, std::memory_order_release);
        const bool ok = a_.ApplyState(actor, want);
        echo_.store(false, std::memory_order_release);
        { std::lock_guard<std::mutex> lk(stateMutex_); lastKnown_[key] = want; }
        MaybeSuppressClientAutonomy(actor);  // HostAuth client: mute auto-revert so the applied state holds
        UE_LOGI("%s: applied %s key='%ls' ok=%d (from slot %u)",
                a_.name, want ? "ON" : "OFF", key.c_str(), ok ? 1 : 0, fromSlot);
    }

    // HostAuth client only: a render-only door must not auto-revert the host's state. No-op on the
    // host and on symmetric channels.
    void MaybeSuppressClientAutonomy(void* actor) {
        if (mode_ != Mode::HostAuth || !a_.SuppressAutonomy) return;
        auto* s = session_.load(std::memory_order_acquire);
        if (s && s->role() != coop::net::Role::Host) a_.SuppressAutonomy(actor);
    }

    const Adapter& a_;
    const Mode mode_;
    std::atomic<coop::net::Session*> session_{nullptr};
    std::atomic<bool> echo_{false};

    static void HubPassBeginThunk(void* ctx, bool isFull) { static_cast<Channel*>(ctx)->HubPassBegin(isFull); }
    static void HubMatchThunk(void* ctx, void* obj) { static_cast<Channel*>(ctx)->HubMatch(obj); }
    static size_t HubPassCompleteThunk(void* ctx, bool isFull, uint32_t gen) {
        return static_cast<Channel*>(ctx)->HubPassComplete(isFull, gen);
    }

    mutable std::mutex indexMutex_;
    size_t   lastCount_    = static_cast<size_t>(-1);  // never equal to a real first count
    int      stablePasses_ = 0;                        // consecutive passes with an unchanged count
    std::unordered_map<std::wstring, Ref> byKey_;
    std::vector<std::pair<std::wstring, Ref>> scanFound_;  // hub-pass scratch (GT-only)
    uint32_t indexGen_ = 0;   // world gen of the last completed hub pass (GT-write, GT-read)
    bool scanRegistered_ = false;

    std::mutex stateMutex_;
    std::unordered_map<std::wstring, bool> lastKnown_;
    std::unordered_set<std::wstring> sendRefused_;               // GT-only: keys whose refused send is already logged

    std::unordered_map<std::wstring, Pending> pending_;            // GT-only
    std::vector<std::pair<std::wstring, Ref>> pollScratch_;       // GT-only: reused poll snapshot buffer
    std::chrono::steady_clock::time_point lastRetry_{};           // GT-only
    size_t lastLogCount_ = SIZE_MAX;                              // GT-only: dedup the rebuilt log
    uint64_t lastLogHash_ = 0;                                   // GT-only
};

}  // namespace coop::interactable_sync
