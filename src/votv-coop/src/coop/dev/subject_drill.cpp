// coop/dev/subject_drill.cpp -- see coop/dev/subject_drill.h.

#include "coop/dev/subject_drill.h"

#include "coop/config/config.h"
#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/director/director.h"
#include "coop/dev/director/routes.h"  // ReachableStandpoints: the port is off the NavMesh
#include "coop/element/registry.h"
#include "coop/interactables/mirror_slot_entry.h"  // Refused: the host's port refused a carried drive
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_snapshot.h"  // IsBracketClosed: a joiner's snapshot is over
#include "coop/props/remote_prop.h"    // IsActorUnderAnyDrive: a client's hold, seen on the host
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"     // HasAnnouncedWorldReady

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflected_offset.h"  // MainPlayer_grabLen
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/desk/drive_chain.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"  // GetComponentLocation
#include "ue_wrap/engine/engine_pawn.h"       // the aim: the camera, the controller's rotation

#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace coop::dev::subject_drill {
namespace {

namespace DC = ue_wrap::drive_chain;
namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
namespace D  = coop::director;
using Clock = std::chrono::steady_clock;

enum class Arm : uint8_t { Off, Insert, Own, Carry };
enum class HostStep : uint8_t { Ready, Named, WaitHold, EmptyPort, WatchCarry, Done };
enum class ClientStep : uint8_t { Ready, Walk, Grab, Hold, Seated, CarryWalk, CarryAim, Done };

constexpr float kReachCm = 150.f;                         // the use key's reach, stood off a little
constexpr int   kWalkDeadlineS = 120;
constexpr auto  kStepBound = std::chrono::seconds(60);    // the element lane names the drives; the insert arrives
constexpr auto  kJoinBound = std::chrono::seconds(600);   // a fresh client's boot, load, join and walk
constexpr float kStandRingCm = 100.f;                     // the carry stands the grab's default length off the port
constexpr float kStandReachCm = 60.f;
constexpr int   kStandpoints = 8;
constexpr int   kStandWalkDeadlineS = 60;

HostStep   g_host = HostStep::Ready;
ClientStep g_client = ClientStep::Ready;
Clock::time_point g_stepAt{};
ue_wrap::CachedObjRef g_drives[2];                // the host's two (insert) or one (own) drives
ue_wrap::CachedObjRef g_held;                     // the client's grabbed drive
ue_wrap::CachedObjRef g_carried;                  // the host's view of the drive the client carries
std::shared_ptr<D::BackgroundWalk> g_walk;
std::vector<ue_wrap::FVector> g_stands;           // the carry's reachable standpoints about the port, in walk order
size_t g_stand = 0;
int g_aimTicks = 0;
bool g_heldInPort = false;
int g_watchTicks = 0;
std::unique_ptr<D::AimedGrab> g_grab;

// carryred is carry's control: coop/interactables/mirror_slot_entry lets a carried prop's entry run.
Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::subject_drill);
        if (v == "insert") return Arm::Insert;
        if (v == "own") return Arm::Own;
        if (v == "carry" || v == "carryred") return Arm::Carry;
        return Arm::Off;
    }();
    return a;
}

const char* ArmName() {
    return ArmOf() == Arm::Insert ? "insert" : ArmOf() == Arm::Own ? "own" : ArmOf() == Arm::Carry ? "carry" : "off";
}

void Enter(HostStep h) { g_host = h; g_stepAt = Clock::now(); }
void Enter(ClientStep c) { g_client = c; g_stepAt = Clock::now(); }
bool StepExpired(std::chrono::seconds bound) { return Clock::now() - g_stepAt >= bound; }

void Abandon(const char* who, const char* why) {
    UE_LOGW("[subject_drill] ABANDONED (%s, arm %s): %s", who, ArmName(), why);
    g_host = HostStep::Done;
    g_client = ClientStep::Done;
}

uint32_t EidOf(void* actor) {
    return actor ? static_cast<uint32_t>(coop::element::Registry::Get().EidForActor(actor)) : 0u;
}
bool Named(void* actor) { return actor && coop::element::Registry::Get().EidForActor(actor) != coop::element::kInvalidId; }

