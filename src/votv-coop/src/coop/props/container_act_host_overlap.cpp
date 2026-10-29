// coop/props/container_act_host_overlap.cpp -- the host judges a put at its own overlap: a shared prop that a CLIENT drives
// (an elected throw, a claimed push, a held pose) enters a shared world container's box on the host. The host's copy is
// kinematic there, so the container's own putObjectIn_overlap refuses it at its first gate; this PRE on that function judges
// the put instead. No client message asks for it: the client's copy of the call is cancelled (container_act_put). MTA tests
// the position its server applied (CObjectSync.cpp:220, CUnoccupiedVehicleSync.cpp:456); here the engine's overlap step of
// the parked copy, which moves every frame, IS that test at our apply. Source's BumpWeapon is the server's own touch.
//
// The judge's order is container_overlap_pure.h's Decide: armed; reach of a held or claimed driver (an elected throw has
// none); no client's hold on the container; the body's silent gates; the fit. Then the driver's row or drive ends, the copy
// simulates, the PRE returns Run and the game's own body files the prop; the POST reads its death, and a prop alive after a
// body whose pre-check passed is a term the judge did not see, counted and loud. A prop nobody drives is the BP's own, but a
// put on a held container waits whoever's it is (HostPre's rule for R2 and R3). Every put is filed, refused with its reason,
// or dropped and counted. A waiting put is released by its edge: a hold's end, its prop's bind, its container's change; a
// prop still in the box with no driver is then the host's own put, pre-checked as the judge does, and the body runs on it,
// since a prop already overlapping fires no second BeginOverlap.

#include "coop/props/container_act_host.h"
#include "coop/props/container_act_host_internal.h"
#include "coop/props/container_overlap_pure.h"

#include "coop/element/intent_authority.h"
#include "coop/element/registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/props/container_act_state.h"
#include "coop/props/container_arming.h"
#include "coop/props/container_reach.h"
#include "coop/props/container_record_edit.h"
#include "coop/props/prop_drive_host.h"
#include "coop/props/remote_prop.h"

#include "ue_wrap/actors/container_box.h"
#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_events.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/actor_end_play.h"
#include "ue_wrap/engine/engine_physics.h"

#include <cstddef>
#include <cstring>
#include <vector>

