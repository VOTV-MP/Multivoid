// coop/kerfur_convert.cpp -- see coop/kerfur_convert.h for the design + the
// kismet ground truth (votv-kerfur-convert-RE-2026-06-12.md).
//
// Post-audit revision (2026-06-12, audit findings C1/C2/I2/I4/I5):
//  - C1: a client request must carry the HOST's element id. The local tracker
//    holds a peer-range shadow the host cannot resolve, and npc_sync's reverse
//    map is host-side bookkeeping -- the wire eid lives ONLY in the wire-mirror
//    Element (IsMirror()==true) bound to the actor. We derive it with a
//    bounded MirrorManager Snapshot walk instead of duplicating lifecycle
//    state in a new reverse map -- and we do it on the GAME THREAD (Tick), not
//    in the interceptor, because Snapshot's raw pointers may race a GT drain
//    when read from a parallel-anim worker.
//  - C2: a host-menu converge pushed during the actionName dispatch could be
//    drained by a NESTED ProcessEvent's pump (UCS/BeginPlay/EndPlay inside the
//    verb re-enter the detour with t_inPump==false) and run MID-verb. Entries
//    therefore arm on one Tick and execute on the next: a nested drain can
//    only ARM; the pump composite's coalescing latch makes two pump executions
//    inside one sub-ms BP chain impossible, so execution always sees the
//    settled post-verb world.
//  Both roles now share one deferred queue: the interceptor only records the
//  action (client: after CANCELLING the local dispatch); Tick() resolves and
//  acts with full game-thread rights.

#include "coop/kerfur_convert.h"

#include "coop/kerfur_command.h"  // v74: route the non-turn_off menu verbs to the command relay
#include "coop/element/mirror_manager.h"
#include "coop/element/npc.h"
#include "coop/element/prop.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/npc_mirror.h"     // DestroyUntrackedClientNpcs -- the turn-on ghost-NPC reconcile
#include "coop/npc_sync.h"
#include "coop/npc_world_enum.h"  // K-0: RegisterExistingWorldNpcs (moved out of npc_sync)
#include "coop/prop_element_tracker.h"
#include "coop/prop_lifecycle.h"
#include "coop/remote_prop_spawn.h"  // HasLoadTailQuiesced -- gate the client poll past the join reconcile
#include "ue_wrap/engine.h"      // GetActorLocation + SetActorSimulatePhysics (freeze a ghost prop)
#include "ue_wrap/game_thread.h"
#include "ue_wrap/kerfur.h"      // NeutralizeAiTimers -- park a claimed conversion-ghost NPC
#include "ue_wrap/log.h"
#include "ue_wrap/puppet.h"      // DisableCharacterTicks -- park a claimed conversion-ghost NPC
#include "ue_wrap/reflection.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::kerfur_convert {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace PT = coop::prop_element_tracker;

std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<bool> g_installed{false};

coop::net::Session* LoadSession() {
    return g_session.load(std::memory_order_acquire);
}

// ---- resolved engine refs (written by Install on the game thread BEFORE the
// interceptors register; read-only afterwards, incl. from parallel-anim
// workers -- the same publish-then-register order every observer module uses).
void* g_kerfurNpcClass  = nullptr;  // kerfurOmega_C (the NPC base; ~20 data-only skin subclasses)
void* g_kerfurPropClass = nullptr;  // prop_kerfurOmega_C (the prop base; skins likewise)
void* g_floppyClass     = nullptr;  // prop_floppyDisc_C (dropKerfurProp may also drop the carried floppy)
void* g_actionNameFn    = nullptr;  // kerfurOmega_C::actionName (the PE-visible menu dispatcher)
void* g_actionIdxFn     = nullptr;  // prop_kerfurOmega_C::actionOptionIndex (ditto, prop side)
int32_t g_nameParamOff  = -1;       // actionName 'name' FString param offset
int32_t g_actionParamOff = -1;      // actionOptionIndex 'action' byte param offset
int32_t g_killOff       = -1;       // kerfurOmega_C::kill bool (the BP's own turn_off guard)
// The verb DECLARERS. dropKerfurProp is overridden by kerfurOmega_col_C and
// kerfurOmega_col_gamer_C (CXXHeaderDump); ProcessEvent executes exactly the
// UFunction we pass (no re-virtualization), so the host-exec path picks the
// most-derived declarer the target's class descends from. The col pair is
// cosmetic-variant OPTIONAL: unresolved by latch time -> base fallback (the
// collar accessory drop is skipped; logged).
void* g_dropPropFnBase     = nullptr;  // kerfurOmega_C::dropKerfurProp
void* g_dropPropFnCol      = nullptr;  // kerfurOmega_col_C::dropKerfurProp (optional)
void* g_dropPropFnColGamer = nullptr;  // kerfurOmega_col_gamer_C::dropKerfurProp (optional)
void* g_colClass           = nullptr;
void* g_colGamerClass      = nullptr;
void* g_spawnKerfuroFn     = nullptr;  // prop_kerfurOmega_C::spawnKerfuro (sole declarer)

// ---- deferred-action queue ----------------------------------------------------
// The interceptors fire inside the ProcessEvent detour (possibly a parallel-
// anim worker) and must not call engine functions / Post / re-enter PE / walk
// element registries (game_thread.h interceptor contract + the Snapshot-race
// note above). They only RECORD the action; Tick() (game thread, net pump)
// resolves + acts.
//
// kind=Request (client): the local dispatch was CANCELLED; Tick resolves the
//   wire eid off the actor's mirror Element and sends KerfurConvertRequest.
// kind=Converge (host): the BP dispatch ran natively; Tick converges the
//   BP-internal side effects -- but only on the tick AFTER it was armed
//   (audit C2: a nested dispatch inside the verb can drain the queue mid-verb;
//   the armed handoff guarantees a full pump-tick boundary).
struct PendingAction {
    void*  actor = nullptr;   // map-key / mirror-match use; only dereferenced via IsLiveByIndex
    int32_t internalIdx = -1; // liveness anchor captured while live (the dispatching self)
    coop::element::ElementId hostEid = coop::element::kInvalidId;  // host path: captured at push
    uint8_t toProp = 0;
    bool    isRequest = false;  // true = client request; false = host converge
    bool    armed = false;      // C2 two-phase: arm on one Tick, execute on the next
};
constexpr int kMaxPending = 8;
std::mutex g_pendingMutex;
PendingAction g_pending[kMaxPending];
int g_pendingCount = 0;

