// coop/interactables/serverbox_sync.cpp -- see coop/interactables/serverbox_sync.h.
//
// The verbs cannot be intercepted: breakServer and fix are dispatched as EX_LocalVirtualFunction,
// which the ProcessEvent detour and the native seam never see and the bytecode seam can only
// observe. So we mirror STATE and drive the box's own re-skin instead, through
// ue_wrap/devices/serverbox: the engine's break state -- the box's IsBroken, the notify-free
// check() it re-skins from, and the farm's three totals -- is the wrapper's, and this lane owns
// only the wire half, which is the mask, its width, the poll, and who may author it.

#include "coop/interactables/serverbox_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"  // kMaxPeers

#include "ue_wrap/engine/engine.h"                 // SetActorTickEnabled (breaker-kill)
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/serverbox.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

namespace coop::serverbox_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;
namespace SB = ue_wrap::serverbox;

std::atomic<coop::net::Session*> g_session{nullptr};

constexpr int      kMaxServers    = 64;    // the isBrokenMask width; a base runs ~54 boxes
constexpr long long kPollIntervalMs = 1000;

// The box list has one owner, ue_wrap/devices/serverbox: this lane keeps only its own cap, since
// the mask it broadcasts is that many bits wide.
int32_t ReadServers(std::vector<void*>& out) {
    out.clear();
    const int32_t num = static_cast<int32_t>(SB::ReadServers(out));
    if (num > kMaxServers) out.resize(kMaxServers);
    return num;
}

// Build the current server-state snapshot from the live gamemode. Returns false if not readable yet.
bool ReadState(coop::net::ServerStatePayload& p) {
    SB::Aggregates agg;
    if (!SB::ReadAggregates(agg)) return false;
    std::vector<void*> servers;
    const int32_t num = ReadServers(servers);
    if (num > kMaxServers) {
        static bool warned = false;
        if (!warned) { warned = true; UE_LOGW("serverbox_sync: %d servers > cap %d -- syncing first %d only",
                                              num, kMaxServers, kMaxServers); }
    }
    uint64_t mask = 0;
    for (size_t i = 0; i < servers.size(); ++i) {
        void* sb = servers[i];
        if (sb && R::IsLive(sb) && SB::ReadIsBroken(sb)) mask |= (1ull << i);
    }
    p.brokenServers = agg.brokenServers;
    p.effCalc  = agg.efficiencyCalc;
    p.effDownl = agg.efficiencyDownload;
    p.serverCount = static_cast<uint8_t>(servers.size());
    p.isBrokenMask = mask;
    return true;
}

// ---- host poll baseline ---------------------------------------------------------------------------
uint32_t g_polledGmGeneration = 0;
bool  g_primed = false;
uint64_t g_lastMask = 0;
int32_t  g_lastBroken = 0;
float    g_lastEffCalc = 0.f, g_lastEffDownl = 0.f;
long long g_lastPollMs = 0;

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Break/fix mask + brokenServers are the primary edge; efficiency is ALSO in the payload and CAN
// vary off a break/fix edge (a download ramps serverEfficiency_downl), so include it with a small
// epsilon and the client's SAT-console sv.*/tw.* reads stay fresh. Bounded by the 1 Hz poll.
bool StateChanged(const coop::net::ServerStatePayload& p) {
    return p.isBrokenMask != g_lastMask || p.brokenServers != g_lastBroken ||
           std::fabs(p.effCalc  - g_lastEffCalc)  > 0.005f ||
           std::fabs(p.effDownl - g_lastEffDownl) > 0.005f;
}
void UpdateBaseline(const coop::net::ServerStatePayload& p) {
    g_lastMask = p.isBrokenMask; g_lastBroken = p.brokenServers;
    g_lastEffCalc = p.effCalc; g_lastEffDownl = p.effDownl;
}

// ---- client apply (drive-real) --------------------------------------------------------------------
void ApplyState(const coop::net::ServerStatePayload& p) {
    // Aggregate mirror (so the SAT-console sv.*/tw.* queries read TRUE host state).
    SB::Aggregates agg;
    agg.brokenServers      = p.brokenServers;
    agg.efficiencyCalc     = p.effCalc;
    agg.efficiencyDownload = p.effDownl;
    if (!SB::WriteAggregates(agg)) return;
    std::vector<void*> servers;
    ReadServers(servers);
    int applied = 0;
    const int32_t n = static_cast<int32_t>(servers.size());
    const int32_t take = n < p.serverCount ? n : p.serverCount;
    for (int32_t i = 0; i < take && i < kMaxServers; ++i) {
        void* sb = servers[i];
        if (!sb || !R::IsLive(sb)) continue;
        const bool desired = (p.isBrokenMask >> i) & 1ull;
        if (SB::ReadIsBroken(sb) == desired) continue;   // already matches -> no re-skin
        if (SB::ApplyBreak(sb, desired)) ++applied;
    }
    if (applied)
        UE_LOGI("serverbox_sync: client applied host state (broken=%d mask=0x%llX, %d server(s) re-skinned)",
                p.brokenServers, p.isBrokenMask, applied);
}

