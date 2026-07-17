// coop/device_occupancy.cpp -- see coop/device_occupancy.h.

#include "coop/interactables/device_occupancy.h"

#include "coop/interactables/desk_input_sync.h"  // v116: PingActiveSlot -> the desk FSM-hold
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_sound.h"

#include "ue_wrap/desk/device_screen.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::device_occupancy {
namespace {

namespace R  = ue_wrap::reflection;
namespace P  = ue_wrap::profile;
namespace DS = ue_wrap::device_screen;
namespace GT = ue_wrap::game_thread;

using coop::net::WireKeyFromString;
using coop::net::StringFromWireKey;

std::atomic<coop::net::Session*> g_session{nullptr};

// The busy table: claim key -> holding peer slot. On the HOST this is the
// authoritative arbitration table; on a CLIENT it is the host-broadcast
// mirror the deny gate reads. All access is game-thread-serial (pump tick /
// event_feed drain / PE observers); the mutex is defensive (turbine shape).
std::mutex g_mutex;
std::unordered_map<std::wstring, uint8_t> g_busy;

// Local claim edge state (GT-only). g_localWidget is the activeInterface
// pointer last seen (edge detector); g_localKey the claim key we currently
// hold/requested (empty = none). g_pendingSend holds a claim that could not
// ship yet (slot unassigned / send refused) -- retried each Tick; cleared by
// the falling edge so a stale claim never ships after exit.
void* g_localWidget = nullptr;
std::wstring g_localKey;
bool g_pendingSend = false;

// Deny-gate dispatch state (GT-only; PRE/POST pair within one dispatch).
bool g_denyPending = false;
std::wstring g_denyKey;  // for the log line only
std::chrono::steady_clock::time_point g_lastDenySound{};
constexpr auto kDenyDebounce = std::chrono::milliseconds(300);  // press+release double-fire

bool g_observersInstalled = false;

// v116: the desk FSM-hold (HOST-only author). While a peer's ping FSM runs
// (desk_input_sync::PingActiveSlot), the desk is functionally that peer's even
// after it physically dismounts (the FSM keeps consuming the committed dots
// for tens of seconds) -- so the host keeps a REAL busy-table entry for the
// pinger. Every existing surface then does the work: BusyByOther's local
// immediate-deny + ForceExitLocal, the arbitration's held-by deny, OnDishAim's
// holder gate (a non-holder's aim never ships -> the presser's dots can't be
// stomped mid-FSM). This replaces the native OnKeyDown coord_isPing swallow
// that the retired v112 raw flag write used to provide by accident -- the raw
// write is gone because it WOKE the phantom FSM (the v116 root).
// g_deskFsmHold marks the entry as FSM-authored: a physical grant of the desk
// clears it (the entry now belongs to the sitting player); a physical release
// mid-FSM is re-asserted by the reconciler on the next tick.
const wchar_t* const kDeskClaim = L"desk";
bool g_deskFsmHold = false;
std::chrono::steady_clock::time_point g_nextFsmHoldPoll{};

void SendClaim(coop::net::Session* s, const std::wstring& key, uint8_t slot, bool busy);  // fwd

void ReconcileDeskFsmHold(coop::net::Session* s) {
    if (s->role() != coop::net::Role::Host || !s->connected()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextFsmHoldPoll) return;
    g_nextFsmHoldPoll = now + std::chrono::milliseconds(250);

    const uint8_t pinger = coop::desk_input_sync::PingActiveSlot();
    if (pinger != 0xFF) {
        bool inserted = false;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            auto it = g_busy.find(kDeskClaim);
            if (it == g_busy.end()) {
                g_busy[kDeskClaim] = pinger;
                inserted = true;
            }
            // held by someone else: a physical holder won a race window --
            // leave it (never force-exit a sitting player from here).
        }
        if (inserted) {
            g_deskFsmHold = true;
            SendClaim(s, kDeskClaim, pinger, true);
            UE_LOGI("device_occupancy: desk FSM-hold asserted for pinging slot %u",
                    static_cast<unsigned>(pinger));
        }
    } else if (g_deskFsmHold) {
        // The ping ended (or its setter left) while OUR fsm entry stands:
        // release it. A physical grant in between cleared g_deskFsmHold, so a
        // sitting player's claim is never torn down here.
        uint8_t held = 0xFF;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            auto it = g_busy.find(kDeskClaim);
            if (it != g_busy.end()) { held = it->second; g_busy.erase(it); }
        }
        g_deskFsmHold = false;
        if (held != 0xFF) {
            SendClaim(s, kDeskClaim, held, false);
            UE_LOGI("device_occupancy: desk FSM-hold released (ping ended, slot %u)",
                    static_cast<unsigned>(held));
        }
    }
}