void PushPending(const PendingAction& pa) {
    std::lock_guard<std::mutex> lk(g_pendingMutex);
    if (g_pendingCount >= kMaxPending) {
        // User-action-rate events; 8 in flight means something is broken --
        // drop newest + count it (visible in the log, no silent loss).
        static std::atomic<uint32_t> sDropped{0};
        UE_LOGW("kerfur_convert: pending queue full -- dropping entry (#%u)",
                sDropped.fetch_add(1, std::memory_order_relaxed) + 1);
        return;
    }
    g_pending[g_pendingCount++] = pa;
}

// Read an FString-typed param out of a ProcessEvent params frame: the 16-byte
// TArray<wchar_t> {Data, Num, Max}. Memory reads only (worker-safe). Empty on
// null/insane.
std::wstring ReadFStringParam(void* params, int32_t off) {
    if (!params || off < 0) return {};
    auto* base = reinterpret_cast<uint8_t*>(params) + off;
    auto* data = *reinterpret_cast<wchar_t* const*>(base);
    const int32_t num = *reinterpret_cast<const int32_t*>(base + 8);
    if (!data || num <= 1 || num > 256) return {};  // Num includes the terminator
    return std::wstring(data, static_cast<size_t>(num - 1));
}

// CLIENT wire-eid resolution (audit C1). The id a request must carry is the
// HOST's element id, which on a client lives ONLY in the wire-mirror Element
// (IsMirror()==true) the receivers bound to this actor. The claim-bound
// owner-client save prop carries TWO elements (peer-range local shadow + the
// wire mirror -- remote_prop.cpp's "two Elements per actor"); prefer the
// mirror. Bounded Snapshot walk at user-action rate. GAME THREAD ONLY (the
// snapshot's raw element pointers race GT drains if read off-thread).
template <typename T>
coop::element::ElementId FindWireEidForActor(void* actor) {
    if (!actor) return coop::element::kInvalidId;
    std::vector<T*> elems;
    coop::element::MirrorManager<T>::Instance().Snapshot(elems);
    coop::element::ElementId nonMirror = coop::element::kInvalidId;
    for (T* el : elems) {
        if (!el || el->GetActor() != actor) continue;
        if (el->IsMirror()) return el->GetId();
        nonMirror = el->GetId();
    }
    return nonMirror;
}

// ---- host-side converge ------------------------------------------------------
// After the verb ran on the host, mirror the BP-INTERNAL side effects the
// pipelines cannot see (header: WHY IT DUPED). Idempotent end to end:
//  - destroy-sync only when the actor actually died (!IsLiveByIndex; the
//    sentient-refusal / locked cases leave it alive -> clean no-op),
//  - prop ingest is latch-deduped (ExpressSpawnedProp/HasProcessedInit) and
//    only offered UNTRACKED actors,
//  - RegisterExistingWorldNpcs skips already-tracked actors.
// Game thread only (walks + ProcessEvent-adjacent reads).

void IngestSpawnedConversionProps() {
    // Targeted GUObjectArray walk: untracked live instances of the kerfur-prop
    // lineage (the dropProp output) + the floppy lineage (the carried-floppy
    // output). User-action-rate cold path -- same cost class as the connect-
    // edge world walks. Fast pointer-compare filter first (the npc_sync walk
    // shape): >99% of slots fail the descendant check before any wstring alloc.
    void* bases[2] = {g_kerfurPropClass, g_floppyClass};
    const size_t nBases = g_floppyClass ? 2u : 1u;
    if (!g_kerfurPropClass) return;
    const int32_t n = R::NumObjects();
    int ingested = 0;
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        void* cls = R::ClassOf(obj);
        if (!cls || !R::IsDescendantOfAny(cls, bases, nBases)) continue;
        if (!R::IsLive(obj)) continue;
        if (PT::GetPropElementIdForActor(obj) != coop::element::kInvalidId) continue;  // tracked
        const std::wstring nm = R::ToString(R::NameOf(obj));
        if (nm.rfind(L"Default__", 0) == 0) continue;  // CDO
        coop::prop_lifecycle::ExpressSpawnedProp(obj);  // latch-deduped canonical keyed broadcast
        ++ingested;
    }
    if (ingested > 0)
        UE_LOGI("kerfur_convert: ingested %d conversion-spawned prop(s) (Init is BP-internal -- explicit ExpressSpawnedProp)", ingested);
}

void ConvergeAfterConversion(void* actor, int32_t internalIdx,
                             coop::element::ElementId eid, uint8_t toProp) {
    if (toProp) {
        // turn_off: the NPC should have died (dropKerfurProp -> K2_DestroyActor,
        // PE-invisible) and a prop (+ maybe floppy) spawned. A sentient kerfur
        // refused: actor still live -> skip both syncs naturally.
        if (actor && !R::IsLiveByIndex(actor, internalIdx)) {
            coop::npc_sync::SyncDestroyedNpcActor(actor);  // pointer-as-map-key only; purge-safe
        }
        IngestSpawnedConversionProps();
    } else {
        // turn on: the prop should have died (spawnKerfuro success path) and a
        // kerfur NPC spawned. Spawn failure leaves the prop alive (BP hint
        // path) -> both syncs no-op.
        if (actor && !R::IsLiveByIndex(actor, internalIdx) &&
            eid != coop::element::kInvalidId) {
            coop::prop_lifecycle::SyncDestroyedTrackedProp(actor, eid);  // element-based; no actor deref
        }
        // Registers the new (BP-internally spawned) kerfur + broadcasts EntitySpawn to connected
        // peers (the v67 addition inside). MidSessionConverge origin -> savePersisted=0: the
        // already-connected peers have NO local twin of this just-spawned kerfur, so they fresh-spawn
        // a mirror immediately instead of being routed into the v75 connect-edge adoption poll (which
        // would cause an ~8s pop-in + a class-only false-bind dupe -- RCA 2026-06-15).
        coop::npc_world_enum::RegisterExistingWorldNpcs(coop::npc_world_enum::NpcEnumOrigin::MidSessionConverge);
    }
}

