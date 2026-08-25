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

#include "coop/creatures/kerfur_convert.h"

#include "coop/creatures/kerfur_command.h"  // v74: route the non-turn_off menu verbs to the command relay
#include "coop/creatures/kerfur_convert_client.h"  // s27 cut: ghost custody + wire apply (SetSession/SetClasses handoffs)
#include "coop/creatures/kerfur_convert_host.h"    // s27 cut: converge + request exec (SetClasses/SetVerbs handoffs)
#include "coop/creatures/kerfur_form_assembler.h"  // 2a-capture: ConsumeCapturedForm/ClearCapturedForm (deterministic which-B)
#include "coop/element/mirror_manager.h"
#include "coop/element/npc.h"
#include "coop/element/prop.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/creatures/npc_sync.h"       // RegisterHostNpcSilent / ReleaseNpcElementSilent (silent host ops)
#include "coop/creatures/kerfur_entity.h"  // BindFormActor / BroadcastConvertRejected + the resolved classes
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_lifecycle.h"
#include "coop/props/join_membership_sweep.h"  // anti-smear 2026-06-30: claim+sweep extracted out of remote_prop_spawn
#include "ue_wrap/engine/engine.h"      // GetActorLocation (converge/seam position reads)
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "coop/config/config.h"  // OBSERVE (2026-07-14 G1): vm_dispatch_log gate for the decline/provenance lines
#include "ue_wrap/core/vm_dispatch.h" // OBSERVE (2026-07-14 G1): CurrentThreadVerb() destroy-provenance at the decline

#include <atomic>
#include <chrono>
#include <cmath>     // sqrt (first-refusal converge distance log)
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cwchar>

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
void* g_actionNameFn    = nullptr;  // kerfurOmega_C::actionName (the menu dispatcher -- kerfur_command relay)
int32_t g_nameParamOff  = -1;       // actionName 'name' FString param offset
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

// NOTE (K-4b 2026-06-16): the deferred-action queue (PendingAction / PushPending / SendQueuedRequest /
// FindWireEidForActor) + the actionOptionIndex (turn-on) interceptor are GONE -- the menu verbs are
// EX_LocalVirtualFunction, INVISIBLE to our single ProcessEvent detour, so those interceptors NEVER
// fired for the conversion (proven: the cancel/queue lines never appeared in any real session). The
// conversion is detected entirely by the death-watch POLL (PollKerfurConversions): the CLIENT sends
// KerfurConvertRequest + claims its local ghost; the HOST converges to KerfurConvert. This is the
// dual-driver collapse (redesign 11): host poll = host-initiated detection; client = request only.
// The actionName interceptor SURVIVES solely for the kerfur_command relay (v74, a separate feature).

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

// The host executor (converge + OnConvertRequest + the request-verb bracket) lives in
// coop/creatures/kerfur_convert_host.cpp; the client apply + conversion-ghost custody in
// coop/creatures/kerfur_convert_client.cpp (s27 cut).

// ---- the actionName PRE interceptor (kerfur_command relay only) --------------
// Contract (game_thread.h): NO engine calls, NO Post, NO PE re-entry, NO
// element-registry walks (Snapshot raw pointers race GT drains off-thread).
// Memory reads + the atomic session load only. The CONVERSION is poll-driven
// (this hook is PE-invisible for the menu verb -- see the note above); this
// interceptor survives solely for the v74 kerfur_command menu relay.

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
    // turn_off (the NPC->prop conversion) is detected by the death-watch POLL, not here -- this
    // interceptor is PE-invisible for the EX_LocalVirtualFunction menu verb (proven: the cancel/queue
    // lines never appeared in any real session). Pass through; the poll carries 100% of detections.
    return false;
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

// R4 (2026-06-18, MTA CClientEntity::m_ucSyncTimeContext shape): `actor` records WHICH
// actor was live when we cached the entry. The death path fires a conversion only if the
// dead actor IS that same one -- so if the eid was freed + reallocated to a NEW kerfur
// mirror (a different actor) since we cached it, and that new mirror died before a poll
// confirmed it live, the cached entry belongs to the OLD generation and we DON'T fire on
// it. (R3 stopped the divergence sweep from killing mirrors -- the dominant flip-flop
// cause; this is the belt against the narrow eid-reuse race. A zero/legacy actor still
// fires, matching m_ucSyncTimeContext's "context 0 == accept".)
struct KerfurWatch { float x, y, z; bool handled; void* actor; };
std::unordered_map<uint32_t, KerfurWatch> g_kerfurWatch;  // eid -> last-live pose + dedupe + generation actor; GT-only
std::chrono::steady_clock::time_point g_lastConvPoll{};

