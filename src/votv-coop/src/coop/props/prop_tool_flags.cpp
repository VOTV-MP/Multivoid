// coop/props/prop_tool_flags.cpp -- see coop/props/prop_tool_flags.h.

#include "coop/props/prop_tool_flags.h"

#include "coop/comms/chat_feed.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/creatures/kerfur_entity.h"
#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/intent_queue.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/refusal_tally.h"
#include "coop/net/session.h"
#include "coop/permissions/gameplay_nodes.h"
#include "coop/permissions/permission_host.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/prop_drive_stream.h"   // IsParked
#include "coop/props/prop_hold.h"
#include "coop/props/prop_tool_park.h"
#include "coop/props/remote_prop.h"          // IsActorUnderAnyDrive
#include "coop/props/toolgun_limits.h"
#include "coop/props/toolgun_sight.h"
#include "coop/session/net_pump.h"

#include "l10n/l10n.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_events.h"   // CallBaseSetPropProps
#include "ue_wrap/actors/prop_flags.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"  // ParamOffset, InstanceOffset
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/hit_result.h"
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace coop::prop_tool_flags {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace EL = coop::element;
namespace sg = ue_wrap::script_gate;
using coop::net::PropPropsReason;
using ue_wrap::FVector;

// The tool's own init is the press: a name watch on the class, as the remover's is, keyed on names so it matches the
// class in every world.
UE_FACT_FN(kRowInit, "prop_tool_flags", L"tool_propProps_C", L"init", Declared);
constexpr int kTag = 0x50505231;  // 'PPR1'

std::atomic<coop::net::Session*> g_session{nullptr};
coop::net::Session* LoadSession() { return g_session.load(std::memory_order_acquire); }

constexpr size_t kReasons = static_cast<size_t>(PropPropsReason::Count);

// Each line below is said once per session: OnDisconnect re-arms it.
bool g_publishSendFailSaid = false;
bool g_unboundSaid = false;
bool g_unreadSaid = false;
bool g_refusalSendFailSaid = false;

// The drill's red arms: the client's press runs locally (gate off), and the host's judge skips the player's right
// (judge off).
bool GateOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::prop_props_gate_off);
    return off;
}
bool JudgeOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::prop_props_judge_off);
    return off;
}

// ---- reading a press ------------------------------------------------------------------------------------------

// The toolgun the press came from and the hit it struck, read off the init frame. False when either did not read.
bool ReadPress(const sg::Call& c, ue_wrap::hit_result::Hit* hit) {
    static R::ParamOffset sToolgun{L"toolgun"};
    const int32_t toolgunOff = sToolgun.Of(c.function);
    void* toolgun = nullptr;
    if (toolgunOff >= 0 && c.locals) std::memcpy(&toolgun, c.locals + toolgunOff, sizeof(toolgun));
    return toolgun && R::IsLive(toolgun) && ue_wrap::hit_result::ReadField(toolgun, L"outHit", hit);
}

bool ToolBool(void* tool, R::InstanceOffset& off, uint8_t* out) {
    const int32_t o = off.Of(tool);
    if (o < 0) return false;
    bool v = false;
    std::memcpy(&v, static_cast<const uint8_t*>(tool) + o, sizeof(v));
    *out = v ? 1 : 0;
    return true;
}

// The tool's four values as the spawn menu set them (assignParameters).
bool ReadToolValues(void* tool, coop::net::PropPropsIntentPayload* p) {
    static R::InstanceOffset sStatic{L"isStatic"}, sFrozen{L"isFrozen"}, sActive{L"isActive"}, sSleeping{L"isSleeping"};
    return ToolBool(tool, sStatic, &p->isStatic) && ToolBool(tool, sFrozen, &p->isFrozen) &&
           ToolBool(tool, sActive, &p->isActive) && ToolBool(tool, sSleeping, &p->isSleeping);
}

// ---- the publish point ----------------------------------------------------------------------------------------