// Pick the most-derived dropKerfurProp declarer for this actor's class --
// ProcessEvent runs exactly the UFunction passed, so this IS the virtual
// dispatch the BP's own by-name call would have done.
void* PickDropPropFn(void* cls) {
    if (g_dropPropFnColGamer && g_colGamerClass &&
        R::IsDescendantOfAny(cls, &g_colGamerClass, 1)) return g_dropPropFnColGamer;
    if (g_dropPropFnCol && g_colClass &&
        R::IsDescendantOfAny(cls, &g_colClass, 1)) {
        return g_dropPropFnCol;
    }
    if (!g_dropPropFnColGamer || !g_dropPropFnCol) {
        // If the actor IS a col variant but its override never resolved, the
        // base body still converts correctly minus the collar drop -- log it.
        static std::atomic<bool> sWarned{false};
        if (g_colClass && R::IsDescendantOfAny(cls, &g_colClass, 1) &&
            !sWarned.exchange(true)) {
            UE_LOGW("kerfur_convert: col-variant kerfur converted via BASE dropKerfurProp (override unresolved at install latch)");
        }
    }
    return g_dropPropFnBase;
}

// ---- the two PRE interceptors ------------------------------------------------
// Contract (game_thread.h): NO engine calls, NO Post, NO PE re-entry, NO
// element-registry walks (Snapshot raw pointers race GT drains off-thread).
// Memory reads, the atomic session load and the leaf pending mutex only --
// every decision needing engine/registry access defers to Tick().

// kerfurOmega_C::actionName (all skin subclasses inherit this one UFunction).
bool OnKerfurActionNamePre(void* self, void* params) {
    if (!self || !params || g_nameParamOff < 0) return false;
    auto* s = LoadSession();
    if (!s || !s->running() || !s->connected()) return false;  // SP untouched
    const std::wstring name = ReadFStringParam(params, g_nameParamOff);
    if (name != L"turn_off") {
        // v74: the State-changing radial verbs (follow/idle/patrol/fix_servers/get_reports/
        // fix_transformers) go to the host-authoritative command relay. It RECORDS the action +
        // returns true (cancel) for a relayed verb, false (leave it) for an unrelayed one. One
        // interceptor per UFunction, so the convert + command paths share this single hook.
        const bool isClient = s->role() == coop::net::Role::Client;
        const bool isHost   = s->role() == coop::net::Role::Host;
        return coop::kerfur_command::TryRecordMenuCommand(self, name, isClient, isHost);
    }
    if (s->role() == coop::net::Role::Client) {
        PendingAction pa{};
        pa.actor = self;
        pa.internalIdx = R::InternalIndexOf(self);  // live now (the dispatching self)
        pa.toProp = 1;
        pa.isRequest = true;
        PushPending(pa);
        UE_LOGI("kerfur_convert[client]: turn_off cancelled -> request queued (actor=%p)", self);
        return true;  // cancel the local conversion -- a client never converts locally
    }
    if (s->role() == coop::net::Role::Host) {
        PendingAction pa{};
        pa.actor = self;
        pa.internalIdx = R::InternalIndexOf(self);
        pa.hostEid = coop::npc_sync::GetNpcIdForActor(self);  // leaf-mutex map, worker-safe
        pa.toProp = 1;
        PushPending(pa);  // BP converts natively NOW; Tick() converges NEXT tick (C2)
    }
    return false;
}

// prop_kerfurOmega_C::actionOptionIndex. Action 8 == the single "turn on"
// option (ubergraph entry 29: NotEqual(action,8) -> return, else spawnKerfuro).
bool OnKerfurPropActionIdxPre(void* self, void* params) {
    if (!self || !params || g_actionParamOff < 0) return false;
    auto* s = LoadSession();
    if (!s || !s->running() || !s->connected()) return false;
    const uint8_t action =
        *(reinterpret_cast<const uint8_t*>(params) + g_actionParamOff);
    if (action != 8) return false;
    if (s->role() == coop::net::Role::Client) {
        PendingAction pa{};
        pa.actor = self;
        pa.internalIdx = R::InternalIndexOf(self);
        pa.toProp = 0;
        pa.isRequest = true;
        PushPending(pa);
        UE_LOGI("kerfur_convert[client]: turn-on cancelled -> request queued (actor=%p)", self);
        return true;
    }
    if (s->role() == coop::net::Role::Host) {
        PendingAction pa{};
        pa.actor = self;
        pa.internalIdx = R::InternalIndexOf(self);
        pa.hostEid = PT::GetPropElementIdForActor(self);  // leaf-mutex map, worker-safe
        pa.toProp = 0;
        PushPending(pa);
    }
    return false;
}

// CLIENT: resolve the wire eid + send the request. Game thread (Tick drain).
// The actor was the live menu target one tick ago; it can only have died
// meanwhile through a wire-applied destroy -- in which case the mirror is
// gone and the resolve fails -> drop with a log (nothing to convert anyway).
void SendQueuedRequest(const PendingAction& pa) {
    auto* s = LoadSession();
    if (!s || !s->connected()) return;
    if (!pa.actor || !R::IsLiveByIndex(pa.actor, pa.internalIdx)) {
        UE_LOGW("kerfur_convert[client]: queued %s target died before send -- dropped",
                pa.toProp ? "turn_off" : "turn-on");
        return;
    }
    const coop::element::ElementId eid =
        pa.toProp ? FindWireEidForActor<coop::element::Npc>(pa.actor)
                  : FindWireEidForActor<coop::element::Prop>(pa.actor);
    if (eid == coop::element::kInvalidId) {
        // No mirror element bound to this actor: a rogue local entity from a
        // pre-v67 session, or a bind gap. The dispatch was already cancelled
        // (a client must never convert locally -- that IS the dupe); surface it.
        UE_LOGW("kerfur_convert[client]: %s on UNTRACKED actor %p -- no request (rogue/bind gap)",
                pa.toProp ? "turn_off" : "turn-on", pa.actor);
        return;
    }
    coop::net::KerfurConvertPayload p{};
    p.elementId = static_cast<uint32_t>(eid);
    p.toProp = pa.toProp;
    s->SendReliable(coop::net::ReliableKind::KerfurConvertRequest, &p, sizeof(p));
    UE_LOGI("kerfur_convert[client]: sent %s request eid=%u",
            pa.toProp ? "turn_off" : "turn-on", p.elementId);
}

