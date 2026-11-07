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
#include "coop/props/remote_prop.h"          // IsActorUnderAnyDrive
#include "coop/props/toolgun_limits.h"
#include "coop/props/toolgun_sight.h"
#include "coop/session/net_pump.h"

#include "l10n/l10n.h"

#include "ue_wrap/actors/prop.h"
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
#include <unordered_map>

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

// The state message for `actor`: its static, frozen and sleep read back from the actor, never from what the press asked.
// A prop keeps no active field to read back, so `active` is the value the setter ran with.
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
// target that is no published prop publishes nothing.
void Publish(coop::net::Session& s, void* actor, bool active) {
    if (!actor || !ue_wrap::prop::IsKeyedInteractable(actor)) return;
    const EL::ElementId eid = EL::Registry::Get().EidForActor(actor);
    if (eid == EL::kInvalidId) return;
    const coop::net::PropPropsStatePayload st = StateOf(static_cast<uint32_t>(eid), actor, PropPropsReason::Applied, active);
    s.SendReliable(coop::net::ReliableKind::PropPropsState, &st, sizeof(st));
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

sg::Verdict OnClientPress(coop::net::Session& s, const sg::Call& c) {
    if (!coop::net_pump::IsInAnnouncedWorld(c.object) || GateOff()) return sg::Verdict::Run;
    ue_wrap::hit_result::Hit hit;
    if (!ReadPress(c, &hit)) return Unplaceable(c, "the toolgun's outHit did not read");
    // The game's body does nothing for a miss and hints "Not a pop" for a hit on a non-prop: both run natively, as does
    // an actor this session does not publish (the remover's miss path).
    if (!hit.blocking || !hit.actor) return sg::Verdict::Run;
    const EL::ElementId eid = EL::Registry::Get().EidForActor(hit.actor);
    if (eid == EL::kInvalidId) return sg::Verdict::Run;
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
    if (!ReadPress(c, &hit) || !hit.blocking || !hit.actor) return sg::Verdict::Run;
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
coop::roster_ledger::PerSlotState<uint32_t> g_refused;

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

void Refuse(uint8_t slot, const char* why) {
    if (slot >= 1 && slot < coop::players::kMaxPeers) ++g_refused[slot];
    g_tally.Note(slot, why, coop::net::RefusalTally::Clock::now());
}

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
        UE_LOGI("prop_tool_flags: a segment was blocked by %ls at %.0f uu from the eye; the hit point is %.0f uu away "
                "and the client's origin %.0f uu from the eye",
                s.blockerActor ? R::ClassNameOf(s.blockerActor).c_str() : L"(no actor)", s.dEyeBlock, s.dEyeHit,
                s.dOriginEye);
    }
    if (s.ok) return Decided(PropPropsReason::Applied, "");
    return Decided(s.judged ? PropPropsReason::OutOfSight : PropPropsReason::Busy, s.why);
}

// Judges one press in the remover's order; on Applied `*out` is the prop to set.
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
    if (!coop::kerfur_entity::KerfurPropBound())
        return Decided(PropPropsReason::NotReady, "the kerfur prop class is not bound yet");
    if (el->GetType() != EL::ElementType::Prop)
        return Decided(PropPropsReason::Unsupported, "the element is not a prop");
    if (ue_wrap::prop::IsTrashActor(actor)) return Decided(PropPropsReason::Unsupported, "a trash pile or clump");
    if (ue_wrap::prop::IsTrashBitsPile(actor)) return Decided(PropPropsReason::Unsupported, "a trash bits pile");
    if (coop::kerfur_entity::IsKerfurPropClass(R::ClassOf(actor)))
        return Decided(PropPropsReason::Unsupported, "a kerfur prop");

    namespace GN = coop::permissions::gameplay_nodes;
    if (!JudgeOff() &&
        !coop::permissions::host::Allows(guid, GN::kToolgunPropProps.node, GN::kToolgunPropProps.defaultGranted, false))
        return Decided(PropPropsReason::NoRight, "the properties-tool node is withheld from the sender");

    if (!coop::hand_item::HeldClassIs(slot, L"prop_toolgun_C"))
        return Decided(PropPropsReason::NoToolgun, "the sender's hand holds no toolgun");

    if (const coop::prop_hold::Holder h = coop::prop_hold::WhoHolds(actor, slot); h != coop::prop_hold::Holder::None)
        return Decided(PropPropsReason::Held, coop::prop_hold::HolderWhy(h));

    const Verdict sight = JudgeSight(puppet, actor, q);
    if (sight.reason == PropPropsReason::Applied) *out = actor;
    return sight;
}

