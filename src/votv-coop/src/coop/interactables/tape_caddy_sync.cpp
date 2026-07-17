// coop/interactables/tape_caddy_sync.cpp -- see the header + the L7 design doc.
//
// Axes (one owner each):
//   SLOT      -- ReelSlot=102, presser-authored -1.0-sentinel change edges, 4 Hz poll
//                (ONE invariant detector for every native writer: playerUsedOn insert,
//                reelbox throw-in overlap, eject). Apply GT-atomic + PRIME.
//   CORRECTOR -- ReelPose=40, HOST-only 1 Hz while any slot occupied (active or not);
//                client per-channel EXACT-SNAP only when local != -1 AND wire != -1
//                (sentinel transitions live exclusively on the reliable lane) + an
//                IsRecent window after a local insert.
//   PROP BIRTH-- NOT here: PropSpawn.savedScalar + ReelEjectIntent (prop_drop_intent).
//   TOGGLE    -- NOT here: the symmetric ApplianceState lane owns wallunit.Active.

#include "coop/interactables/tape_caddy_sync.h"

#include "coop/net/session.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/desk/tape_caddy.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace coop::tape_caddy_sync {
namespace {

namespace TC = ue_wrap::tape_caddy;
namespace GT = ue_wrap::game_thread;

constexpr uint64_t kSlotPollMs     = 250;   // the 4 Hz sentinel-edge poll
constexpr uint64_t kMissBackoffMs  = 1000;  // unit unresolved/not spawned -> throttle the re-walk
constexpr uint64_t kCorrectorMs    = 1000;  // HOST corrector cadence
constexpr uint64_t kRecentInsertMs = 2000;  // corrector hold-off after a local insert edge
constexpr float    kProgressEps    = 0.01f; // write-if-differs threshold

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_installLogged = false;

// Poll state (game thread only).
bool     g_havePrev = false;
float    g_prevBig = TC::kSlotEmpty, g_prevSmall = TC::kSlotEmpty;
uint64_t g_nextSlotPoll = 0;
uint64_t g_nextCorrector = 0;
uint64_t g_recentInsert[2] = {0, 0};  // [kReelBig, kReelSmall] -- local-insert stamps

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline bool SlotEmpty(float v) { return v < -0.5f; }

// Prime the poll baselines to the CURRENT engine values (one read) so the next
// poll sees no edge. Called inside every wire-apply GT task (apply+prime atomic)
// and after a local edge broadcast (sender primes at send).
void PrimeBaselines() {
    float big, small;
    if (TC::ReadReels(big, small)) {
        g_prevBig = big;
        g_prevSmall = small;
        g_havePrev = true;
    }
}

void SendEdge(coop::net::Session* s, uint8_t reel, uint8_t op, float progress) {
    coop::net::ReelSlotPayload p{};
    p.progress = progress;
    p.reel = reel;
    p.op = op;
    s->SendReliable(coop::net::ReliableKind::ReelSlot, &p, sizeof(p));
    UE_LOGI("[reel] local %s edge reel=%s progress=%.2f -- broadcast",
            op == 0 ? "INSERT" : "EJECT", reel == 0 ? "big" : "small", progress);
}

// Classify one channel's poll delta into an edge event (accrual P->P' is NOT an event).
void CheckEdge(coop::net::Session* s, uint8_t reel, float prev, float cur, uint64_t now) {
    const bool prevEmpty = SlotEmpty(prev);
    const bool curEmpty  = SlotEmpty(cur);
    if (prevEmpty && !curEmpty) {
        SendEdge(s, reel, /*op=*/0, cur);            // INSERT
        g_recentInsert[reel] = now;                  // corrector hold-off (own insert)
    } else if (!prevEmpty && curEmpty) {
        SendEdge(s, reel, /*op=*/1, prev);           // EJECT (progress informational)
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!g_installLogged) {
        g_installLogged = true;
        UE_LOGI("[reel] tape_caddy_sync installed (ReelSlot=102 4 Hz sentinel poll + "
                "ReelPose=40 host 1 Hz corrector; client accrual NOT parked -- corrector-bounded)");
    }
}

void Tick() {
    UE_ASSERT_GAME_THREAD("tape_caddy_sync::Tick");
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    const uint64_t now = NowMs();

    // CLIENT corrector apply -- cheap consume-once drain, every tick.
    if (s->role() == coop::net::Role::Client) {
        coop::net::ReelPosePayload rp{};
        bool isNew = false;
        if (s->TryGetHostReelPose(rp, &isNew) && isNew && TC::EnsureResolved()) {
            float big, small;
            if (TC::ReadReels(big, small)) {
                // Per-channel exact-snap: only occupied->occupied, outside the
                // local-insert hold-off. Sentinels never cross on this lane.
                if (!SlotEmpty(big) && !SlotEmpty(rp.reelBig) &&
                    now - g_recentInsert[TC::kReelBig] > kRecentInsertMs &&
                    std::fabs(big - rp.reelBig) > 1e-6f) {
                    TC::WriteReel(TC::kReelBig, rp.reelBig);
                    g_prevBig = rp.reelBig;  // keep the poll baseline in step (no edge)
                }
                if (!SlotEmpty(small) && !SlotEmpty(rp.reelSmall) &&
                    now - g_recentInsert[TC::kReelSmall] > kRecentInsertMs &&
                    std::fabs(small - rp.reelSmall) > 1e-6f) {
                    TC::WriteReel(TC::kReelSmall, rp.reelSmall);
                    g_prevSmall = rp.reelSmall;
                }
            }
        }
    }

    // The 4 Hz slot sentinel poll (both peers).
    if (now < g_nextSlotPoll) return;
    g_nextSlotPoll = now + kSlotPollMs;
    if (!TC::EnsureResolved()) {              // classes not loaded yet
        g_nextSlotPoll = now + kMissBackoffMs;
        return;
    }
    float big, small;
    if (!TC::ReadReels(big, small)) {         // unit not spawned (pre-world / other map)
        g_nextSlotPoll = now + kMissBackoffMs; // throttle the GUObjectArray re-walk
        g_havePrev = false;
        return;
    }
    if (!g_havePrev) {                        // prime-on-first-sight (join seed = the save)
        g_prevBig = big; g_prevSmall = small;
        g_havePrev = true;
    } else {
        CheckEdge(s, static_cast<uint8_t>(TC::kReelBig),   g_prevBig,   big,   now);
        CheckEdge(s, static_cast<uint8_t>(TC::kReelSmall), g_prevSmall, small, now);
        g_prevBig = big; g_prevSmall = small; // sender primes at send (edges consumed)
    }

    // HOST corrector publish -- 1 Hz while ANY slot is occupied (active or not:
    // heals inactive divergence; the values are static then, 12 B/s).
    if (s->role() == coop::net::Role::Host && now >= g_nextCorrector) {
        g_nextCorrector = now + kCorrectorMs;
        if (!SlotEmpty(big) || !SlotEmpty(small)) {
            coop::net::ReelPosePayload rp{};
            rp.reelBig = big;
            rp.reelSmall = small;
            s->SetHostReelPose(rp);
        }
    }
}

void OnReelSlot(const coop::net::ReelSlotPayload& p, uint8_t senderSlot) {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (p.reel > 1 || p.op > 1) {
        UE_LOGW("[reel] OnReelSlot malformed (reel=%u op=%u slot=%u) -- dropped",
                p.reel, p.op, senderSlot);
        return;
    }
    // Trust boundary (perf-audit F2): a raw wire float enters an engine field the
    // accrual + taskNew grading read -- NaN/huge would propagate. Native range = [0,100].
    if (p.op == 0 && !(p.progress >= 0.0f && p.progress <= 100.0f)) {
        UE_LOGW("[reel] OnReelSlot INSERT progress %.3f out of [0,100] (slot=%u) -- dropped",
                p.progress, senderSlot);
        return;
    }
    const coop::net::ReelSlotPayload cp = p;
    const bool isHost = (s->role() == coop::net::Role::Host);
    GT::Post([cp, senderSlot, isHost]() {
        if (!TC::EnsureResolved()) {
            UE_LOGW("[reel] OnReelSlot apply declined: classes unresolved (reel=%u op=%u slot=%u)",
                    cp.reel, cp.op, senderSlot);
            return;
        }
        float big, small;
        if (!TC::ReadReels(big, small)) {
            UE_LOGW("[reel] OnReelSlot apply declined: no live wallunit (reel=%u op=%u slot=%u)",
                    cp.reel, cp.op, senderSlot);
            return;
        }
        const float local = (cp.reel == TC::kReelBig) ? big : small;
        if (cp.op == 0) {  // INSERT
            if (SlotEmpty(local)) {
                TC::WriteReel(cp.reel, cp.progress);
                TC::CallUpd();
                UE_LOGI("[reel] wire INSERT applied reel=%s progress=%.2f (slot=%u)",
                        cp.reel == 0 ? "big" : "small", cp.progress, senderSlot);
                g_recentInsert[cp.reel] = NowMs();  // protect the fresh value from a stale corrector
            } else if (std::fabs(local - cp.progress) > kProgressEps) {
                if (isHost) {
                    // Authority tiebreak (design D1 / residual L7-R5): the host keeps its own
                    // occupied value; its corrector re-asserts it <= 1 s.
                    UE_LOGW("[reel] wire INSERT onto OCCUPIED slot reel=%s local=%.2f wire=%.2f "
                            "(slot=%u) -- HOST keeps own (corrector re-asserts)",
                            cp.reel == 0 ? "big" : "small", local, cp.progress, senderSlot);
                } else {
                    TC::WriteReel(cp.reel, cp.progress);
                    UE_LOGW("[reel] wire INSERT onto OCCUPIED slot reel=%s local=%.2f -> wire=%.2f "
                            "(slot=%u) -- client converges to the wire value",
                            cp.reel == 0 ? "big" : "small", local, cp.progress, senderSlot);
                }
            }  // equal -> the M-I both-peer self-run; silent
        } else {           // EJECT
            if (!SlotEmpty(local)) {
                TC::WriteReel(cp.reel, TC::kSlotEmpty);
                TC::CallUpd();
                UE_LOGI("[reel] wire EJECT applied reel=%s (slot=%u)",
                        cp.reel == 0 ? "big" : "small", senderSlot);
            }  // already empty (own edge / self-run) -> prime only
        }
        PrimeBaselines();  // apply + prime in ONE GT task -- the poll echo is dead
    });
}

void OnDisconnect() {
    UE_ASSERT_GAME_THREAD("tape_caddy_sync::OnDisconnect");
    g_havePrev = false;
    g_prevBig = g_prevSmall = TC::kSlotEmpty;
    g_nextSlotPoll = g_nextCorrector = 0;
    g_recentInsert[0] = g_recentInsert[1] = 0;
    TC::ResetCache();
    UE_LOGI("[reel] tape_caddy_sync reset (disconnect)");
}

}  // namespace coop::tape_caddy_sync