uint8_t LocalSlot() {
    return coop::players::Registry::Get().LocalPeerId();
}

bool BusyByOther(const std::wstring& key, uint8_t mySlot, uint8_t* holder) {
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_busy.find(key);
    if (it == g_busy.end()) return false;
    if (holder) *holder = it->second;
    return it->second != mySlot;
}

void SendClaim(coop::net::Session* s, const std::wstring& key, uint8_t slot, bool busy) {
    coop::net::DeviceClaimPayload p{};
    WireKeyFromString(key, p.key);
    p.slot = slot;
    p.busy = busy ? 1 : 0;
    if (s->role() == coop::net::Role::Host) {
        s->SendReliable(coop::net::ReliableKind::DeviceClaim, &p, sizeof(p));  // broadcast
    } else {
        s->SendReliableToSlot(0, coop::net::ReliableKind::DeviceClaim, &p, sizeof(p));
    }
}

// Force the LOCAL player out of its current interface + deny click. Clears
// the local claim state FIRST so the resulting activeInterface falling edge
// (next Tick) sees an empty key and does NOT send a release for a claim we
// never owned.
void ForceExitLocal(void* local, const std::wstring& key, uint8_t holder) {
    g_localKey.clear();
    g_pendingSend = false;
    if (DS::ForceExitInterface(local)) {
        UE_LOGI("device_occupancy: FORCE-EXIT local from '%ls' (held by slot %u)",
                key.c_str(), static_cast<unsigned>(holder));
    } else {
        UE_LOGW("device_occupancy: force-exit dispatch failed for '%ls' -- player keeps "
                "the screen until its own exit (claim stays with slot %u)",
                key.c_str(), static_cast<unsigned>(holder));
    }
    coop::prop_sound::PlayDenyClick(local);
}

// ---- the InpActEvt_use deny gate (PRE clears the aim, POST restores) -------
// Both roles: ANY peer aiming E at a device the wire says another peer holds
// gets the native chain no-opped + the deny click. Door-precedent discipline:
// leak-restore first in PRE (a SEH-faulted dispatch skips POST), restore
// FIRST on every POST path.

void OnUseInputPre(void* self, void*, void*) {
    if (DS::HasClearedAim()) DS::RestoreAim();  // leak-heal (door audit IMP-3)
    if (!self) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    if (!DS::EnsureResolved()) return;
    // Only the LOCAL player processes input (puppets are unpossessed), so
    // `self` is the local mainPlayer_C by construction.
    void* aimed = ue_wrap::engine::ReadMainPlayerLookAtActor(self);
    if (!aimed) return;
    const std::wstring key = DS::ClassifyDeviceActorClaimKey(aimed);
    if (key.empty()) return;  // not aiming at an enterable device
    uint8_t holder = 0xFF;
    if (!BusyByOther(key, LocalSlot(), &holder)) return;  // free or our own
    if (DS::ClearAimForDispatch(self)) {
        g_denyPending = true;
        g_denyKey = key;
    }
}

void OnUseInputPost(void* self, void*, void*) {
    // Restore FIRST -- every exit path must leave the aim fields intact.
    if (DS::HasClearedAim()) DS::RestoreAim();
    if (!g_denyPending) return;
    g_denyPending = false;
    const auto now = std::chrono::steady_clock::now();
    if (now - g_lastDenySound < kDenyDebounce) return;  // press+release = one click
    g_lastDenySound = now;
    UE_LOGI("device_occupancy: DENIED E at busy device '%ls'", g_denyKey.c_str());
    if (self) coop::prop_sound::PlayDenyClick(self);
}