// Is `actor`, the live copy of element `el`, an Aprop_C of a prop element? Nothing of the prop's layout is read from an
// actor for which this is false.
bool IsPropElement(const EL::Element* el, void* actor) {
    return el->GetType() == EL::ElementType::Prop && ue_wrap::prop::IsDescendantOfProp(actor);
}

// Why the prop `actor` is none the tool changes in multiplayer, or null when it is one: the judge's Unsupported
// branches for a prop element, asked also before the host publishes its own press.
const char* UnsupportedWhy(void* actor) {
    if (ue_wrap::prop::IsTrashActor(actor)) return "a trash pile or clump";
    if (ue_wrap::prop::IsTrashBitsPile(actor)) return "a trash bits pile";
    if (coop::kerfur_entity::IsKerfurPropClass(R::ClassOf(actor))) return "a kerfur prop";
    return nullptr;
}

// The state message for `actor`: its static, frozen and sleep read back from the actor, never from what the press asked.
// A prop keeps no active field to read back, so `active` is the value the setter ran with. `actor` is null, and the
// flags zero, when the host resolved no prop to read.
coop::net::PropPropsStatePayload StateOf(uint32_t eid, void* actor, PropPropsReason reason, bool active) {
    coop::net::PropPropsStatePayload st{};
    st.eid = eid;
    st.isActive = active ? 1 : 0;
    st.reason = static_cast<uint8_t>(reason);
    if (actor) {
        st.isStatic = ue_wrap::prop::IsStatic(actor) ? 1 : 0;
        st.isFrozen = ue_wrap::prop::IsFrozen(actor) ? 1 : 0;
        st.isSleeping = ue_wrap::prop::IsSleeping(actor) ? 1 : 0;
    }
    return st;
}

// The one publish point: after the host's judged apply and after its own press, the prop's flags go to every peer. A
// target that is no live prop element the judge would accept publishes nothing. The sender is included, unlike MTA's
// relay, which skips the source (CGame.cpp:2768): its press was cancelled, so this message alone changes its copy.
void Publish(coop::net::Session& s, void* actor, bool active) {
    if (!actor) return;
    if (!coop::kerfur_entity::KerfurPropBound()) {
        if (!g_unboundSaid) {
            g_unboundSaid = true;
            UE_LOGW("prop_tool_flags: the host's press: the kerfur prop class is not bound yet; its change reaches no peer "
                    "until the join-window correction or the next press");
        }
        return;
    }
    const EL::ElementId eid = EL::Registry::Get().EidForActor(actor);
    if (eid == EL::kInvalidId) return;
    const EL::Element* el = EL::Registry::Get().Get(eid);
    if (!el || el->LiveActor() != actor || !IsPropElement(el, actor) || UnsupportedWhy(actor)) return;
    const coop::net::PropPropsStatePayload st = StateOf(static_cast<uint32_t>(eid), actor, PropPropsReason::Applied, active);
    if (!s.SendReliable(coop::net::ReliableKind::PropPropsState, &st, sizeof(st)) && !g_publishSendFailSaid) {
        g_publishSendFailSaid = true;
        UE_LOGW("prop_tool_flags: the flags of element %u were not sent; a peer keeps its old ones until the join-window "
                "correction or the next press", st.eid);
    }
    UE_LOGI("prop_tool_flags: element %u static=%d frozen=%d sleep=%d active=%d published", st.eid, st.isStatic,
            st.isFrozen, st.isSleeping, st.isActive);
}

// ---- client ---------------------------------------------------------------------------------------------------

// A notice is shown at most once per reason per window; the slot after the host's reasons is the client's own
// "cannot place this press" line.
constexpr uint64_t kShowEveryMs = 5000;
constexpr size_t   kNotChangeable = kReasons;
uint64_t g_shownAt[kReasons + 1] = {};
bool     g_shown[kReasons + 1] = {};