void* SpawnDriveBeside(void* player, float forwardCm, float sideCm) {
    ue_wrap::FVector at{};
    void* cls = DC::DriveClass();
    if (!cls || !E::TryGetActorLocation(player, at)) return nullptr;
    const ue_wrap::FVector fwd = E::GetActorForwardVector(player);
    const ue_wrap::FVector side{-fwd.Y, fwd.X, 0.f};
    return E::SpawnActor(cls, {at.X + fwd.X * forwardCm + side.X * sideCm, at.Y + fwd.Y * forwardCm + side.Y * sideCm,
                               at.Z + 40.f});
}

void* PlaySlot() { return DC::EnsureResolved() ? DC::SlotActor(DC::kRoleDeskPlay) : nullptr; }

// BOTH, carry: the port's overlap entries this peer saw, and those of a prop a remote player carries.
uint64_t g_portEntries = 0, g_portEntriesCarried = 0;
// HOST, carry: the port's entry of a drive a remote player carries, and whether its body ran -- its post runs only
// when it did, a Cancel skipping the posts.
bool g_carriedEntrySeen = false, g_carriedEntryRan = false;
// HOST, carry: the verdict passed; an insert of the carried drive after it fails the run (its done-grace).
bool g_passed = false;
bool g_entryWatch = false;
int32_t g_entryActorOff = -1;
void* g_entryFn = nullptr;

// The play port's overlap entry, as the engine names it; one literal, since the gate keys a watch on its address.
constexpr const wchar_t* kPortEntry =
    L"BndEvt__driveSlot_drivePort_play_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature";

ue_wrap::script_gate::Verdict OnPortEntryPre(const ue_wrap::script_gate::Call& call) {
    if (g_entryFn != call.function) {
        g_entryFn = call.function;
        g_entryActorOff = R::FindParamOffset(call.function, L"OtherActor");
        UE_LOGI("[subject_drill] the port's entry fired first on %ls: its OtherActor at %d", R::ClassNameOf(call.object).c_str(),
                g_entryActorOff);
    }
    // The class has three slots (the desk's play and comp ports, the eraser's); the drill's is the play port.
    if (g_entryActorOff < 0 || !call.locals || call.object != PlaySlot()) return ue_wrap::script_gate::Verdict::Run;
    void* actor = *reinterpret_cast<void* const*>(call.locals + g_entryActorOff);
    const bool carried = actor && coop::remote_prop::IsActorUnderAnyDrive(actor);
    ++g_portEntries;
    if (carried) {
        ++g_portEntriesCarried;
        g_carriedEntrySeen = true;
    }
    if (g_portEntries <= 10)
        UE_LOGI("[subject_drill] a port's overlap entry: %ls eid=%u, carried by a remote player %d",
                actor ? R::ClassNameOf(actor).c_str() : L"null", EidOf(actor), carried ? 1 : 0);
    return ue_wrap::script_gate::Verdict::Run;
}

void OnPortEntryPost(const ue_wrap::script_gate::Call& call) {
    if (g_entryActorOff < 0 || !call.locals || call.object != PlaySlot()) return;
    void* actor = *reinterpret_cast<void* const*>(call.locals + g_entryActorOff);
    if (actor && coop::remote_prop::IsActorUnderAnyDrive(actor)) g_carriedEntryRan = true;
}

// HOST, carry: the desk's inserts of a drive a remote player still carries, counted at the insert's own body.
uint64_t g_carriedInserts = 0;
bool g_insertWatch = false;
int32_t g_overlappedOff = -1;
void* g_putFn = nullptr;

ue_wrap::script_gate::Verdict OnPutDriveInPre(const ue_wrap::script_gate::Call& call) {
    if (g_putFn != call.function) {
        g_putFn = call.function;
        g_overlappedOff = R::FindParamOffset(call.function, L"overlapped");
    }
    if (g_overlappedOff < 0 || !call.locals || call.object != PlaySlot()) return ue_wrap::script_gate::Verdict::Run;
    void* drive = *reinterpret_cast<void* const*>(call.locals + g_overlappedOff);
    if (drive && coop::remote_prop::IsActorUnderAnyDrive(drive)) {
        ++g_carriedInserts;
        UE_LOGW("[subject_drill] host: the desk's port inserts drive eid=%u, which a client still carries (called by %ls "
                "on %ls, from our code %d, depth %d)%s", EidOf(drive),
                call.callerFunction ? R::ToString(R::NameOf(call.callerFunction)).c_str() : L"no frame",
                call.callerObject ? R::ClassNameOf(call.callerObject).c_str() : L"null", call.fromOurCode ? 1 : 0,
                call.depth, g_passed ? " after the verdict -- FAIL" : "");
    }
    return ue_wrap::script_gate::Verdict::Run;
}

