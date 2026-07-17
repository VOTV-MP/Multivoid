// coop/wisp_attack_sync.cpp -- see coop/wisp_attack_sync.h.

#include "coop/creatures/wisp_attack_sync.h"

#include "coop/element/element.h"
#include "coop/element/mirror_manager.h"
#include "coop/element/mirror_managers.h"  // PropMirrors/NpcMirrors/WaMirrors
#include "coop/element/npc.h"
#include "coop/element/player.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/creatures/npc_sync.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/creatures/wisp_grab_hold.h"
#include "coop/creatures/wisp_tear_mirror.h"

#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/actors/wisp.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <random>
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
// canRagdoll belt (audit CRITICAL 2026-07-03 night): the false-grab montage's d1 notify
// writes playerDamaged and the drop notify fires ragdollMode(true,false,true) -- both
// bytecode-internal, so neither the AddPlayerDamage cancel nor the HP pin can stop the
// resulting host DEATH (a ragdoll-death carries no HP write). mainPlayer.canRagdoll=false
// is the BP's own ragdollMode pre-condition early-out: force it for the window, restore
// on the falling edge / OnDisconnect. Game-thread only.
bool g_canRagdollForced = false;
// Native-grab rising edge per wisp (game-thread only): the false-grab abort fires
// releasePlayer ONCE per grab (per-tick spam would queue a latent 1s grab-reset chain
// on the wisp every tick and re-flop the host before the canRagdoll belt lands).
std::unordered_map<uint32_t, bool> g_lastNativeGrab;

std::unordered_set<uint32_t> g_relayed;  // wisp eids whose grab was already relayed
// The grab window: the wisp despawns at deadlineMs (breaks the re-grab loop), and
// UNTIL then the host LIFTS it (v2: the native kill's signature rise -- the pose
// stream carries the lift to every peer; the attached victim + the held puppet ride).
struct PendingDestroy { uint32_t eid; uint64_t deadlineMs; uint64_t lastLiftMs; };
std::vector<PendingDestroy> g_pendingDestroy;

// ---- v2 aggro selector state (host-authoritative Target owner) ------------------------
// While >=1 PLAYER candidate (host pawn or a live puppet) is inside the native 5000u
// acquire radius AND canReach-visible, WE own the wisp's Target: a UNIFORM RANDOM pick
// among the eligible (user 2026-07-03: "рандомом чтоб всем досталось" -- the BP's own
// nearest-pick made the host the perpetual victim) with STICKINESS (hold the victim
// while it stays valid/in range; re-roll only on death/leave/out-of-range), re-asserted
// through the raw Target write every Tick so it dominates the BP's re-scan. With NO
// eligible player the native scan owns Target (kerfur/fossilhound hunting preserved).
// DIVERGENCE (documented): natively a kerfur closer than any player would win the
// nearest-pick; our selector prefers players whenever one is eligible -- the killer
// wisp is the player-hunting event creature, and fairness among PLAYERS is the point.
struct Aggro { uint8_t slot; void* actor; int32_t idx; };
std::unordered_map<uint32_t, Aggro> g_aggro;

// ---- v2 two-stage close (the native Capture fires at contact, not at 550u) ------------
// v1 relayed at the 550u arm radius -- the victim died "grabbed" from 5 m away. Now the
// 550u+LOS edge only ARMS a closing window; the wisp keeps chasing (MoveTo acceptance
// ~5u) and the grab fires at contact (or at the hover timeout, LOS re-verified so a
// blocked hover never kills through a wall -- it just re-arms when the victim is
// visible again). The closing entry doubles as the arm-edge memory (no prev-tick map).
struct Closing { void* victim; int32_t victimIdx; uint64_t armedAtMs; };
std::unordered_map<uint32_t, Closing> g_closing;

constexpr float    kAcquireRadius  = 5000.f;  // native scanForActors radius
constexpr float    kKeepRadius     = 5500.f;  // stickiness hysteresis (re-roll past this)
constexpr float    kContactRadius  = 200.f;   // fire the synthetic grab at contact-ish
constexpr uint64_t kCloseTimeoutMs = 2500;    // hovering-wisp fallback -> fire anyway (LOS-gated)
constexpr float    kLiftCmPerSec   = 150.f;   // grab-window rise (~5 m over the 3.5 s tear)
constexpr float    kLiftMaxStepCm  = 50.f;    // per-tick cap (a hitch must not teleport it)

int UniformPick(int n) {
    static std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> d(0, n - 1);
    return d(rng);
}

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