// client breaker-kill: neutralize the local ticker_serverBreaker (disable its actor tick, the
// autonomous false-break source). One-shot latch on the first successful kill (idempotent). There
// is NO re-arm -- a breaker respawned after the latch ticks autonomously until the next host mirror
// overwrites its break; a world with NO breaker instance re-walks at 1 Hz until one exists
// (alarm_sync-parity; in practice a breaker exists whenever servers do, so it latches within a tick
// or two).
bool g_breakerKilled = false;

void KillLocalBreaker() {
    if (g_breakerKilled) return;
    int killed = 0;
    for (void* obj : R::FindObjectsByClass(L"ticker_serverBreaker_C")) {
        if (!obj || !R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
        if (E::SetActorTickEnabled(obj, false)) ++killed;
    }
    if (killed > 0) {
        g_breakerKilled = true;
        UE_LOGI("serverbox_sync: CLIENT neutralized %d ticker_serverBreaker (tick disabled -- no autonomous "
                "self-break; host state is authoritative)", killed);
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    if (!GT::IsGameThread()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    const long long now = NowMs();
    if (now - g_lastPollMs < kPollIntervalMs) return;
    g_lastPollMs = now;
    if (!SB::EnsureBreakResolved()) return;

    if (s->role() != coop::net::Role::Host) {
        // CLIENT: state is push-only (OnReliable). Keep the local autonomous breaker neutralized.
        KillLocalBreaker();
        return;
    }

    // HOST: poll -> broadcast on change.
    coop::net::ServerStatePayload p{};
    if (!ReadState(p)) return;
    // World/save reload minted a new gamemode -> baseline meaningless; re-prime silently (a prime must
    // never masquerade as an edge; the join-edge + the next real transition deliver state).
    const uint32_t gen = SB::GamemodeGeneration();
    if (gen != g_polledGmGeneration || !g_primed) {
        g_polledGmGeneration = gen; g_primed = true; UpdateBaseline(p);
        return;
    }
    if (!StateChanged(p)) return;
    UpdateBaseline(p);
    if (s->SendReliable(coop::net::ReliableKind::ServerState, &p, sizeof(p)))
        UE_LOGI("serverbox_sync: host broadcast (broken=%d mask=0x%llX count=%d)",
                p.brokenServers, p.isBrokenMask, p.serverCount);
    else
        UE_LOGW("serverbox_sync: host broadcast send FAILED (broken=%d mask=0x%llX)", p.brokenServers,
                p.isBrokenMask);
}

void QueueConnectBroadcastForSlot(int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return;
    if (!SB::EnsureBreakResolved()) return;  // no world yet -> the first transition delivers state
    coop::net::ServerStatePayload p{};
    if (!ReadState(p)) return;
    // Unconditional (even all-healthy): the joiner's own sim may have diverged before the mirror lands.
    if (s->SendReliableToSlot(slot, coop::net::ReliableKind::ServerState, &p, sizeof(p)))
        UE_LOGI("serverbox_sync: connect-snapshot -- sent (broken=%d mask=0x%llX) to slot %d",
                p.brokenServers, p.isBrokenMask, slot);
    else
        UE_LOGW("serverbox_sync: connect-snapshot to slot %d send FAILED", slot);
}

void OnReliable(const coop::net::ServerStatePayload& payload, int senderPeerSlot) {
    if (!GT::IsGameThread()) {
        UE_LOGW("serverbox_sync: OnReliable off-game-thread -- dropping");
        return;
    }
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (s->role() == coop::net::Role::Host) {
        UE_LOGW("serverbox_sync: ServerState received on the HOST (host-authoritative one-directional) -- "
                "dropping from slot=%d", senderPeerSlot);
        return;
    }
    if (senderPeerSlot != 0) {
        UE_LOGW("serverbox_sync: ServerState from non-host senderPeerSlot=%d -- dropping", senderPeerSlot);
        return;
    }
    if (!SB::EnsureBreakResolved()) {
        UE_LOGW("serverbox_sync: ServerState arrived before resolution -- dropped (the next host change / "
                "connect-snapshot re-delivers)");
        return;
    }
    ApplyState(payload);
    KillLocalBreaker();  // ensure the local breaker stays off (join-window timing)
}

void OnDisconnect() {
    g_polledGmGeneration = 0; g_primed = false;
    g_lastMask = 0; g_lastBroken = 0; g_lastPollMs = 0;
    // Restore the neutralized breaker: KillLocalBreaker disabled the actor tick, and resetting only
    // the latch left servers permanently unbreakable in the SAME process after the session (solo
    // play, or re-hosting) -- the event_fire_sync restore precedent applies here identically. The
    // fanout runs on the game thread (net_pump teardown; wisp_grab_hold dispatches UFunctions from
    // the same fanout). Re-walk rather than a stored pointer: restoring EVERY live breaker instance
    // is idempotent and also covers a breaker respawned after the kill.
    if (g_breakerKilled && GT::IsGameThread()) {
        int restored = 0;
        for (void* obj : R::FindObjectsByClass(L"ticker_serverBreaker_C")) {
            if (!obj || !R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
            if (E::SetActorTickEnabled(obj, true)) ++restored;
        }
        if (restored > 0)
            UE_LOGI("serverbox_sync: restored %d ticker_serverBreaker on session end (local sim resumes)",
                    restored);
    }
    g_breakerKilled = false;
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::serverbox_sync