// A refusal goes to the sender alone with the host's current flags; Applied is published to everyone by the publish
// point. A reason that could not be judged is said at once with its cause.
void Refusal(coop::net::Session& s, uint8_t slot, uint32_t eid, void* actor, const Verdict& v) {
    g_tally.Note(slot, v.why, coop::net::RefusalTally::Clock::now());
    if (v.reason == PropPropsReason::Busy)
        UE_LOGW("prop_tool_flags: slot %u's element %u not changed: %s", slot, eid, v.why);
    const coop::net::PropPropsStatePayload st = StateOf(eid, actor, v.reason, false);
    s.SendReliableToSlot(slot, coop::net::ReliableKind::PropPropsState, &st, sizeof(st));
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
// (coop/element/quiescence_drain.cpp, the held/driven deferral) and dropped under the same ten-second bound.
struct Parked {
    coop::net::PropPropsStatePayload state;
    uint64_t since;
};
std::unordered_map<uint32_t, Parked> g_parked;
constexpr uint64_t kParkMs = 10000;

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
    if (g_parked.empty()) return;
    const uint64_t now = ::GetTickCount64();
    for (auto it = g_parked.begin(); it != g_parked.end();) {
        void* const actor = PropCopy(it->first);
        if (!actor) {
            it = g_parked.erase(it);
        } else if (!HeldHere(actor)) {
            ApplyState(it->second.state, actor);
            it = g_parked.erase(it);
        } else if (now - it->second.since > kParkMs) {
            UE_LOGW("prop_tool_flags: element %u stayed held or driven for %llu ms -- its flags are not applied; the "
                    "hold's release states its own", it->first, static_cast<unsigned long long>(kParkMs));
            it = g_parked.erase(it);
        } else {
            ++it;
        }
    }
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
    if (!g_settled && g_reg != Reg::Pending) {
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

void OnIntent(const coop::net::PropPropsIntentPayload& payload, uint8_t senderSlot) {
    coop::net::Session* s = LoadSession();
    if (!s || s->role() != coop::net::Role::Host) {
        UE_LOGW("prop_tool_flags: PropPropsIntent received on a non-host -- dropping");
        return;
    }
    // Slot 0 is the host, whose own presses never arrive over the wire.
    if (senderSlot == 0 || senderSlot >= coop::players::kMaxPeers) {
        Refuse(senderSlot, "unknown sender");
        return;
    }
    // A full queue refuses before anything is read: a flood must not pay the eye read per message.
    if (g_slots[senderSlot].queue.size() >= coop::toolgun_limits::kMaxPending) {
        Refuse(senderSlot, "queue full");
        return;
    }
    Queued q;
    q.req = payload;
    // Only a puppet that has had its first pose has an eye: before it, it stands at its spawn point.
    coop::RemotePlayer& rp = coop::puppet_drive::Puppet(senderSlot);
    q.haveEye = rp.valid() && rp.HasPose() && rp.TryGetEyePosition(q.eye);
    g_slots[senderSlot].queue.Push(std::move(q), coop::toolgun_limits::kMaxPending);   // room was checked above
}

void NoteRefused(uint8_t senderSlot, const char* why) { Refuse(senderSlot, why); }

void OnState(const coop::net::PropPropsStatePayload& payload) {
    if (payload.reason >= kReasons) return;
    const PropPropsReason reason = static_cast<PropPropsReason>(payload.reason);
    if (reason != PropPropsReason::Applied) {
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
        g_parked[payload.eid] = Parked{payload, ::GetTickCount64()};
        UE_LOGI("prop_tool_flags: element %u is held or driven here -- its flags wait for the release", payload.eid);
        return;
    }
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
    g_parked.clear();
}

}  // namespace coop::prop_tool_flags