// ============================================================================
// THE conversion detection: a POLL, not an interceptor.
//
// The radial-menu conversion is 100% INVISIBLE to our hook engine. The engine is
// ONE MinHook detour on UObject::ProcessEvent (ue_wrap/game_thread.h); the menu
// verb is EX_LocalVirtualFunction, the spawn EX_CallMath, the destroy
// EX_VirtualFunction-native-branch -- NONE dispatch through ProcessEvent
// (votv-kerfur-convert-RE-2026-06-12.md 3). So every interceptor we tried on
// actionName / actionOptionIndex / BeginDeferred could never fire (proven: two
// full sessions, zero firings). We detect the conversion the way the project
// handles all invisible BP lifecycle -- a death-watch POLL:
//
//   a kerfur MIRROR whose actor has DIED while its wire Element is STILL present
//   == the local game just converted it. (A wire-driven destroy releases the
//   Element FIRST, so element-present + actor-dead is ALWAYS a local conversion;
//   and we only fire on a genuine ALIVE->DEAD transition, never a stale mirror.)
//
//   CLIENT: forward the host-auth KerfurConvertRequest (the host's OnConvertRequest
//     runs the real verb + converges EXPLICITLY to the wire -- no ProcessEvent
//     dependency) AND sweep the local ghost the invisible BeginDeferred spawned
//     (the dupe the user saw: an untracked ghost prop, broadcast when grabbed).
//   HOST: converge its OWN toggle straight to the wire (ConvergeAfterConversion).
//
// Game-thread only (Tick). 5 Hz throttle bounds the Prop snapshot walk.
// ============================================================================

struct KerfurWatch { float x, y, z; bool handled; };
std::unordered_map<uint32_t, KerfurWatch> g_kerfurWatch;  // eid -> last-live pose + dedupe; GT-only
std::chrono::steady_clock::time_point g_lastConvPoll{};

void SendConvertRequestDirect(uint32_t eid, uint8_t toProp) {
    auto* s = LoadSession();
    if (!s || !s->connected()) return;
    coop::net::KerfurConvertPayload p{};
    p.elementId = eid;
    p.toProp = toProp;
    s->SendReliable(coop::net::ReliableKind::KerfurConvertRequest, &p, sizeof(p));
    UE_LOGI("kerfur_convert[client]: POLL -> sent %s request eid=%u (mirror converted locally)",
            toProp ? "turn_off" : "turn-on", eid);
}

// ---- conversion-ghost CLAIM + ADOPT (2026-06-15 hands-on) ---------------------
// The client's own toggle spawns the NEW-form actor through the PE-invisible
// EX_CallMath path -- a live UNTRACKED ghost we cannot prevent. DESTROYING it (the
// earlier sweep) only traded the dupe for a destroy/respawn pop AND still lost the
// race to a grab: an untracked ghost, once grabbed, is broadcast as a NEW client
// entity the host mirrors -> the cascade behind "A LOT of dupes on host" (proven by
// eid 44116: a turn-off ghost prop, grabbed, broadcast client-range, then toggled).
// ROOT FIX: never leave an untracked ghost. The instant the poll detects our toggle,
// CLAIM the ghost (park NPC / freeze prop so it stays put + stops local AI/physics),
// then ADOPT it as the host mirror when the authoritative entity arrives:
//   - NPC (turn-on):  npc_mirror::OnEntitySpawn calls FindParkedGhostNpcNear + binds
//                     THAT actor (AdoptExistingNpcAsMirror) -- no respawn pop.
//   - PROP (turn-off): the EXISTING Gap-I-1 fuzzy match (remote_prop_spawn::OnSpawn)
//                     binds the frozen prop. Freezing is the enabler: the dropped prop
//                     used to FALL out of Gap-I-1's 30 cm window before the host's
//                     PropSpawn arrived; frozen, it stays put and the match lands.
// A ghost the host never confirms (sentient-kerfur reject / dropped request) is
// destroyed by CleanupParkedGhosts after a timeout, so it can never be grabbed.
struct ParkedGhost {
    void*   actor;
    int32_t idx;
    uint8_t toProp;  // 1 = prop ghost (turn-off result), 0 = NPC ghost (turn-on result)
    std::chrono::steady_clock::time_point armed;
};
std::vector<ParkedGhost> g_parkedGhosts;  // GT-only (poll + OnEntitySpawn are both game thread)

// Collect the actors bound to WIRE MIRROR Elements (IsMirror()==true) of the requested type(s).
// THIS is the authoritative "the host owns this actor" signal -- NOT the per-actor
// prop_element_tracker / npc_sync reverse maps. A Gap-I-1 fuzzy-match rekey (prop adopt) and an
// npc_mirror Install (NPC adopt) bind the ghost as a MIRROR Element but do NOT update those reverse
// maps, so the maps falsely read a just-adopted ghost as untracked. (That exact staleness made the
// cleanup destroy a freshly Gap-I-1-adopted prop -> the poll saw the prop "die" -> a SPURIOUS
// turn-on -> churn; caught by the 2026-06-15 kerfurtoggle autonomous test.) Built once per caller
// (small vector-pointer copy under the manager's leaf mutex), then O(1) lookups.
void CollectMirrorActors(bool wantProp, bool wantNpc, std::unordered_set<void*>& out) {
    if (wantProp) {
        std::vector<coop::element::Prop*> v;
        coop::element::MirrorManager<coop::element::Prop>::Instance().Snapshot(v);
        for (auto* el : v)
            if (el && el->IsMirror() && el->GetActor()) out.insert(el->GetActor());
    }
    if (wantNpc) {
        std::vector<coop::element::Npc*> v;
        coop::element::MirrorManager<coop::element::Npc>::Instance().Snapshot(v);
        for (auto* el : v)
            if (el && el->IsMirror() && el->GetActor()) out.insert(el->GetActor());
    }
}