void Show(size_t slot, const std::string& line) {
    const uint64_t now = ::GetTickCount64();
    if (g_shown[slot] && now - g_shownAt[slot] < kShowEveryMs) return;
    g_shown[slot] = true;
    g_shownAt[slot] = now;
    coop::chat_feed::Push(line, coop::chat_feed::Keep::Transient);
}

void ShowReason(PropPropsReason r) {
    const size_t slot = static_cast<size_t>(r);
    switch (r) {
    case PropPropsReason::NoRight:
        Show(slot, std::string(l10n::T("The host does not allow the properties tool")));
        return;
    case PropPropsReason::Unsupported:
        Show(slot, std::string(l10n::T("The properties tool cannot change that in multiplayer")));
        return;
    case PropPropsReason::NoToolgun:
        Show(slot, std::string(l10n::T("You are not holding a toolgun")));
        return;
    case PropPropsReason::Held:
        Show(slot, std::string(l10n::T("That is being held")));
        return;
    case PropPropsReason::OutOfSight:
        Show(slot, std::string(l10n::T("You can't see that from there")));
        return;
    case PropPropsReason::Busy:
        Show(slot, std::string(l10n::T("Not changeable right now")));
        return;
    default:
        return;
    }
}

// The press is cancelled and the tool destroyed (the body's own end would have done it): the toolgun is left as after
// a native miss.
sg::Verdict CancelPress(const sg::Call& c) {
    E::DestroyActor(c.object);
    return sg::Verdict::Cancel;
}

// A press this client cannot place: nothing is changed locally and the player is told.
sg::Verdict Unplaceable(const sg::Call& c, const char* why) {
    UE_LOGW("prop_tool_flags: a press is cancelled: %s", why);
    Show(kNotChangeable, std::string(l10n::T("Not changeable right now")));
    return CancelPress(c);
}

// MTA applies a client's change first and lets a refusal diverge, then resyncs it (Client CStaticFunctionDefinitions.cpp:1050,
// Server CGame.cpp:2743-2766); a press cancels first, as the remover does, since a setter override (arming a landmine, a
// TV's state) cannot be undone on a refusal.
sg::Verdict OnClientPress(coop::net::Session& s, const sg::Call& c) {
    if (!coop::net_pump::IsInAnnouncedWorld(c.object) || GateOff()) return sg::Verdict::Run;
    ue_wrap::hit_result::Hit hit;
    if (!ReadPress(c, &hit)) return Unplaceable(c, "the toolgun's outHit did not read");
    // The game's body does nothing for a miss and hints "Not a pop" for a hit on a non-prop: both run natively, as does
    // an actor this session does not share. A keyed prop this client cannot name is refused, as the remover refuses it:
    // run natively it would change this copy alone.
    if (!hit.blocking || !hit.actor) return sg::Verdict::Run;
    const EL::ElementId eid = EL::Registry::Get().EidForActor(hit.actor);
    if (eid == EL::kInvalidId) {
        if (ue_wrap::prop::IsKeyedInteractable(hit.actor)) return Unplaceable(c, "a keyed prop with no element id");
        return sg::Verdict::Run;
    }
    FVector camera{};
    void* player = coop::players::Registry::Get().Local();
    if (!player || !E::ReadMainPlayerCameraLocation(player, camera))
        return Unplaceable(c, "this player's camera did not read");
    coop::net::PropPropsIntentPayload p{};
    p.eid = static_cast<uint32_t>(eid);
    p.origin[0] = camera.X;
    p.origin[1] = camera.Y;
    p.origin[2] = camera.Z;
    p.hit[0] = hit.location.X;
    p.hit[1] = hit.location.Y;
    p.hit[2] = hit.location.Z;
    if (!ReadToolValues(c.object, &p)) return Unplaceable(c, "the tool's values did not read");
    if (!s.SendReliableToSlot(0, coop::net::ReliableKind::PropPropsIntent, &p, sizeof(p)))
        return Unplaceable(c, "the press was not sent");
    UE_LOGI("prop_tool_flags: this client's press on element %u sent to the host (static=%d frozen=%d active=%d "
            "sleep=%d)", p.eid, p.isStatic, p.isFrozen, p.isActive, p.isSleeping);
    return CancelPress(c);
}