using coop::element::NpcMirrors;   // canonical accessor (coop/element/mirror_managers.h)

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

    // The host doesn't receive its own broadcast -- run the tear on its OWN wisp directly,
    // and hold the victim's puppet at the socket (third peers do the same on WispTear).
    coop::wisp_tear_mirror::PlayTearOnWisp(wispActor, victimSlot);
    coop::wisp_grab_hold::EngagePuppet(wispEid, victimSlot);

    const uint64_t now = NowMs();
    g_pendingDestroy.push_back({wispEid, now + kKillDelayMs, now});
    UE_LOGI("wisp_attack: RELAYED grab -- wispEid=%u victimSlot=%u victimEid=%u (WispGrab->slot, "
            "WispTear->all, local tear+hold, lift armed, wisp despawn scheduled +%ums)",
            wispEid, victimSlot, victimEid, kKillDelayMs);
}

void DischargePendingDestroys() {
    if (g_pendingDestroy.empty()) return;
    const uint64_t now = NowMs();
    for (size_t i = 0; i < g_pendingDestroy.size();) {
        // Resolve the wisp actor by eid (the host's own Npc Element). Re-check IsKillerWisp:
        // the 3.5s window could (rarely) see the wisp die + its eid recycle to a DIFFERENT
        // NPC -- lifting/destroying that would be a wrong-actor hit. The class re-check makes
        // the worst case "an unrelated killerwisp", not "an unrelated NPC". IsLive-gated
        // before any deref.
        void* actor = nullptr;
        if (auto* el = coop::element::Registry::Get().Get(
                static_cast<coop::element::ElementId>(g_pendingDestroy[i].eid))) {
            void* a = el->GetActor();
            if (a && R::IsLive(a) && ue_wrap::wisp::IsKillerWisp(a)) actor = a;
        }
        if (now < g_pendingDestroy[i].deadlineMs) {
            // Grab window still open: LIFT the wisp (the native kill's signature rise).
            // The pose stream mirrors it everywhere; the attached victim and the held
            // puppet ride the socket up with it. dt-scaled + step-capped.
            if (actor) {
                const uint64_t last = g_pendingDestroy[i].lastLiftMs;
                float dz = kLiftCmPerSec * static_cast<float>(now - last) * 0.001f;
                if (dz > kLiftMaxStepCm) dz = kLiftMaxStepCm;
                if (dz > 0.f) {
                    ue_wrap::FVector loc = E::GetActorLocation(actor);
                    loc.Z += dz;
                    E::SetActorLocation(actor, loc);
                }
                g_pendingDestroy[i].lastLiftMs = now;
            }
            ++i;
            continue;
        }
        const uint32_t eid = g_pendingDestroy[i].eid;
        // K2_Destroy fires the npc destroy PRE -> EntityDestroy broadcast, so the mirrors
        // despawn too (and every peer's grab-hold self-releases on its liveness guard).
        if (actor) {
            E::DestroyActor(actor);
            UE_LOGI("wisp_attack: despawned host wisp eid=%u after tear (breaks the re-grab loop)", eid);
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
        auto& reg = coop::players::Registry::Get();

        // ---- v2 AGGRO SELECTOR (one owner; see the g_aggro block comment) ----
        // Hands-off set: our relayed grab window, a committed fatality, and a NATIVE
        // tryGrab/grab when the selector's pick IS the host (or there is no pick) -- the
        // native host kill must not have its Target yanked mid-choreography. But a native
        // tryGrab/grab while our pick is a PUPPET is the FALSE-grab (the BP arms on host
        // PROXIMITY, not on Target) -- there the selector keeps asserting the puppet and
        // the false-grab protections below abort the host side.
        const bool nativeBusy = st.tryGrab || st.grab || st.killed;
        const auto aitPeek = g_aggro.find(eid);
        const bool pickedPuppet =
            aitPeek != g_aggro.end() && aitPeek->second.slot != coop::players::kPeerIdHost;
        const bool midSequence =
            g_relayed.count(eid) != 0 || st.killed || (nativeBusy && !pickedPuppet);
        if (!st.harmless && !midSequence) {
            auto ait = g_aggro.find(eid);
            bool haveValid = false;
            if (ait != g_aggro.end()) {
                Aggro& a = ait->second;
                const bool live = a.actor && R::IsLiveByIndex(a.actor, a.idx);
                const bool identity = live && (a.slot == coop::players::kPeerIdHost
                                                   ? reg.IsLocal(a.actor)
                                                   : reg.PeerIdOfActor(a.actor) == a.slot);
                if (identity && ue_wrap::wisp::DistanceTo(actor, a.actor) <= kKeepRadius) {
                    haveValid = true;  // stickiness: hold the victim
                } else {
                    g_aggro.erase(ait);  // died / left / out of range -> re-roll below
                }
            }
            if (!haveValid) {
                struct Cand { uint8_t slot; void* a; };
                Cand cands[coop::players::kMaxPeers + 1];
                int n = 0;
                void* hostPawn = reg.Local();
                if (hostPawn && R::IsLive(hostPawn) &&
                    ue_wrap::wisp::DistanceTo(actor, hostPawn) <= kAcquireRadius &&
                    ue_wrap::wisp::CanReach(actor, hostPawn))
                    cands[n++] = {coop::players::kPeerIdHost, hostPawn};
                for (uint8_t s2 = 0; s2 < coop::players::kMaxPeers; ++s2) {
                    coop::RemotePlayer* rp = reg.Puppet(s2);
                    if (!rp || !rp->valid()) continue;
                    void* pa = rp->GetActor();
                    if (!pa || !R::IsLive(pa)) continue;
                    if (ue_wrap::wisp::DistanceTo(actor, pa) > kAcquireRadius) continue;
                    if (!ue_wrap::wisp::CanReach(actor, pa)) continue;
                    cands[n++] = {s2, pa};
                }
                if (n > 0) {
                    const Cand& c = cands[UniformPick(n)];
                    g_aggro[eid] = Aggro{c.slot, c.a, R::InternalIndexOf(c.a)};
                    haveValid = true;
                    UE_LOGI("wisp_aggro: wisp eid=%u picked victim slot=%u (%d eligible; "
                            "uniform random + sticky)", eid, c.slot, n);
                }
            }
            if (haveValid) {
                // Re-assert every tick: the BP's nearest re-scan writes Target on its own
                // cadence; our per-tick write dominates it between scans.
                Aggro& a = g_aggro.find(eid)->second;
                ue_wrap::wisp::WriteTarget(actor, a.actor);
                victim = a.actor;  // downstream classification sees OUR pick this tick
            }
        }

        // Classify the wisp's victim (the selector's pick, or the BP's own Target when no
        // player is eligible). The BP already CHASES any of {host pawn, puppet, kerfur,
        // fossilhound} (scanForActors + Target-bound moveToTarg), but it only GRABS/KILLS
        // player 0 (the host). We synthesize the missing effect against the real victim.
        const bool isPuppet = victim && reg.IsPuppet(victim);
        const uint32_t npcVictimEid =
            (victim && !isPuppet && !reg.IsLocal(victim))
                ? static_cast<uint32_t>(coop::npc_sync::GetNpcIdForActor(victim))
                : static_cast<uint32_t>(coop::element::kInvalidId);
        const bool isNpcVictim = npcVictimEid != static_cast<uint32_t>(coop::element::kInvalidId);

        // The wisp is in lethal range of its victim (the BP's 550u grab radius), evaluated
        // against the ACTUAL victim rather than the host's pawn. IsLive(victim) is REQUIRED
        // before the deref: the BP does not null `Target` the frame its target dies, so a
        // dead-but-still-referenced pointer would UAF in InGrabRange's GetActorLocation /
        // EnrollNpcKillWatch's InternalIndexOf (the classification above is map-key-only, no
        // deref). IsLive is the SEH-firewalled liveness primitive -- safe on a freed pointer.
        const bool inRange = victim && R::IsLive(victim) && !st.harmless &&
                             ue_wrap::wisp::InGrabRange(actor, victim);

        // NPC victim: a wisp closing on a kerfur/fossilhound will addDamage(1000)->death (its
        // own BP). Watch the eid so the BP-internal self-destroy gets mirrored (EntityDestroy).
        if (isNpcVictim && inRange) EnrollNpcKillWatch(npcVictimEid, victim);

        // PUPPET victim: v2 two-stage close (arm at 550u+LOS -> swoop -> fire at contact).
        if (isPuppet) {
            // If the BP ALSO grabbed (or is winding up to grab) the host -- the host happened
            // to be within 550 too -- that's a false-grab: protect the host across the whole
            // grab + tryGrab window while we redirect the kill to the puppet.
            if (st.grab || st.tryGrab) {
                anyHostFalseGrab = true;
                // Audit CRITICAL (2026-07-03 night): abort the native false-grab on ITS OWN
                // rising edge -- decoupled from the closing machinery below (the old
                // edge/contact-gated abort could run AFTER the grab montage's d1 notify set
                // playerDamaged inline, turning releasePlayer's own ragdollMode(...,
                // playerDamaged) LETHAL to the host -- or never run at all on a LOS-blocked
                // hover). Once playerDamaged is up, releasePlayer is no longer a safe abort:
                // skip it -- the canRagdoll=false belt (end of Tick) keeps the montage's
                // drop notify from killing the host, and the wisp despawn breaks the hold.
                if (st.grab && !g_lastNativeGrab[eid] && !st.playerDamaged)
                    ue_wrap::wisp::CallReleasePlayer(actor);
            }
            const bool relayed = g_relayed.count(eid) != 0;
            auto cit = g_closing.find(eid);
            if (cit == g_closing.end()) {
                // The ARM edge: in the BP's 550u grab radius AND canReach-visible (the native
                // grab-arm's own LOS gate -- v1 was distance-only, a wall-through divergence).
                if (!relayed && inRange && ue_wrap::wisp::CanReach(actor, victim)) {
                    g_closing[eid] = Closing{victim, R::InternalIndexOf(victim), NowMs()};
                    UE_LOGI("wisp_attack: CLOSING -- wispEid=%u armed (550u + LOS), swooping to "
                            "%.0fu (timeout %llu ms)",
                            eid, kContactRadius, static_cast<unsigned long long>(kCloseTimeoutMs));
                }
            } else {
                Closing& c = cit->second;
                if (victim != c.victim || !R::IsLiveByIndex(c.victim, c.victimIdx)) {
                    g_closing.erase(cit);  // victim changed/died mid-close -> re-arm fresh
                } else {
                    const float d = ue_wrap::wisp::DistanceTo(actor, victim);
                    const bool timeout = (NowMs() - c.armedAtMs) >= kCloseTimeoutMs;
                    if (d <= kContactRadius || timeout) {
                        if (ue_wrap::wisp::CanReach(actor, victim)) {
                            // Relay once. relayed-latch + the scheduled wisp despawn break
                            // the re-grab loop. (The false-grab abort is per-tick above.)
                            if (!relayed) { RelayGrab(s, actor, eid, victim); g_relayed.insert(eid); }
                            UE_LOGI("wisp_attack: CONTACT%s -- wispEid=%u d=%.0f -> grab fired",
                                    timeout ? " (timeout)" : "", eid, d);
                            g_closing.erase(cit);
                        } else if (timeout) {
                            // Blocked hover at the timeout: NEVER kill through a wall. Drop
                            // the window; it re-arms when the victim is visible again.
                            UE_LOGI("wisp_attack: closing TIMEOUT with LOS blocked -- wispEid=%u "
                                    "re-arms when visible", eid);
                            g_closing.erase(cit);
                        }
                        // contact-but-blocked (thin floor between): keep closing, retry next tick
                    }
                }
            }
        } else {
            g_closing.erase(eid);  // victim no longer a puppet -> drop any stale window
        }
        g_lastNativeGrab[eid] = st.grab;  // rising-edge memory for the false-grab abort
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
        // canRagdoll belt: block EVERY ragdoll cause on the host for the window (see the
        // g_canRagdollForced comment -- the montage's own notifies would ragdoll-kill it).
        if (!g_canRagdollForced) {
            void* local = coop::players::Registry::Get().Local();
            if (local && R::IsLive(local) &&
                ue_wrap::engine::SetMainPlayerCanRagdoll(local, false)) {
                g_canRagdollForced = true;
                UE_LOGI("wisp_attack: canRagdoll=false forced on the host for the false-grab window");
            }
        }
    } else if (g_haveHostHp || g_canRagdollForced) {
        g_haveHostHp = false;
        if (g_canRagdollForced) {
            void* local = coop::players::Registry::Get().Local();
            if (local && R::IsLive(local))
                ue_wrap::engine::SetMainPlayerCanRagdoll(local, true);
            g_canRagdollForced = false;
            UE_LOGI("wisp_attack: canRagdoll restored on the host (false-grab window over)");
        }
    }

    DischargePendingDestroys();
    DischargeNpcKillWatch();  // mirror any wisp-killed NPC's BP-internal self-destroy

    // Drop state for wisps that despawned (no longer tracked) so a recycled eid starts clean.
    for (auto it = g_aggro.begin(); it != g_aggro.end();)
        it = liveWispEids.count(it->first) ? std::next(it) : g_aggro.erase(it);
    for (auto it = g_closing.begin(); it != g_closing.end();)
        it = liveWispEids.count(it->first) ? std::next(it) : g_closing.erase(it);
    for (auto it = g_relayed.begin(); it != g_relayed.end();)
        it = liveWispEids.count(*it) ? std::next(it) : g_relayed.erase(it);
    for (auto it = g_lastNativeGrab.begin(); it != g_lastNativeGrab.end();)
        it = liveWispEids.count(it->first) ? std::next(it) : g_lastNativeGrab.erase(it);
}

void OnDisconnect() {
    g_cancelHostDamage.store(false, std::memory_order_release);
    g_haveHostHp = false;
    if (g_canRagdollForced) {
        // Never strand the local player un-ragdollable past the session (a mid-window
        // teardown would otherwise block every future ragdoll cause incl. real deaths).
        void* local = coop::players::Registry::Get().Local();
        if (local && ue_wrap::reflection::IsLive(local))
            ue_wrap::engine::SetMainPlayerCanRagdoll(local, true);
        g_canRagdollForced = false;
    }
    g_aggro.clear();
    g_closing.clear();
    g_relayed.clear();
    g_lastNativeGrab.clear();
    g_pendingDestroy.clear();
    g_npcKillWatch.clear();
}

}  // namespace coop::wisp_attack_sync
