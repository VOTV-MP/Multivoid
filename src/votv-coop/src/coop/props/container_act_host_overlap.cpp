// coop/props/container_act_host_overlap.cpp -- the host judges a put at its own overlap: a shared prop that a CLIENT drives
// (an elected throw, a claimed push, a held pose) enters a shared world container's box on the host. The host's copy is
// kinematic there, so the container's own putObjectIn_overlap refuses it at its first gate; this PRE on that function judges
// the put instead. No client message asks for it: the client's copy of the call is cancelled (container_act_put). MTA tests
// the position its server applied (CObjectSync.cpp:220, CUnoccupiedVehicleSync.cpp:456); here the engine's overlap step of
// the parked copy, which moves every frame, IS that test at our apply. Source's BumpWeapon is the server's own touch.
//
// The judge, in order, each refusal counted with its line and the BP call cancelled: the box is armed; a HELD or CLAIMED
// driver is within reach of the prop (an ELECTED throw has none: its rows are step-tested and flight-bounded); no client
// holds the container (else the put waits); the body's silent gates pass; the prop fits. Then the driver's row or drive
// ends, the copy simulates, the PRE returns Run and the game's own body files the prop; the POST reads its death, and a
// prop alive after the body is a term the judge did not see, counted and loud. A prop nobody drives is the BP's own, but a
// put on a held container waits whoever's it is (HostPre's rule for R2 and R3). A waiting put is judged again when the hold
// ends, or at the next tick for a read that did not come (a few times); a prop still in the box with no driver is the
// host's own put by then, and the body runs on it, since a prop already overlapping fires no second BeginOverlap.

#include "coop/props/container_act_host.h"
#include "coop/props/container_act_host_internal.h"

#include "coop/element/intent_authority.h"
#include "coop/element/registry.h"
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

// The tables hold at most this many entries each. A design value, not a measurement: a hold ends within
// container_act_state::kHoldDeadlineMs (30 s), and a handful of props can reach held containers in that time. Past it the
// oldest wait is dropped, counted (put_overlap_wait_dropped) and said once; its prop then rests unfiled in the box, as one
// whose put was refused does.
constexpr size_t kMaxEntries = 8;
// A read that did not come is tried again at the next tick this many times, then the put is refused as unread.
constexpr uint8_t kReadTries = 5;

coop::net::Session* g_session = nullptr;
bool g_applying = false;     // our own call of the body is running: the PRE stands aside
bool g_unreadSaid = false;   // the parameters did not read: said once a session
bool g_dropSaid = false;     // a wait was dropped for a full table: said once a session

// Who drives the prop: a client's row on it (elected or claimed) and/or a held pose stream (a remote drive).
struct Driver {
    uint8_t rowSlot = 0;     // 0: no client row
    int driveSlot = -1;      // -1: no remote drive
    bool elected = false;    // the row is an elected throw
    uint8_t Slot() const { return rowSlot != 0 ? rowSlot : static_cast<uint8_t>(driveSlot); }
};

bool DriverOf(void* prop, uint32_t propEid, Driver& d) {
    d.rowSlot = coop::prop_drive_host::ClientSyncerOf(propEid);
    d.driveSlot = coop::remote_prop::DrivingSlotOf(prop);
    d.elected = d.rowSlot != 0 && coop::prop_drive_host::ClientRowElected(propEid);
    return d.rowSlot != 0 || d.driveSlot >= 0;
}

// A put that cannot be judged yet. onHold: it waits on a client's hold and is judged again when the hold ends; else a read
// did not come and it is judged at the next tick, `tries` times at most.
struct Waiting {
    ue_wrap::CachedObjRef prop;
    uint32_t containerEid = 0;
    bool onHold = true;
    uint8_t tries = 0;
};
std::vector<Waiting> g_waiting;

// A refusal that stands for as long as the drive does: the same prop entering the same container again under the same
// driver is refused again without a second judgement. Position and read dependent refusals (reach, unread) are not kept.
struct Latch {
    ue_wrap::CachedObjRef prop;
    uint32_t containerEid = 0;
    uint8_t rowSlot = 0;
    int driveSlot = -1;
    Tally tally = Tally::kCount;
};
std::vector<Latch> g_latched;

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