// The drive a client's hold moves on this host, if any: a drive the pose stream drives, named by the element lane
// unless `anyDrive`.
void* HeldDrive(bool anyDrive = false) {
    struct Find { void* found; bool any; } f{nullptr, anyDrive};
    ue_wrap::object_index::ForEachInstance(DC::DriveClass(), [](void* ctx, void* obj, int32_t) {
        auto& q = *static_cast<Find*>(ctx);
        if (!q.found && (q.any || Named(obj)) && coop::remote_prop::IsActorUnderAnyDrive(obj)) q.found = obj;
    }, &f);
    return f.found;
}

void HostTick(coop::net::Session& s) {
    void* player = coop::players::Registry::Get().Local();
    switch (g_host) {
    case HostStep::Ready: {
        // Before any client's world is ready, so a joiner meets the drives through the join.
        if (!player || !PlaySlot()) return;
        for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers); ++slot)
            if (s.IsSlotWorldReady(slot)) {
                Abandon("host", "a client's world was ready before the host could bring its drives in");
                return;
            }
        const int n = ArmOf() == Arm::Insert ? 2 : 1;
        if (ArmOf() == Arm::Carry) {
            if (!g_entryWatch)
                g_entryWatch = ue_wrap::script_gate::WatchClassName(L"driveSlot_C", kPortEntry, 0x53444345 /*'SDCE'*/,
                                                                    &OnPortEntryPre, &OnPortEntryPost);
            if (!g_insertWatch)
                g_insertWatch = ue_wrap::script_gate::WatchClassName(L"driveSlot_C", L"putDriveIn",
                                                                     0x53444349 /*'SDCI'*/, &OnPutDriveInPre, nullptr);
            // The port is emptied once a client's world stands: the save seats its drive there some seconds after this
            // world is up.
            Enter(HostStep::EmptyPort);
            return;
        }
        for (int i = 0; i < n; ++i) {
            g_drives[i].Set(SpawnDriveBeside(player, 150.f, i == 0 ? 0.f : 120.f));
            if (!g_drives[i].Get()) {
                Abandon("host", "a drive could not be spawned");
                return;
            }
        }
        UE_LOGI("[subject_drill] host spawned %d drive(s) in front of its player", n);
        Enter(HostStep::Named);
        return;
    }
    case HostStep::Named: {
        const int n = ArmOf() == Arm::Insert ? 2 : 1;
        for (int i = 0; i < n; ++i)
            if (!Named(g_drives[i].Get())) {
                if (StepExpired(kStepBound)) Abandon("host", "the element lane did not name the drives within 60 s");
                return;
            }
        UE_LOGI("[subject_drill] host armed (arm %s): drive eid=%u%s", ArmName(), EidOf(g_drives[0].Get()),
                n == 2 ? " and a second, the one to seat" : "");
        Enter(ArmOf() == Arm::Insert ? HostStep::WaitHold
              : ArmOf() == Arm::Carry ? HostStep::WatchCarry : HostStep::Done);
        return;
    }
    case HostStep::EmptyPort: {
        // The carry needs the port empty. The save's seated drive leaves the world rather than the slot: an eject that
        // no grab follows leaves the drive frozen in the port and the slot's latch set, which only that drive's
        // EndOverlap clears, while a removed drive ends its overlap. The removal crosses to every client.
        bool ready = false;
        for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers); ++slot)
            ready = ready || s.IsSlotWorldReady(slot);
        if (!ready) {
            if (StepExpired(kJoinBound)) Abandon("host", "no client's world stood within 600 s");
            return;
        }
        if (void* seated = DC::SlotDrive(PlaySlot())) {
            const uint32_t eid = EidOf(seated);
            E::DestroyActor(seated);
            UE_LOGI("[subject_drill] host FIXTURE: removed drive eid=%u, which the save seated in the desk's play slot, "
                    "so the carry meets the port empty", eid);
        }
        // A saved world can hold the slot's eject latch set with no drive left to end it (only the ejected drive's
        // EndOverlap clears it), which turns every entry away: the fixture ends it, as that EndOverlap would.
        bool latch = false;
        int collision = -1;
        if (DC::ReadSlotLatch(PlaySlot(), latch, collision) && latch) {
            DC::CompleteEjectLatch(PlaySlot(), nullptr);
            DC::ReadSlotLatch(PlaySlot(), latch, collision);
            UE_LOGI("[subject_drill] host FIXTURE: the desk's play slot held a stale eject latch; ended, it reads %d",
                    latch ? 1 : 0);
        }
        // The saved world's drives serve: the client carries whichever is nearest it.
        UE_LOGI("[subject_drill] host armed (arm carry): the desk's play slot is empty");
        Enter(HostStep::WatchCarry);
        return;
    }
    case HostStep::WatchCarry: {
        // The client carries a drive into its own port; its insert reaches this one by the drive lane. This port
        // meets the mirror first, since a pose is sent the tick the drive enters and the slot line a tick after it.
        // The drive is whichever one the client's hold moves here first.
        if (!g_carried.Get()) {
            if (void* held = HeldDrive(/*anyDrive=*/true)) {
                g_carried.Set(held);
                UE_LOGI("[subject_drill] host sees the client carry drive eid=%u", EidOf(held));
            }
        }
        void* drive = g_carried.Get();
        void* slotActor = PlaySlot();
        const bool seated = drive && slotActor && DC::SlotDrive(slotActor) == drive;
        if (drive && slotActor && ++g_watchTicks % 120 == 1) {
            ue_wrap::FVector at{}, port{};
            E::TryGetActorLocation(drive, at);
            if (void* pc = DC::Port(slotActor)) port = E::GetComponentLocation(pc);
            bool latch = false;
            int collision = -1;
            DC::ReadSlotLatch(slotActor, latch, collision);
            const float dx = at.X - port.X, dy = at.Y - port.Y, dz = at.Z - port.Z;
            UE_LOGI("[subject_drill] host's mirror of drive eid=%u: %.0f cm from the port, overlapping %d; the slot's "
                    "eject latch %d, its port's collision %d, its drive %u, refused %llu, entries %llu (%llu carried)",
                    EidOf(drive), std::sqrt(dx * dx + dy * dy + dz * dz), DC::PortOverlaps(slotActor, drive) ? 1 : 0,
                    latch ? 1 : 0, collision, EidOf(DC::SlotDrive(slotActor)),
                    static_cast<unsigned long long>(coop::mirror_slot_entry::Refused()),
                    static_cast<unsigned long long>(g_portEntries), static_cast<unsigned long long>(g_portEntriesCarried));
        }
        // The verdict is the carried mirror's first entry into this port, read the tick after it: the client's latched
        // port holds the drive while its player carries it. The entry's post ran if its body did.
        if (!g_carriedEntrySeen) {
            if (StepExpired(kJoinBound)) Abandon("host", "the client's carried drive never entered this port");
            return;
        }
        g_passed = !g_carriedEntryRan && g_carriedInserts == 0 && !seated;
        if (g_passed)
            UE_LOGI("[subject_drill] host DONE -- the drive a client carries entered this port %llu time(s); the port's "
                    "entry never ran for it, nothing inserted it and the slot is empty -- PASS",
                    static_cast<unsigned long long>(g_portEntriesCarried));
        else
            UE_LOGW("[subject_drill] host DONE -- the drive a client carries entered this port; its entry ran %d, the "
                    "port inserted it %llu time(s), the slot holds it %d -- FAIL", g_carriedEntryRan ? 1 : 0,
                    static_cast<unsigned long long>(g_carriedInserts), seated ? 1 : 0);
        Enter(HostStep::Done);
        return;
    }
    case HostStep::WaitHold: {
        // Once a client's join is over and a drive -- its nearest, one of these or the world's -- moves under its
        // hold, one of the host's own that is not held is seated.
        bool joined = false;
        for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers) && !joined; ++slot)
            joined = s.IsSlotWorldReady(slot) && coop::prop_snapshot::IsBracketClosed(slot);
        void* held = joined ? HeldDrive() : nullptr;
        if (!held) {
            if (StepExpired(kJoinBound)) Abandon("host", "no client held a drive within 600 s");
            return;
        }
        void* seat = nullptr;
        for (auto& d : g_drives)
            if (void* own = d.Get(); own && own != held && !seat) seat = own;
        void* slotActor = PlaySlot();
        if (!seat || !slotActor || DC::SlotDrive(slotActor) || !DC::CallPutDriveIn(slotActor, seat)) {
            Abandon("host", "no free drive of its own, or the desk's play slot is gone, full, or did not take it");
            return;
        }
        UE_LOGI("[subject_drill] host seated drive eid=%u in the desk's play slot while a client held eid=%u",
                EidOf(seat), EidOf(held));
        Enter(HostStep::Done);
        return;
    }
    case HostStep::Done:
        return;
    }
}

