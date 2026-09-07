// coop/kerfur_convert.cpp -- the kerfur conversion (NPC to prop and back) on the wire. The
// radial-menu verb, its spawn and its destroy all dispatch past ProcessEvent, so no interceptor
// sees the conversion: the host detects it at the generic express and destroy chokepoints, the
// client by a death-watch poll (a request plus the local ghost claim), and the poll is the
// solo-host backstop. The host executor lives in kerfur_convert_host.cpp, the client apply and
// ghost custody in kerfur_convert_client.cpp. See coop/kerfur_convert.h.

#include "coop/creatures/kerfur_convert.h"

#include "coop/creatures/kerfur_command.h"  // the menu-verb command relay
#include "coop/creatures/kerfur_convert_client.h"  // ghost custody and the wire apply
#include "coop/creatures/kerfur_convert_host.h"    // converge and request execution
#include "coop/creatures/kerfur_form_assembler.h"  // ConsumeCapturedForm
#include "coop/element/mirror_manager.h"
#include "coop/element/npc.h"
#include "coop/element/prop.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/creatures/npc_sync.h"       // RegisterHostNpcSilent / ReleaseNpcElementSilent (silent host ops)
#include "coop/creatures/kerfur_entity.h"  // BindFormActor / BroadcastConvertRejected + the resolved classes
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_lifecycle.h"
#include "coop/props/join_membership_sweep.h"  // HasLoadTailQuiesced
#include "ue_wrap/engine/engine.h"      // GetActorLocation (converge/seam position reads)
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "coop/config/config.h"  // the vm_dispatch_log flag
#include "ue_wrap/core/vm_dispatch.h"  // CurrentThreadVerb, the destroy provenance

#include <atomic>
#include <chrono>
#include <cmath>     // sqrt
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

// Resolved engine refs: written by Install on the game thread before the interceptor registers,
// read-only after, including from parallel-anim workers.
void* g_kerfurNpcClass  = nullptr;  // kerfurOmega_C (the NPC base; ~20 data-only skin subclasses)
void* g_kerfurPropClass = nullptr;  // prop_kerfurOmega_C (the prop base; skins likewise)
void* g_floppyClass     = nullptr;  // prop_floppyDisc_C (dropKerfurProp may also drop the carried floppy)
void* g_actionNameFn    = nullptr;  // kerfurOmega_C::actionName (the menu dispatcher -- kerfur_command relay)
int32_t g_nameParamOff  = -1;       // actionName 'name' FString param offset
int32_t g_killOff       = -1;       // kerfurOmega_C::kill bool (the BP's own turn_off guard)
// The verb declarers. dropKerfurProp is overridden by kerfurOmega_col_C and
// kerfurOmega_col_gamer_C; ProcessEvent executes exactly the UFunction passed, so the host picks
// the most-derived declarer the target descends from. The col pair is optional: unresolved by
// latch time, the base runs and the collar drop is skipped.
void* g_dropPropFnBase     = nullptr;  // kerfurOmega_C::dropKerfurProp
void* g_dropPropFnCol      = nullptr;  // kerfurOmega_col_C::dropKerfurProp (optional)
void* g_dropPropFnColGamer = nullptr;  // kerfurOmega_col_gamer_C::dropKerfurProp (optional)
void* g_colClass           = nullptr;
void* g_colGamerClass      = nullptr;
void* g_spawnKerfuroFn     = nullptr;  // prop_kerfurOmega_C::spawnKerfuro (sole declarer)

// The menu verbs are EX_LocalVirtualFunction, invisible to the ProcessEvent detour, so no
// interceptor fires for the conversion; the actionName interceptor survives only for the
// kerfur_command relay.

// An FString param out of a ProcessEvent frame: the 16-byte TArray<wchar_t> {Data, Num, Max}.
// Memory reads only, so worker-safe. Empty on null or an insane length.
std::wstring ReadFStringParam(void* params, int32_t off) {
    if (!params || off < 0) return {};
    auto* base = reinterpret_cast<uint8_t*>(params) + off;
    auto* data = *reinterpret_cast<wchar_t* const*>(base);
    const int32_t num = *reinterpret_cast<const int32_t*>(base + 8);
    if (!data || num <= 1 || num > 256) return {};  // Num includes the terminator
    return std::wstring(data, static_cast<size_t>(num - 1));
}

