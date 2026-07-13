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
#include "coop/element/mirror_manager.h"
#include "coop/element/npc.h"
#include "coop/element/prop.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/creatures/npc_mirror.h"     // OnEntityDestroy + adopt/spawn mirror helpers (client apply)
#include "coop/creatures/npc_sync.h"       // RegisterHostNpcSilent / ReleaseNpcElementSilent (silent host ops)
#include "coop/creatures/kerfur_entity.h"  // BindFormActor / BroadcastConvertRejected + the resolved classes
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_lifecycle.h"
#include "coop/props/remote_prop.h"        // OnDestroy (eid teardown of the old prop mirror in OnKerfurConvert)
#include "coop/props/remote_prop_spawn.h"  // OnSpawn (prop materialize) + HasLoadTailQuiesced (poll gate)
#include "coop/props/join_membership_sweep.h"  // anti-smear 2026-06-30: claim+sweep extracted out of remote_prop_spawn
#include "ue_wrap/engine.h"      // GetActorLocation + SetActorSimulatePhysics (freeze a ghost prop)
#include "ue_wrap/game_thread.h"
#include "ue_wrap/kerfur.h"      // NeutralizeAiTimers -- park a claimed conversion-ghost NPC
#include "ue_wrap/log.h"
#include "ue_wrap/puppet.h"      // DisableCharacterTicks -- park a claimed conversion-ghost NPC
#include "ue_wrap/reflection.h"

#include <atomic>
#include <chrono>
#include <cmath>     // sqrt (first-refusal converge distance log)
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

// ---- host-side converge (kerfur redesign K-4b) -------------------------------
// After the verb ran on the host (its own radial menu OR a client request), drive the SOLE
// conversion signal KerfurConvert: find the new-form actor (the verb spawned it via EX_CallMath --
// PE-invisible, so it is UNTRACKED), register it SILENTLY at a host-range eid, release the dying form
// SILENTLY, and BindFormActor (rebinds the stable KerfurId IN PLACE + broadcasts KerfurConvert). No
// EntityDestroy / PropSpawn for the kerfur itself (redesign 10.3 -- the kerfur is one entity, not a
// destroy+create across two pipelines). The dropped FLOPPY (a normal prop, NOT part of the kerfur
// identity) still rides the ordinary keyed ExpressSpawnedProp -> PropSpawn. Game thread.

// Find the host's untracked, live kerfur actor of the requested form nearest (x,y,z): the verb's
// freshly-spawned new-form body. UNTRACKED = not yet a host element (g_actorToNpcId / the prop
// tracker) -- the verb output, before we register it. One cold GUObjectArray walk per conversion.
void* FindNewFormKerfurActor(bool wantNpc, float x, float y, float z) {
    void* base = wantNpc ? g_kerfurNpcClass : g_kerfurPropClass;
    if (!base) return nullptr;
    const int32_t n = R::NumObjects();
    constexpr float kR2 = 500.f * 500.f;  // the new form spawns at the kerfur's own transform
    void* best = nullptr;
    float bestD2 = kR2;
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        void* cls = R::ClassOf(obj);
        if (!cls || !R::IsDescendantOfAny(cls, &base, 1)) continue;
        if (!R::IsLive(obj)) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
        const bool tracked = wantNpc
            ? (coop::npc_sync::GetNpcIdForActor(obj) != coop::element::kInvalidId)
            : (PT::GetPropElementIdForActor(obj) != coop::element::kInvalidId);
        if (tracked) continue;
        const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(obj);
        const float dx = loc.X - x, dy = loc.Y - y, dz = loc.Z - z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bestD2) { bestD2 = d2; best = obj; }
    }
    return best;
}

// turn_off may also drop the kerfur's carried FLOPPY (a normal prop_floppyDisc_C -- a plain keyed
// prop, NOT part of the kerfur identity). It too spawns BP-internally (Init POST misses it) -> express
// it the NORMAL keyed way (ExpressSpawnedProp -> PropSpawn), latch-deduped, only UNTRACKED, near the
// conversion site.
void ExpressConversionFloppies(float x, float y, float z) {
    if (!g_floppyClass) return;
    const int32_t n = R::NumObjects();
    constexpr float kR2 = 500.f * 500.f;
    int ingested = 0;
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        void* cls = R::ClassOf(obj);
        if (!cls || !R::IsDescendantOfAny(cls, &g_floppyClass, 1)) continue;
        if (!R::IsLive(obj)) continue;
        if (PT::GetPropElementIdForActor(obj) != coop::element::kInvalidId) continue;  // tracked
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
        const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(obj);
        const float dx = loc.X - x, dy = loc.Y - y, dz = loc.Z - z;
        if (dx * dx + dy * dy + dz * dz > kR2) continue;
        coop::prop_lifecycle::ExpressSpawnedProp(obj);  // normal keyed PropSpawn (floppy is a plain prop)
        ++ingested;
    }
    if (ingested > 0)
        UE_LOGI("kerfur_convert: expressed %d dropped floppy prop(s) (normal keyed PropSpawn)", ingested);
}

