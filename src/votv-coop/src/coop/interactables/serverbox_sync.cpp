// coop/interactables/serverbox_sync.cpp -- see coop/interactables/serverbox_sync.h.
//
// Measured ground truth (RE 2026-07-09, research/findings/votv-notifications-suppress-mirror-DESIGN-2026-07-09.md):
//   M1: breakServer/fix/break_type are EX_LocalVirtualFunction -> invisible to BOTH our seams -> we can
//       NOT intercept the verb; we mirror STATE + drive the box's own check() ourselves (reflected).
//   M2: serverBox_C.check() re-skins PURELY from the raw IsBroken bool (reads it, drives SetMaterial/
//       SetActive) -- notify-free (no delegate/minigame), unlike the verbs. So raw-write IsBroken +
//       reflected check() = a clean host-authoritative apply.
//   Offsets (shipped CXXHeaderDump, resolved-by-name here + logged to verify): serverBox.IsBroken@0x378;
//       mainGamemode.servers@0x3F0 (TArray<serverBox*>), serverEfficiency_calc@0x400/_downl@0x404,
//       brokenServers@0x8A0.

#include "coop/interactables/serverbox_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"  // kMaxPeers

#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"                 // SetActorTickEnabled (breaker-kill)
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

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

std::atomic<coop::net::Session*> g_session{nullptr};

constexpr int      kMaxServers    = 64;    // the isBrokenMask width; VOTV has a handful
constexpr long long kPollIntervalMs = 1000;

// ---- resolution (lazy, 2 s retry, capped LOUD latch -- the alarm_sync/event_active pattern) --------
void*   g_gmCls = nullptr;        // mainGamemode_C
int32_t g_offServers  = -1;       // TArray<serverBox_C*>
int32_t g_offBroken   = -1;       // int32 brokenServers
int32_t g_offEffCalc  = -1;       // float serverEfficiency_calc
int32_t g_offEffDownl = -1;       // float serverEfficiency_downl
void*   g_sbCls = nullptr;        // serverBox_C
int32_t g_offIsBroken = -1;       // bool IsBroken (byte offset)
uint8_t g_maskIsBroken = 0;       // ...and its FBoolProperty real bit
void*   g_checkFn = nullptr;      // serverBox_C::check() (no params)
std::chrono::steady_clock::time_point g_nextResolve{};
int  g_postClassAttempts = 0;
bool g_resolveLatched = false;
constexpr int kMaxPostClassAttempts = 5;

bool Resolved() {
    return g_offServers >= 0 && g_offBroken >= 0 && g_offEffCalc >= 0 && g_offEffDownl >= 0 &&
           g_offIsBroken >= 0 && g_maskIsBroken != 0 && g_checkFn != nullptr;
}

void ResolvePass() {
    if (g_resolveLatched) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextResolve) return;
    g_nextResolve = now + std::chrono::seconds(2);
    if (!g_gmCls) g_gmCls = R::FindClass(L"mainGamemode_C");
    if (!g_sbCls) g_sbCls = R::FindClass(L"serverBox_C");
    if (!g_gmCls || !g_sbCls) return;  // world not loaded yet
    if (g_offServers  < 0) g_offServers  = R::FindPropertyOffset(g_gmCls, L"servers");
    if (g_offBroken   < 0) g_offBroken   = R::FindPropertyOffset(g_gmCls, L"brokenServers");
    if (g_offEffCalc  < 0) g_offEffCalc  = R::FindPropertyOffset(g_gmCls, L"serverEfficiency_calc");
    if (g_offEffDownl < 0) g_offEffDownl = R::FindPropertyOffset(g_gmCls, L"serverEfficiency_downl");
    if (g_offIsBroken < 0) R::FindBoolProperty(g_sbCls, L"IsBroken", g_offIsBroken, g_maskIsBroken);
    if (!g_checkFn) g_checkFn = R::FindFunction(g_sbCls, L"check");
    if (Resolved()) {
        g_resolveLatched = true;
        UE_LOGI("serverbox_sync: resolved (servers=0x%X broken=0x%X eff=0x%X/0x%X IsBroken=0x%X mask=0x%02X "
                "check=yes)", g_offServers, g_offBroken, g_offEffCalc, g_offEffDownl, g_offIsBroken,
                g_maskIsBroken);
        return;
    }
    if (++g_postClassAttempts >= kMaxPostClassAttempts) {
        g_resolveLatched = true;
        UE_LOGW("serverbox_sync: resolution INCOMPLETE after %d passes (servers=0x%X broken=0x%X "
                "IsBroken=0x%X check=%s) -- latched OFF; game version mismatch?",
                g_postClassAttempts, g_offServers, g_offBroken, g_offIsBroken, g_checkFn ? "yes" : "no");
    }
}