// Find + park/freeze the client's own just-spawned conversion ghost(s): UNTRACKED live
// kerfur(s) of the new form near `pos`. NPC turn-on spawns one kerfurOmega; prop
// turn-off can drop the prop AND its carried floppy -> claim all near. One cold-path
// GUObjectArray walk, only on a detected client toggle.
void ClaimConversionGhosts(bool wantNpc, float x, float y, float z) {
    void* bases[2];
    size_t nBases = 0;
    if (wantNpc) {
        if (g_kerfurNpcClass) { bases[0] = g_kerfurNpcClass; nBases = 1; }
    } else if (g_kerfurPropClass) {
        bases[0] = g_kerfurPropClass; nBases = 1;
        if (g_floppyClass) { bases[1] = g_floppyClass; nBases = 2; }
    }
    if (nBases == 0) return;
    // The host's authoritative kerfurs of this form are WIRE MIRRORS -- skip them; the only
    // non-mirror kerfur of the new form at the conversion site is our own local ghost.
    std::unordered_set<void*> mirrors;
    CollectMirrorActors(/*wantProp=*/!wantNpc, /*wantNpc=*/wantNpc, mirrors);
    const auto now = std::chrono::steady_clock::now();
    const int32_t n = R::NumObjects();
    constexpr float kR2 = 500.f * 500.f;  // the new form spawns at the kerfur's own transform
    int claimed = 0;
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        void* cls = R::ClassOf(obj);
        if (!cls || !R::IsDescendantOfAny(cls, bases, nBases)) continue;
        if (!R::IsLive(obj)) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
        if (mirrors.count(obj)) continue;  // a host wire mirror -- not our ghost
        const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(obj);
        const float dx = loc.X - x, dy = loc.Y - y, dz = loc.Z - z;
        if (dx * dx + dy * dy + dz * dz > kR2) continue;
        if (wantNpc) {
            ue_wrap::puppet::DisableCharacterTicks(obj);  // park: the streamed pose becomes authoritative
            ue_wrap::kerfur::NeutralizeAiTimers(obj);      // stop the kerfur AI timers on the ghost
        } else {
            ue_wrap::engine::SetActorSimulatePhysics(obj, false);  // freeze -> stays inside Gap-I-1's 30 cm
        }
        g_parkedGhosts.push_back(ParkedGhost{obj, R::InternalIndexOf(obj),
                                             wantNpc ? uint8_t(0) : uint8_t(1), now});
        ++claimed;
    }
    if (claimed)
        UE_LOGI("kerfur_convert[client]: claimed %d local conversion ghost(s) (%s) near (%.0f,%.0f,%.0f) -- parked for host adoption",
                claimed, wantNpc ? "NPC turn-on" : "prop turn-off", x, y, z);
}

// Reap parked ghosts each poll: drop ones that DIED or got ADOPTED (now tracked by the
// host's authoritative entity), and destroy ones the host never confirmed after the
// timeout (a rejected sentient-kerfur / dropped request) so they cannot be grabbed into
// a client-eid dupe. Cheap no-op when the list is empty. Game thread.
void CleanupParkedGhosts() {
    if (g_parkedGhosts.empty()) return;
    bool haveProp = false, haveNpc = false;
    for (const auto& g : g_parkedGhosts) { if (g.toProp) haveProp = true; else haveNpc = true; }
    // "Adopted" == bound as a WIRE MIRROR (Gap-I-1 RegisterPropMirror / npc_mirror Install). The
    // per-actor reverse maps do NOT reflect that, so we ask the MirrorManager directly.
    std::unordered_set<void*> mirrors;
    CollectMirrorActors(haveProp, haveNpc, mirrors);
    const auto now = std::chrono::steady_clock::now();
    constexpr long kOrphanTimeoutMs = 4000;  // host replies within ~RTT; 4 s un-adopted == orphan
    for (auto it = g_parkedGhosts.begin(); it != g_parkedGhosts.end();) {
        void* actor = it->actor;
        if (!actor || !R::IsLiveByIndex(actor, it->idx)) { it = g_parkedGhosts.erase(it); continue; }
        if (mirrors.count(actor)) { it = g_parkedGhosts.erase(it); continue; }  // adopted as a wire mirror -> success
        const long ageMs = static_cast<long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - it->armed).count());
        if (ageMs >= kOrphanTimeoutMs) {
            UE_LOGI("kerfur_convert[client]: orphan conversion ghost actor=%p (%s) un-adopted after %ldms -- destroying (host rejected/dropped the toggle)",
                    actor, it->toProp ? "prop" : "NPC", ageMs);
            if (it->toProp) coop::prop_lifecycle::DestroyLocalProp(actor, /*deferred=*/false);
            else coop::npc_mirror::DestroyLocalNpcActor(actor);
            it = g_parkedGhosts.erase(it);
            continue;
        }
        ++it;
    }
}