// Silent teardown of a dying host PROP form (the actor is already dead -- pointer-as-map-key only, no
// deref, no PropDestroy broadcast). KerfurConvert carries oldEid for the clients.
void ReleaseHostPropSilent(void* deadActor) {
    if (!deadActor) return;
    PT::UnmarkProcessedInit(deadActor);
    PT::UnmarkKnownKeyedProp(deadActor);  // drains the Prop Element + frees its eid (ABBA-safe)
}

void ConvergeAfterConversion(void* oldActor, int32_t oldIdx, coop::element::ElementId oldEid,
                             uint8_t toProp, float px, float py, float pz) {
    namespace KE = coop::kerfur_entity;
    if (toProp) {
        // turn_off: NPC -> prop. The NPC should have died; a kerfur prop (+ maybe floppy) spawned at
        // its position. A SENTIENT kerfur refused -> the NPC is still live -> echo a reject.
        if (oldActor && R::IsLiveByIndex(oldActor, oldIdx)) {
            const auto loc = ue_wrap::engine::GetActorLocation(oldActor);
            const auto rot = ue_wrap::engine::GetActorRotation(oldActor);
            void* cls = R::ClassOf(oldActor);
            KE::BroadcastConvertRejected(oldEid, KE::Form::Npc, loc.X, loc.Y, loc.Z,
                                         rot.Pitch, rot.Yaw, rot.Roll,
                                         cls ? R::ToString(R::NameOf(cls)) : std::wstring());
            return;
        }
        void* newProp = FindNewFormKerfurActor(/*wantNpc=*/false, px, py, pz);
        if (!newProp) {
            UE_LOGW("kerfur_convert: turn_off converge -- no new kerfur prop near (%.0f,%.0f,%.0f); releasing dead NPC eid=%u (no broadcast)",
                    px, py, pz, static_cast<uint32_t>(oldEid));
            coop::npc_sync::ReleaseNpcElementSilent(oldEid);
            return;
        }
        const auto loc = ue_wrap::engine::GetActorLocation(newProp);
        const auto rot = ue_wrap::engine::GetActorRotation(newProp);
        void* ncls = R::ClassOf(newProp);
        const std::wstring cls = ncls ? R::ToString(R::NameOf(ncls)) : std::wstring();
        const coop::element::ElementId newEid = coop::prop_lifecycle::RegisterHostPropSilent(newProp);
        if (newEid == coop::element::kInvalidId) {
            UE_LOGW("kerfur_convert: turn_off converge -- RegisterHostPropSilent failed for prop %p; releasing NPC eid=%u",
                    newProp, static_cast<uint32_t>(oldEid));
            coop::npc_sync::ReleaseNpcElementSilent(oldEid);
            return;
        }
        coop::npc_sync::ReleaseNpcElementSilent(oldEid);
        KE::BindFormActor(oldEid, newProp, R::InternalIndexOf(newProp), newEid, KE::Form::Prop, cls,
                          loc.X, loc.Y, loc.Z, rot.Pitch, rot.Yaw, rot.Roll);
        ExpressConversionFloppies(px, py, pz);
    } else {
        // turn on: prop -> NPC. The prop should have died; a kerfur NPC spawned. Spawn failure (the BP
        // hint path) leaves the prop alive -> echo a reject.
        if (oldActor && R::IsLiveByIndex(oldActor, oldIdx)) {
            const auto loc = ue_wrap::engine::GetActorLocation(oldActor);
            const auto rot = ue_wrap::engine::GetActorRotation(oldActor);
            void* cls = R::ClassOf(oldActor);
            KE::BroadcastConvertRejected(oldEid, KE::Form::Prop, loc.X, loc.Y, loc.Z,
                                         rot.Pitch, rot.Yaw, rot.Roll,
                                         cls ? R::ToString(R::NameOf(cls)) : std::wstring());
            return;
        }
        void* newNpc = FindNewFormKerfurActor(/*wantNpc=*/true, px, py, pz);
        if (!newNpc) {
            UE_LOGW("kerfur_convert: turn-on converge -- no new kerfur NPC near (%.0f,%.0f,%.0f); releasing dead prop eid=%u (no broadcast)",
                    px, py, pz, static_cast<uint32_t>(oldEid));
            ReleaseHostPropSilent(oldActor);
            return;
        }
        const auto loc = ue_wrap::engine::GetActorLocation(newNpc);
        const auto rot = ue_wrap::engine::GetActorRotation(newNpc);
        void* ncls = R::ClassOf(newNpc);
        const std::wstring cls = ncls ? R::ToString(R::NameOf(ncls)) : std::wstring();
        const coop::element::ElementId newEid = coop::npc_sync::RegisterHostNpcSilent(newNpc, cls);
        if (newEid == coop::element::kInvalidId) {
            UE_LOGW("kerfur_convert: turn-on converge -- RegisterHostNpcSilent failed for NPC %p; releasing prop eid=%u",
                    newNpc, static_cast<uint32_t>(oldEid));
            ReleaseHostPropSilent(oldActor);
            return;
        }
        ReleaseHostPropSilent(oldActor);
        KE::BindFormActor(oldEid, newNpc, R::InternalIndexOf(newNpc), newEid, KE::Form::Npc, cls,
                          loc.X, loc.Y, loc.Z, rot.Pitch, rot.Yaw, rot.Roll);
    }
}