// ---- live mainGamemode (cached, revalidated by internal index -- recycled-slot-safe) --------------
void* g_gm = nullptr;
int32_t g_gmIdx = -1;

void* Gamemode() {
    if (!g_gm || !R::IsLiveByIndex(g_gm, g_gmIdx)) {
        g_gm = R::FindObjectByClass(L"mainGamemode_C");
        g_gmIdx = g_gm ? R::InternalIndexOf(g_gm) : -1;
    }
    return g_gm;
}

// Raw TArray header (UE4 layout: Data ptr, Num, Max).
struct FArrayRaw { void* Data; int32_t Num; int32_t Max; };

// Read servers[] into `out` (bounded to kMaxServers). Returns the true Num (may exceed kMaxServers).
int32_t ReadServers(void* gm, std::vector<void*>& out) {
    out.clear();
    if (!gm || g_offServers < 0) return 0;
    const FArrayRaw* arr = reinterpret_cast<const FArrayRaw*>(reinterpret_cast<uint8_t*>(gm) + g_offServers);
    const int32_t num = arr->Num;
    if (!arr->Data || num <= 0) return num;
    void* const* elems = reinterpret_cast<void* const*>(arr->Data);
    const int32_t take = num < kMaxServers ? num : kMaxServers;
    for (int32_t i = 0; i < take; ++i) out.push_back(elems[i]);
    return num;
}

bool ReadIsBroken(void* sb) {
    if (!sb || g_offIsBroken < 0) return false;
    const uint8_t b = *(reinterpret_cast<const uint8_t*>(sb) + g_offIsBroken);
    return (b & g_maskIsBroken) != 0;
}

void WriteIsBroken(void* sb, bool broken) {
    if (!sb || g_offIsBroken < 0) return;   // symmetric with ReadIsBroken (audit LOW-2)
    uint8_t* p = reinterpret_cast<uint8_t*>(sb) + g_offIsBroken;
    if (broken) *p |= g_maskIsBroken;
    else        *p &= static_cast<uint8_t>(~g_maskIsBroken);
}

int32_t ReadInt(void* gm, int32_t off) { return *reinterpret_cast<const int32_t*>(reinterpret_cast<uint8_t*>(gm) + off); }
float   ReadFlt(void* gm, int32_t off) { return *reinterpret_cast<const float*>  (reinterpret_cast<uint8_t*>(gm) + off); }
void    WriteInt(void* gm, int32_t off, int32_t v) { if (!gm || off < 0) return; *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(gm) + off) = v; }
void    WriteFlt(void* gm, int32_t off, float v)   { if (!gm || off < 0) return; *reinterpret_cast<float*>  (reinterpret_cast<uint8_t*>(gm) + off) = v; }

// Build the current server-state snapshot from the live gamemode. Returns false if not readable yet.
bool ReadState(coop::net::ServerStatePayload& p) {
    void* gm = Gamemode();
    if (!gm) return false;
    std::vector<void*> servers;
    const int32_t num = ReadServers(gm, servers);
    if (num > kMaxServers) {
        static bool warned = false;
        if (!warned) { warned = true; UE_LOGW("serverbox_sync: %d servers > cap %d -- syncing first %d only",
                                              num, kMaxServers, kMaxServers); }
    }
    uint64_t mask = 0;
    for (size_t i = 0; i < servers.size(); ++i) {
        void* sb = servers[i];
        if (sb && R::IsLive(sb) && ReadIsBroken(sb)) mask |= (1ull << i);
    }
    p.brokenServers = ReadInt(gm, g_offBroken);
    p.effCalc  = ReadFlt(gm, g_offEffCalc);
    p.effDownl = ReadFlt(gm, g_offEffDownl);
    p.serverCount = static_cast<uint8_t>(servers.size());
    p.isBrokenMask = mask;
    return true;
}