// One ALIVE->DEAD transition pass over the kerfur mirror Elements (NPC = turn_off,
// prop = turn_on). Fires only for an eid we previously cached LIVE, so a stale /
// never-live mirror never false-triggers.
void PollKerfurConversions() {
    auto* s = LoadSession();
    if (!s || !s->connected()) return;
    if (!g_kerfurNpcClass || !g_kerfurPropClass) return;  // classes not resolved yet
    const auto now = std::chrono::steady_clock::now();
    if (g_lastConvPoll.time_since_epoch().count() != 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastConvPoll).count() < 200)
        return;  // ~5 Hz
    g_lastConvPoll = now;

    // Reap claimed conversion ghosts (drop adopted/dead, destroy un-confirmed orphans). Runs at
    // the poll cadence for both roles; a no-op on the host (it never claims ghosts).
    CleanupParkedGhosts();

    const bool isHost   = s->role() == coop::net::Role::Host;
    const bool isClient = s->role() == coop::net::Role::Client;
    // SYMPTOM-1 FIX (connect dupe, 2026-06-15 hands-on). This poll is a STEADY-STATE
    // detector: "kerfur mirror Element present + its actor died == the local game just
    // converted it." That invariant only holds once the world is settled. On a JOINING
    // client the world churns for ~tens of seconds (live-save load, multi-bracket
    // snapshot, divergence/claim sweeps, Gap-I-1 fuzzy de-dup) and mirror actors are
    // spawned then destroyed by the RECONCILE, not by a player operating a radial menu.
    // Reading those reconcile deaths as conversions fired spurious turn-on requests at
    // connect (client log 19:32:43: prop eid 3471/3472/3473 "died invisibly" -- BEFORE
    // load-tail quiescence at 19:33:26) and the host dutifully turned its lying props into
    // live NPCs ("the object turned into a live kerfur on connect"). Gate on the SAME
    // load-tail quiescence the divergence sweep + npc_adoption already use. CLIENT-only:
    // the host never arms that sweep (HasLoadTailQuiesced is permanently false there), and
    // the host has no transferred-save load tail -- its own kerfurs are stable from boot,
    // so it polls immediately and only ever sees real host-side conversions.
    if (isClient && !coop::remote_prop_spawn::HasLoadTailQuiesced()) return;
    std::unordered_set<uint32_t> seen;

    // turn_OFF: a kerfur NPC mirror whose actor died.
    std::vector<coop::element::Npc*> npcs;
    coop::element::MirrorManager<coop::element::Npc>::Instance().Snapshot(npcs);
    for (auto* el : npcs) {
        if (!el) continue;
        void* actor = el->GetActor();
        if (!actor || el->GetTypeName().find("kerfurOmega") == std::string::npos) continue;
        const uint32_t eid = static_cast<uint32_t>(el->GetId());
        seen.insert(eid);
        if (R::IsLiveByIndex(actor, el->GetInternalIdx())) {
            const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(actor);
            g_kerfurWatch[eid] = KerfurWatch{loc.X, loc.Y, loc.Z, false};
            continue;
        }
        auto it = g_kerfurWatch.find(eid);
        if (it == g_kerfurWatch.end() || it->second.handled) continue;  // never-live / already handled
        const float lx = it->second.x, ly = it->second.y, lz = it->second.z;
        UE_LOGI("kerfur_convert: POLL turn_off (kerfur NPC eid=%u died invisibly) -> %s",
                eid, isClient ? "client requests host" : "host broadcasts destroy+prop");
        if (isClient) {
            SendConvertRequestDirect(eid, 1);
            // The local turn-off dropped a kerfur prop (+ maybe its floppy) via the un-hookable
            // path. FREEZE it (do not destroy) so the host's authoritative prop ADOPTS it through
            // the Gap-I-1 fuzzy match -- freezing keeps it inside the 30 cm window (it used to
            // FALL out before the host's PropSpawn arrived, which broke the match -> dupe + pop).
            ClaimConversionGhosts(/*wantNpc=*/false, lx, ly, lz);
        } else if (isHost)
            ConvergeAfterConversion(actor, el->GetInternalIdx(),
                                    static_cast<coop::element::ElementId>(eid), /*toProp=*/1);
        it->second.handled = true;
    }

    // turn_ON: a kerfur PROP mirror whose actor died.
    std::vector<coop::element::Prop*> props;
    coop::element::MirrorManager<coop::element::Prop>::Instance().Snapshot(props);
    for (auto* el : props) {
        if (!el) continue;
        void* actor = el->GetActor();
        if (!actor || el->GetTypeName().find("prop_kerfurOmega") == std::string::npos) continue;
        const uint32_t eid = static_cast<uint32_t>(el->GetId());
        seen.insert(eid);
        if (R::IsLiveByIndex(actor, el->GetInternalIdx())) {
            const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(actor);
            g_kerfurWatch[eid] = KerfurWatch{loc.X, loc.Y, loc.Z, false};
            continue;
        }
        auto it = g_kerfurWatch.find(eid);
        if (it == g_kerfurWatch.end() || it->second.handled) continue;
        const float lx = it->second.x, ly = it->second.y, lz = it->second.z;
        UE_LOGI("kerfur_convert: POLL turn-on (kerfur prop eid=%u died invisibly) -> %s",
                eid, isClient ? "client requests host" : "host broadcasts destroy+npc");
        if (isClient) {
            SendConvertRequestDirect(eid, 0);
            // The local turn-on spawned a kerfur NPC via the un-hookable EX_CallMath path -- a
            // live UNTRACKED ghost beside the host's incoming mirror ("two kerfurs out of one
            // object"). CLAIM it (park) instead of destroying it: npc_mirror::OnEntitySpawn adopts
            // THAT exact actor as the host mirror (FindParkedGhostNpcNear), so no destroy/respawn
            // pop and no untracked ghost that a grab could cascade into a dupe.
            ClaimConversionGhosts(/*wantNpc=*/true, lx, ly, lz);
        } else if (isHost) {
            ConvergeAfterConversion(actor, el->GetInternalIdx(),
                                    static_cast<coop::element::ElementId>(eid), /*toProp=*/0);
        }
        it->second.handled = true;
    }

    // Prune watch entries whose element is gone (released by a wire destroy / session end).
    for (auto it = g_kerfurWatch.begin(); it != g_kerfurWatch.end();) {
        if (seen.count(it->first) == 0) it = g_kerfurWatch.erase(it);
        else ++it;
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_installed.load(std::memory_order_acquire)) return;
    // FindClass/FindFunction walk GUObjectArray -- throttle to ~1 attempt per
    // ~2 s of the pump (the ambient_spawner_suppress shape: the all-resolved
    // latch is the only early-out; partial-resolution retries are idempotent).
    // Deliberately NO give-up cap: the kerfur BP classes load lazily (a kerfur
    // can be PURCHASED mid-session), so the module must keep watching; the
    // unresolved-window cost is two scratch-buffer FindClass walks per attempt.
    static uint32_t sResolveN = 0;
    if ((sResolveN++ % 125) != 0) return;

    if (!g_kerfurNpcClass)  g_kerfurNpcClass  = R::FindClass(L"kerfurOmega_C");
    if (!g_kerfurPropClass) g_kerfurPropClass = R::FindClass(L"prop_kerfurOmega_C");
    if (!g_kerfurNpcClass || !g_kerfurPropClass) return;  // BP classes not loaded yet

    if (!g_actionNameFn) {
        g_actionNameFn = R::FindFunction(g_kerfurNpcClass, L"actionName");
        if (g_actionNameFn) {
            g_nameParamOff = R::FindParamOffset(g_actionNameFn, L"name");
            if (g_nameParamOff < 0) g_nameParamOff = R::FindParamOffset(g_actionNameFn, L"Name");
        }
    }
    if (!g_actionIdxFn) {
        g_actionIdxFn = R::FindFunction(g_kerfurPropClass, L"actionOptionIndex");
        if (g_actionIdxFn) {
            g_actionParamOff = R::FindParamOffset(g_actionIdxFn, L"action");
            if (g_actionParamOff < 0) g_actionParamOff = R::FindParamOffset(g_actionIdxFn, L"Action");
        }
    }
    if (!g_dropPropFnBase) g_dropPropFnBase = R::FindFunction(g_kerfurNpcClass, L"dropKerfurProp");
    if (!g_spawnKerfuroFn) g_spawnKerfuroFn = R::FindFunction(g_kerfurPropClass, L"spawnKerfuro");
    if (g_killOff < 0)     g_killOff = R::FindPropertyOffset(g_kerfurNpcClass, L"kill");

    // Optional refs (cosmetic col-variant overrides + the floppy lineage for
    // the ingest walk). Resolved opportunistically until the latch sets; a
    // miss degrades gracefully (PickDropPropFn fallback / 1-base walk).
    if (!g_colClass)      g_colClass      = R::FindClass(L"kerfurOmega_col_C");
    if (!g_colGamerClass) g_colGamerClass = R::FindClass(L"kerfurOmega_col_gamer_C");
    if (g_colClass && !g_dropPropFnCol)
        g_dropPropFnCol = R::FindFunction(g_colClass, L"dropKerfurProp");
    if (g_colGamerClass && !g_dropPropFnColGamer)
        g_dropPropFnColGamer = R::FindFunction(g_colGamerClass, L"dropKerfurProp");
    if (!g_floppyClass)   g_floppyClass   = R::FindClass(L"prop_floppyDisc_C");

    if (!g_actionNameFn || g_nameParamOff < 0 || !g_actionIdxFn ||
        g_actionParamOff < 0 || !g_dropPropFnBase || !g_spawnKerfuroFn) {
        UE_LOGW("kerfur_convert: partial resolve (actionName=%p nameOff=%d actionIdx=%p actionOff=%d drop=%p spawn=%p) -- retrying",
                g_actionNameFn, g_nameParamOff, g_actionIdxFn, g_actionParamOff,
                g_dropPropFnBase, g_spawnKerfuroFn);
        return;
    }
    if (g_killOff < 0) {
        // Non-fatal: the guard read degrades to "no guard" (a murder-mode
        // kerfur could be turned off by a request -- SP denies it). Log once.
        UE_LOGW("kerfur_convert: kerfurOmega_C 'kill' offset unresolved -- BP murder-guard not replicated");
    }
    // Audit I4: the host-exec path hands the verbs a zeroed 16-byte frame on
    // the kismet-proven fact they take NO params. If a game update adds one,
    // refuse to install rather than over-read the frame (loud, not silent).
    if (!R::FunctionParams(g_dropPropFnBase).empty() ||
        !R::FunctionParams(g_spawnKerfuroFn).empty()) {
        UE_LOGE("kerfur_convert: verb signature changed (dropKerfurProp/spawnKerfuro now take params) -- module DISABLED (re-RE the conversion BPs)");
        g_installed.store(true, std::memory_order_release);  // latch off
        return;
    }

    if (!GT::RegisterInterceptor(g_actionNameFn, &OnKerfurActionNamePre)) {
        UE_LOGE("kerfur_convert: RegisterInterceptor(actionName) failed (table full?)");
        return;
    }
    if (!GT::RegisterInterceptor(g_actionIdxFn, &OnKerfurPropActionIdxPre)) {
        GT::UnregisterInterceptor(g_actionNameFn, &OnKerfurActionNamePre);
        UE_LOGE("kerfur_convert: RegisterInterceptor(actionOptionIndex) failed -- rolled back actionName slot");
        return;
    }
    g_installed.store(true, std::memory_order_release);
    UE_LOGI("kerfur_convert: installed (actionName nameOff=%d, actionOptionIndex actionOff=%d, killOff=%d, col=%s colGamer=%s floppy=%s)",
            g_nameParamOff, g_actionParamOff, g_killOff,
            g_dropPropFnCol ? "yes" : "no", g_dropPropFnColGamer ? "yes" : "no",
            g_floppyClass ? "yes" : "no");
}

