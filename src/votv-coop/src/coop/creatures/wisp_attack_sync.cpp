// coop/wisp_attack_sync.cpp -- see coop/wisp_attack_sync.h.

#include "coop/creatures/wisp_attack_sync.h"

#include "coop/element/element.h"
#include "coop/element/mirror_manager.h"
#include "coop/element/npc.h"
#include "coop/element/player.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/creatures/npc_sync.h"
#include "coop/player/players_registry.h"
#include "coop/creatures/wisp_tear_mirror.h"

#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/vitals.h"
#include "ue_wrap/wisp.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::wisp_attack_sync {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

// ~Tear length: the victim ragdolls + the host wisp despawns this long after the grab, so
// the tear animation gets to play first. Host-decided; carried in WispGrab.
constexpr uint32_t kKillDelayMs = 3500;

std::atomic<coop::net::Session*> g_session{nullptr};

// Drives the AddPlayerDamage PRE-cancel. ON while ANY tracked wisp grabs/tryGrabs a client
// puppet -> the wisp's per-limb AddPlayerDamage(24) to the HOST is zeroed (beats the
// grab->d1 race). The interceptor reads this atomically (it can fire off the game thread on
// a parallel-anim worker, like the npc suppressor).
//
// SCOPE NOTE (audit 2026-06-14): the interceptor cannot identify the CALLER (its `self` is
// the host's mainPlayer, not the wisp), so while this latch is on it cancels ALL host
// "Add Player Damage" -- the host is fully damage-invulnerable for the grab window (up to
// kKillDelayMs ~3.5 s, until the wisp despawns + anyClientGrab clears). Acceptable: the host
// is being false-grabbed by the wisp during that window, and the C4 health pin below already
// implies invulnerability. A tighter caller-scoped cancel would need the wisp pointer in the
// damage params (not present in this BP).
std::atomic<bool> g_cancelHostDamage{false};
std::atomic<bool> g_interceptorInstalled{false};

// C4 belt (audit 2026-06-14): pin the host's PRE-grab health across the false-grab window.
// The AddPlayerDamage cancel arms one tick AFTER the rising edge (g_cancelHostDamage is the
// PREVIOUS tick's store), so a hit on the rising-edge tick can land; snapshot before that +
// re-write each tick so any slipped damage is immediately undone. Game-thread only.
bool  g_haveHostHp = false;
float g_hostHpSnapshot = 0.f;

// Per-wisp rising-edge tracking (game-thread only): the last tick's puppet-attack trigger
// (the wisp is in grab range of its puppet Target) for each wisp eid, so releasePlayer +
// the relay fire once per grab, not per tick.
std::unordered_map<uint32_t, bool> g_lastGrabClient;
std::unordered_set<uint32_t> g_relayed;  // wisp eids whose grab was already relayed
struct PendingDestroy { uint32_t eid; uint64_t deadlineMs; };
std::vector<PendingDestroy> g_pendingDestroy;

// NPC-victim death-watch (game-thread only). A wisp kills a kerfur/fossilhound via
// int_objects::addDamage(1000) -> the NPC's OWN death() + self-K2_DestroyActor, which is
// EX_VirtualFunction (PE-invisible) -- so our NpcDestroy PRE observer never fires and the
// kerfur mirror would survive as a ghost on clients. When a wisp closes on a tracked NPC
// Target we enroll its eid here; the discharge polls liveness and, on death, runs
// SyncDestroyedNpcActor (the idempotent NpcDestroy body -> EntityDestroy broadcast -> the
// mirror despawns). Keyed by victim NPC eid; the actor+idx snapshot + a GetNpcIdForActor
// identity re-check guard against pointer recycling.
struct NpcWatch { void* actor; int32_t idx; uint64_t deadlineMs; };
std::unordered_map<uint32_t, NpcWatch> g_npcKillWatch;
constexpr uint64_t kNpcWatchWindowMs = 6000;  // watch a closed-on NPC for death up to 6s, else drop

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// PRE-interceptor on mainPlayer_C "Add Player Damage": cancel (return true) while a wisp is
// false-grabbing a client. Side-effect-free + cheap (one atomic load). Re-entrancy-safe (no
// posts / recursive Calls).
bool AddPlayerDamage_PreCancel(void* /*self*/, void* /*params*/) {
    return g_cancelHostDamage.load(std::memory_order_acquire);
}