// ---- host: its own press --------------------------------------------------------------------------------------

// The host's press runs natively; the tool, its target and the tool's active value are held to the post row, which
// publishes the flags.
void*                 g_pressTool = nullptr;
ue_wrap::CachedObjRef g_pressTarget;
bool                  g_pressActive = false;

sg::Verdict OnHostPress(const sg::Call& c) {
    g_pressTool = nullptr;
    g_pressTarget.Reset();
    ue_wrap::hit_result::Hit hit;
    if (!ReadPress(c, &hit)) {
        if (!g_unreadSaid) {
            g_unreadSaid = true;
            UE_LOGW("prop_tool_flags: the host's press: the toolgun's outHit did not read; its change reaches no peer");
        }
        return sg::Verdict::Run;
    }
    if (!hit.blocking || !hit.actor) return sg::Verdict::Run;
    g_pressTool = c.object;
    g_pressTarget.Set(hit.actor);
    static R::InstanceOffset sActive{L"isActive"};
    uint8_t active = 0;
    g_pressActive = ToolBool(c.object, sActive, &active) && active != 0;
    return sg::Verdict::Run;
}

sg::Verdict OnInitPre(const sg::Call& c) {
    coop::net::Session* s = LoadSession();
    if (!s || !s->running()) return sg::Verdict::Run;
    if (s->role() == coop::net::Role::Client) return OnClientPress(*s, c);
    if (s->role() == coop::net::Role::Host) return OnHostPress(c);
    return sg::Verdict::Run;
}

void OnInitPost(const sg::Call& c) {
    if (!g_pressTool || c.object != g_pressTool) return;
    g_pressTool = nullptr;
    void* const target = g_pressTarget.Get();
    g_pressTarget.Reset();
    coop::net::Session* s = LoadSession();
    if (target && s && s->running() && s->role() == coop::net::Role::Host) Publish(*s, target, g_pressActive);
}

// ---- host: a client's press -----------------------------------------------------------------------------------

coop::net::RefusalTally g_tally{"prop_props"};

// A request with the sender's eye as the host held it when the request arrived: queued presses are judged against the
// pose they were made at, not the pose of the tick that serves them.
struct Queued {
    coop::net::PropPropsIntentPayload req;
    FVector eye{};
    bool haveEye = false;
};

struct Slot {
    coop::net::IntentQueue<Queued> queue;
};
coop::roster_ledger::PerSlotState<Slot> g_slots;

void Refuse(uint8_t slot, const char* why) { g_tally.Note(slot, why, coop::net::RefusalTally::Clock::now()); }

inline bool Finite3(const float v[3]) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

// What the judge decided and, for a reason that is not Applied or Gone, the exact branch it took: a static string the
// tally and the log carry, so a refusal says which check stopped it.
struct Verdict {
    PropPropsReason reason;
    const char*     why;
};

constexpr Verdict Decided(PropPropsReason r, const char* why) { return {r, why}; }

// Could the sender have struck `actor`? The judge is the toolgun lanes' shared one.
Verdict JudgeSight(void* puppet, void* actor, const Queued& q) {
    const coop::net::PropPropsIntentPayload& p = q.req;
    if (!q.haveEye) return Decided(PropPropsReason::NotReady, "no eye (the sender's head position did not read)");
    const coop::toolgun_sight::SightResult s = coop::toolgun_sight::JudgeToolgunSight(
        puppet, actor, FVector{p.origin[0], p.origin[1], p.origin[2]}, FVector{p.hit[0], p.hit[1], p.hit[2]}, q.eye);
    if (s.blocked) {
        static coop::net::RefusalTally::Clock::time_point sLast{};
        coop::toolgun_sight::NoteBlocked("prop_tool_flags", sLast, s);
    }
    if (s.ok) return Decided(PropPropsReason::Applied, "");
    return Decided(s.judged ? PropPropsReason::OutOfSight : PropPropsReason::Busy, s.why);
}