void OnConvertRequest(const coop::net::KerfurConvertPayload& payload,
                      uint8_t senderPeerSlot) {
    // Host-only (gated by the event_dispatch_state router). Game thread (the
    // event_feed drain) -- ProcessEvent calls are legal here, and because the
    // drain runs INSIDE the pump task (t_inPump set), the verb's nested
    // dispatches cannot re-enter the pump: the converge below always runs
    // strictly after the verb returns (audit C2, request-path analysis).
    if (!g_installed.load(std::memory_order_acquire)) {
        UE_LOGW("kerfur_convert: request before install resolved -- dropped");
        return;
    }
    // Audit I5: every legit target is a host-range element (host props/NPCs
    // are AllocHostId'd); reject out-of-range ids loudly (spoof/bug).
    if (!coop::element::Registry::IsAllowedHostAllocatedEid(
            static_cast<coop::element::ElementId>(payload.elementId))) {
        UE_LOGW("kerfur_convert: request eid=%u outside the host range -- dropped (slot %u)",
                payload.elementId, senderPeerSlot);
        return;
    }
    const auto eid = static_cast<coop::element::ElementId>(payload.elementId);
    if (payload.toProp) {
        auto* el = coop::element::MirrorManager<coop::element::Npc>::Instance().Get(eid);
        void* actor = el ? el->GetActor() : nullptr;
        const int32_t idx = el ? el->GetInternalIdx() : -1;
        if (!actor || !R::IsLiveByIndex(actor, idx)) {
            UE_LOGW("kerfur_convert: turn_off request eid=%u from slot %u -- no live NPC (already converted / stale) -- dropped",
                    payload.elementId, senderPeerSlot);
            return;
        }
        void* cls = R::ClassOf(actor);
        if (!cls || !R::IsDescendantOfAny(cls, &g_kerfurNpcClass, 1)) {
            UE_LOGW("kerfur_convert: turn_off request eid=%u targets a non-kerfur element -- dropped", payload.elementId);
            return;
        }
        // The BP's own guard (actionName ubergraph: `if (kill) return;` before
        // dropKerfurProp) -- replicated byte-exactly from the disassembly.
        if (g_killOff >= 0 &&
            *(reinterpret_cast<const bool*>(reinterpret_cast<const uint8_t*>(actor) + g_killOff))) {
            UE_LOGI("kerfur_convert: turn_off eid=%u denied -- kerfur is in kill mode (SP parity)", payload.elementId);
            return;
        }
        UE_LOGI("kerfur_convert: HOST executing turn_off eid=%u (slot %u)", payload.elementId, senderPeerSlot);
        uint8_t frame[16] = {};  // verbs take no params (install-guarded); zeroed frame for safety
        R::CallFunction(actor, PickDropPropFn(cls), frame);
        ConvergeAfterConversion(actor, idx, eid, /*toProp=*/1);
    } else {
        auto* el = coop::element::MirrorManager<coop::element::Prop>::Instance().Get(eid);
        void* actor = el ? el->GetActor() : nullptr;
        const int32_t idx = el ? el->GetInternalIdx() : -1;
        if (!actor || !R::IsLiveByIndex(actor, idx)) {
            UE_LOGW("kerfur_convert: turn-on request eid=%u from slot %u -- no live prop (already converted / stale) -- dropped",
                    payload.elementId, senderPeerSlot);
            return;
        }
        void* cls = R::ClassOf(actor);
        if (!cls || !R::IsDescendantOfAny(cls, &g_kerfurPropClass, 1)) {
            UE_LOGW("kerfur_convert: turn-on request eid=%u targets a non-kerfur-prop element -- dropped", payload.elementId);
            return;
        }
        UE_LOGI("kerfur_convert: HOST executing turn-on eid=%u (slot %u)", payload.elementId, senderPeerSlot);
        uint8_t frame[16] = {};  // spawnKerfuro takes no params (install-guarded)
        R::CallFunction(actor, g_spawnKerfuroFn, frame);
        ConvergeAfterConversion(actor, idx, eid, /*toProp=*/0);
    }
}