enum class Judged : uint8_t { Pass, Wait, Refuse };
struct Result {
    Judged judged = Judged::Refuse;
    Tally tally = Tally::kCount;   // the counter of a refusal or a hold
    const char* why = "";
    bool transient = false;        // a Wait on a read that did not come, not on a hold
};

Result Refuse(Tally t, const char* why) { return Result{Judged::Refuse, t, why, false}; }

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

// The judge. `mesh` gets the prop's static mesh, the component the body is asked about. The cheap terms come first; the
// record capture and the fit, which read the whole container, come last.
Result Judge(void* prop, void* container, uint32_t containerEid, const Driver& d, void*& mesh) {
    if (!ca::IsArmed(container)) return Refuse(Tally::PutOverlapUnarmed, "the container's box is not armed");
    if (!d.elected) {
        const auto sub = EL::IntentTarget::ForClientIntent(*g_session, d.Slot(), cr::kReachUU).Authorize(prop);
        if (sub.outcome == EL::IntentOutcome::OutOfReach) return Refuse(Tally::PutOverlapReach, "the driver is out of reach");
        if (sub.outcome != EL::IntentOutcome::Ok) return Refuse(Tally::PutOverlapUnread, "the driver's reach did not read");
    }
    if (cas::Live().HoldOn(containerEid)) return Result{Judged::Wait, Tally::PutOverlapHeld, "a client holds the container", false};
    // The body's own silent gates: an answer that does not resolve is no pass (a host prop is a prop_C, which has both).
    mesh = ue_wrap::prop::GetStaticMesh(prop);
    bool ignoreSave = false, canPickup = false;
    if (!mesh) return Refuse(Tally::PutOverlapGate, "the prop has no mesh");
    if (!ue_wrap::prop::ReadIgnoreSave(prop, ignoreSave)) return Refuse(Tally::PutOverlapGate, "ignoreSave does not read");
    if (ignoreSave) return Refuse(Tally::PutOverlapGate, "the prop is not saved");
    if (!ue_wrap::prop::ReadCanPickup(prop, canPickup)) return Refuse(Tally::PutOverlapGate, "canPickup does not read");
    if (!canPickup) return Refuse(Tally::PutOverlapGate, "the prop cannot be picked up");
    internal::PutInputs pin;
    Tally specific = Tally::kCount;
    const internal::Verdict fit = DecideWorldPut(d.Slot(), containerEid, prop, pin, specific);
    if (fit == internal::Verdict::Wait) return Result{Judged::Wait, Tally::PutOverlapUnread, internal::PutWaitCause(pin), true};
    if (fit != internal::Verdict::Grant) {
        if (specific != Tally::kCount) detail::Bump(specific);
        return Refuse(Tally::PutOverlapFit, internal::VerdictName(fit));
    }
    return Result{Judged::Pass, Tally::kCount, "", false};
}

// The driver's row or drive ends, so the copy simulates and the body finds a simulating component.
void Take(void* prop, const Driver& d) {
    if (d.rowSlot != 0) coop::prop_drive_host::OverrideClientSyncer(prop, "put by overlap");
    if (d.driveSlot >= 0) coop::remote_prop::EndDriveForPut(d.driveSlot);
}

void AddWaiting(void* prop, uint32_t containerEid, bool onHold) {
    for (Waiting& w : g_waiting) {
        if (w.containerEid != containerEid || w.prop.Raw() != prop) continue;
        w.onHold = w.onHold && onHold;   // a read still to come keeps it on the tick
        return;
    }
    if (g_waiting.size() >= kMaxEntries) {
        detail::Bump(Tally::PutOverlapWaitDropped);
        if (!g_dropSaid) {
            g_dropSaid = true;
            UE_LOGW("container_act: the overlap-put wait table is full (%zu): the oldest wait (container %u) is dropped, "
                    "its prop rests unfiled in the box (put_overlap_wait_dropped; said once a session)", kMaxEntries,
                    static_cast<unsigned>(g_waiting.front().containerEid));
        }
        g_waiting.erase(g_waiting.begin());
    }
    g_waiting.emplace_back();
    g_waiting.back().prop.Set(prop);
    g_waiting.back().containerEid = containerEid;
    g_waiting.back().onHold = onHold;
}