// The actionName PRE interceptor, for the kerfur_command relay only. Contract: no engine calls,
// no Post, no ProcessEvent re-entry, no registry walks (Snapshot's raw pointers race game-thread
// drains off-thread); memory reads and the atomic session load only.

// kerfurOmega_C::actionName (every skin subclass inherits this one UFunction).
bool OnKerfurActionNamePre(void* self, void* params) {
    if (!self || !params || g_nameParamOff < 0) return false;
    auto* s = LoadSession();
    if (!s || !s->running() || !s->connected()) return false;  // SP untouched
    const std::wstring name = ReadFStringParam(params, g_nameParamOff);
    if (name != L"turn_off") {
        // The state-changing radial verbs (follow, idle, patrol, fix_servers, get_reports,
        // fix_transformers) go to the host-authoritative command relay, which records the action
        // and returns true (cancel) for a relayed verb and false for an unrelayed one.
        const bool isClient = s->role() == coop::net::Role::Client;
        return coop::kerfur_command::TryRecordMenuCommand(self, name, isClient);
    }
    // turn_off is detected by the poll and the chokepoints, never here: the verb does not reach
    // this interceptor. Pass through.
    return false;
}

// The conversion detection on the client and the solo host: a poll. The engine is one detour on
// UObject::ProcessEvent, and the menu verb, the spawn and the destroy all dispatch past it. So a
// kerfur mirror whose actor has died while its wire Element is still present was converted by
// the local game (a wire-driven destroy releases the Element first), and only a genuine
// alive-to-dead transition fires. The client forwards a KerfurConvertRequest (the host's
// OnConvertRequest runs the real verb and converges to the wire) and claims the local ghost the
// invisible spawn made; the host converges its own toggle. Game thread, 5 Hz.

// `actor` records which actor was live when the entry was cached; the death path fires only if
// the dead actor is that one, so an eid freed and reallocated to a new mirror that died before a
// poll confirmed it live does not fire on the old generation's entry. A null actor still fires.
struct KerfurWatch { float x, y, z; bool handled; void* actor; };
std::unordered_map<uint32_t, KerfurWatch> g_kerfurWatch;  // eid -> last-live pose, handled flag, generation actor; GT-only
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