void Tick() {
    // THE conversion sync: poll for kerfur mirrors that converted invisibly (the
    // menu's PE-invisible EX_CallMath/EX_Local* flow). MUST run before the pending-
    // queue early-return below (it is independent of the queue). Cheap 5 Hz-gated.
    PollKerfurConversions();

    // Two-phase drain (audit C2): execute only entries armed by a PREVIOUS
    // Tick, then arm the rest. A host-menu entry pushed during the actionName
    // dispatch can have THIS Tick run from a nested dispatch INSIDE the verb
    // (UCS/BeginPlay/EndPlay re-enter the detour with t_inPump cleared) --
    // that nested drain only ARMS it; the pump composite's coalescing makes a
    // second pump execution within the same BP chain impossible, so the
    // executing Tick always sees the settled post-verb world. Client request
    // entries ride the same handoff (one extra ~16 ms tick, irrelevant vs RTT).
    PendingAction ready[kMaxPending];
    int nReady = 0;
    {
        std::lock_guard<std::mutex> lk(g_pendingMutex);
        if (g_pendingCount == 0) return;
        int w = 0;
        for (int i = 0; i < g_pendingCount; ++i) {
            if (g_pending[i].armed) {
                ready[nReady++] = g_pending[i];
            } else {
                g_pending[i].armed = true;
                g_pending[w++] = g_pending[i];
            }
        }
        g_pendingCount = w;
    }
    for (int i = 0; i < nReady; ++i) {
        if (ready[i].isRequest) {
            SendQueuedRequest(ready[i]);
        } else {
            UE_LOGI("kerfur_convert: HOST menu conversion converge (toProp=%u eid=%u)",
                    ready[i].toProp, static_cast<uint32_t>(ready[i].hostEid));
            ConvergeAfterConversion(ready[i].actor, ready[i].internalIdx,
                                    ready[i].hostEid, ready[i].toProp);
        }
    }
}

void* FindParkedGhostNpcNear(float x, float y, float z) {
    // The actor of the parked turn-on conversion-ghost NPC nearest (x,y,z), or nullptr.
    // npc_mirror::OnEntitySpawn calls this for a host kerfur EntitySpawn to ADOPT the client's
    // own local conversion result (the kerfur it just turned on) instead of fresh-spawning a
    // duplicate beside it. Only the INITIATING client has a parked ghost -- other peers get
    // nullptr and fresh-spawn normally. Game thread (poll + OnEntitySpawn share the GT).
    void* best = nullptr;
    float bestD2 = 500.f * 500.f;
    for (const auto& g : g_parkedGhosts) {
        if (g.toProp) continue;  // NPC (turn-on) ghosts only
        if (!g.actor || !R::IsLiveByIndex(g.actor, g.idx)) continue;
        const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(g.actor);
        const float dx = loc.X - x, dy = loc.Y - y, dz = loc.Z - z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bestD2) { bestD2 = d2; best = g.actor; }
    }
    return best;
}

void OnDisconnect() {
    {
        std::lock_guard<std::mutex> lk(g_pendingMutex);
        g_pendingCount = 0;
    }
    g_kerfurWatch.clear();   // GT-only; Tick is the sole toucher
    g_parkedGhosts.clear();  // GT-only conversion-ghost claim list
    g_lastConvPoll = {};
}

}  // namespace coop::kerfur_convert