// Judges one press in the remover's order; `*out` is the live prop once the element is known to be a prop element, to
// set on Applied and to read the flags a refusal carries. A refusal before that carries zero flags.
Verdict Judge(coop::net::Session& s, uint8_t slot, const Queued& q, void** out) {
    const coop::net::PropPropsIntentPayload& p = q.req;
    if (slot < 1 || slot >= coop::players::kMaxPeers) return Decided(PropPropsReason::NotReady, "slot out of range");
    if (!s.IsSlotWorldReady(slot))
        return Decided(PropPropsReason::NotReady, "the sender's world is not announced ready");
    const std::string guid = s.ProvedGuidForSlotWithToken(slot, coop::roster_ledger::Get(slot).bornGeneration);
    if (guid.empty()) return Decided(PropPropsReason::NotReady, "the sender's id is not proved");
    if (!Finite3(p.origin) || !Finite3(p.hit))
        return Decided(PropPropsReason::NotReady, "a non-finite origin or hit point");
    coop::RemotePlayer& rp = coop::puppet_drive::Puppet(slot);
    void* puppet = rp.valid() && rp.HasPose() ? rp.GetActor() : nullptr;
    if (!puppet) return Decided(PropPropsReason::NotReady, "no posed puppet for the sender");

    const EL::Element* el = p.eid < EL::kMaxElements ? EL::Registry::Get().Get(p.eid) : nullptr;
    void* actor = el ? el->LiveActor() : nullptr;
    if (!actor) return Decided(PropPropsReason::Gone, "no live element");
    if (!IsPropElement(el, actor)) return Decided(PropPropsReason::Unsupported, "the element is not a prop");
    *out = actor;
    if (!coop::kerfur_entity::KerfurPropBound())
        return Decided(PropPropsReason::NotReady, "the kerfur prop class is not bound yet");
    if (const char* why = UnsupportedWhy(actor)) return Decided(PropPropsReason::Unsupported, why);

    namespace GN = coop::permissions::gameplay_nodes;
    if (!JudgeOff() &&
        !coop::permissions::host::Allows(guid, GN::kToolgunPropProps.node, GN::kToolgunPropProps.defaultGranted, false))
        return Decided(PropPropsReason::NoRight, "the properties-tool node is withheld from the sender");

    if (!coop::hand_item::HeldClassIs(slot, L"prop_toolgun_C"))
        return Decided(PropPropsReason::NoToolgun, "the sender's hand holds no toolgun");

    if (const coop::prop_hold::Holder h = coop::prop_hold::WhoHolds(actor, slot); h != coop::prop_hold::Holder::None)
        return Decided(PropPropsReason::Held, coop::prop_hold::HolderWhy(h));

    return JudgeSight(puppet, actor, q);
}

// A refusal goes to the sender alone with the host's flags when the prop resolved, else with none; Applied is published to everyone by the publish
// point. A reason that could not be judged is said at once with its cause.
void Refusal(coop::net::Session& s, uint8_t slot, uint32_t eid, void* actor, const Verdict& v) {
    if (v.reason != PropPropsReason::Gone) g_tally.Note(slot, v.why, coop::net::RefusalTally::Clock::now());
    if (v.reason == PropPropsReason::Busy)
        UE_LOGW("prop_tool_flags: slot %u's element %u not changed: %s", slot, eid, v.why);
    const coop::net::PropPropsStatePayload st = StateOf(eid, actor, v.reason, false);
    if (!s.SendReliableToSlot(slot, coop::net::ReliableKind::PropPropsState, &st, sizeof(st)) &&
        !g_refusalSendFailSaid) {
        g_refusalSendFailSaid = true;
        UE_LOGW("prop_tool_flags: the answer to slot %u was not sent; its press ends without a notice", slot);
    }
}