// TakeParkedGhostByEid is declared in the header (defined with g_parkedGhosts below).

// ---- client-side apply (kerfur redesign K-4b) --------------------------------
// Materialize (or adopt this peer's claimed local ghost into) a kerfur mirror of `form` at host-range
// `eid`. `adoptEid` (D2 relay) = the converting mirror's eid (the host's KerfurConvert oldEid); the parked
// ghost tagged with it is adopted DETERMINISTICALLY (no fuzzy position miss = no flash). kInvalidId = no
// eid-keyed adopt (the rejected-restore path). NPC: adopt the parked turn-on ghost (by eid, then position)
// else fresh-spawn. PROP: adopt the frozen turn-off ghost (by eid directly, else the synthetic-PropSpawn
// Gap-I-1 fuzzy match) else fresh-spawn. The prop mirror carries a deterministic SYNTHETIC key from eid.
void MaterializeKerfurMirror(bool toNpc, coop::element::ElementId eid, coop::element::ElementId adoptEid,
                             const std::wstring& classW,
                             float lx, float ly, float lz, float rp, float ry, float rr,
                             void* localPlayer) {
    if (toNpc) {
        // D2 relay: adopt the eid-tagged ghost DETERMINISTICALLY (v91: the 500cm position fallback is gone --
        // the initiator's ghost is always tagged with the converting eid, so a position miss never happens; a
        // non-initiator has no ghost at this eid -> fresh-spawn below, eid-deduped, the correct action).
        void* ghost = (adoptEid != coop::element::kInvalidId)
                          ? TakeParkedGhostByEid(static_cast<uint32_t>(adoptEid), /*wantNpc=*/true)
                          : nullptr;
        if (ghost) {
            if (coop::npc_mirror::AdoptExistingNpcAsMirror(ghost, static_cast<uint32_t>(eid), classW)) {
                UE_LOGI("kerfur_convert[client]: adopted parked turn-on ghost as NPC mirror eid=%u (by eid)", eid);
                return;
            }
            // adopt failed (dup-eid race) -> fall through to fresh-spawn beside it (cleanup reaps ghost)
        }
        void* actorClass = R::FindClass(classW.c_str());
        if (!actorClass) {
            UE_LOGW("kerfur_convert[client]: cannot resolve class '%ls' for NPC materialize eid=%u",
                    classW.c_str(), eid);
            return;
        }
        coop::npc_mirror::SpawnFreshNpcMirror(classW, actorClass, static_cast<uint32_t>(eid),
                                              lx, ly, lz, rp, ry, rr);
    } else {
        const std::string key8 = "coopkerfur#" + std::to_string(static_cast<unsigned>(eid));
        // D2 relay: if our own turn-off parked a prop ghost tagged with the converting eid, adopt THAT exact
        // actor as the host-eid mirror DIRECTLY (no fuzzy position match -> no miss -> no orphan-destroy flash).
        // The ghost is already frozen (claim) and at the conversion site; snap it to the authoritative pose.
        if (adoptEid != coop::element::kInvalidId) {
            if (void* ghost = TakeParkedGhostByEid(static_cast<uint32_t>(adoptEid), /*wantNpc=*/false)) {
                std::wstring key8w(key8.begin(), key8.end());
                coop::remote_prop::RegisterPropMirror(eid, ghost, key8w, classW, /*senderSlot=*/0,
                                                      /*rebindInPlace=*/false);
                ue_wrap::engine::SetActorLocation(ghost, ue_wrap::FVector{lx, ly, lz});
                ue_wrap::engine::SetActorRotation(ghost, ue_wrap::FRotator{rp, ry, rr});
                // HANG-IN-AIR ROOT FIX (2026-06-30, hands-on 14:42). The claim NO LONGER freezes this ghost
                // (ClaimConversionGhosts dropped the SetActorSimulatePhysics(false) -- it was RULE-2 baggage from
                // the Gap-I-1 fuzzy era; the eid adopt finds the ghost wherever it settled). So the off-prop
                // already FELL + settled by LOCAL physics during the host round-trip (SP-like, no air-hang); the
                // adopt is just the authoritative-pose snap above (sub-meter, same floor). No physics toggle here.
                UE_LOGI("kerfur_convert[client]: adopted parked turn-off ghost as PROP mirror eid=%u (by eid -- "
                        "deterministic, no fuzzy miss; settled by local physics, snapped to host pose)", eid);
                return;
            }
        }
        // No eid-tagged ghost (a non-initiator peer, or the rejected-restore path): drive the PropSpawn
        // receiver (exact-key/fuzzy-adopt/fresh-spawn + mirror register). Synthetic deterministic key from eid.
        coop::net::PropSpawnPayload sp{};
        sp.className.len = 0;
        for (size_t i = 0; i < classW.size() && i < 63; ++i)
            sp.className.data[sp.className.len++] = static_cast<char>(classW[i]);
        sp.key.len = 0;
        for (size_t i = 0; i < key8.size() && i < 31; ++i) sp.key.data[sp.key.len++] = key8[i];
        sp.propName.len = 0;
        sp.locX = lx; sp.locY = ly; sp.locZ = lz;
        sp.rotPitch = rp; sp.rotYaw = ry; sp.rotRoll = rr;
        sp.scaleX = sp.scaleY = sp.scaleZ = 1.f;
        sp.physFlags = 0;
        sp.chipType = 0;
        sp.elementId = static_cast<uint32_t>(eid);
        // deferKerfur=false: this is the CONVERSION materialize -- the client's parked ghost is ready
        // NOW (claim/adopt), so OnSpawn must run the inline fuzzy-adopt, NOT defer to the join-time
        // kerfur_prop_adoption poll (which is for un-converted save-loaded twins). K-6.
        // ROOT-1 FIX (2026-06-29, 10:30 hands-on -- the turn-off DOUBLE): the prior call
        // `OnSpawn(sp, 0, localPlayer, /*deferKerfur=*/false)` bound `false` to param #4 (fromConvert),
        // leaving deferKerfur at its DEFAULT true -> this synthetic kerfur PropSpawn ARMED the join-window
        // fuzzy adopter (remote_prop_spawn.cpp:763 -> kerfur_prop_adoption::Arm) instead of adopting inline.
        // That adopter can't match the local conversion ghost (fresh per-load key fails its anti-collision
        // gate) -> it fresh-spawned a SECOND prop beside the orphaned ghost -> the visible double. This is the
        // IDENTICAL arg-slot bug already fixed at kerfur_prop_adoption.cpp:171-179 (the "OBS-2 ROOT FIX"),
        // never fixed at this 2nd call site. Pass fromConvert=false EXPLICITLY so deferKerfur=false lands right.
        coop::remote_prop_spawn::OnSpawn(sp, /*senderSlot=*/0, localPlayer,
                                         /*fromConvert=*/false, /*deferKerfur=*/false);
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

// (take-9) request-verb bracket + converge handshake between OnConvertRequest and the destroy-edge
// first refusal. The seam fires INSIDE the request-executed verb, so OnConvertRequest would run its
// explicit ConvergeAfterConversion a second time right after the verb returns -- and the fresh
// NPC, now tracked by the seam converge, would read as "no new kerfur NPC near" (a spurious
// release + WARN). g_requestVerbEid brackets the CallFunction; the capture records into
// g_seamConvergedEid ONLY when it fires inside the matching bracket (audit 2026-07-13 finding 3:
// an unconditional write left one stale eid per host-OWN toggle with nobody to consume it -- a
// later request whose recycled eid matched would skip its converge AND its reject echo). Verbs
// are synchronous on the GT, so single slots cannot be raced.
uint32_t g_requestVerbEid   = 0xFFFFFFFFu;  // eid of the request-path verb currently executing; GT-only
uint32_t g_seamConvergedEid = 0xFFFFFFFFu;  // set by the capture inside that bracket; GT-only

bool ConsumeSeamConverged(uint32_t eid) {
    if (g_seamConvergedEid != eid) return false;
    g_seamConvergedEid = 0xFFFFFFFFu;
    return true;
}

// (take-9, audit findings 1+2) FRESH-SPAWN STAMPS -- the real "this NPC is a conversion product"
// discriminator. "Unowned within 5 m" alone is the take-8 lesson's dead premise wearing a new
// coat: during enrollment gaps (host pre-ClientWorldReady NPC registration, client join-window
// NPC adoption) a save-loaded live kerfur is ALSO unowned, and proximity alone would let a plain
// hold-R pickup / incinerator burn beside it suppress a legit relay (client) or weld the dying
// prop's KerfurId onto an untouched wanderer (host). The conversion product is different in ONE
// measured way: it was FinishSpawningActor'd moments ago (spawnKerfuro/dropKerfurProp spawn-then-
// destroy, same call chain) -- so the FinishSpawningActor Func seam (host_spawn_watcher, both
// roles) stamps every fresh kerfur NPC here, and the capture requires a stamp. A save-loaded /
// long-lived NPC is NEVER stamped. GT-only.
struct FreshNpcStamp { void* actor; int32_t idx; std::chrono::steady_clock::time_point at; };
std::vector<FreshNpcStamp> g_freshNpcStamps;
constexpr long kFreshNpcStampMs = 2000;  // verb chains are sub-tick; 2 s is a generous ceiling

void PruneFreshNpcStamps(std::chrono::steady_clock::time_point now) {
    for (auto it = g_freshNpcStamps.begin(); it != g_freshNpcStamps.end();) {
        const long age = static_cast<long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - it->at).count());
        if (age > kFreshNpcStampMs || !it->actor || !R::IsLiveByIndex(it->actor, it->idx))
            it = g_freshNpcStamps.erase(it);
        else
            ++it;
    }
}