// One alive-to-dead pass over the kerfur mirror Elements (an NPC dying is turn_off, a prop dying
// turn_on). Fires only for an eid cached live, so a stale or never-live mirror cannot trigger.
void PollKerfurConversions() {
    // The host polls even solo: a solo radial-menu conversion must still converge (old form
    // released, new form silently enrolled), or a later joiner's snapshot has neither form. A
    // client needs a live connection, since its branch requests the host.
    auto* s = LoadSession();
    if (!s) return;
    if (s->role() != coop::net::Role::Host && !s->connected()) return;
    if (!g_kerfurNpcClass || !g_kerfurPropClass) return;  // classes not resolved yet
    const auto now = std::chrono::steady_clock::now();
    if (g_lastConvPoll.time_since_epoch().count() != 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastConvPoll).count() < 200)
        return;  // ~5 Hz
    g_lastConvPoll = now;

    // Reap claimed conversion ghosts (adopted or dead ones dropped, unconfirmed orphans destroyed).
    // A no-op on the host, which never claims ghosts.
    coop::kerfur_convert_client::CleanupParkedGhosts();

    const bool isHost   = s->role() == coop::net::Role::Host;
    const bool isClient = s->role() == coop::net::Role::Client;
    // A steady-state detector: "Element present, actor dead, so the local game converted it" holds
    // only in a settled world. A joining client's world churns for tens of seconds (the live-save
    // load, the snapshot brackets, the divergence and claim sweeps), and mirrors there are spawned
    // and destroyed by the reconcile, not by a radial menu; read as conversions, those deaths sent
    // turn-on requests at connect and the host turned its props into live NPCs. So the client waits
    // for the same load-tail quiescence the sweeps use. The host never arms that sweep and has no
    // transferred-save tail, so it polls from boot.
    if (isClient && !coop::join_membership_sweep::HasLoadTailQuiesced()) return;
    // The off-to-active duplicate retire is sequenced by quiescence_drain::RunReconcile, the one
    // order owner of the join window; this poll only detects conversions.
    std::unordered_set<uint32_t> seen;

    // turn_off: a kerfur NPC mirror whose actor died.
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
            g_kerfurWatch[eid] = KerfurWatch{loc.X, loc.Y, loc.Z, false, actor};  // the live actor is the generation identity
            continue;
        }
        auto it = g_kerfurWatch.find(eid);
        if (it == g_kerfurWatch.end() || it->second.handled) continue;  // never-live / already handled
        // The stale-generation guard: the dead actor must be the one cached live, else the eid was
        // reused by a newer mirror.
        if (it->second.actor && it->second.actor != actor) { it->second.handled = true; continue; }
        const float lx = it->second.x, ly = it->second.y, lz = it->second.z;
        UE_LOGI("kerfur_convert: POLL turn_off (kerfur NPC eid=%u died invisibly) -> %s",
                eid, isClient ? "client requests host" : "host broadcasts destroy+prop");
        if (isClient) {
            SendConvertRequestDirect(eid, 1);
            // The local turn-off dropped a kerfur prop (and maybe its floppy) on the invisible
            // path. It is frozen, not destroyed, so the host's authoritative prop adopts it through
            // the fuzzy match; frozen, it stays inside the 30 cm window instead of falling out
            // before the host's PropSpawn arrives.
            coop::kerfur_convert_client::ClaimConversionGhosts(eid, /*wantNpc=*/false, lx, ly, lz);
        } else if (isHost)
            coop::kerfur_convert_host::ConvergeAfterConversion(actor, el->GetInternalIdx(),
                                    static_cast<coop::element::ElementId>(eid), /*toProp=*/1,
                                    lx, ly, lz);
        it->second.handled = true;
    }

    // turn_on: a kerfur prop mirror whose actor died.
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
            g_kerfurWatch[eid] = KerfurWatch{loc.X, loc.Y, loc.Z, false, actor};  // the live actor is the generation identity
            continue;
        }
        auto it = g_kerfurWatch.find(eid);
        if (it == g_kerfurWatch.end() || it->second.handled) continue;
        // The stale-generation guard, as above.
        if (it->second.actor && it->second.actor != actor) { it->second.handled = true; continue; }
        const float lx = it->second.x, ly = it->second.y, lz = it->second.z;
        UE_LOGI("kerfur_convert: POLL turn-on (kerfur prop eid=%u died invisibly) -> %s",
                eid, isClient ? "client requests host" : "host broadcasts destroy+npc");
        if (isClient) {
            SendConvertRequestDirect(eid, 0);
            // The local turn-on spawned a kerfur NPC on the invisible path, an untracked ghost
            // beside the host's incoming mirror. It is claimed (parked) tagged with the converting
            // eid, and npc_mirror::OnEntitySpawn adopts that exact actor by eid: no
            // destroy-and-respawn pop, and no untracked ghost a grab could duplicate.
            coop::kerfur_convert_client::ClaimConversionGhosts(eid, /*wantNpc=*/true, lx, ly, lz);
        } else if (isHost) {
            coop::kerfur_convert_host::ConvergeAfterConversion(actor, el->GetInternalIdx(),
                                    static_cast<coop::element::ElementId>(eid), /*toProp=*/0,
                                    lx, ly, lz);
        }
        it->second.handled = true;
    }

    // Prune watch entries whose element is gone (released by a wire destroy or the session end).
    for (auto it = g_kerfurWatch.begin(); it != g_kerfurWatch.end();) {
        if (seen.count(it->first) == 0) it = g_kerfurWatch.erase(it);
        else ++it;
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    coop::kerfur_convert_client::SetSession(session);  // mirrors the store above
    if (g_installed.load(std::memory_order_acquire)) return;
    // FindClass and FindFunction walk GUObjectArray, so one attempt per 125 pump ticks; the
    // all-resolved latch is the only early-out, and partial retries are idempotent. No give-up cap:
    // the kerfur classes load lazily (a kerfur can be bought mid-session), so the module keeps
    // watching.
    static uint32_t sResolveN = 0;
    if ((sResolveN++ % 125) != 0) return;

    if (!g_kerfurNpcClass)  g_kerfurNpcClass  = R::FindClass(L"kerfurOmega_C");
    if (!g_kerfurPropClass) g_kerfurPropClass = R::FindClass(L"prop_kerfurOmega_C");
    if (!g_kerfurNpcClass || !g_kerfurPropClass) return;  // BP classes not loaded yet
    // The resolved bases are shared with kerfur_entity, so its class gates answer without
    // re-resolving.
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

    // Optional refs (the collar-variant overrides and the floppy class), resolved opportunistically
    // until the latch; a miss degrades to the base verb and a one-class walk.
    if (!g_colClass)      g_colClass      = R::FindClass(L"kerfurOmega_col_C");
    if (!g_colGamerClass) g_colGamerClass = R::FindClass(L"kerfurOmega_col_gamer_C");
    if (g_colClass && !g_dropPropFnCol)
        g_dropPropFnCol = R::FindFunction(g_colClass, L"dropKerfurProp");
    if (g_colGamerClass && !g_dropPropFnColGamer)
        g_dropPropFnColGamer = R::FindFunction(g_colGamerClass, L"dropKerfurProp");
    if (!g_floppyClass)   g_floppyClass   = R::FindClass(L"prop_floppyDisc_C");
    // The class pointers are pushed to the client and host TUs on every attempt, so they see them
    // as soon as they resolve, the disabled state included, where claims keep working.
    coop::kerfur_convert_client::SetClasses(g_kerfurNpcClass, g_kerfurPropClass, g_floppyClass);
    coop::kerfur_convert_host::SetClasses(g_kerfurNpcClass, g_kerfurPropClass, g_floppyClass);

    if (!g_actionNameFn || g_nameParamOff < 0 || !g_dropPropFnBase || !g_spawnKerfuroFn) {
        UE_LOGW("kerfur_convert: partial resolve (actionName=%p nameOff=%d drop=%p spawn=%p) -- retrying",
                g_actionNameFn, g_nameParamOff, g_dropPropFnBase, g_spawnKerfuroFn);
        return;
    }
    if (g_killOff < 0) {
        // Non-fatal: the guard read degrades to no guard (a murder-mode kerfur could be turned off
        // by a request; single-player denies it).
        UE_LOGW("kerfur_convert: kerfurOmega_C 'kill' offset unresolved -- BP murder-guard not replicated");
    }
    // The host hands the verbs a zeroed 16-byte frame because they take no params; should a game
    // update add one, the module refuses to install rather than over-read the frame.
    if (!R::FunctionParams(g_dropPropFnBase).empty() ||
        !R::FunctionParams(g_spawnKerfuroFn).empty()) {
        UE_LOGE("kerfur_convert: verb signature changed (dropKerfurProp/spawnKerfuro now take params) -- module DISABLED (re-RE the conversion BPs)");
        g_installed.store(true, std::memory_order_release);  // latch off
        return;
    }

    // The one interceptor, for the kerfur_command relay.
    if (!GT::RegisterInterceptor(g_actionNameFn, &OnKerfurActionNamePre)) {
        UE_LOGE("kerfur_convert: RegisterInterceptor(actionName) failed (table full?)");
        return;
    }
    // The verb refs and the request latch flip only at this success site; the disabled path above
    // never reaches it, so requests drop there (fail closed; see kerfur_convert_host.h).
    coop::kerfur_convert_host::SetVerbs(g_dropPropFnBase, g_dropPropFnCol, g_dropPropFnColGamer,
                                        g_colClass, g_colGamerClass, g_spawnKerfuroFn, g_killOff);
    g_installed.store(true, std::memory_order_release);
    UE_LOGI("kerfur_convert: installed (actionName nameOff=%d, killOff=%d, col=%s colGamer=%s floppy=%s; conversion = death-watch poll)",
            g_nameParamOff, g_killOff,
            g_dropPropFnCol ? "yes" : "no", g_dropPropFnColGamer ? "yes" : "no",
            g_floppyClass ? "yes" : "no");
}