// A press judged: the host runs the game's own setter on its copy with the tool's four values, so an override (a
// landmine, a TV) takes its own path as in single player, and publishes what the actor then holds.
void Serve(coop::net::Session& s, uint8_t slot, const Queued& q) {
    void* actor = nullptr;
    Verdict v = Judge(s, slot, q, &actor);
    if (v.reason == PropPropsReason::Applied) {
        const coop::net::PropPropsIntentPayload& p = q.req;
        if (!ue_wrap::prop::CallSetPropProps(actor, p.isStatic != 0, p.isFrozen != 0, p.isActive != 0,
                                             p.isSleeping != 0)) {
            v = Decided(PropPropsReason::Busy, "the game's setter did not dispatch");
        } else {
            UE_LOGI("prop_tool_flags: slot %u's press on element %u applied (static=%d frozen=%d active=%d sleep=%d)",
                    slot, q.req.eid, p.isStatic, p.isFrozen, p.isActive, p.isSleeping);
            Publish(s, actor, p.isActive != 0);
            return;
        }
    }
    Refusal(s, slot, q.req.eid, actor, v);
}

void ServeQueues(coop::net::Session& s) {
    for (uint8_t slot = 1; slot < coop::players::kMaxPeers; ++slot) {
        g_slots[slot].queue.Serve(coop::toolgun_limits::kBudget, coop::net::NowMs, [&](const Queued& q) {
            Serve(s, slot, q);
            return coop::net::Served::Done;
        });
    }
}

// ---- client: the host's flags ---------------------------------------------------------------------------------

// A state for a prop this player holds, or a drive moves, waits for its release: the setter's init() switches physics
// and collision, which breaks the physics-handle grab (ue_wrap/engine/engine_mainplayer.cpp, the hold's mirror), so it
// is deferred as the join-window drain defers a correction on a held or driven prop
// (coop/element/quiescence_drain.cpp, the held/driven deferral). The drain drops its correction after a bound because
// the hold's release re-states a pose; nothing re-states a tool press, so the parked state stays until the hold ends and
// is applied there, and the drive's end edge converges static too (prop_drive_stream.cpp, ApplyEnd).
coop::prop_tool_park::Ledger g_parked;

bool HeldHere(void* actor) {
    void* const local = coop::players::Registry::Get().Local();
    return (local && E::IsMainPlayerGrabbing(local, actor)) || coop::remote_prop::IsActorUnderAnyDrive(actor) ||
           coop::prop_drive_stream::IsParked(actor);
}

// The element's copy on this peer when it is a prop, else null: an eid this peer does not know, or one that is no
// prop here, has nothing to set.
void* PropCopy(uint32_t eid) {
    const EL::Element* el = eid < EL::kMaxElements ? EL::Registry::Get().Get(eid) : nullptr;
    void* const actor = el ? el->LiveActor() : nullptr;
    return actor && ue_wrap::prop::IsDescendantOfProp(actor) ? actor : nullptr;
}

// The press as the host ran it: the game's setter on the copy's own class with the four values, so an override (a
// landmine, a TV, a light) takes its own path exactly as in single player. Every state is a press and is applied: an
// override reads `active` as its own state, so equal flags are no sign that nothing is to be done.
void ApplyState(const coop::net::PropPropsStatePayload& st, void* actor) {
    const bool wantStatic = st.isStatic != 0, wantFrozen = st.isFrozen != 0, wantSleep = st.isSleeping != 0;
    const bool wantActive = st.isActive != 0;
    if (!ue_wrap::prop::CallSetPropProps(actor, wantStatic, wantFrozen, wantActive, wantSleep)) {
        UE_LOGW("prop_tool_flags: element %u -- the game's setter did not dispatch; the copy keeps its flags", st.eid);
        return;
    }
    UE_LOGI("prop_tool_flags: element %u set static=%d frozen=%d active=%d sleep=%d", st.eid, wantStatic ? 1 : 0,
            wantFrozen ? 1 : 0, wantActive ? 1 : 0, wantSleep ? 1 : 0);
}