// ---- host poll baseline ---------------------------------------------------------------------------
void* g_polledGm = nullptr;
bool  g_primed = false;
uint64_t g_lastMask = 0;
int32_t  g_lastBroken = 0;
float    g_lastEffCalc = 0.f, g_lastEffDownl = 0.f;
long long g_lastPollMs = 0;

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Break/fix mask + brokenServers are the primary edge; efficiency is ALSO in the payload and CAN vary
// off a break/fix edge (a download ramps serverEfficiency_downl), so include it with a small epsilon so
// the client's SAT-console sv.*/tw.* reads stay fresh (audit MEDIUM-2). Bounded by the 1 Hz poll.
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
    void* gm = Gamemode();
    if (!gm || !g_checkFn) return;
    // Aggregate mirror (so the SAT-console sv.*/tw.* queries read TRUE host state).
    WriteInt(gm, g_offBroken, p.brokenServers);
    WriteFlt(gm, g_offEffCalc, p.effCalc);
    WriteFlt(gm, g_offEffDownl, p.effDownl);
    std::vector<void*> servers;
    ReadServers(gm, servers);
    int applied = 0;
    const int32_t n = static_cast<int32_t>(servers.size());
    const int32_t take = n < p.serverCount ? n : p.serverCount;
    for (int32_t i = 0; i < take && i < kMaxServers; ++i) {
        void* sb = servers[i];
        if (!sb || !R::IsLive(sb)) continue;
        const bool desired = (p.isBrokenMask >> i) & 1ull;
        if (ReadIsBroken(sb) == desired) continue;   // already matches -> no re-skin
        WriteIsBroken(sb, desired);
        ue_wrap::ParamFrame frame(g_checkFn);         // check() re-skins from the raw IsBroken (M2)
        if (frame.valid()) ue_wrap::Call(sb, frame);
        ++applied;
    }
    if (applied)
        UE_LOGI("serverbox_sync: client applied host state (broken=%d mask=0x%llX, %d server(s) re-skinned)",
                p.brokenServers, p.isBrokenMask, applied);
}

// ---- client breaker-kill: neutralize the local ticker_serverBreaker (disable its actor tick, the
// autonomous false-break source). One-shot latch on the first successful kill (idempotent). NOTE (audit
// LOW-1): there is NO re-arm -- a breaker respawned after the latch ticks autonomously until the next
// host mirror overwrites its break; a world with NO breaker instance re-walks at 1 Hz until one exists
// (alarm_sync-parity; in practice a breaker exists whenever servers do -> latches in 1-2 ticks). ------
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
    ResolvePass();
    if (!Resolved()) return;

    if (s->role() != coop::net::Role::Host) {
        // CLIENT: state is push-only (OnReliable). Keep the local autonomous breaker neutralized.
        KillLocalBreaker();
        return;
    }

    // HOST: poll -> broadcast on change.
    void* gm = Gamemode();
    if (!gm) return;
    coop::net::ServerStatePayload p{};
    if (!ReadState(p)) return;
    // World/save reload minted a new gamemode -> baseline meaningless; re-prime silently (a prime must
    // never masquerade as an edge; the join-edge + the next real transition deliver state).
    if (gm != g_polledGm || !g_primed) {
        g_polledGm = gm; g_primed = true; UpdateBaseline(p);
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
    if (!Resolved()) return;  // no world yet -> the first transition delivers state
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
    ResolvePass();
    if (!Resolved()) {
        UE_LOGW("serverbox_sync: ServerState arrived before resolution -- dropped (the next host change / "
                "connect-snapshot re-delivers)");
        return;
    }
    ApplyState(payload);
    KillLocalBreaker();  // ensure the local breaker stays off (join-window timing)
}

void OnDisconnect() {
    g_gm = nullptr; g_gmIdx = -1;
    g_polledGm = nullptr; g_primed = false;
    g_lastMask = 0; g_lastBroken = 0; g_lastPollMs = 0;
    g_breakerKilled = false;
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::serverbox_sync
