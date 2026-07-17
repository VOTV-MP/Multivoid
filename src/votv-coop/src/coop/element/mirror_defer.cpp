// coop/mirror_defer.cpp -- see coop/mirror_defer.h. Instant-world deferred-spawn visibility.

#include "coop/element/mirror_defer.h"

#include "ue_wrap/engine/engine.h"          // SetActorHiddenInGame + SetActorEnableCollision
#include "ue_wrap/core/hot_path_guard.h"  // UE_ASSERT_GAME_THREAD
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"      // InternalIndexOf + IsLiveByIndex

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace coop::mirror_defer {
namespace {

namespace R = ue_wrap::reflection;

// One hidden host mirror. `idx` is the captured InternalIndex so reveal-time liveness is the
// GC-robust IsLiveByIndex (the raw actor ptr can be freed by a sweep that destroys a ghost
// between hide and reveal -- never deref it first). `collisionOff` records whether we also
// disabled collision (so reveal restores exactly what hide changed). `hold` = held to quiescence.
struct HiddenRec { void* actor; int32_t idx; bool collisionOff; bool hold; };

// eid -> hidden record. This IS the authoritative hidden set: a mirror is hidden ONLY via
// OnMirrorSpawned (which records here), so walking this map at quiescence reveals everything
// we hid -> nothing can be stuck hidden (the audit's stuck-hidden third category, closed by
// construction). Game-thread only (no mutex).
std::unordered_map<uint32_t, HiddenRec> g_hidden;
// eids already revealed this join -- OnMirrorSpawned must NEVER re-hide one (a proxy SpawnProxy is
// idempotent + re-skins for the same eid; a post-lift re-spawn convergence of an already-revealed
// confirmed proxy would otherwise vanish it until quiescence). The audit's "do not re-hide on the
// idempotent re-spawn return", generalized to all mirror kinds.
std::unordered_set<uint32_t> g_revealed;
bool g_armed = false;

// Reveal one record: Show + (if we disabled it) restore collision. Liveness-gated -- a record
// whose actor was destroyed by the quiescence sweep (a ghost) is simply dropped, never touched.
void RevealRec(const HiddenRec& r) {
    if (!R::IsLiveByIndex(r.actor, r.idx)) return;  // swept ghost / GC'd -> nothing to reveal
    ue_wrap::engine::SetActorHiddenInGame(r.actor, false);
    if (r.collisionOff) ue_wrap::engine::SetActorEnableCollision(r.actor, true);
}

}  // namespace

void Arm() {
    UE_ASSERT_GAME_THREAD("mirror_defer::Arm");
    // Reveal any stale hidden mirrors from a PRIOR bracket before re-arming (a soft re-bracket -- two
    // SnapshotBegin without a disconnect in between -- would otherwise strand them invisible). Liveness-gated,
    // so a full teardown's dead actors are a harmless no-op. Same reveal-then-clear discipline as Reset().
    for (auto& kv : g_hidden) RevealRec(kv.second);
    g_armed = true;
    g_hidden.clear();
    g_revealed.clear();
    UE_LOGI("mirror_defer: ARMED -- join deferred-hide window open (new host mirrors spawn hidden "
            "until lift/quiescence reveal; reconcile backup untouched)");
}

void Reset() {
    // Abort / mid-drain re-bracket / teardown. REVEAL everything still alive FIRST so a hidden mirror is
    // never stranded invisible (on a soft re-bracket the world persists; on a full teardown the actors are
    // dead -> liveness-gated reveal is a harmless no-op), then drop all tracking + disarm.
    for (auto& kv : g_hidden) RevealRec(kv.second);
    g_armed = false;
    g_hidden.clear();
    g_revealed.clear();
}

bool IsArmed() { return g_armed; }

void OnMirrorSpawned(uint32_t eid, void* actor, bool collisionOff, bool holdUntilQuiescence) {
    UE_ASSERT_GAME_THREAD("mirror_defer::OnMirrorSpawned");
    if (!g_armed || !actor || eid == 0u) return;
    if (g_revealed.count(eid)) return;  // already shown this join -> never re-hide (idempotent re-spawn)
    ue_wrap::engine::SetActorHiddenInGame(actor, true);
    if (collisionOff) ue_wrap::engine::SetActorEnableCollision(actor, false);
    g_hidden[eid] = HiddenRec{actor, R::InternalIndexOf(actor), collisionOff, holdUntilQuiescence};
}

void RevealConfirmedAtLift() {
    UE_ASSERT_GAME_THREAD("mirror_defer::RevealConfirmedAtLift");
    if (!g_armed) return;
    int revealed = 0;
    for (auto it = g_hidden.begin(); it != g_hidden.end();) {
        if (it->second.hold) { ++it; continue; }  // local twin still visible -> wait for quiescence
        RevealRec(it->second);
        g_revealed.insert(it->first);
        it = g_hidden.erase(it);
        ++revealed;
    }
    UE_LOGI("mirror_defer: lift-reveal -- %d confirmed mirror(s) shown, %zu held to quiescence "
            "(the assembled world is now visible; the uncertain tail stays hidden)",
            revealed, g_hidden.size());
}

void RevealAllSurvivorsAtQuiescence() {
    UE_ASSERT_GAME_THREAD("mirror_defer::RevealAllSurvivorsAtQuiescence");
    if (!g_armed) return;
    const int n = static_cast<int>(g_hidden.size());
    for (auto& kv : g_hidden) RevealRec(kv.second);
    g_hidden.clear();
    g_armed = false;
    UE_LOGI("mirror_defer: quiescence-reveal -- %d held/tail mirror(s) shown; deferred-hide window "
            "CLOSED (steady-state spawns now visible immediately)", n);
}

}  // namespace coop::mirror_defer