bool HasFreshNpcStamp(void* actor, std::chrono::steady_clock::time_point now) {
    for (const auto& st : g_freshNpcStamps) {
        if (st.actor != actor) continue;
        const long age = static_cast<long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - st.at).count());
        return age <= kFreshNpcStampMs;
    }
    return false;
}

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
// CLAIM the ghost (park the NPC so it stops local AI/physics; the PROP is left to settle by local physics),
// then ADOPT it as the host mirror when the authoritative entity arrives -- BY EXACT srcEid (tagged below):
//   - NPC (turn-on):  npc_mirror::OnEntitySpawn adopts the ghost by EXACT eid
//                     (TakeParkedGhostByEid(convertFromEid), AdoptExistingNpcAsMirror) -- no respawn pop.
//   - PROP (turn-off): MaterializeKerfurMirror adopts the ghost by EXACT eid (TakeParkedGhostByEid). The prop
//                     is NOT frozen (2026-06-30 14:42): the old SetActorSimulatePhysics(false) held it inside
//                     Gap-I-1's 30 cm FUZZY window, but the eid adopt finds it wherever it settled -- so the
//                     freeze was RULE-2 baggage that pinned it at the NPC's standing height (the air-hang). It
//                     now falls + settles by LOCAL physics like single-player; the adopt snaps it to the host pose.
// A ghost the host never confirms (sentient-kerfur reject / dropped request) is
// destroyed by CleanupParkedGhosts after a timeout, so it can never be grabbed.
struct ParkedGhost {
    void*    actor;
    int32_t  idx;
    uint8_t  toProp;  // 1 = prop ghost (turn-off result), 0 = NPC ghost (turn-on result)
    uint32_t srcEid;  // (D2 relay, 2026-06-27) the eid of the mirror that converted -- the host's KerfurConvert
                      // carries this as oldEid, so the ghost is adopted by STABLE EID, not fuzzy position
                      // (the position-match miss = the 15:01 "appears-freezes-disappears" flash root).
    std::chrono::steady_clock::time_point armed;
};
std::vector<ParkedGhost> g_parkedGhosts;  // GT-only (poll + OnEntitySpawn are both game thread)

