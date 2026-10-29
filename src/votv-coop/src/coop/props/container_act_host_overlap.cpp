// coop/props/container_act_host_overlap.cpp -- the host judges a put at its own overlap: a shared prop that a CLIENT drives
// (an elected throw, a claimed push, a held pose) enters a shared world container's box on the host. The host's copy is
// kinematic there, so the container's own putObjectIn_overlap refuses it at its first gate (the component does not
// simulate); this is a PRE on that function that judges the put instead, as the host's authority over its world. No client
// message asks for it: the client's own copy of the call is cancelled (container_act_put), and the host tests the position
// it applied, as MTA's server hit-tests the object it syncs (CObjectSync.cpp:220, CUnoccupiedVehicleSync.cpp:456) and
// Source's BumpWeapon is the server's own touch (player.cpp:6754-6790).
//
// The judge, in order, each refusal counted with its line and the BP call cancelled: the box is armed; a HELD or CLAIMED
// driver's puppet is within reach of the prop (an ELECTED throw has none, its rows are step-tested and flight-bounded);
// no client's hold stands on the container (then the put waits, and is judged again when the hold ends while the prop
// still overlaps); the prop fits (the common put terms on the host's capture of the prop); the body's silent gates pass.
// Then the apply: the driver's row or drive ends, so the copy simulates, and the PRE returns Run: the game's own body
// files the prop, destroys it and arms the box. Success is the prop's death, read in the POST; a prop alive after the body
// is a term the judge did not see, counted and loud. A prop nobody drives, or one that simulates, is the BP's own.

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

// Held puts: a put that waits on a hold is judged again when the hold ends. Past this many the oldest is dropped.
constexpr size_t kMaxWaiting = 8;

coop::net::Session* g_session = nullptr;
bool g_applying = false;     // our own call of the body is running: the PRE stands aside
bool g_unreadSaid = false;   // the parameters did not read: said once a session

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

// A put waiting on a hold.
struct Waiting {
    ue_wrap::CachedObjRef prop;
    uint32_t containerEid = 0;
};
std::vector<Waiting> g_waiting;

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
    Tally tally = Tally::kCount;   // the counter of a refusal
    const char* why = "";
};

Result Refuse(Tally t, const char* why) { return Result{Judged::Refuse, t, why}; }

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

// The judge. `mesh` gets the prop's static mesh, the component the body is asked about.
Result Judge(void* prop, void* container, uint32_t containerEid, const Driver& d, void*& mesh) {
    if (!ca::IsArmed(container)) return Refuse(Tally::PutOverlapUnarmed, "the container's box is not armed");
    if (!d.elected) {
        const auto sub = EL::IntentTarget::ForClientIntent(*g_session, d.Slot(), cr::kReachUU).Authorize(prop);
        if (sub.outcome == EL::IntentOutcome::OutOfReach) return Refuse(Tally::PutOverlapReach, "the driver is out of reach");
        if (sub.outcome != EL::IntentOutcome::Ok) return Refuse(Tally::PutOverlapUnread, "the driver's reach did not read");
    }
    if (cas::Live().HoldOn(containerEid)) return Result{Judged::Wait, Tally::PutOverlapHeld, "a client holds the container"};
    internal::PutInputs pin;
    Tally specific = Tally::kCount;
    const internal::Verdict fit = DecideWorldPut(d.Slot(), containerEid, prop, pin, specific);
    if (fit == internal::Verdict::Wait) return Refuse(Tally::PutOverlapUnread, internal::PutWaitCause(pin));
    if (fit != internal::Verdict::Grant) {
        if (specific != Tally::kCount) detail::Bump(specific);
        return Refuse(Tally::PutOverlapFit, internal::VerdictName(fit));
    }
    // The body's own silent gates: an unresolved answer is no pass.
    bool ignoreSave = true, canPickup = false;
    mesh = ue_wrap::prop::GetStaticMesh(prop);
    if (!mesh || !ue_wrap::prop::ReadIgnoreSave(prop, ignoreSave) || ignoreSave ||
        !ue_wrap::prop::ReadCanPickup(prop, canPickup) || !canPickup)
        return Refuse(Tally::PutOverlapGate, ignoreSave ? "the prop is not saved" : "the prop cannot be picked up");
    return Result{Judged::Pass, Tally::kCount, ""};
}

// The driver's row or drive ends, so the copy simulates and the body finds a simulating component.
void Take(void* prop, const Driver& d) {
    if (d.rowSlot != 0) coop::prop_drive_host::OverrideClientSyncer(prop, "put by overlap");
    if (d.driveSlot >= 0) coop::remote_prop::EndDriveForPut(d.driveSlot);
}