// The nearest named drive on this peer, or null.
void* NearestNamedDrive(void* player) {
    ue_wrap::FVector me{};
    if (!E::TryGetActorLocation(player, me)) return nullptr;
    struct Best { ue_wrap::FVector me; void* drive; float d; } best{me, nullptr, 1e30f};
    ue_wrap::object_index::ForEachInstance(DC::DriveClass(), [](void* ctx, void* obj, int32_t) {
        auto& b = *static_cast<Best*>(ctx);
        ue_wrap::FVector at{};
        if (!Named(obj) || !E::TryGetActorLocation(obj, at)) return;
        const float d = std::hypot(at.X - b.me.X, at.Y - b.me.Y);
        if (d < b.d) { b.d = d; b.drive = obj; }
    }, &best);
    return best.drive;
}

void ClientTick() {
    void* player = coop::players::Registry::Get().Local();
    switch (g_client) {
    case ClientStep::Ready: {
        if (!player || !coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle || !DC::EnsureResolved() ||
            !DC::DriveClass())
            return;
        if (g_stepAt == Clock::time_point{}) g_stepAt = Clock::now();
        // The carry starts at an empty port: the host's fixture removes the drive its save seated there.
        if (ArmOf() == Arm::Carry && (!PlaySlot() || DC::SlotDrive(PlaySlot()))) {
            if (StepExpired(kStepBound))
                Abandon("client", "this copy's play slot is gone or still held a drive 60 s after the join");
            return;
        }
        void* drive = NearestNamedDrive(player);
        ue_wrap::FVector at{};
        if (!drive || !E::TryGetActorLocation(drive, at)) {
            if (StepExpired(kStepBound)) Abandon("client", "no named drive here within 60 s of the join");
            return;
        }
        g_held.Set(drive);
        g_walk = D::StartBackgroundWalk(at, kReachCm, kWalkDeadlineS);
        UE_LOGI("[subject_drill] client walks to drive eid=%u", EidOf(drive));
        Enter(ClientStep::Walk);
        return;
    }
    case ClientStep::Walk: {
        const int st = g_walk ? g_walk->state.load() : 2;
        if (st == 0) return;
        if (st == 2) {
            Abandon("client", "the walk to the drive did not arrive");
            return;
        }
        g_grab = std::make_unique<D::AimedGrab>(player, g_held.Get());
        Enter(ClientStep::Grab);
        return;
    }
    case ClientStep::Grab: {
        const D::GrabState gs = g_grab->Tick();
        if (gs == D::GrabState::Working) return;
        if (gs == D::GrabState::Failed) {
            Abandon("client", g_grab->Why());
            return;
        }
        void* slotActor = PlaySlot();
        if (!slotActor || DC::SlotDrive(slotActor)) {
            Abandon("client", "this copy's play slot is gone or already holds a drive");
            return;
        }
        UE_LOGI("[subject_drill] client holds drive eid=%u (aimed after %d fan pose(s))", EidOf(g_held.Get()),
                g_grab->AimPoses());
        if (ArmOf() == Arm::Carry) {
            // Carried into its own port by hand: the walk keeps the hold, then the aim puts the drive in the port.
            if (!g_entryWatch)
                g_entryWatch = ue_wrap::script_gate::WatchClassName(L"driveSlot_C", kPortEntry, 0x53444345 /*'SDCE'*/,
                                                                    &OnPortEntryPre, &OnPortEntryPost);
            void* portComp = DC::Port(slotActor);
            if (!portComp) {
                Abandon("client", "this copy's play slot has no port");
                return;
            }
            g_stands = D::ReachableStandpoints(player, E::GetComponentLocation(portComp), kStandRingCm, kStandpoints,
                                               kStandReachCm);
            g_stand = 0;
            if (g_stands.empty()) {
                Abandon("client", "no standpoint about the desk's play port is reachable");
                return;
            }
            g_walk = D::StartBackgroundWalk(g_stands[0], kStandReachCm, kStandWalkDeadlineS, /*carry=*/true);
            UE_LOGI("[subject_drill] client carries drive eid=%u to the first of %zu standpoints about the desk's play "
                    "port", EidOf(g_held.Get()), g_stands.size());
            Enter(ClientStep::CarryWalk);
            return;
        }
        if (ArmOf() == Arm::Own) {
            // Its own insert, as the port's overlap runs it while the drive is carried in.
            if (!DC::CallPutDriveIn(slotActor, g_held.Get())) {
                Abandon("client", "its own putDriveIn did not run");
                return;
            }
            Enter(ClientStep::Seated);
            return;
        }
        Enter(ClientStep::Hold);
        return;
    }
    case ClientStep::Hold: {
        // The host's insert, replayed on this copy: its play slot filled with the other drive.
        void* slotActor = PlaySlot();
        void* seated = slotActor ? DC::SlotDrive(slotActor) : nullptr;
        if (!seated) {
            if (D::Grabbing(player) != g_held.Get()) {
                Abandon("client", "the grab ended before any insert reached this copy");
                return;
            }
            if (StepExpired(kStepBound)) Abandon("client", "the host's insert did not reach this copy within 60 s");
            return;
        }
        const bool kept = D::Grabbing(player) == g_held.Get();
        if (kept)
            UE_LOGI("[subject_drill] client DONE -- the host seated drive eid=%u in this copy's play slot and this "
                    "client still holds eid=%u -- PASS", EidOf(seated), EidOf(g_held.Get()));
        else
            UE_LOGW("[subject_drill] client DONE -- the host seated drive eid=%u in this copy's play slot and this "
                    "client's grab of eid=%u ended with it -- FAIL", EidOf(seated), EidOf(g_held.Get()));
        if (kept) D::CallOnPlayer(player, L"dropGrabObject");  // the drill ends with an empty hand
        Enter(ClientStep::Done);
        return;
    }
    case ClientStep::Seated: {
        void* slotActor = PlaySlot();
        const bool seated = slotActor && DC::SlotDrive(slotActor) == g_held.Get();
        const bool ended = D::Grabbing(player) == nullptr;
        if (seated && ended)
            UE_LOGI("[subject_drill] client DONE -- its own insert seated drive eid=%u and ended its grab, as single "
                    "player's does -- PASS", EidOf(g_held.Get()));
        else
            UE_LOGW("[subject_drill] client DONE -- its own insert: seated=%d, grab ended=%d -- FAIL", seated ? 1 : 0,
                    ended ? 1 : 0);
        Enter(ClientStep::Done);
        return;
    }
    case ClientStep::CarryWalk: {
        const int st = g_walk ? g_walk->state.load() : 2;
        if (st == 0) return;
        if (D::Grabbing(player) != g_held.Get()) {
            Abandon("client", "the drive left the hand on the way to the port");
            return;
        }
        if (st == 2) {
            if (++g_stand >= g_stands.size()) {
                Abandon("client", "no standpoint about the desk's play port could be walked to");
                return;
            }
            g_walk = D::StartBackgroundWalk(g_stands[g_stand], kStandReachCm, kStandWalkDeadlineS, /*carry=*/true);
            UE_LOGI("[subject_drill] client steps to standpoint %zu of %zu about the port", g_stand + 1, g_stands.size());
            return;
        }
        // Its own port must hold the drive without taking it, so the host's mirror of it reaches the host's port
        // while this player still carries it: the slot's eject latch turns the entry away, as after an eject.
        if (!DC::WriteSlotLatch(PlaySlot(), true)) {
            Abandon("client", "its own play slot's eject latch could not be set");
            return;
        }
        UE_LOGI("[subject_drill] client FIXTURE: latched its own play slot, so its port holds the carried drive");
        Enter(ClientStep::CarryAim);
        return;
    }
    case ClientStep::CarryAim: {
        // The hold puts the drive the grab's length along the aim, as a player's scroll and look do: the length set to
        // the port's distance and the aim on the port, each tick until the port's own overlap takes it.
        void* slotActor = PlaySlot();
        void* portComp = slotActor ? DC::Port(slotActor) : nullptr;
        if (slotActor && DC::SlotDrive(slotActor) == g_held.Get()) {
            Abandon("client", "its latched port took the carried drive");
            return;
        }
        if (!portComp || D::Grabbing(player) != g_held.Get()) {
            Abandon("client", "the drive left the hand, or the port is gone, while it was held in the port");
            return;
        }
        // Held in the latched port until the host's verdict ends the run.
        if (!g_heldInPort && DC::PortOverlaps(slotActor, g_held.Get())) {
            g_heldInPort = true;
            UE_LOGI("[subject_drill] client holds drive eid=%u in its latched play port", EidOf(g_held.Get()));
        }
        if (StepExpired(kJoinBound)) {
            Abandon("client", "the host gave no verdict while the drive was held in the port");
            return;
        }
        const ue_wrap::FVector cam = E::GetCameraLocation();
        const ue_wrap::FVector port = E::GetComponentLocation(portComp);
        const float dx = port.X - cam.X, dy = port.Y - cam.Y, dz = port.Z - cam.Z;
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (++g_aimTicks % 120 == 1) {
            ue_wrap::FVector held{};
            E::TryGetActorLocation(g_held.Get(), held);
            const float hx = held.X - port.X, hy = held.Y - port.Y, hz = held.Z - port.Z;
            bool latch = false;
            int collision = -1;
            DC::ReadSlotLatch(slotActor, latch, collision);
            UE_LOGI("[subject_drill] client aims the drive at the port: camera %.0f cm from it, the drive %.0f cm "
                    "(%.0f, %.0f, %.0f), overlapping %d; the slot's eject latch %d, its port's collision %d, its drive %u",
                    dist, std::sqrt(hx * hx + hy * hy + hz * hz), hx, hy, hz,
                    DC::PortOverlaps(slotActor, g_held.Get()) ? 1 : 0, latch ? 1 : 0, collision,
                    EidOf(DC::SlotDrive(slotActor)));
        }
        const int32_t lenOff = ue_wrap::reflected_offset::MainPlayer_grabLen();
        if (lenOff >= 0)
            *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(player) + lenOff) = std::fmin(150.f, std::fmax(50.f, dist));
        E::SetControlRotation(E::GetController(player), D::LookAt(cam, port));
        return;
    }
    case ClientStep::Done:
        return;
    }
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (ArmOf() == Arm::Off || !session || !session->running()) return;
    if (session->role() == coop::net::Role::Host) HostTick(*session);
    else                                           ClientTick();
}

void OnDisconnect() {
    if (ArmOf() == Arm::Off) return;
    g_host = HostStep::Ready;
    g_client = ClientStep::Ready;
    g_stepAt = {};
    for (auto& d : g_drives) d.Reset();
    g_held.Reset();
    g_carried.Reset();
    g_walk.reset();
    g_grab.reset();
    g_carriedInserts = 0;
    g_carriedEntrySeen = g_carriedEntryRan = g_passed = false;
    g_portEntries = g_portEntriesCarried = 0;
    g_stands.clear();
    g_stand = 0;
    g_aimTicks = 0;
    g_heldInPort = false;
    g_watchTicks = 0;
}

}  // namespace coop::dev::subject_drill