// TakeParkedGhostByEid is a PUBLIC API (declared in the header) -> defined in the coop::kerfur_convert
// namespace proper (after the anonymous namespace closes), NOT here. g_parkedGhosts (anon-ns file static)
// is still visible there within this TU.

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
void ClaimConversionGhosts(uint32_t srcEid, bool wantNpc, float x, float y, float z) {
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
    const int32_t n = R::NumObjects();
    constexpr float kR2 = 500.f * 500.f;  // the new form spawns at the kerfur's own transform
    // K-6 over-claim fix: the conversion verb spawns EXACTLY ONE actor per base class (1
    // prop_kerfurOmega body +, for turn_off, optionally 1 prop_floppyDisc) AT the conversion position
    // (RE redesign sec 3). So claim ONLY the NEAREST untracked candidate PER base class -- NOT every
    // kerfur prop within 5 m. The old "claim all within radius" grabbed the player's PRE-EXISTING
    // save-loaded kerfur props and the 4 s orphan-reaper then DESTROYED them ("kerfur props removed
    // entirely"; proven from the 2026-06-16 client log "claimed 4 local conversion ghost(s)"). The
    // join-time kerfur_prop_adoption makes those pre-existing props mirrors (-> excluded just below),
    // so this nearest-only rule is the belt-and-suspenders for a conversion racing the adoption poll.
    void* bestObj[2] = {nullptr, nullptr};
    float bestD2[2]  = {kR2, kR2};
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        void* cls = R::ClassOf(obj);
        if (!cls) continue;
        if (!R::IsLive(obj)) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
        if (mirrors.count(obj)) continue;  // a host wire mirror -- not our ghost
        int b = -1;  // which base class this actor is (nearest tracked per base)
        for (size_t bi = 0; bi < nBases; ++bi)
            if (R::IsDescendantOfAny(cls, &bases[bi], 1)) { b = static_cast<int>(bi); break; }
        if (b < 0) continue;
        const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(obj);
        const float dx = loc.X - x, dy = loc.Y - y, dz = loc.Z - z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bestD2[b]) { bestD2[b] = d2; bestObj[b] = obj; }
    }
    const auto now = std::chrono::steady_clock::now();
    int claimed = 0;
    for (size_t bi = 0; bi < nBases; ++bi) {
        void* obj = bestObj[bi];
        if (!obj) continue;
        if (wantNpc) {
            ue_wrap::puppet::DisableCharacterTicks(obj);  // park: the streamed pose becomes authoritative
            ue_wrap::kerfur::NeutralizeAiTimers(obj);      // stop the kerfur AI timers on the ghost
        }
        // PROP (turn-off) ghost: do NOT freeze its physics. The old `SetActorSimulatePhysics(false)` here held
        // the dropped prop inside the Gap-I-1 30cm FUZZY window; adopt is now DETERMINISTIC by srcEid
        // (TakeParkedGhostByEid finds it by eid wherever it settled), so the freeze was RULE-2 baggage that
        // pinned the prop at the NPC's STANDING height for the ~0.4s host round-trip = the "hang in the air then
        // snap" (2026-06-30 14:42 hands-on). Let it fall + settle by LOCAL physics like single-player;
        // MaterializeKerfurMirror rebinds it in place by eid (a sub-meter snap to the host's settled pose).
        g_parkedGhosts.push_back(ParkedGhost{obj, R::InternalIndexOf(obj),
                                             wantNpc ? uint8_t(0) : uint8_t(1), srcEid, now});
        ++claimed;
    }
    if (claimed)
        UE_LOGI("kerfur_convert[client]: claimed %d local conversion ghost(s) (%s) nearest-per-class near (%.0f,%.0f,%.0f) -- parked for host adoption",
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
            ClaimConversionGhosts(eid, /*wantNpc=*/false, lx, ly, lz);
        } else if (isHost)
            ConvergeAfterConversion(actor, el->GetInternalIdx(),
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
            ClaimConversionGhosts(eid, /*wantNpc=*/true, lx, ly, lz);
        } else if (isHost) {
            ConvergeAfterConversion(actor, el->GetInternalIdx(),
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
    g_installed.store(true, std::memory_order_release);
    UE_LOGI("kerfur_convert: installed (actionName nameOff=%d, killOff=%d, col=%s colGamer=%s floppy=%s; conversion = death-watch poll)",
            g_nameParamOff, g_killOff,
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
        // Capture the kerfur's pose BEFORE the verb -- it K2_DestroyActor's the NPC, so the converge
        // can no longer read it; the new-form prop spawns at this position (position continuity).
        const ue_wrap::FVector pos0 = ue_wrap::engine::GetActorLocation(actor);
        uint8_t frame[16] = {};  // verbs take no params (install-guarded); zeroed frame for safety
        R::CallFunction(actor, PickDropPropFn(cls), frame);
        ConvergeAfterConversion(actor, idx, eid, /*toProp=*/1, pos0.X, pos0.Y, pos0.Z);
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
        // Capture the prop's pose BEFORE the verb -- it spawns the NPC at this position then
        // K2_DestroyActor's the prop (position continuity for the converge).
        const ue_wrap::FVector pos0 = ue_wrap::engine::GetActorLocation(actor);
        uint8_t frame[16] = {};  // spawnKerfuro takes no params (install-guarded)
        // (take-9) bracket the verb: the prop's K2_DestroyActor INSIDE it hits the destroy seam,
        // whose kerfur first refusal converges inline and records the eid -- consumed just below
        // instead of double-converging (the NPC is tracked by then; the search in
        // ConvergeAfterConversion would spuriously fail and release the eid with a WARN).
        g_requestVerbEid = static_cast<uint32_t>(eid);
        R::CallFunction(actor, g_spawnKerfuroFn, frame);
        g_requestVerbEid = 0xFFFFFFFFu;
        if (ConsumeSeamConverged(static_cast<uint32_t>(eid))) {
            UE_LOGI("kerfur_convert: turn-on eid=%u already converged at the destroy edge (first refusal) -- request satisfied",
                    payload.elementId);
            return;
        }
        ConvergeAfterConversion(actor, idx, eid, /*toProp=*/0, pos0.X, pos0.Y, pos0.Z);
    }
}

void OnKerfurConvert(const coop::net::KerfurConvertBroadcastPayload& p, void* localPlayer) {
    auto* s = LoadSession();
    if (!s) return;
    // CLIENT-only apply: the host already converged inline (silent host register + KerfurConvert
    // send), so it never applies its own broadcast. The slot-0 (host-origin) gate is in
    // event_dispatch_state.
    if (s->role() != coop::net::Role::Client) return;
    const coop::element::ElementId oldEid = static_cast<coop::element::ElementId>(p.oldEid);
    const coop::element::ElementId newEid = static_cast<coop::element::ElementId>(p.newEid);
    const bool toNpc = (p.toForm == 0);  // toForm: 0 = NPC (turn-on), 1 = prop (turn_off)
    std::wstring classW;
    classW.reserve(p.newClassName.len);
    for (uint8_t i = 0; i < p.newClassName.len && i < 63; ++i)
        classW.push_back(static_cast<wchar_t>(static_cast<unsigned char>(p.newClassName.data[i])));

    if (p.rejected) {
        // Host refused (sentient/kill). If our old-form mirror still exists, keep it (no-op). If we
        // optimistically converted locally (the poll detected our own conversion + parked a ghost) the
        // old mirror is gone -> RESTORE it at oldEid in the form-to-restore (toForm = the OLD form).
        if (coop::element::Registry::Get().Get(oldEid)) {
            UE_LOGI("kerfur_convert[client]: KerfurConvert rejected K=%u oldEid=%u -- mirror intact, no-op",
                    p.kerfurId, p.oldEid);
            return;
        }
        UE_LOGI("kerfur_convert[client]: KerfurConvert rejected K=%u oldEid=%u -- restoring local mirror (converted optimistically)",
                p.kerfurId, p.oldEid);
        // Restore the OLD form fresh (kInvalidId = no eid-keyed adopt: the parked ghost is the rejected NEW
        // form, wrong form to adopt; the cleanup timeout reaps it).
        MaterializeKerfurMirror(toNpc, oldEid, coop::element::kInvalidId, classW, p.locX, p.locY, p.locZ,
                                p.rotPitch, p.rotYaw, p.rotRoll, localPlayer);
        return;
    }

    // SUCCESS. Destroy the OLD-form mirror at oldEid (old form = the opposite of toForm), then
    // materialize/adopt the NEW form at the authoritative newEid.
    if (!toNpc) {
        // new form is prop -> old form was NPC
        coop::net::EntityDestroyPayload dp{};
        dp.elementId = static_cast<uint32_t>(oldEid);
        coop::npc_mirror::OnEntityDestroy(dp);
    } else {
        // new form is NPC -> old form was prop (eid-only teardown of the old prop mirror). K-5: evict
        // the old prop mirror from the client held-pose map BEFORE OnDestroy frees its actor (the map
        // self-heals on read too, but evicting on the known teardown keeps it tight).
        if (auto* oldEl = coop::element::Registry::Get().Get(oldEid))
            coop::kerfur_entity::ForgetKerfurPropMirror(oldEl->GetActor());
        coop::net::PropDestroyPayload dp{};
        dp.key.len = 0;
        dp.elementId = static_cast<uint32_t>(oldEid);
        coop::remote_prop::OnDestroy(dp, localPlayer);
    }
    // D2 relay: adopt the initiator's parked ghost by the converting eid (oldEid) -- deterministic, no flash.
    MaterializeKerfurMirror(toNpc, newEid, /*adoptEid=*/oldEid, classW, p.locX, p.locY, p.locZ,
                            p.rotPitch, p.rotYaw, p.rotRoll, localPlayer);
    // NOTE (scope A v1): the off->active dup RETIRE is NOT triggered here. A join-window turn-on's
    // KerfurConvert SendReliable FAILS (the joiner isn't ready for reliable gameplay mid save-transfer --
    // hands-on 16:37), so this apply never runs for the case that needs the retire. The retire is armed
    // from the npc EntitySpawn instead (npc_mirror::OnEntitySpawn, the channel that reaches the joiner)
    // and run by the quiescence sweep below. A steady-state turn-on (this path) already retires the old
    // form via the OnDestroy(oldEid) above -- nothing to retire here.
    UE_LOGI("kerfur_convert[client]: applied KerfurConvert K=%u %s oldEid=%u -> newEid=%u class='%ls'",
            p.kerfurId, toNpc ? "->NPC(turn-on)" : "->prop(turn_off)", p.oldEid, p.newEid, classW.c_str());
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

// (D2 relay) Take (find + remove) the parked ghost tagged with `srcEid` of the requested form, returning its
// actor or nullptr. Deterministic eid correlation replaces the fuzzy position adopt -> the local ghost is
// ALWAYS adopted by the host's authoritative KerfurConvert(oldEid) / EntitySpawn(convertFromEid), never
// orphaned -> destroyed (no flash). PUBLIC (header-declared) -> external linkage, so defined here outside the
// anonymous namespace; g_parkedGhosts (anon-ns file static) is still visible within this TU.
void* TakeParkedGhostByEid(uint32_t srcEid, bool wantNpc) {
    const uint8_t wantProp = wantNpc ? uint8_t(0) : uint8_t(1);
    for (auto it = g_parkedGhosts.begin(); it != g_parkedGhosts.end(); ++it) {
        if (it->srcEid != srcEid || it->toProp != wantProp) continue;
        void* actor = it->actor;
        const int32_t idx = it->idx;
        g_parkedGhosts.erase(it);
        return (actor && R::IsLiveByIndex(actor, idx)) ? actor : nullptr;
    }
    return nullptr;
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
    ExpressConversionFloppies(wx, wy, wz);
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
    constexpr float kR2 = 500.f * 500.f;  // spwn billboard + (0,0,50); 500cm mirrors every kerfur proximity site
    // The conversion-product test (ALL THREE required; audit 2026-07-13 findings 1+2):
    //   1. FRESH-SPAWN STAMP -- FinishSpawningActor'd within the last 2 s (the seam fires inside
    //      the spawn-then-destroy verb, so the true product is milliseconds old). This is the
    //      load-bearing discriminator: a save-loaded / long-lived kerfur is NEVER stamped, so an
    //      enrollment-gap "unowned" wanderer can neither suppress a legit relay (client) nor be
    //      identity-welded onto the dying prop (host).
    //   2. Not an established identity: HOST untracked by npc_sync (mirrors
    //      FindNewFormKerfurActor); CLIENT not a bound NPC wire mirror (CollectMirrorActors is
    //      the authoritative ownership signal) and not a PARKED ghost (another pending
    //      conversion's product).
    //   3. Within the verb's spawn radius of the dying prop.
    const auto now = std::chrono::steady_clock::now();
    PruneFreshNpcStamps(now);
    if (g_freshNpcStamps.empty()) return false;  // nothing fresh in the world -> genuine destroy
    std::unordered_set<void*> mirrors;
    if (!isHost) CollectMirrorActors(/*wantProp=*/false, /*wantNpc=*/true, mirrors);
    void* freshNpc = nullptr;
    float bestD2 = kR2;
    // Walk the STAMP LIST, not GUObjectArray -- the candidate universe is "kerfur NPCs finished
    // in the last 2 s" (typically 0-1 entries), already class-gated at stamp time.
    for (const auto& st : g_freshNpcStamps) {
        void* obj = st.actor;
        if (!obj || !R::IsLiveByIndex(obj, st.idx)) continue;
        if (isHost) {
            if (coop::npc_sync::GetNpcIdForActor(obj) != coop::element::kInvalidId) continue;
        } else {
            if (mirrors.count(obj)) continue;
            bool parked = false;
            for (const auto& g : g_parkedGhosts)
                if (g.actor == obj) { parked = true; break; }
            if (parked) continue;
        }
        const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(obj);
        const float dx = loc.X - ploc.X, dy = loc.Y - ploc.Y, dz = loc.Z - ploc.Z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bestD2) { bestD2 = d2; freshNpc = obj; }
    }
    if (!freshNpc) return false;  // no conversion product beside it -> a genuine destroy -> generic relay

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
    // Record the converge for OnConvertRequest ONLY when we are inside ITS verb bracket -- a
    // host-OWN toggle has no consumer and an unconditional write would leave a stale eid that a
    // later recycled-eid request could falsely consume (audit finding 3).
    if (g_requestVerbEid == static_cast<uint32_t>(dyingEid))
        g_seamConvergedEid = static_cast<uint32_t>(dyingEid);
    UE_LOGI("kerfur_convert: FIRST-REFUSAL turn-on converge -- dying kerfur prop eid=%u converged to fresh "
            "NPC %p eid=%u (%.0f cm; KerfurConvert broadcast, NO generic PropDestroy)",
            static_cast<unsigned>(dyingEid), freshNpc, static_cast<unsigned>(newEid), std::sqrt(bestD2));
    return true;
}

// Stamp a freshly-finished kerfur NPC (see the header). Self-gating: called for EVERY finished
// actor from the FinishSpawningActor Func seam, so the reject path must stay pointer-cheap --
// null checks, one atomic session load, one ClassOf deref, one SuperStruct climb. No walks, no
// allocations before the class gate.
void NoteFreshKerfurNpcSpawn(void* actor) {
    if (!actor || !g_kerfurNpcClass) return;
    auto* s = LoadSession();
    if (!s || !s->connected()) return;                       // solo/SP: no capture consumer
    if (!ue_wrap::game_thread::IsGameThread()) return;       // seam is GT; defensive
    void* cls = R::ClassOf(actor);
    if (!cls || !R::IsDescendantOfAny(cls, &g_kerfurNpcClass, 1)) return;
    const auto now = std::chrono::steady_clock::now();
    PruneFreshNpcStamps(now);
    g_freshNpcStamps.push_back(FreshNpcStamp{actor, R::InternalIndexOf(actor), now});
}

void OnDisconnect() {
    g_kerfurWatch.clear();   // GT-only; Tick is the sole toucher
    g_parkedGhosts.clear();  // GT-only conversion-ghost claim list
    g_seamConvergedEid = 0xFFFFFFFFu;  // GT-only destroy-edge converge handshake (take-9)
    g_requestVerbEid   = 0xFFFFFFFFu;
    g_freshNpcStamps.clear();          // GT-only fresh kerfur NPC spawn stamps (take-9)
    g_lastConvPoll = {};
}

}  // namespace coop::kerfur_convert