void AddWaiting(void* prop, uint32_t containerEid) {
    for (const Waiting& w : g_waiting)
        if (w.containerEid == containerEid && w.prop.Raw() == prop) return;
    if (g_waiting.size() >= kMaxWaiting) {
        UE_LOGW("container_act: the held-put table is full: the oldest wait (container %u) is dropped",
                static_cast<unsigned>(g_waiting.front().containerEid));
        g_waiting.erase(g_waiting.begin());
    }
    g_waiting.emplace_back();
    g_waiting.back().prop.Set(prop);
    g_waiting.back().containerEid = containerEid;
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

}  // namespace

void Install(coop::net::Session* session) { g_session = session; }

sg::Verdict Pre(const sg::Call& c) {
    if (g_applying || !g_session || g_session->role() != coop::net::Role::Host) return sg::Verdict::Run;
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
    // The overlap comes from a delayed continuation: the actor and the component may be gone by now. A body that
    // simulates is the host's own put, which the BP judges as it always did.
    if (!actor || !comp || actor == c.object || !R::IsLive(actor) || !R::IsLive(comp) ||
        E::IsComponentSimulatingPhysics(comp))
        return sg::Verdict::Run;
    if (!ci::IsContainer(c.object)) return sg::Verdict::Run;
    void* const inv = ci::InventoryOf(c.object);
    if (!inv || !ci::IsWorldInventory(inv)) return sg::Verdict::Run;
    const uint32_t containerEid = EL::Registry::Get().EidForActor(c.object);
    const uint32_t propEid = EL::Registry::Get().EidForActor(actor);
    Driver d;
    if (containerEid == EL::kInvalidId || propEid == EL::kInvalidId || !DriverOf(actor, propEid, d))
        return sg::Verdict::Run;   // a prop nobody drives: the BP's own refusal
    void* mesh = nullptr;
    const Result r = Judge(actor, c.object, containerEid, d, mesh);
    if (r.judged == Judged::Refuse) {
        Refused(r, propEid, containerEid, d);
        return sg::Verdict::Cancel;
    }
    if (r.judged == Judged::Wait) {
        AddWaiting(actor, containerEid);
        detail::Bump(r.tally);
        UE_LOGI("container_act: overlap put prop=%u container=%u driven by slot %u: waits, %s", static_cast<unsigned>(propEid),
                static_cast<unsigned>(containerEid), static_cast<unsigned>(d.Slot()), r.why);
        return sg::Verdict::Cancel;
    }
    Take(actor, d);
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

void OnContainerFree(uint32_t eid) {
    if (g_waiting.empty() || !g_session) return;
    UE_ASSERT_GAME_THREAD("container_act_host::overlap::OnContainerFree");
    std::vector<Waiting> mine;
    for (size_t i = 0; i < g_waiting.size();) {
        if (g_waiting[i].containerEid != eid) {
            ++i;
            continue;
        }
        mine.push_back(std::move(g_waiting[i]));
        g_waiting.erase(g_waiting.begin() + static_cast<std::ptrdiff_t>(i));
    }
    for (const Waiting& w : mine) {
        void* const prop = w.prop.Get();
        void* const container = EL::LivePropActor(eid);
        const uint32_t propEid = prop ? EL::Registry::Get().EidForActor(prop) : EL::kInvalidId;
        Driver d;
        if (!prop || !container || propEid == EL::kInvalidId || !cb::OverlapsActor(container, prop) ||
            !DriverOf(prop, propEid, d)) {
            UE_LOGI("container_act: a waiting overlap put on container %u is dropped: the prop is gone, out of the box or "
                    "no longer driven by a client", static_cast<unsigned>(eid));
            continue;
        }
        void* mesh = nullptr;
        const Result r = Judge(prop, container, eid, d, mesh);
        if (r.judged == Judged::Refuse) {
            Refused(r, propEid, eid, d);
            continue;
        }
        if (r.judged == Judged::Wait) {
            AddWaiting(prop, eid);
            continue;
        }
        Take(prop, d);
        ue_wrap::CachedObjRef ref;
        ref.Set(prop);
        g_applying = true;
        const bool ran = cb::CallPutObjectInOverlap(container, prop, mesh);
        g_applying = false;
        if (!ran) UE_LOGW("container_act: overlap put prop=%u container=%u: the body did not run", static_cast<unsigned>(propEid),
                          static_cast<unsigned>(eid));
        NoteOutcome(!ref.Alive(), propEid, eid, d.Slot(), mesh);
    }
}

void Reset() {
    g_waiting.clear();
    g_pending = Pending{};
    g_applying = false;
    g_unreadSaid = false;
}

}  // namespace coop::props::container_act_host::overlap