void InstallObservers() {
    if (g_observersInstalled) return;
    void* playerCls = R::FindClass(P::name::MainPlayerClass);
    if (!playerCls) return;  // retry until mainPlayer_C loads
    void* fn = R::FindFunction(playerCls, P::name::MainPlayerUseInputEventFn);
    if (!fn) {
        UE_LOGW("device_occupancy: InpActEvt_use UFunction not found -- busy devices "
                "cannot be denied on this peer");
        g_observersInstalled = true;  // don't retry forever
        return;
    }
    if (!GT::RegisterPreObserver(fn, &OnUseInputPre)) {
        UE_LOGW("device_occupancy: InpActEvt_use PRE observer register failed");
        return;
    }
    if (!GT::RegisterPostObserver(fn, &OnUseInputPost)) {
        UE_LOGW("device_occupancy: InpActEvt_use POST observer register failed");
        return;
    }
    g_observersInstalled = true;
    UE_LOGI("device_occupancy: InpActEvt_use PRE+POST deny gate installed (both roles)");
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    InstallObservers();
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    // v116: the desk FSM-hold reconciler runs regardless of the widget layer
    // (a REMOTE ping needs no local screen resolved).
    ReconcileDeskFsmHold(s);
    if (!DS::EnsureResolved()) return;
    void* local = coop::players::Registry::Get().Local();
    if (!local) {
        // World down (level transition / flee). If we held a claim, release it
        // -- the screen is gone with the world. The HOST's table erase is
        // UNCONDITIONAL (audit CRIT-1: gating it on connected() let a solo
        // host strand its own claim across a world transition -- the stale
        // entry then shipped in the connect snapshot and denied every later
        // joiner until session end); only the SEND needs a connection.
        if (!g_localKey.empty()) {
            if (s->role() == coop::net::Role::Host) {
                std::lock_guard<std::mutex> lk(g_mutex);
                g_busy.erase(g_localKey);
            }
            if (s->connected()) SendClaim(s, g_localKey, LocalSlot(), false);
        }
        g_localKey.clear();
        g_localWidget = nullptr;
        g_pendingSend = false;
        return;
    }

    const uint8_t mySlot = LocalSlot();

    // Pending claim retry: the rising edge fired before the slot was assigned
    // or before the channel accepted the send (menu-window client). The
    // falling edge clears this, so a retry can never ship a stale claim.
    if (g_pendingSend && !g_localKey.empty() && mySlot != 0xFF && s->connected()) {
        SendClaim(s, g_localKey, mySlot, true);
        g_pendingSend = false;
        UE_LOGI("device_occupancy: pending claim '%ls' sent (slot %u)",
                g_localKey.c_str(), static_cast<unsigned>(mySlot));
    }

    void* w = DS::ReadActiveInterface(local);
    if (w == g_localWidget) return;  // no edge -- the per-tick steady state

    // FALLING edge (or direct widget switch): release what we held.
    if (!g_localKey.empty()) {
        UE_LOGI("device_occupancy: local EXIT '%ls' -- releasing", g_localKey.c_str());
        if (s->role() == coop::net::Role::Host) {
            {
                std::lock_guard<std::mutex> lk(g_mutex);
                g_busy.erase(g_localKey);
            }
            if (s->connected()) SendClaim(s, g_localKey, mySlot, false);
        } else if (s->connected()) {
            SendClaim(s, g_localKey, mySlot, false);
        }
        g_localKey.clear();
        g_pendingSend = false;
    }

    // RISING edge: classify + claim.
    if (w) {
        const std::wstring key = DS::ClassifyWidgetClaimKey(w);
        if (!key.empty()) {
            uint8_t holder = 0xFF;
            if (BusyByOther(key, mySlot, &holder)) {
                // Known-busy locally: lose immediately, no round-trip.
                UE_LOGI("device_occupancy: entered '%ls' but slot %u holds it -- immediate deny",
                        key.c_str(), static_cast<unsigned>(holder));
                ForceExitLocal(local, key, holder);
                g_localWidget = DS::ReadActiveInterface(local);  // null after force-exit
                return;
            }
            g_localKey = key;
            if (s->role() == coop::net::Role::Host) {
                // Host arbitrates its own claim directly. (v116: a physical
                // self-grant of the desk supersedes an FSM-hold entry -- only
                // reachable when the host itself is the pinger, else
                // BusyByOther already denied above.)
                if (key == kDeskClaim) g_deskFsmHold = false;
                {
                    std::lock_guard<std::mutex> lk(g_mutex);
                    g_busy[key] = mySlot;
                }
                if (s->connected()) SendClaim(s, key, mySlot, true);
                UE_LOGI("device_occupancy: HOST claimed '%ls'", key.c_str());
            } else if (mySlot != 0xFF && s->connected()) {
                SendClaim(s, key, mySlot, true);  // optimistic: stay in, host arbitrates
                UE_LOGI("device_occupancy: CLIENT claim '%ls' requested (slot %u, optimistic)",
                        key.c_str(), static_cast<unsigned>(mySlot));
            } else {
                g_pendingSend = true;  // slot unassigned / not connected yet -- retry above
            }
        }
    }
    g_localWidget = w;
}