inline coop::element::MirrorManager<coop::element::Npc>& NpcMirrors() {
    return coop::element::MirrorManager<coop::element::Npc>::Instance();
}

// Relay one client-victim grab: WispGrab -> the victim slot, WispTear -> all, run the tear
// on the host's own wisp, and schedule the host wisp's despawn after the tear. releasePlayer
// was already called on the rising edge (DROP-notify halt) before this.
void RelayGrab(coop::net::Session* s, void* wispActor, uint32_t wispEid, void* victimActor) {
    const uint8_t victimSlot = coop::players::Registry::Get().PeerIdOfActor(victimActor);
    if (victimSlot == coop::players::kPeerIdUnknown || victimSlot == coop::players::kPeerIdHost) {
        UE_LOGW("wisp_attack: victim puppet has no client slot (slot=%u) -- not relaying", victimSlot);
        return;
    }
    coop::element::Player* victimEl = coop::players::Registry::Get().GetPlayerElement(victimSlot);
    if (!victimEl) {
        UE_LOGW("wisp_attack: no Player Element for victim slot %u -- not relaying", victimSlot);
        return;
    }
    const uint32_t victimEid = static_cast<uint32_t>(victimEl->GetId());
    // (Host-health protection is the AddPlayerDamage cancel + the C4 per-tick health pin in
    // Tick -- not a read-write here, which the audit caught as inert.)

    coop::net::WispGrabPayload g{};
    g.victimElementId = victimEid;
    g.wispElementId   = wispEid;
    g.killDelayMs     = kKillDelayMs;
    s->SendReliableToSlot(victimSlot, coop::net::ReliableKind::WispGrab, &g, sizeof(g));

    coop::net::WispTearPayload t{};
    t.wispElementId = wispEid;
    t.victimSlot    = victimSlot;
    s->SendReliable(coop::net::ReliableKind::WispTear, &t, sizeof(t));

    // The host doesn't receive its own broadcast -- run the tear on its OWN wisp directly.
    coop::wisp_tear_mirror::PlayTearOnWisp(wispActor, victimSlot);

    g_pendingDestroy.push_back({wispEid, NowMs() + kKillDelayMs});
    UE_LOGI("wisp_attack: RELAYED grab -- wispEid=%u victimSlot=%u victimEid=%u (WispGrab->slot, "
            "WispTear->all, local tear, wisp despawn scheduled +%ums)",
            wispEid, victimSlot, victimEid, kKillDelayMs);
}

void DischargePendingDestroys() {
    if (g_pendingDestroy.empty()) return;
    const uint64_t now = NowMs();
    for (size_t i = 0; i < g_pendingDestroy.size();) {
        if (now < g_pendingDestroy[i].deadlineMs) { ++i; continue; }
        const uint32_t eid = g_pendingDestroy[i].eid;
        // Resolve the wisp actor by eid (the host's own Npc Element) + destroy it. K2_Destroy
        // fires the npc destroy PRE -> EntityDestroy broadcast, so the mirrors despawn too.
        if (auto* el = coop::element::Registry::Get().Get(static_cast<coop::element::ElementId>(eid))) {
            void* actor = el->GetActor();
            // Re-check IsKillerWisp: the 3.5s window could (rarely) see the wisp die + its eid
            // recycle to a DIFFERENT NPC -- destroying that would be a wrong-actor kill. The
            // class re-check makes the worst case "destroyed an unrelated killerwisp", not "an
            // unrelated NPC". IsLive-gated before any deref.
            if (actor && R::IsLive(actor) && ue_wrap::wisp::IsKillerWisp(actor)) {
                E::DestroyActor(actor);
                UE_LOGI("wisp_attack: despawned host wisp eid=%u after tear (breaks the re-grab loop)", eid);
            }
        }
        g_pendingDestroy.erase(g_pendingDestroy.begin() + i);
    }
}

// Enroll (or refresh) a wisp-targeted NPC for the death-watch. Snapshot the actor + its
// internal index so a later death is detectable even after the pointer is GC'd.
void EnrollNpcKillWatch(uint32_t npcEid, void* npcActor) {
    const uint64_t deadline = NowMs() + kNpcWatchWindowMs;
    auto it = g_npcKillWatch.find(npcEid);
    if (it != g_npcKillWatch.end()) { it->second.deadlineMs = deadline; return; }  // refresh window
    g_npcKillWatch[npcEid] = NpcWatch{npcActor, R::InternalIndexOf(npcActor), deadline};
    UE_LOGI("wisp_attack: watching NPC eid=%u (a wisp closed on it) for a wisp-kill death", npcEid);
}