void RetryParked() {
    if (g_parked.Empty()) return;
    static const coop::prop_tool_park::Env env{&PropCopy, &HeldHere, &ApplyState};
    g_parked.Retry(::GetTickCount64(), env);
}

// A refusal after the host resolved the prop carries the host's current flags: if this copy differs (a state it missed,
// a press it ran before the host's word) they are put back through prop_C's own setter, MTA's resync of a refused
// change (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:2779-2795); otherwise only the notice shows.
void ResyncToHost(const coop::net::PropPropsStatePayload& st) {
    switch (static_cast<PropPropsReason>(st.reason)) {
    case PropPropsReason::Unsupported:
    case PropPropsReason::NoRight:
    case PropPropsReason::NoToolgun:
    case PropPropsReason::Held:
    case PropPropsReason::OutOfSight:
    case PropPropsReason::Busy:       // resolved, then the sight or the setter failed: the flags are the host's
        break;
    default:
        return;   // the host had no prop to read: the flags are not its own
    }
    // An Unsupported answer for an element that is no prop carries zero flags; this peer has no prop copy of it, so
    // PropCopy is null and nothing is put back.
    void* const actor = PropCopy(st.eid);
    if (!actor || HeldHere(actor)) return;
    g_parked.Supersede(st.eid);   // the host's current flags are newer than any state parked for this copy
    const bool wantStatic = st.isStatic != 0, wantFrozen = st.isFrozen != 0, wantSleep = st.isSleeping != 0;
    if (ue_wrap::prop::IsStatic(actor) == wantStatic && ue_wrap::prop::IsFrozen(actor) == wantFrozen &&
        ue_wrap::prop::IsSleeping(actor) == wantSleep)
        return;
    if (!ue_wrap::prop::CallBaseSetPropProps(actor, wantStatic, wantFrozen, wantSleep)) {
        UE_LOGW("prop_tool_flags: element %u -- the game's setter did not dispatch; the copy keeps its flags", st.eid);
        return;
    }
    UE_LOGI("prop_tool_flags: element %u put back to the host's static=%d frozen=%d sleep=%d", st.eid,
            wantStatic ? 1 : 0, wantFrozen ? 1 : 0, wantSleep ? 1 : 0);
}

// ---- the watch's settle --------------------------------------------------------------------------------------

enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg = Reg::Pending;
bool g_settled = false;

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_reg != Reg::Pending) return;
    // A client cancels the press by this watch, and the host publishes its own press from the post row; the gate
    // refuses a watch it cannot keep for the process.
    g_reg = sg::WatchName(kRowInit, kTag, &OnInitPre, &OnInitPost) ? Reg::Registered : Reg::Refused;
}

void Tick() {
    RetryParked();
    // The names resolve on a game-thread task the registration posted; the poll of the gate's table runs once a second
    // until the watch is live or dead, not every tick.
    static uint64_t sSettleTryAt = 0;
    const uint64_t tickNow = ::GetTickCount64();
    if (!g_settled && g_reg != Reg::Pending && tickNow - sSettleTryAt >= 1000) {
        sSettleTryAt = tickNow;
        if (g_reg == Reg::Registered) sg::ResolvePendingNames();
        if (g_reg == Reg::Registered && sg::NameWatchLive(kRowInit, kTag)) {
            g_settled = true;
            UE_LOGI("prop_tool_flags: the watch on %ls::%ls is live", kRowInit.Class(), kRowInit.Name());
        } else if (g_reg == Reg::Refused || sg::NameWatchSettled(kRowInit, kTag)) {
            g_settled = true;
            UE_LOGE("prop_tool_flags: the watch on %ls::%ls is dead -- a client's properties tool runs natively and "
                    "changes its own copy alone", kRowInit.Class(), kRowInit.Name());
        }
    }
    coop::net::Session* s = LoadSession();
    if (s && s->running() && s->role() == coop::net::Role::Host) {
        ServeQueues(*s);
        g_tally.Flush(coop::net::RefusalTally::Clock::now());
    }
}