void Tick() {
    // The backstop half of conversion sync. The primary host detectors are event-driven at the
    // chokepoints: turn_off at the fresh prop's expression edge (TryAdoptFreshKerfurProp), turn_on
    // at the prop's destroy edge (TryCaptureKerfurPropDestroy); a host prop's Element is drained
    // synchronously with its death, so the poll's premise never holds there. The poll remains the
    // client driver (the request plus the ghost claim; client mirror rows survive the seam) and the
    // solo-host backstop. 5 Hz, game thread.
    PollKerfurConversions();
}

// First refusal on the generic expression of a kerfur prop-form actor. At the spawn edge rather
// than the poll because the spawn-seam drain expresses every fresh host prop on the next tick,
// which a 5 Hz poll cannot beat; the express chokepoints offer the actor here first. The
// conversion-product question is answered by untracked plus a dead-NPC watch match, both
// required: tracking state alone was the poll's dead premise, and proximity alone steals
// neighbours.
bool TryAdoptFreshKerfurProp(void* actor) {
    namespace KE = coop::kerfur_entity;
    if (!actor) return false;
    auto* s = LoadSession();
    if (!s || s->role() != coop::net::Role::Host) return false;  // host-only authority
    if (!g_kerfurNpcClass || !g_kerfurPropClass) return false;
    if (!ue_wrap::game_thread::IsGameThread()) return false;     // express lanes are GT; defensive
    void* cls = R::ClassOf(actor);
    if (!cls || !R::IsDescendantOfAny(cls, &g_kerfurPropClass, 1)) return false;
    // Untracked only: an already-tracked kerfur prop is an established identity (a standing
    // off-prop near a dying neighbour, or any row the connect-snapshot drain enumerates, which is
    // fed from the Registry). Matching one would steal its eid into the dying kerfur's record and
    // corrupt the client mirror, while the real product later expresses generically as the
    // duplicate. A genuine product is always untracked at both consult sites.
    if (PT::GetPropElementIdForActor(actor) != coop::element::kInvalidId) return false;

    // Match the fresh prop against a dead, unhandled, generation-valid kerfur NPC mirror within the
    // verb's spawn radius (the new form spawns at the kerfur's own transform).
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

    // Converge with the given actor (untracked by the entry gate, so minted silently).
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

// First refusal at the destroy chokepoint, the twin of TryAdoptFreshKerfurProp. Runs inside the
// conversion verb (the prop's K2_DestroyActor seam): spawnKerfuro spawns the NPC and then
// destroys itself, so the product NPC is zero ticks old here and no periodic enroll lane can
// have claimed it between the two statements of one verb. The poll never had that synchronous
// premise: this seam drains the Element in the same call chain that kills the actor.
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
    // The form assembler's captured in-bracket successor, captured at its FinishSpawningActor and
    // consumed here at the paired destroy edge (spawn before destroy, measured). The only
    // which-successor path: GetActorLocation on the dying prop reads near the origin, so a
    // proximity walk from it rejected the real successor.
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
        // No captured successor: a genuine kerfur-prop destroy, relayed generically. A miss while a
        // verb bracket is live would mean the spawn-before-destroy invariant broke, so that case is
        // logged loud with its provenance, which tells the verb's own self-destroy (the active verb
        // with self as context, or the request eid on the CallFunction route) from an unrelated
        // teardown. The flag is latched: this sits on the destroy edge, and a per-call read
        // re-opened the ini.
        // The provenance is computed unconditionally, because it decides and does not merely
        // narrate: a capture miss with a bracket still open is a conversion whose successor we
        // failed to pair, and the converge that follows will rebind the record to the new form.
        const ue_wrap::vm_dispatch::ActiveVerb av = ue_wrap::vm_dispatch::CurrentThreadVerb();
        const coop::element::ElementId reqEid = coop::kerfur_convert_host::ActiveRequestVerbEid();
        // The active flag alone would claim in-bracket for any module's verb; the verb name is
        // what is gated on.
        const bool inOurVerb = av.active && av.verbName &&
                               (std::wcscmp(av.verbName, L"dropKerfurProp") == 0 ||
                                std::wcscmp(av.verbName, L"spawnKerfuro") == 0);
        const bool inConversion = inOurVerb || reqEid != coop::element::kInvalidId;
        static const bool s_vmLog = coop::config::ResolveFlag(::coop::config_registry::rows::vm_dispatch_log);
        if (s_vmLog && inConversion)
            UE_LOGW("kerfur_convert: 2a-capture MISS in-bracket (%s) actor=%p -- no captured B at the destroy "
                    "edge (ORDER ASSERT: spawn<destroy expected) -- treating as genuine destroy (generic "
                    "relay). provenance{inOurVerb=%d verb=%ls ctxSelf=%d reqEid=%d}",
                    isHost ? "HOST" : "CLIENT", actor, inOurVerb ? 1 : 0,
                    av.verbName ? av.verbName : L"<none>",
                    (inOurVerb && av.ctx == actor) ? 1 : 0,
                    reqEid == coop::element::kInvalidId ? -1 : static_cast<int>(reqEid));
        // Outside any bracket this prop form is simply gone, so the kerfur's record goes with it:
        // otherwise its id is never freed and its currentEid keeps answering "kerfur" after the
        // registry recycles it. A no-op unless this eid is a tracked kerfur's current form.
        if (!inConversion) KE::ReleaseKerfurForEid(dyingEid);
        return false;  // no captured conversion successor -> genuine destroy -> generic relay
    }

    if (!isHost) {
        // Client: a capture suppresses the keyed-destroy relay only. The poll owns the conversion
        // (the request plus the ghost claim): the mirror row survives this seam, since client
        // reverse maps never learn mirror actors, so the poll's premise holds and fires within 200
        // ms.
        UE_LOGI("kerfur_convert: CLIENT destroy-edge first refusal -- dying kerfur prop %p is conversion "
                "churn (fresh NPC %.0f cm away); keyed-destroy relay SUPPRESSED (the poll owns the request)",
                actor, std::sqrt(bestD2));
        return true;
    }

    // Host: converge inline at the destroy edge. The seam captured dyingEid before its unmarks
    // drained the prop Element; BindFormActor needs only the record table, not the element row.
    if (dyingEid == coop::element::kInvalidId) {
        // Never on the wire (the pre-enroll window): nothing to converge; the generic destroy and
        // the periodic NPC enroll express the flip as destroy plus spawn. Rare, lossless.
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
    coop::kerfur_convert_host::RecordSeamConvergedInBracket(dyingEid);  // bracket-conditional
    UE_LOGI("kerfur_convert: FIRST-REFUSAL turn-on converge -- dying kerfur prop eid=%u converged to fresh "
            "NPC %p eid=%u (%.0f cm; KerfurConvert broadcast, NO generic PropDestroy)",
            static_cast<unsigned>(dyingEid), freshNpc, static_cast<unsigned>(newEid), std::sqrt(bestD2));
    return true;
}

void OnDisconnect() {
    g_kerfurWatch.clear();   // GT-only; Tick is the sole toucher
    coop::kerfur_convert_client::OnDisconnect();  // the parked ghosts
    coop::kerfur_convert_host::OnDisconnect();  // the bracket pair
    g_lastConvPoll = {};
}

}  // namespace coop::kerfur_convert