// (2026-07-14) The FRESH-SPAWN STAMP proximity fallback that used to live here is RETIRED. Its job
// -- "which fresh NPC is this dying prop's conversion product?" -- is now answered DETERMINISTICALLY
// by the form assembler's in-bracket captured-B (ConsumeCapturedForm), consumed at the paired
// destroy edge in TryCaptureKerfurPropDestroy. Retirement evidence: across the 2026-07-14 takes
// (G1 + 19:09 + 20:20, both peers) the captured-B path HIT 85 conversions and the stamp fallback
// was entered ZERO times (0 "MISS in-bracket", 0 DECLINE). Dead as a fallback -> gone (RULE 2).

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

// One ALIVE->DEAD transition pass over the kerfur mirror Elements (NPC = turn_off,
// prop = turn_on). Fires only for an eid we previously cached LIVE, so a stale /
// never-live mirror never false-triggers.
void PollKerfurConversions() {
    // HOSTING-gated for the HOST (RULE 1 class fix 2026-07-05, the 0s failure family):
    // a solo-host radial-menu conversion must still CONVERGE (old-form release + new-form
    // silent enroll into the mirrors) or a later joiner's connect-snapshot has neither
    // form -- the kerfur vanishes for the joiner and a grab of the untracked prop dupes.
    // A CLIENT still requires a live connection (its branch REQUESTS the host).
    auto* s = LoadSession();
    if (!s) return;
    if (s->role() != coop::net::Role::Host && !s->connected()) return;
    if (!g_kerfurNpcClass || !g_kerfurPropClass) return;  // classes not resolved yet
    const auto now = std::chrono::steady_clock::now();
    if (g_lastConvPoll.time_since_epoch().count() != 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastConvPoll).count() < 200)
        return;  // ~5 Hz
    g_lastConvPoll = now;

    // Reap claimed conversion ghosts (drop adopted/dead, destroy un-confirmed orphans). Runs at
    // the poll cadence for both roles; a no-op on the host (it never claims ghosts).
    coop::kerfur_convert_client::CleanupParkedGhosts();

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
    if (isClient && !coop::join_membership_sweep::HasLoadTailQuiesced()) return;
    // scope A (kerfur off->active dup retire) is NO LONGER driven here. Its SEQUENCING moved to the ONE
    // join-window order owner (coop::element::quiescence_drain::RunReconcile, whose steady-state tick is
    // bracket-INDEPENDENT and ORs kerfur_reconcile::HasPendingRetire into its HasPendingWork gate). This poll
    // was a THIRD parallel order owner for the join-window axis -- removed in the 2026-06-30 anti-smear
    // refactor. [[feedback-one-owner-order-axis]] This poll keeps its OWN job: detecting ALIVE->DEAD kerfur
    // conversions (below).
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
            g_kerfurWatch[eid] = KerfurWatch{loc.X, loc.Y, loc.Z, false, actor};  // R4: stamp the live actor (generation identity)
            continue;
        }
        auto it = g_kerfurWatch.find(eid);
        if (it == g_kerfurWatch.end() || it->second.handled) continue;  // never-live / already handled
        // R4 stale-generation guard: only fire if the dead actor is the one we cached live
        // (else the eid was reused by a newer mirror -- this death is the old generation's).
        if (it->second.actor && it->second.actor != actor) { it->second.handled = true; continue; }
        const float lx = it->second.x, ly = it->second.y, lz = it->second.z;
        UE_LOGI("kerfur_convert: POLL turn_off (kerfur NPC eid=%u died invisibly) -> %s",
                eid, isClient ? "client requests host" : "host broadcasts destroy+prop");
        if (isClient) {
            SendConvertRequestDirect(eid, 1);
            // The local turn-off dropped a kerfur prop (+ maybe its floppy) via the un-hookable
            // path. FREEZE it (do not destroy) so the host's authoritative prop ADOPTS it through
            // the Gap-I-1 fuzzy match -- freezing keeps it inside the 30 cm window (it used to
            // FALL out before the host's PropSpawn arrived, which broke the match -> dupe + pop).
            coop::kerfur_convert_client::ClaimConversionGhosts(eid, /*wantNpc=*/false, lx, ly, lz);
        } else if (isHost)
            coop::kerfur_convert_host::ConvergeAfterConversion(actor, el->GetInternalIdx(),
                                    static_cast<coop::element::ElementId>(eid), /*toProp=*/1,
                                    lx, ly, lz);
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
            g_kerfurWatch[eid] = KerfurWatch{loc.X, loc.Y, loc.Z, false, actor};  // R4: stamp the live actor (generation identity)
            continue;
        }
        auto it = g_kerfurWatch.find(eid);
        if (it == g_kerfurWatch.end() || it->second.handled) continue;
        // R4 stale-generation guard (see turn_off above): drop a death whose eid was
        // reused by a newer prop mirror since we cached it live.
        if (it->second.actor && it->second.actor != actor) { it->second.handled = true; continue; }
        const float lx = it->second.x, ly = it->second.y, lz = it->second.z;
        UE_LOGI("kerfur_convert: POLL turn-on (kerfur prop eid=%u died invisibly) -> %s",
                eid, isClient ? "client requests host" : "host broadcasts destroy+npc");
        if (isClient) {
            SendConvertRequestDirect(eid, 0);
            // The local turn-on spawned a kerfur NPC via the un-hookable EX_CallMath path -- a
            // live UNTRACKED ghost beside the host's incoming mirror ("two kerfurs out of one
            // object"). CLAIM it (park) tagged with `eid` (the converting eid) instead of destroying it:
            // npc_mirror::OnEntitySpawn adopts THAT exact actor by eid (TakeParkedGhostByEid via the
            // EntitySpawn's convertFromEid), so no destroy/respawn pop and no untracked ghost a grab could dupe.
            coop::kerfur_convert_client::ClaimConversionGhosts(eid, /*wantNpc=*/true, lx, ly, lz);
        } else if (isHost) {
            coop::kerfur_convert_host::ConvergeAfterConversion(actor, el->GetInternalIdx(),
                                    static_cast<coop::element::ElementId>(eid), /*toProp=*/0,
                                    lx, ly, lz);
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
    coop::kerfur_convert_client::SetSession(session);  // s27 cut: per-call, mirrors the store above
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
    // K-3: share the resolved kerfur bases with the KerfurId table (idempotent) so its class-gate
    // predicates (IsKerfurClass/IsKerfurEid) and the K-5 mint-gate can answer without re-resolving.
    coop::kerfur_entity::SetKerfurClasses(g_kerfurNpcClass, g_kerfurPropClass);

    if (!g_actionNameFn) {
        g_actionNameFn = R::FindFunction(g_kerfurNpcClass, L"actionName");
        if (g_actionNameFn) {
            g_nameParamOff = R::FindParamOffset(g_actionNameFn, L"name");
            if (g_nameParamOff < 0) g_nameParamOff = R::FindParamOffset(g_actionNameFn, L"Name");
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
    // s27 two-layer handoff: push the class pointers EVERY attempt (idempotent overwrite) so
    // the custody/apply TU sees them as soon as they resolve -- exactly like the pre-cut
    // single-TU statics (incl. the DISABLED install state, where claims keep working).
    coop::kerfur_convert_client::SetClasses(g_kerfurNpcClass, g_kerfurPropClass, g_floppyClass);
    coop::kerfur_convert_host::SetClasses(g_kerfurNpcClass, g_kerfurPropClass, g_floppyClass);

    if (!g_actionNameFn || g_nameParamOff < 0 || !g_dropPropFnBase || !g_spawnKerfuroFn) {
        UE_LOGW("kerfur_convert: partial resolve (actionName=%p nameOff=%d drop=%p spawn=%p) -- retrying",
                g_actionNameFn, g_nameParamOff, g_dropPropFnBase, g_spawnKerfuroFn);
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

    // Only the actionName interceptor survives -- for the v74 kerfur_command relay (the conversion is
    // poll-driven now; the actionOptionIndex interceptor was retired in K-4b as never-firing dead code).
    if (!GT::RegisterInterceptor(g_actionNameFn, &OnKerfurActionNamePre)) {
        UE_LOGE("kerfur_convert: RegisterInterceptor(actionName) failed (table full?)");
        return;
    }
    // s27: verb refs + the request latch flip at THIS success site only (the same instant the
    // pre-cut g_installed gated OnConvertRequest). The DISABLED path above never reaches this
    // -> requests drop there (documented fail-closed deviation; see kerfur_convert_host.h).
    coop::kerfur_convert_host::SetVerbs(g_dropPropFnBase, g_dropPropFnCol, g_dropPropFnColGamer,
                                        g_colClass, g_colGamerClass, g_spawnKerfuroFn, g_killOff);
    g_installed.store(true, std::memory_order_release);
    UE_LOGI("kerfur_convert: installed (actionName nameOff=%d, killOff=%d, col=%s colGamer=%s floppy=%s; conversion = death-watch poll)",
            g_nameParamOff, g_killOff,
            g_dropPropFnCol ? "yes" : "no", g_dropPropFnColGamer ? "yes" : "no",
            g_floppyClass ? "yes" : "no");
}

void Tick() {
    // Conversion sync, BACKSTOP half: poll for kerfur mirrors that converted invisibly (the menu's
    // PE-invisible EX_CallMath/EX_Local* flow). The PRIMARY host detectors are event-driven at the
    // generic chokepoints: turn_off = TryAdoptFreshKerfurProp at the fresh prop's expression edge
    // (take-8 2026-07-12 -- the poll's untracked-search converge always lost the race to the
    // per-tick spawn-seam drain); turn-on = TryCaptureKerfurPropDestroy at the prop's destroy edge
    // (take-9 2026-07-13 -- the poll's "element present + actor dead" premise NEVER holds for a
    // host prop: the destroy seam drains the element synchronously with the death). This poll
    // remains the CLIENT-side driver (request + ghost claim; client mirror rows survive the seam)
    // and the solo-host / seam-missed backstop. Cheap 5 Hz-gated. Game thread.
    PollKerfurConversions();
}

// FIRST REFUSAL on the generic expression of a kerfur PROP-form actor (see the header + the
// take-8 2026-07-12 RCA). PUBLIC (header-declared); anon-ns file statics (g_kerfurWatch,
// g_kerfurNpcClass, ExpressConversionFloppies, ...) are visible within this TU.
//
// WHY spawn-edge, not the poll: the poll's converge premise ("the verb's fresh prop is
// UNTRACKED") died with spawn_authority Inc-1 (2026-07-10) -- the FinishSpawningActor seam
// drain expresses every fresh host prop on the NEXT TICK, the 5 Hz poll can never win that
// race, and the failed converge silently released the dead NPC with NO KerfurConvert (take-8
// 14:43: five host toggles, five "no new kerfur prop near", client kept five NPC mirrors AND
// gained five generic prop mirrors). The express chokepoints now offer the actor HERE first;
// the conversion-product question is answered by UNTRACKED + the DEAD-NPC WATCH match (both
// required: tracking state alone was the poll's dead premise, proximity alone steals neighbors).
bool TryAdoptFreshKerfurProp(void* actor) {
    namespace KE = coop::kerfur_entity;
    if (!actor) return false;
    auto* s = LoadSession();
    if (!s || s->role() != coop::net::Role::Host) return false;  // host-only authority
    if (!g_kerfurNpcClass || !g_kerfurPropClass) return false;
    if (!ue_wrap::game_thread::IsGameThread()) return false;     // express lanes are GT; defensive
    void* cls = R::ClassOf(actor);
    if (!cls || !R::IsDescendantOfAny(cls, &g_kerfurPropClass, 1)) return false;
    // UNTRACKED-ONLY (audit CRITICAL 2026-07-12, confidence 88): an already-tracked kerfur prop
    // is an ESTABLISHED identity -- a standing off-prop 80 cm from a dying neighbor, or any row
    // the connect-snapshot drain enumerates (that lane is fed from the Registry, so EVERY actor
    // it offers is tracked by construction). Matching one would steal its eid into the dying
    // kerfur's K (BindFormActor overwrites the victim's reverse maps -> zombie KerfurRecord,
    // client-side mirror corruption at newEid) while the REAL conversion product later
    // generic-expresses = the dupe again. Mirrors FindNewFormKerfurActor's `if (tracked)
    // continue;`. A genuine conversion product is ALWAYS untracked at both consult sites
    // (the prop_lifecycle express body consults BEFORE its MarkPropElement).
    if (PT::GetPropElementIdForActor(actor) != coop::element::kInvalidId) return false;

    // Match the fresh prop against a DEAD, un-handled, generation-valid kerfur NPC mirror
    // within the verb's spawn radius (the new form spawns at the kerfur's own transform;
    // 500 cm mirrors FindNewFormKerfurActor / ClaimConversionGhosts).
    const ue_wrap::FVector ploc = ue_wrap::engine::GetActorLocation(actor);
    constexpr float kR2 = 500.f * 500.f;
    coop::element::ElementId oldEid = coop::element::kInvalidId;
    void*   deadActor = nullptr;
    int32_t deadIdx   = -1;
    float   bestD2    = kR2;
    float   wx = 0, wy = 0, wz = 0;
    std::vector<coop::element::Npc*> npcs;
    coop::element::MirrorManager<coop::element::Npc>::Instance().Snapshot(npcs);
    for (auto* el : npcs) {
        if (!el) continue;
        void* a = el->GetActor();
        if (!a || el->GetTypeName().find("kerfurOmega") == std::string::npos) continue;
        if (R::IsLiveByIndex(a, el->GetInternalIdx())) continue;  // alive -> not a conversion source
        const uint32_t eid = static_cast<uint32_t>(el->GetId());
        auto it = g_kerfurWatch.find(eid);
        if (it == g_kerfurWatch.end() || it->second.handled) continue;      // never-live / handled
        if (it->second.actor && it->second.actor != a) continue;            // stale generation (R4)
        const float dx = it->second.x - ploc.X, dy = it->second.y - ploc.Y, dz = it->second.z - ploc.Z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bestD2) {
            bestD2 = d2;
            oldEid = el->GetId();
            deadActor = a;
            deadIdx = el->GetInternalIdx();
            wx = it->second.x; wy = it->second.y; wz = it->second.z;
        }
    }
    if (oldEid == coop::element::kInvalidId) return false;  // no dead kerfur nearby -> ordinary spawn

    // Converge WITH THE GIVEN actor (guaranteed untracked by the entry gate -> mint silently).
    const coop::element::ElementId newEid = coop::prop_lifecycle::RegisterHostPropSilent(actor);
    if (newEid == coop::element::kInvalidId) {
        UE_LOGW("kerfur_convert: first-refusal converge -- silent register failed for fresh prop %p; "
                "leaving it to the generic path (poll may still converge)", actor);
        return false;
    }
    coop::npc_sync::ReleaseNpcElementSilent(oldEid);
    const auto rot = ue_wrap::engine::GetActorRotation(actor);
    KE::BindFormActor(oldEid, actor, R::InternalIndexOf(actor), newEid, KE::Form::Prop,
                      R::ClassNameOf(actor),
                      ploc.X, ploc.Y, ploc.Z, rot.Pitch, rot.Yaw, rot.Roll);
    coop::kerfur_convert_host::ExpressConversionFloppies(wx, wy, wz);
    g_kerfurWatch[static_cast<uint32_t>(oldEid)].handled = true;  // the poll skips this death
    UE_LOGI("kerfur_convert: FIRST-REFUSAL turn_off converge -- fresh prop %p eid=%u adopted as the "
            "conversion product of dead kerfur NPC eid=%u (%.0f cm from its last-live pose; "
            "KerfurConvert broadcast, NO generic PropSpawn)",
            actor, static_cast<unsigned>(newEid), static_cast<unsigned>(oldEid),
            std::sqrt(bestD2));
    (void)deadActor; (void)deadIdx;
    return true;
}

// FIRST REFUSAL at the DESTROY chokepoint (see the header + the take-9 2026-07-13 RCA) -- the
// destroy-edge twin of TryAdoptFreshKerfurProp. Runs INSIDE the conversion verb (the prop's
// K2_DestroyActor Func-patch seam): spawnKerfuro is kismet-proven SPAWN-then-DESTROY
// (votv-kerfur-convert-RE-2026-06-12.md 2: BeginDeferred+FinishSpawning the NPC at spwn+50Z ->
// IsValid -> K2_DestroyActor self), so the conversion-product NPC exists ZERO ticks old here --
// no periodic enroll lane (kerfur_entity reserve wave, npc census) can have claimed it between
// the two statements of one BP verb. That synchronous premise is what the poll never had
// ([[lesson-new-generic-lane-must-inherit-owner-boundaries]]: its "element present + actor dead"
// premise is structurally FALSE for host props -- this very seam drains the element in the same
// call chain that kills the actor).
bool TryCaptureKerfurPropDestroy(void* actor, coop::element::ElementId dyingEid) {
    namespace KE = coop::kerfur_entity;
    if (!actor) return false;
    auto* s = LoadSession();
    if (!s || !s->connected()) return false;                 // solo / SP: the game owns itself
    if (!g_kerfurNpcClass || !g_kerfurPropClass) return false;
    if (!ue_wrap::game_thread::IsGameThread()) return false;  // the destroy seam is GT; defensive
    void* cls = R::ClassOf(actor);
    if (!cls || !R::IsDescendantOfAny(cls, &g_kerfurPropClass, 1)) return false;

    const bool isHost = s->role() == coop::net::Role::Host;
    const ue_wrap::FVector ploc = ue_wrap::engine::GetActorLocation(actor);
    // 2a-capture (2026-07-14): the form assembler's DETERMINISTIC in-bracket successor B -- captured
    // at B's FinishSpawningActor, consumed here at the paired A-destroy edge (the measured
    // spawn<destroy order, DESTROY_NO_SPAWN=0). This is the ONLY which-B path now: it bypasses the
    // (0,0,0) post-destroy proximity anchor (GetActorLocation on the dying prop reads ~origin) that
    // made the old stamp-list proximity walk reject the real B on every real toggle (take-8/take-10
    // misfilter). That walk is RETIRED (see the note above SendConvertRequestDirect).
    void* freshNpc = nullptr;
    float bestD2 = 0.f;  // real dist filled in on a capture HIT below; only read for the CLIENT log
    {
        auto cap = coop::kerfur_form_assembler::ConsumeCapturedForm(/*wantNpc=*/true);
        if (cap.actor) {
            freshNpc = cap.actor;
            const ue_wrap::FVector floc = ue_wrap::engine::GetActorLocation(freshNpc);
            const float dx = floc.X - ploc.X, dy = floc.Y - ploc.Y, dz = floc.Z - ploc.Z;
            bestD2 = dx * dx + dy * dy + dz * dz;  // honest distance; the anchor may be stale, NOT gated on kR2
            UE_LOGI("kerfur_convert: 2a-capture HIT (%s) -- dying kerfur prop %p paired to captured successor "
                    "NPC %p (deterministic, bracket-paired; proximity anchor bypassed)",
                    isHost ? "HOST" : "CLIENT", actor, freshNpc);
        }
    }

    if (!freshNpc) {
        // No captured conversion successor at this destroy edge. B is captured at FinishSpawning and
        // consumed here at the paired A-destroy (measured spawn<destroy, DESTROY_NO_SPAWN=0), so a MISS
        // means this is a genuine (non-conversion) kerfur-prop destroy -> generic relay. ORDER ASSERT
        // (user rule 2026-07-14): a MISS *while a verb bracket is live* would mean that invariant broke --
        // log THAT case loud, with destroy provenance, so it can never hide silently. provenance{}
        // distinguishes the conversion verb's own self-destroy (vmActive+ctxSelf, or reqEid on the
        // CallFunction route) from an unrelated teardown.
        // T11 (arc 2): latched -- this sits on the kerfur-prop destroy edge; the
        // old per-call read re-opened the ini every time (F34).
        static const bool s_vmLog = coop::config::ResolveFlag(::coop::config_registry::rows::vm_dispatch_log);
        if (s_vmLog) {
            const ue_wrap::vm_dispatch::ActiveVerb av = ue_wrap::vm_dispatch::CurrentThreadVerb();
            const coop::element::ElementId reqEid = coop::kerfur_convert_host::ActiveRequestVerbEid();
            // vmActive alone would claim "in-bracket" for ANY module's verb (see
            // vm_dispatch.h: gate on verbName, never on verbId/active). A diagnostic that
            // asserts the wrong provenance is the false-comment family this lane keeps
            // paying for, so it names the verb it actually saw.
            const bool inOurVerb = av.active && av.verbName &&
                                   (std::wcscmp(av.verbName, L"dropKerfurProp") == 0 ||
                                    std::wcscmp(av.verbName, L"spawnKerfuro") == 0);
            if (inOurVerb || reqEid != coop::element::kInvalidId)
                UE_LOGW("kerfur_convert: 2a-capture MISS in-bracket (%s) actor=%p -- no captured B at the destroy "
                        "edge (ORDER ASSERT: spawn<destroy expected) -- treating as genuine destroy (generic "
                        "relay). provenance{inOurVerb=%d verb=%ls ctxSelf=%d reqEid=%d}",
                        isHost ? "HOST" : "CLIENT", actor, inOurVerb ? 1 : 0,
                        av.verbName ? av.verbName : L"<none>",
                        (inOurVerb && av.ctx == actor) ? 1 : 0,
                        reqEid == coop::element::kInvalidId ? -1 : static_cast<int>(reqEid));
        }
        return false;  // no captured conversion successor -> genuine destroy -> generic relay
    }

    if (!isHost) {
        // CLIENT: capture = suppress the keyed-destroy relay ONLY. The conversion axis owner stays
        // the poll (SendConvertRequestDirect + ClaimConversionGhosts, proven in take-9: eid 9931
        // converged perfectly whenever the relay lost the race). The mirror row survives this seam
        // (client reverse maps never learn mirror actors, so the Unmarks above it no-op'd) -- the
        // poll's ALIVE->DEAD premise holds and fires within 200 ms. [[feedback-one-owner-order-axis]]
        UE_LOGI("kerfur_convert: CLIENT destroy-edge first refusal -- dying kerfur prop %p is conversion "
                "churn (fresh NPC %.0f cm away); keyed-destroy relay SUPPRESSED (the poll owns the request)",
                actor, std::sqrt(bestD2));
        return true;
    }

    // HOST: converge inline at the destroy edge. The seam already captured dyingEid BEFORE its
    // Unmarks drained the prop Element (BindFormActor needs only the KerfurRecord table, not the
    // element row -- it even mints a fresh K for a first-sighted save prop).
    if (dyingEid == coop::element::kInvalidId) {
        // Never on the wire (pre-enroll window): no identity to converge; the generic keyed destroy
        // + the periodic NPC enroll express the flip as destroy+spawn. Rare, lossless, logged.
        UE_LOGI("kerfur_convert: destroy-edge first refusal declined -- dying kerfur prop %p has no eid "
                "(never enrolled); generic destroy relay proceeds", actor);
        return false;
    }
    const std::wstring ncls = R::ClassNameOf(freshNpc);
    const coop::element::ElementId newEid = coop::npc_sync::RegisterHostNpcSilent(freshNpc, ncls);
    if (newEid == coop::element::kInvalidId) {
        UE_LOGW("kerfur_convert: destroy-edge converge -- RegisterHostNpcSilent failed for NPC %p; "
                "generic destroy relay proceeds for prop eid=%u", freshNpc, static_cast<unsigned>(dyingEid));
        return false;
    }
    const auto nloc = ue_wrap::engine::GetActorLocation(freshNpc);
    const auto nrot = ue_wrap::engine::GetActorRotation(freshNpc);
    KE::BindFormActor(dyingEid, freshNpc, R::InternalIndexOf(freshNpc), newEid, KE::Form::Npc, ncls,
                      nloc.X, nloc.Y, nloc.Z, nrot.Pitch, nrot.Yaw, nrot.Roll);
    auto wit = g_kerfurWatch.find(static_cast<uint32_t>(dyingEid));
    if (wit != g_kerfurWatch.end()) wit->second.handled = true;      // the poll skips this death
    coop::kerfur_convert_host::RecordSeamConvergedInBracket(dyingEid);  // bracket-conditional (audit finding 3)
    UE_LOGI("kerfur_convert: FIRST-REFUSAL turn-on converge -- dying kerfur prop eid=%u converged to fresh "
            "NPC %p eid=%u (%.0f cm; KerfurConvert broadcast, NO generic PropDestroy)",
            static_cast<unsigned>(dyingEid), freshNpc, static_cast<unsigned>(newEid), std::sqrt(bestD2));
    return true;
}

void OnDisconnect() {
    g_kerfurWatch.clear();   // GT-only; Tick is the sole toucher
    coop::kerfur_convert_client::OnDisconnect();  // g_parkedGhosts (GT-only conversion-ghost claim list)
    coop::kerfur_convert_host::OnDisconnect();  // bracket pair (GT-only destroy-edge converge handshake)
    g_lastConvPoll = {};
}

}  // namespace coop::kerfur_convert