namespace coop::props::container_act_host::overlap {
namespace {

namespace cas = coop::props::container_act_state;
namespace ca  = coop::props::container_arming;
namespace cb  = ue_wrap::container_box;
namespace ci  = ue_wrap::container_inventory;
namespace cr  = coop::props::container_reach;
namespace cre = coop::props::container_record_edit;
namespace E   = ue_wrap::engine;
namespace EL  = coop::element;
namespace R   = ue_wrap::reflection;
namespace SR  = ue_wrap::save_record;
namespace sg  = ue_wrap::script_gate;
namespace P   = pure;

using WaitRow = P::WaitRow<ue_wrap::CachedObjRef>;

coop::net::Session* g_session = nullptr;
int g_applyDepth = 0;        // our own call of the body is running: the PRE stands aside
bool g_unreadSaid = false;   // the parameters did not read: said once a session
bool g_dropSaid = false;     // a wait was dropped for a full table: said once a session
P::WaitTable<ue_wrap::CachedObjRef> g_waiting;
P::LatchTable<ue_wrap::CachedObjRef> g_latched;

// Who drives the prop: a client's row on it (elected or claimed) and/or a held pose stream (a remote drive).
struct Driver {
    uint8_t rowSlot = 0;     // 0: no client row
    int driveSlot = -1;      // -1: no remote drive
    bool elected = false;    // the row is an elected throw
    uint32_t key = 0;        // the drive key: its slot and the generation of its row and hold
    uint8_t Slot() const { return rowSlot != 0 ? rowSlot : static_cast<uint8_t>(driveSlot); }
};

bool DriverOf(void* prop, uint32_t propEid, Driver& d) {
    d.rowSlot = coop::prop_drive_host::ClientSyncerOf(propEid);
    d.driveSlot = coop::remote_prop::DrivingSlotOf(prop);
    if (d.rowSlot == 0 && d.driveSlot < 0) return false;
    d.elected = d.rowSlot != 0 && coop::prop_drive_host::ClientRowElected(propEid);
    const uint8_t rowGen = d.rowSlot != 0 ? coop::prop_drive_host::ContextOf(prop, propEid).gen : uint8_t{0};
    const uint16_t holdGen = d.driveSlot >= 0 ? coop::remote_prop::HoldGenOf(d.driveSlot) : uint16_t{0};
    d.key = P::DriveKey(d.Slot(), rowGen, holdGen);
    return true;
}

// The prop whose body the PRE let run, until the POST reads whether it died.
struct Pending {
    ue_wrap::CachedObjRef prop;
    uint32_t propEid = 0;
    uint32_t containerEid = 0;
    uint8_t slot = 0;
};
Pending g_pending;

template <class T>
bool ReadParam(R::ParamOffset& offset, const sg::Call& c, T& out) {
    const int32_t o = offset.Of(c.function);
    if (o < 0 || !c.locals) return false;
    std::memcpy(&out, c.locals + o, sizeof(T));
    return true;
}

// The common put terms on the host's own capture of the prop, through the judge a hand item's put runs. The holds are the
// caller's. Wait: a read did not come; Grant: the prop fits; anything else is the reason it does not.
internal::Verdict DecideWorldPut(uint8_t slot, uint32_t containerEid, void* prop, internal::PutInputs& pin, Tally& refusal) {
    internal::Inputs& b = pin.base;
    b.worldReady = true;
    b.served = true;
    b.holdsWait = false;
    b.reach = cr::Reach::Reached;
    switch (cre::Probe(containerEid)) {
        case cre::EditResult::Done:     b.container = internal::Container::Ready; break;
        case cre::EditResult::NotReady: b.container = internal::Container::NotReady; break;
        default:                        b.container = internal::Container::Gone; break;
    }
    if (b.container == internal::Container::Ready) {
        SR::SaveRecord rec;
        if (SR::CaptureOwnRecord(prop, rec)) detail::ReadPutInputs(slot, containerEid, rec, /*slowable=*/false, pin);
        else                                 pin.nameOk = false;   // no record, so no name to file it by
    }
    return internal::DecidePut(pin, &refusal);
}

P::GateRead ReadGates(void* prop, void*& mesh) {
    mesh = ue_wrap::prop::GetStaticMesh(prop);
    if (!mesh) return P::GateRead::NoMesh;
    bool ignoreSave = false, canPickup = false;
    if (!ue_wrap::prop::ReadIgnoreSave(prop, ignoreSave)) return P::GateRead::IgnoreSaveUnread;
    if (ignoreSave) return P::GateRead::Ignored;
    if (!ue_wrap::prop::ReadCanPickup(prop, canPickup)) return P::GateRead::CanPickupUnread;
    return canPickup ? P::GateRead::Pass : P::GateRead::CannotPickup;
}

// The judge: Decide names the next input, this reads it. `d` is the driver, null for a prop the host simulates (the same
// armed, hold, gate and fit terms, no reach). `mesh` gets the prop's static mesh, the component the body is asked about.
P::Decision Judge(void* prop, void* container, uint32_t containerEid, const Driver* d, void*& mesh) {
    P::Inputs in;
    in.needsReach = d && !d->elected;
    const uint8_t slot = d ? d->Slot() : uint8_t{0};
    internal::PutInputs pin;
    internal::Verdict fit = internal::Verdict::Grant;
    Tally specific = Tally::kCount;
    for (;;) {
        P::Decision dec = P::Decide(in);
        switch (dec.need) {
            case P::Need::Nothing:
                if (in.fitKnown && in.fit == P::FitRead::Unread) {
                    dec.why = internal::PutWaitCause(pin);
                } else if (in.fitKnown && in.fit == P::FitRead::Refused) {
                    if (specific != Tally::kCount) detail::Bump(specific);
                    dec.why = internal::VerdictName(fit);
                }
                return dec;
            case P::Need::Armed:
                in.armed = ca::IsArmed(container);
                in.armedKnown = true;
                break;
            case P::Need::Reach: {
                const auto sub = EL::IntentTarget::ForClientIntent(*g_session, slot, cr::kReachUU).Authorize(prop);
                in.reach = sub.outcome == EL::IntentOutcome::Ok          ? P::ReachRead::Ok
                           : sub.outcome == EL::IntentOutcome::OutOfReach ? P::ReachRead::OutOfReach
                                                                          : P::ReachRead::Unread;
                in.reachKnown = true;
                break;
            }
            case P::Need::Hold:
                in.held = cas::Live().HoldOn(containerEid);
                in.holdKnown = true;
                break;
            case P::Need::Gates:
                in.gate = ReadGates(prop, mesh);
                in.gatesKnown = true;
                break;
            case P::Need::Fit:
                fit = DecideWorldPut(slot, containerEid, prop, pin, specific);
                in.fit = fit == internal::Verdict::Grant ? P::FitRead::Fits
                         : fit == internal::Verdict::Wait ? P::FitRead::Unread
                                                          : P::FitRead::Refused;
                in.fitKnown = true;
                break;
        }
    }
}

// The driver's row or drive ends, so the copy simulates and the body finds a simulating component.
void Take(void* prop, const Driver& d) {
    if (d.rowSlot != 0) coop::prop_drive_host::OverrideClientSyncer(prop, "put by overlap");
    if (d.driveSlot >= 0) coop::remote_prop::EndDriveForPut(d.driveSlot);
}

void AddWaiting(void* prop, void* container, uint32_t containerEid, P::WaitKind kind) {
    const uint64_t until = kind == P::WaitKind::Hold ? 0 : coop::net::NowMs() + P::kUnreadWaitMs;
    if (g_waiting.Add(prop, container, containerEid, kind, until) != P::WaitTable<ue_wrap::CachedObjRef>::Added::NewDroppedOldest)
        return;
    detail::Bump(Tally::PutOverlapWaitDropped);
    if (g_dropSaid) return;
    g_dropSaid = true;
    UE_LOGW("container_act: the overlap-put wait table is full (%zu): the oldest wait is dropped, its prop rests unfiled in "
            "the box (put_overlap_wait_dropped; said once a session)", P::kTableRows);
}

void AddLatch(void* prop, uint32_t containerEid, const Driver& d, Tally tally) {
    if (g_latched.Add(prop, containerEid, d.key, tally)) detail::Bump(Tally::PutOverlapLatchEvicted);
}

void Refused(const char* why, Tally tally, uint32_t propEid, uint32_t containerEid, uint8_t slot) {
    detail::Bump(tally);
    detail::NoteRefusal(slot, TallyName(tally));
    UE_LOGI("container_act: overlap put prop=%u container=%u driven by slot %u: refused, %s (%s)",
            static_cast<unsigned>(propEid), static_cast<unsigned>(containerEid), static_cast<unsigned>(slot), why,
            TallyName(tally));
}

// The outcome of a body run on a prop whose pre-check passed: it must have filed the prop, else the judge missed a term.
void NoteOutcome(bool dead, uint32_t propEid, uint32_t containerEid, uint8_t slot, void* mesh) {
    if (dead) {
        detail::Bump(Tally::PutOverlapFiled);
        UE_LOGI("container_act: overlap put prop=%u container=%u driven by slot %u: filed", static_cast<unsigned>(propEid),
                static_cast<unsigned>(containerEid), static_cast<unsigned>(slot));
        return;
    }
    detail::Bump(Tally::PutOverlapGap);
    UE_LOGE("container_act: overlap put prop=%u container=%u driven by slot %u: the body ran and the prop lived "
            "(simulating %d) -- a term the judge did not see; the host keeps the prop", static_cast<unsigned>(propEid),
            static_cast<unsigned>(containerEid), static_cast<unsigned>(slot),
            mesh && E::IsComponentSimulatingPhysics(mesh) ? 1 : 0);
}

// The body, run by us on a prop that is already in the box and has passed the pre-check, and its outcome read at once.
void RunBody(void* prop, void* container, void* mesh, uint32_t propEid, uint32_t containerEid, uint8_t slot) {
    ue_wrap::CachedObjRef ref;
    ref.Set(prop);
    ++g_applyDepth;
    const bool ran = cb::CallPutObjectInOverlap(container, prop, mesh);
    --g_applyDepth;
    if (!ran) UE_LOGW("container_act: overlap put prop=%u container=%u: the body did not run", static_cast<unsigned>(propEid),
                      static_cast<unsigned>(containerEid));
    NoteOutcome(!ref.Alive(), propEid, containerEid, slot, mesh);
}

// A waiting put judged again. True: it stays in the table, as `w` now says.
bool Rejudge(WaitRow& w) {
    void* const prop = w.prop.Get();
    void* const container = EL::LivePropActor(w.containerEid);
    if (!prop || !container) {
        detail::Bump(Tally::PutOverlapLeft);
        UE_LOGI("container_act: a waiting overlap put on container %u is dropped: the prop or the container is gone",
                static_cast<unsigned>(w.containerEid));
        return false;
    }
    if (!cb::OverlapsActor(container, prop)) {
        detail::Bump(Tally::PutOverlapLeft);
        UE_LOGI("container_act: a waiting overlap put on container %u is dropped: the prop left the box (put_overlap_left)",
                static_cast<unsigned>(w.containerEid));
        return false;
    }
    const uint32_t propEid = EL::Registry::Get().EidForActor(prop);
    if (propEid == EL::kInvalidId) {   // still no id: the bind has not come, and the time bound ends it
        w.ready = false;
        return true;
    }
    if (cas::Live().HoldOn(w.containerEid)) {   // another hold stands on the container
        w.kind = P::WaitKind::Hold;
        w.untilMs = 0;
        w.ready = false;
        return true;
    }
    Driver d;
    const bool driven = DriverOf(prop, propEid, d);
    void* mesh = nullptr;
    if (!driven) {
        // Nobody drives it now. A prop that still overlaps and simulates is the host's own put: no second BeginOverlap
        // will fire for it, so it is pre-checked as a driven one is and the body runs here.
        void* const body = ue_wrap::prop::GetStaticMesh(prop);
        if (!body || !E::IsComponentSimulatingPhysics(body)) {
            detail::Bump(Tally::PutOverlapLeft);
            UE_LOGI("container_act: a waiting overlap put on container %u is dropped: the prop is no longer driven and does "
                    "not simulate", static_cast<unsigned>(w.containerEid));
            return false;
        }
    }
    const P::Decision r = Judge(prop, container, w.containerEid, driven ? &d : nullptr, mesh);
    const uint8_t slot = driven ? d.Slot() : uint8_t{0};
    if (r.judged == P::Judged::Refuse) {
        Refused(r.why, r.tally, propEid, w.containerEid, slot);
        if (driven) AddLatch(prop, w.containerEid, d, r.tally);
        return false;
    }
    if (r.judged == P::Judged::Wait) {   // a hold or a read: it waits for that edge, in the time the first wait had
        w.kind = r.transient ? P::WaitKind::Read : P::WaitKind::Hold;
        if (r.transient && w.untilMs == 0) w.untilMs = coop::net::NowMs() + P::kUnreadWaitMs;
        if (!r.transient) w.untilMs = 0;
        w.ready = false;
        return true;
    }
    if (driven) Take(prop, d);
    RunBody(prop, container, mesh, propEid, w.containerEid, slot);
    return false;
}

void RejudgeAll(std::vector<WaitRow> rows) {
    for (WaitRow& w : rows)
        if (Rejudge(w)) g_waiting.Readd(std::move(w));
}

// A container ends its play: the puts waiting on it go with it.
void OnEndPlay(void* actor, ue_wrap::actor_end_play::Reason) {
    if (g_waiting.Empty()) return;
    for (const WaitRow& w : g_waiting.TakeContainerActor(actor)) {
        detail::Bump(Tally::PutOverlapLeft);
        UE_LOGI("container_act: a waiting overlap put on container %u is dropped: the container is destroyed",
                static_cast<unsigned>(w.containerEid));
    }
}

bool IsHost() { return g_session && g_session->role() == coop::net::Role::Host; }

}  // namespace

void Install(coop::net::Session* session) {
    g_session = session;
    if (IsHost() && !ue_wrap::actor_end_play::AddSink(&OnEndPlay)) UE_LOGW("container_act: the end-of-play sink table is full");
}

sg::Verdict Pre(const sg::Call& c) {
    if (g_applyDepth > 0 || !IsHost()) return sg::Verdict::Run;
    UE_ASSERT_GAME_THREAD("container_act_host::overlap::Pre");
    static R::ParamOffset sActor{L"actor"};
    static R::ParamOffset sComp{L"component"};
    void* actor = nullptr;
    void* comp = nullptr;
    if (!ReadParam(sActor, c, actor) || !ReadParam(sComp, c, comp)) {
        if (!g_unreadSaid) {
            g_unreadSaid = true;
            UE_LOGW("container_act: the overlap put's actor or component does not read -- the body runs natively");
        }
        return sg::Verdict::Run;
    }
    // The overlap comes from a delayed continuation: the actor and the component may be gone by now.
    if (!actor || !comp || actor == c.object || !R::IsLive(actor) || !R::IsLive(comp)) return sg::Verdict::Run;
    if (!ci::IsContainer(c.object)) return sg::Verdict::Run;
    void* const inv = ci::InventoryOf(c.object);
    if (!inv || !ci::IsWorldInventory(inv)) return sg::Verdict::Run;
    const uint32_t containerEid = EL::Registry::Get().EidForActor(c.object);
    if (containerEid == EL::kInvalidId) return sg::Verdict::Run;
    const uint32_t propEid = EL::Registry::Get().EidForActor(actor);
    if (propEid == EL::kInvalidId) {
        // A prop with no id yet: a client's thrown copy before its bind, or a prop the lane does not share. One that
        // simulates is the BP's own; one that does not would be refused by the BP's first gate anyway, so it waits for its
        // bind (the call list of RegisterPropMirror) instead, at most kUnreadWaitMs, and then is judged.
        if (!ue_wrap::prop::IsDescendantOfProp(actor) || E::IsComponentSimulatingPhysics(comp)) return sg::Verdict::Run;
        AddWaiting(actor, c.object, containerEid, P::WaitKind::Bind);
        UE_LOGI("container_act: overlap put on container %u: the prop has no id yet, it waits for its bind",
                static_cast<unsigned>(containerEid));
        return sg::Verdict::Cancel;
    }
    Driver d;
    const bool driven = DriverOf(actor, propEid, d);
    const bool held = cas::Live().HoldOn(containerEid);
    // A prop nobody drives, on a container no client holds, is the BP's own: no engine call was spent on it.
    if (!driven && !held) return sg::Verdict::Run;
    if (E::IsComponentSimulatingPhysics(comp)) {
        if (!held) return sg::Verdict::Run;   // a simulating body is the host's own put, which the BP judges
        // The host's own put on a container a client holds waits, as HostPre makes R2 and R3 wait: the restore of the
        // client's escrow could overflow the container.
        AddWaiting(actor, c.object, containerEid, P::WaitKind::Hold);
        detail::Bump(Tally::PutOverlapHeld);
        UE_LOGI("container_act: overlap put prop=%u container=%u (simulating): waits, a client holds the container",
                static_cast<unsigned>(propEid), static_cast<unsigned>(containerEid));
        return sg::Verdict::Cancel;
    }
    if (!driven) return sg::Verdict::Run;   // not simulating and nobody drives it: the BP's own refusal
    Tally latched = Tally::kCount;
    if (g_latched.Hit(actor, containerEid, d.key, latched)) {   // refused already under this drive: counted, not judged again
        detail::Bump(latched);
        return sg::Verdict::Cancel;
    }
    void* mesh = nullptr;
    const P::Decision r = Judge(actor, c.object, containerEid, &d, mesh);
    if (r.judged == P::Judged::Refuse) {
        Refused(r.why, r.tally, propEid, containerEid, d.Slot());
        AddLatch(actor, containerEid, d, r.tally);
        return sg::Verdict::Cancel;
    }
    if (r.judged == P::Judged::Wait) {
        AddWaiting(actor, c.object, containerEid, r.transient ? P::WaitKind::Read : P::WaitKind::Hold);
        if (!r.transient) detail::Bump(r.tally);
        UE_LOGI("container_act: overlap put prop=%u container=%u driven by slot %u: waits, %s", static_cast<unsigned>(propEid),
                static_cast<unsigned>(containerEid), static_cast<unsigned>(d.Slot()), r.why);
        return sg::Verdict::Cancel;
    }
    Take(actor, d);
    g_latched.ClearContainer(containerEid);
    g_pending.prop.Set(actor);
    g_pending.propEid = propEid;
    g_pending.containerEid = containerEid;
    g_pending.slot = d.Slot();
    return sg::Verdict::Run;
}

void Post(const sg::Call& c) {
    if (!g_pending.prop.Raw()) return;
    static R::ParamOffset sActor{L"actor"};
    void* actor = nullptr;
    if (!ReadParam(sActor, c, actor) || actor != g_pending.prop.Raw()) return;   // another call's end
    const Pending done = g_pending;
    g_pending = Pending{};
    void* const mesh = done.prop.Alive() ? ue_wrap::prop::GetStaticMesh(done.prop.Get()) : nullptr;
    NoteOutcome(!done.prop.Alive(), done.propEid, done.containerEid, done.slot, mesh);
}

void Tick() {
    if (!g_waiting.HasEdgeRows()) return;   // the tick pays a scan of at most kTableRows rows, and only with such a row
    UE_ASSERT_GAME_THREAD("container_act_host::overlap::Tick");
    for (const WaitRow& w : g_waiting.TakeExpired(coop::net::NowMs())) {
        detail::Bump(Tally::PutOverlapUnread);
        detail::NoteRefusal(0, TallyName(Tally::PutOverlapUnread));
        UE_LOGI("container_act: a waiting overlap put on container %u is refused: its %s did not come in %llu ms (%s)",
                static_cast<unsigned>(w.containerEid), w.kind == P::WaitKind::Bind ? "bind" : "read",
                static_cast<unsigned long long>(P::kUnreadWaitMs), TallyName(Tally::PutOverlapUnread));
    }
    RejudgeAll(g_waiting.TakeReady());
}

void OnBound(void* actor) {
    if (!g_waiting.Empty()) g_waiting.MarkBound(actor);
}

void OnContainerChanged(uint32_t eid) {
    if (!IsHost()) return;
    g_latched.ClearContainer(eid);
    g_waiting.MarkChanged(eid);
}

void OnContainerFree(uint32_t eid) {
    if (!IsHost()) return;
    g_latched.ClearContainer(eid);
    if (g_waiting.Empty()) return;
    UE_ASSERT_GAME_THREAD("container_act_host::overlap::OnContainerFree");
    RejudgeAll(g_waiting.TakeHold(eid));
}

void Reset() {
    g_waiting.Clear();
    g_latched.Clear();
    g_pending = Pending{};
    g_applyDepth = 0;
    g_unreadSaid = false;
    g_dropSaid = false;
}

}  // namespace coop::props::container_act_host::overlap