void OnIntentReceived(int senderSlot, const uint8_t* bytes, uint16_t len) {
    coop::net::Session* s = LoadSession();
    if (!s || s->role() != coop::net::Role::Host) {
        UE_LOGW("prop_tool_flags: PropPropsIntent received on a non-host -- dropping");
        return;
    }
    const uint8_t slot = (senderSlot >= 0 && senderSlot < coop::net::kMaxPeers) ? static_cast<uint8_t>(senderSlot)
                                                                                  : static_cast<uint8_t>(0xFF);
    // The refusal is the lane's to say, in its tally: a sender cannot make the router's log the cost.
    if (len < sizeof(coop::net::PropPropsIntentPayload)) {
        Refuse(slot, "payload too short");
        return;
    }
    // Slot 0 is the host, whose own presses never arrive over the wire.
    if (slot == 0 || slot >= coop::players::kMaxPeers) {
        Refuse(slot, "unknown sender");
        return;
    }
    // A full queue refuses before anything is read: a flood must not pay the eye read per message.
    if (g_slots[slot].queue.size() >= coop::toolgun_limits::kMaxPending) {
        Refuse(slot, "queue full");
        return;
    }
    Queued q;
    std::memcpy(&q.req, bytes, sizeof(q.req));
    // Only a puppet that has had its first pose has an eye: before it, it stands at its spawn point.
    coop::RemotePlayer& rp = coop::puppet_drive::Puppet(slot);
    q.haveEye = rp.valid() && rp.HasPose() && rp.TryGetEyePosition(q.eye);
    g_slots[slot].queue.Push(std::move(q), coop::toolgun_limits::kMaxPending);   // room was checked above
}

void OnStateReceived(int senderSlot, const uint8_t* bytes, uint16_t len) {
    coop::net::Session* s = LoadSession();
    if (!s || s->role() != coop::net::Role::Client || senderSlot != 0 || len < sizeof(coop::net::PropPropsStatePayload)) {
        UE_LOGW("prop_tool_flags: PropPropsState dropped (role, sender slot %d or length %u)", senderSlot,
                static_cast<unsigned>(len));
        return;
    }
    coop::net::PropPropsStatePayload payload{};
    std::memcpy(&payload, bytes, sizeof(payload));
    if (payload.reason >= kReasons) {
        UE_LOGW("prop_tool_flags: PropPropsState reason %u unknown -- dropping", static_cast<unsigned>(payload.reason));
        return;
    }
    const PropPropsReason reason = static_cast<PropPropsReason>(payload.reason);
    if (reason != PropPropsReason::Applied) {
        ResyncToHost(payload);
        ShowReason(reason);
        return;
    }
    // An eid this peer does not know is dropped; the join-window correction carries the flags of a prop that was born
    // or loaded after.
    void* const actor = PropCopy(payload.eid);
    if (!actor) {
        UE_LOGI("prop_tool_flags: flags for element %u name no prop here -- dropped", payload.eid);
        return;
    }
    if (HeldHere(actor)) {
        g_parked.Park(payload, ::GetTickCount64());
        UE_LOGI("prop_tool_flags: element %u is held or driven here -- its flags wait for the release", payload.eid);
        return;
    }
    g_parked.Supersede(payload.eid);   // an older state parked for this copy is not replayed over this one
    ApplyState(payload, actor);
}

void OnDisconnect() {
    g_tally.Clear();
    for (size_t i = 0; i <= kReasons; ++i) {
        g_shown[i] = false;
        g_shownAt[i] = 0;
    }
    g_pressTool = nullptr;
    g_pressTarget.Reset();
    g_parked.Clear();
    g_publishSendFailSaid = g_unboundSaid = g_unreadSaid = g_refusalSendFailSaid = false;
}

}  // namespace coop::prop_tool_flags