void OnReliable(const coop::net::DeviceClaimPayload& p, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    const std::wstring key = StringFromWireKey(p.key);
    if (key.empty()) return;
    if (p.busy != 0 && p.busy != 1) return;

    if (s->role() == coop::net::Role::Host) {
        // Arbitration. Trust the TRANSPORT sender slot, not the payload slot
        // (a client cannot claim on another's behalf).
        if (senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) {
            UE_LOGW("device_occupancy: claim from invalid senderSlot=%u -- dropping",
                    static_cast<unsigned>(senderSlot));
            return;
        }
        if (p.busy) {
            uint8_t cur = 0xFF;
            bool free = true;
            {
                std::lock_guard<std::mutex> lk(g_mutex);
                auto it = g_busy.find(key);
                if (it != g_busy.end()) { cur = it->second; free = false; }
                if (free || cur == senderSlot) g_busy[key] = senderSlot;
            }
            if (free || cur == senderSlot) {
                // v116: a physical grant of the desk supersedes an FSM-hold
                // entry (the pinger sat back down) -- the entry is theirs now.
                if (g_deskFsmHold && key == kDeskClaim) g_deskFsmHold = false;
                UE_LOGI("device_occupancy: HOST grants '%ls' to slot %u",
                        key.c_str(), static_cast<unsigned>(senderSlot));
                SendClaim(s, key, senderSlot, true);  // broadcast the verdict (acks the winner)
            } else {
                // Held by someone else: point-to-point loss notice to the
                // requester only (the table is unchanged for everyone else).
                UE_LOGI("device_occupancy: HOST denies '%ls' to slot %u (held by %u)",
                        key.c_str(), static_cast<unsigned>(senderSlot),
                        static_cast<unsigned>(cur));
                coop::net::DeviceClaimPayload reply{};
                WireKeyFromString(key, reply.key);
                reply.slot = cur;
                reply.busy = 1;
                s->SendReliableToSlot(senderSlot, coop::net::ReliableKind::DeviceClaim,
                                      &reply, sizeof(reply));
            }
        } else {
            bool held = false;
            {
                std::lock_guard<std::mutex> lk(g_mutex);
                auto it = g_busy.find(key);
                if (it != g_busy.end() && it->second == senderSlot) {
                    g_busy.erase(it);
                    held = true;
                }
            }
            if (held) {
                UE_LOGI("device_occupancy: slot %u released '%ls'",
                        static_cast<unsigned>(senderSlot), key.c_str());
                SendClaim(s, key, senderSlot, false);  // broadcast the release
            }
        }
        return;
    }

    // CLIENT: mirror the host's verdicts. Trust-gate to the host (slot 0) --
    // claims are never client-relayed, so any other sender is bogus.
    if (senderSlot != 0) {
        UE_LOGW("device_occupancy: DeviceClaim from non-host senderSlot=%u -- dropping",
                static_cast<unsigned>(senderSlot));
        return;
    }
    const uint8_t mySlot = LocalSlot();
    if (p.busy) {
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_busy[key] = p.slot;
        }
        // Lost the race? We are inside that device but the verdict names
        // another slot -> the game's own forced-exit + the deny click.
        if (p.slot != mySlot && g_localKey == key) {
            void* local = coop::players::Registry::Get().Local();
            if (local) {
                UE_LOGI("device_occupancy: lost '%ls' to slot %u -- force-exiting",
                        key.c_str(), static_cast<unsigned>(p.slot));
                ForceExitLocal(local, key, p.slot);
                g_localWidget = DS::ReadActiveInterface(local);
            } else {
                g_localKey.clear();  // world down -- nothing to exit
                g_pendingSend = false;
            }
        }
    } else {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_busy.find(key);
        if (it != g_busy.end() && it->second == p.slot) g_busy.erase(it);
    }
}

bool LocalHolds(const wchar_t* key) {
    return !g_localKey.empty() && g_localKey == key;
}

uint8_t HolderOf(const wchar_t* key) {
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_busy.find(key);
    return it == g_busy.end() ? 0xFF : it->second;
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    std::vector<std::pair<std::wstring, uint8_t>> items;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        items.reserve(g_busy.size());
        for (auto& kv : g_busy) items.emplace_back(kv.first, kv.second);
    }
    for (auto& it : items) {
        coop::net::DeviceClaimPayload p{};
        WireKeyFromString(it.first, p.key);
        p.slot = it.second;
        p.busy = 1;
        s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::DeviceClaim, &p, sizeof(p));
    }
    if (!items.empty())
        UE_LOGI("device_occupancy: connect-snapshot -- %zu live claim(s) to slot %d",
                items.size(), peerSlot);
}

void OnDisconnectForSlot(int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    std::vector<std::wstring> dropped;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        for (auto it = g_busy.begin(); it != g_busy.end();) {
            if (it->second == static_cast<uint8_t>(slot)) {
                dropped.push_back(it->first);
                it = g_busy.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (dropped.empty()) return;
    UE_LOGI("device_occupancy: slot %d left -- released %zu claim(s)", slot, dropped.size());
    if (s && s->role() == coop::net::Role::Host && s->connected()) {
        for (auto& key : dropped) SendClaim(s, key, static_cast<uint8_t>(slot), false);
    }
}

void OnDisconnect() {
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_busy.clear();
    }
    g_localKey.clear();
    g_localWidget = nullptr;
    g_pendingSend = false;
    g_denyPending = false;
    g_deskFsmHold = false;   // v116
    g_nextFsmHoldPoll = {};  // v116
}

}  // namespace coop::device_occupancy