bool LatchHit(void* prop, uint32_t containerEid, const Driver& d, Tally& tally) {
    for (const Latch& l : g_latched) {
        if (l.containerEid != containerEid || l.rowSlot != d.rowSlot || l.driveSlot != d.driveSlot || !l.prop.Is(prop)) continue;
        tally = l.tally;
        return true;
    }
    return false;
}

void Unlatch(uint32_t containerEid) {
    for (size_t i = 0; i < g_latched.size();) {
        if (g_latched[i].containerEid == containerEid) g_latched.erase(g_latched.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
}

void AddLatch(void* prop, uint32_t containerEid, const Driver& d, Tally tally) {
    if (tally == Tally::PutOverlapReach || tally == Tally::PutOverlapUnread) return;
    for (size_t i = 0; i < g_latched.size();) {   // an older latch of the pair, or of a prop that is gone, goes
        if ((g_latched[i].containerEid == containerEid && g_latched[i].prop.Is(prop)) || !g_latched[i].prop.Alive())
            g_latched.erase(g_latched.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
    if (g_latched.size() >= kMaxEntries) g_latched.erase(g_latched.begin());
    g_latched.emplace_back();
    Latch& l = g_latched.back();
    l.prop.Set(prop);
    l.containerEid = containerEid;
    l.rowSlot = d.rowSlot;
    l.driveSlot = d.driveSlot;
    l.tally = tally;
}

void Refused(const Result& r, uint32_t propEid, uint32_t containerEid, const Driver& d) {
    detail::Bump(r.tally);
    detail::NoteRefusal(d.Slot(), TallyName(r.tally));
    UE_LOGI("container_act: overlap put prop=%u container=%u driven by slot %u: refused, %s (%s)",
            static_cast<unsigned>(propEid), static_cast<unsigned>(containerEid), static_cast<unsigned>(d.Slot()), r.why,
            TallyName(r.tally));
}

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

// The body, run by us on a prop that is already in the box, and its outcome read at once.
void RunBody(void* prop, void* container, void* mesh, uint32_t propEid, uint32_t containerEid, uint8_t slot) {
    ue_wrap::CachedObjRef ref;
    ref.Set(prop);
    g_applying = true;
    const bool ran = cb::CallPutObjectInOverlap(container, prop, mesh);
    g_applying = false;
    if (!ran) UE_LOGW("container_act: overlap put prop=%u container=%u: the body did not run", static_cast<unsigned>(propEid),
                      static_cast<unsigned>(containerEid));
    NoteOutcome(!ref.Alive(), propEid, containerEid, slot, mesh);
}

// A waiting put judged again. True: it stays in the table.
bool Rejudge(Waiting& w) {
    void* const prop = w.prop.Get();
    void* const container = EL::LivePropActor(w.containerEid);
    if (!prop || !container) {
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
    Driver d;
    if (propEid == EL::kInvalidId || !DriverOf(prop, propEid, d)) {
        // Nobody drives it now. A prop that still overlaps and simulates is the host's own put: no second BeginOverlap
        // will fire for it, so the body runs here, once the container is free.
        if (cas::Live().HoldOn(w.containerEid)) {
            w.onHold = true;
            return true;
        }
        void* const mesh = ue_wrap::prop::GetStaticMesh(prop);
        if (!mesh || !E::IsComponentSimulatingPhysics(mesh)) {
            UE_LOGI("container_act: a waiting overlap put on container %u is dropped: the prop is no longer driven and does "
                    "not simulate", static_cast<unsigned>(w.containerEid));
            return false;
        }
        RunBody(prop, container, mesh, propEid, w.containerEid, 0);
        return false;
    }
    void* mesh = nullptr;
    const Result r = Judge(prop, container, w.containerEid, d, mesh);
    if (r.judged == Judged::Refuse) {
        Refused(r, propEid, w.containerEid, d);
        AddLatch(prop, w.containerEid, d, r.tally);
        return false;
    }
    if (r.judged == Judged::Wait) {
        w.onHold = !r.transient;
        if (!r.transient || ++w.tries < kReadTries) return true;
        Refused(Refuse(Tally::PutOverlapUnread, r.why), propEid, w.containerEid, d);
        return false;
    }
    Take(prop, d);
    RunBody(prop, container, mesh, propEid, w.containerEid, d.Slot());
    return false;
}

// Every waiting put that `pick` names is judged again; the ones that still wait go back.
template <class Pick>
void RejudgeWhere(Pick pick) {
    std::vector<Waiting> mine;
    for (size_t i = 0; i < g_waiting.size();) {
        if (!pick(g_waiting[i])) {
            ++i;
            continue;
        }
        mine.push_back(std::move(g_waiting[i]));
        g_waiting.erase(g_waiting.begin() + static_cast<std::ptrdiff_t>(i));
    }
    for (Waiting& w : mine)
        if (Rejudge(w)) g_waiting.push_back(std::move(w));
}

}  // namespace

void Install(coop::net::Session* session) { g_session = session; }

sg::Verdict Pre(const sg::Call& c) {
    if (g_applying || !g_session || g_session->role() != coop::net::Role::Host) return sg::Verdict::Run;
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
    Driver d;
    const bool driven = propEid != EL::kInvalidId && DriverOf(actor, propEid, d);
    const bool held = cas::Live().HoldOn(containerEid);
    // A prop nobody drives, on a container no client holds, is the BP's own: no engine call was spent on it.
    if (!driven && !held) return sg::Verdict::Run;
    if (E::IsComponentSimulatingPhysics(comp)) {
        if (!held) return sg::Verdict::Run;   // a simulating body is the host's own put, which the BP judges
        // The host's own put on a container a client holds waits, as HostPre makes R2 and R3 wait: the restore of the
        // client's escrow could overflow the container.
        AddWaiting(actor, containerEid, /*onHold=*/true);
        detail::Bump(Tally::PutOverlapHeld);
        UE_LOGI("container_act: overlap put prop=%u container=%u (simulating): waits, a client holds the container",
                static_cast<unsigned>(propEid), static_cast<unsigned>(containerEid));
        return sg::Verdict::Cancel;
    }
    if (!driven) return sg::Verdict::Run;   // not simulating and nobody drives it: the BP's own refusal
    Tally latched = Tally::kCount;
    if (LatchHit(actor, containerEid, d, latched)) {   // refused already under this drive: counted, not judged again
        detail::Bump(latched);
        return sg::Verdict::Cancel;
    }
    void* mesh = nullptr;
    const Result r = Judge(actor, c.object, containerEid, d, mesh);
    if (r.judged == Judged::Refuse) {
        Refused(r, propEid, containerEid, d);
        AddLatch(actor, containerEid, d, r.tally);
        return sg::Verdict::Cancel;
    }
    if (r.judged == Judged::Wait) {
        AddWaiting(actor, containerEid, /*onHold=*/!r.transient);
        if (!r.transient) detail::Bump(r.tally);
        UE_LOGI("container_act: overlap put prop=%u container=%u driven by slot %u: waits, %s", static_cast<unsigned>(propEid),
                static_cast<unsigned>(containerEid), static_cast<unsigned>(d.Slot()), r.why);
        return sg::Verdict::Cancel;
    }
    Take(actor, d);
    Unlatch(containerEid);
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
    if (g_waiting.empty()) return;   // the tick pays a size test; a table only a hold waits on pays nothing more
    bool anyRead = false;
    for (const Waiting& w : g_waiting) anyRead = anyRead || !w.onHold;
    if (!anyRead) return;
    UE_ASSERT_GAME_THREAD("container_act_host::overlap::Tick");
    RejudgeWhere([](const Waiting& w) { return !w.onHold; });
}

void OnContainerFree(uint32_t eid) {
    if (!g_session) return;
    Unlatch(eid);
    if (g_waiting.empty()) return;
    UE_ASSERT_GAME_THREAD("container_act_host::overlap::OnContainerFree");
    RejudgeWhere([eid](const Waiting& w) { return w.containerEid == eid; });
}

void Reset() {
    g_waiting.clear();
    g_latched.clear();
    g_pending = Pending{};
    g_applying = false;
    g_unreadSaid = false;
    g_dropSaid = false;
}

}  // namespace coop::props::container_act_host::overlap