// Poll watched NPCs; mirror the death of any that died (EX_VirtualFunction self-destroy our
// PE observer can't see), drop survivors at their deadline.
void DischargeNpcKillWatch() {
    if (g_npcKillWatch.empty()) return;
    const uint64_t now = NowMs();
    for (auto it = g_npcKillWatch.begin(); it != g_npcKillWatch.end();) {
        const uint32_t eid = it->first;
        NpcWatch& w = it->second;
        // Identity re-check: the map must still bind THIS actor to THIS eid. If it doesn't, the
        // element was already drained (a normal PE-visible destroy beat us, or the eid recycled)
        // -> nothing to do, drop the watch. (GetNpcIdForActor uses the actor as a key only.)
        if (static_cast<uint32_t>(coop::npc_sync::GetNpcIdForActor(w.actor)) != eid) {
            it = g_npcKillWatch.erase(it);
            continue;
        }
        if (R::IsLiveByIndex(w.actor, w.idx)) {
            if (now >= w.deadlineMs) { it = g_npcKillWatch.erase(it); continue; }  // survived the window
            ++it;
            continue;  // still alive -- keep watching
        }
        // Dead AND still mapped to our eid -> the wisp killed it. Broadcast the destroy.
        coop::npc_sync::SyncDestroyedNpcActor(w.actor);
        UE_LOGI("wisp_attack: NPC victim eid=%u died (wisp kill, BP-internal self-destroy) -- "
                "mirrored EntityDestroy so the kerfur/fossilhound mirror despawns", eid);
        it = g_npcKillWatch.erase(it);
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_interceptorInstalled.load(std::memory_order_acquire)) return;
    // THROTTLE (audit 2026-06-14 CRITICAL): AddPlayerDamageFunctionPtr -> FindClass walks
    // GUObjectArray with per-entry wstring allocs; mainPlayer_C loads with gameplay, so this
    // pre-install path runs from menu->world. Bound to ~1 Hz of the 125 Hz pump (the same
    // shape as ambient_spawner_suppress / firefly_sync / kerfur_convert Install).
    static uint32_t sResolveN = 0;
    if ((sResolveN++ % 125) != 0) return;
    void* fn = E::AddPlayerDamageFunctionPtr();  // null until mainPlayer_C loads
    if (!fn) return;
    if (ue_wrap::game_thread::RegisterInterceptor(fn, &AddPlayerDamage_PreCancel)) {
        g_interceptorInstalled.store(true, std::memory_order_release);
        UE_LOGI("wisp_attack: installed AddPlayerDamage PRE-cancel interceptor @ %p", fn);
    }
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) {
        if (g_cancelHostDamage.load(std::memory_order_relaxed))
            g_cancelHostDamage.store(false, std::memory_order_release);
        return;
    }
    // Walk the host's tracked Npc Elements (small set), NOT GUObjectArray. Find killerwisps.
    std::vector<coop::element::Npc*> npcs;
    NpcMirrors().Snapshot(npcs);
    bool anyHostFalseGrab = false;  // a wisp's BP grabbed the HOST while its real Target is a puppet
    std::unordered_set<uint32_t> liveWispEids;

    for (coop::element::Npc* npc : npcs) {
        if (!npc) continue;
        void* actor = npc->GetActor();
        if (!actor || !R::IsLive(actor) || !ue_wrap::wisp::IsKillerWisp(actor)) continue;
        const uint32_t eid = static_cast<uint32_t>(coop::npc_sync::GetNpcIdForActor(actor));
        if (eid == 0 || eid == static_cast<uint32_t>(coop::element::kInvalidId)) continue;
        liveWispEids.insert(eid);

        ue_wrap::wisp::State st;
        if (!ue_wrap::wisp::ReadState(actor, st)) continue;
        void* victim = st.target;

        // Classify the wisp's acquired Target. The BP already CHASES any of {host pawn,
        // puppet, kerfur, fossilhound} (scanForActors), but it only GRABS/KILLS player 0
        // (the host). We synthesize the missing effect against the real Target.
        auto& reg = coop::players::Registry::Get();
        const bool isPuppet = victim && reg.IsPuppet(victim);
        const uint32_t npcVictimEid =
            (victim && !isPuppet && !reg.IsLocal(victim))
                ? static_cast<uint32_t>(coop::npc_sync::GetNpcIdForActor(victim))
                : static_cast<uint32_t>(coop::element::kInvalidId);
        const bool isNpcVictim = npcVictimEid != static_cast<uint32_t>(coop::element::kInvalidId);

        // The wisp is in lethal range of its Target (the BP's 550u grab radius), evaluated
        // against the ACTUAL Target rather than the host's pawn. IsLive(victim) is REQUIRED
        // before the deref: the BP does not null `Target` the frame its target dies, so a
        // dead-but-still-referenced pointer would UAF in InGrabRange's GetActorLocation /
        // EnrollNpcKillWatch's InternalIndexOf (the classification above is map-key-only, no
        // deref). IsLive is the SEH-firewalled liveness primitive -- safe on a freed pointer.
        const bool inRange = victim && R::IsLive(victim) && !st.harmless &&
                             ue_wrap::wisp::InGrabRange(actor, victim);

        // NPC victim: a wisp closing on a kerfur/fossilhound will addDamage(1000)->death (its
        // own BP). Watch the eid so the BP-internal self-destroy gets mirrored (EntityDestroy).
        if (isNpcVictim && inRange) EnrollNpcKillWatch(npcVictimEid, victim);

        // PUPPET victim: drive the grab/kill the BP cannot (it would grab the host instead).
        bool curGrab = false;
        if (isPuppet) {
            curGrab = inRange;  // host-proximity-independent: fires whenever the wisp reaches the puppet
            // If the BP ALSO grabbed (or is winding up to grab) the host -- the host happened to
            // be within 550 too -- that's a false-grab: protect the host's health across the
            // whole grab + tryGrab window while we redirect the kill to the puppet.
            if (st.grab || st.tryGrab) anyHostFalseGrab = true;
            const bool prevGrab = g_lastGrabClient[eid];
            if (curGrab && !prevGrab) {
                // Abort any host false-grab (no-op if the BP didn't grab the host), then relay
                // once. relayed-latch + the scheduled wisp despawn break the re-grab loop.
                if (st.grab) ue_wrap::wisp::CallReleasePlayer(actor);
                if (!g_relayed.count(eid)) {
                    RelayGrab(s, actor, eid, victim);
                    g_relayed.insert(eid);
                }
            }
        }
        g_lastGrabClient[eid] = curGrab;
    }

    g_cancelHostDamage.store(anyHostFalseGrab, std::memory_order_release);

    // C4 host-health pin: snapshot on the rising edge (before the wisp's d1 limb damage), then
    // re-write each tick while a wisp false-grabs the host (its real Target being a puppet) so
    // any hit that slipped past the (one-tick-late) cancel is immediately undone. Clear when over.
    if (anyHostFalseGrab) {
        if (!g_haveHostHp) {
            float hp = 0.f;
            g_haveHostHp = ue_wrap::vitals::Read(ue_wrap::vitals::Field::Health, &hp);
            g_hostHpSnapshot = hp;
        } else {
            ue_wrap::vitals::Write(ue_wrap::vitals::Field::Health, g_hostHpSnapshot);
        }
    } else if (g_haveHostHp) {
        g_haveHostHp = false;
    }

    DischargePendingDestroys();
    DischargeNpcKillWatch();  // mirror any wisp-killed NPC's BP-internal self-destroy

    // Drop state for wisps that despawned (no longer tracked) so a recycled eid starts clean.
    for (auto it = g_lastGrabClient.begin(); it != g_lastGrabClient.end();) {
        if (liveWispEids.count(it->first)) { ++it; continue; }
        g_relayed.erase(it->first);
        it = g_lastGrabClient.erase(it);
    }
}

void OnDisconnect() {
    g_cancelHostDamage.store(false, std::memory_order_release);
    g_haveHostHp = false;
    g_lastGrabClient.clear();
    g_relayed.clear();
    g_pendingDestroy.clear();
    g_npcKillWatch.clear();
}

}  // namespace coop::wisp_attack_sync
