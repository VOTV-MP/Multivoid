// coop/dev/atv_riders_drill_steps.cpp -- the reads and the steps of the ATV riders drill, each decided on the records of
// coop/interactables/atv_drill_events.h and on a read of the body or the kerfur at the decision's event; see
// coop/dev/atv_riders_drill.h and the order in atv_riders_drill.cpp. A step polls the rings from the cursor taken before its
// act, so a verdict is a function of what was recorded since, and the order of two records is their `ord`.

#include "atv_riders_drill_detail.h"

#include "atv_seat.h"   // co-located private header: the game's own seat verb

#include "coop/dev/atv_riders_arm.h"
#include "coop/dev/atv_riders_join_hold.h"
#include "coop/dev/spawn_npc.h"
#include "coop/element/mirror_defer.h"
#include "coop/element/mirror_managers.h"
#include "coop/element/registry.h"
#include "coop/interactables/atv_sync.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/kerfur.h"
#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::dev::atv_riders_drill::detail {
namespace {

namespace E = ue_wrap::engine;
namespace K = ue_wrap::kerfur;
using ue_wrap::FRotator;
using ue_wrap::FVector;

constexpr float kWedgedUpZ = 0.9f;    // an ATV whose up vector points lower than this is tipped or climbing what it struck
constexpr int kDriveLogGapS = 2;      // at most one drive line per this many seconds of block events
constexpr int kProbeS = 3;            // the throttle's first seconds: the root must have left its start by kProbeCm in them
constexpr float kProbeCm = 20.f;

float Dist(const FVector& a, const FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Counts the kerfur mirrors the game's own sit left invited and not seated: `occupyCar` the ATV, no attach to it. Game thread.
int InvitedMirrors(void* atv) {
    std::vector<coop::element::Npc*> npcs;
    coop::element::NpcMirrors().Snapshot(npcs);
    int n = 0;
    for (coop::element::Npc* npc : npcs) {
        void* k = npc ? npc->LiveActor() : nullptr;
        if (!k || !K::IsKerfurActor(k)) continue;
        void* parent = nullptr;
        E::GetAttachParentActor(k, parent);
        if (K::GetOccupyCar(k) == atv && parent != atv) ++n;
    }
    return n;
}

bool ReadRoot(FVector& out) {
    bool ok = false;
    OnGt([&] {
        void* atv = AD::Atv(g_key);
        FRotator rot{};
        ok = atv && ue_wrap::atv::GetRootTransform(atv, out, rot);
    });
    return ok;
}

constexpr uint8_t kStateIdle = 1;   // enum_kerfurCommand: idle (0 follows the player, 6 goes to an ATV)

// The host's kerfur, as the drive line reads it: its distance to the ATV's sitarrow (cm, -1 unread), its CharacterMovement's
// mode and speed (-1 unread), its command state (-1 unread), what its last move() went for, and the path-following status
// of its AI controller (EPathFollowingStatus, -1 unread).
enum class Target { Unread, None, Host, Other };
struct KerfurDrive {
    float sitarrowCm = -1.f, speed = -1.f;
    int mode = -1, state = -1, path = -1;
    Target target = Target::Unread;
};

// What step 7's drive leg reads of the host's ATV in one game-thread hop, and the kerfur's seat beside it. `ok` is false when the
// root or the up vector does not read; `speed` is the root body's linear speed (cm/s), -1 when unread; `occupant` and `syncer`
// are the table's slots, -1 when the table does not read.
struct DriveRead {
    bool ok = false;
    FVector root{};
    float upZ = 0.f, yaw = 0.f, speed = -1.f, fuel = 0.f;
    bool brake = false, turbo = false, kerfurOn = false;
    int occupant = -1, syncer = -1;
    KerfurDrive kerfur;
};

// The kerfur of g_eid on this peer (the host's own, on the host), or null. Game thread.
void* KerfurActor() {
    coop::element::Npc* npc = coop::element::NpcMirrors().Get(g_eid);
    void* actor = npc ? npc->LiveActor() : nullptr;
    return actor && K::IsKerfurActor(actor) ? actor : nullptr;
}

void ReadKerfurDrive(KerfurDrive& k) {
    OnGt([&] {
        void* actor = KerfurActor();
        if (!actor) return;
        uint8_t mode = 0, state = 0, face = 0, status = 0;
        bool spooky = false;
        FVector vel{};
        if (ue_wrap::puppet::ReadCharacterMovement(actor, mode, vel)) {
            k.mode = mode;
            k.speed = std::sqrt(vel.X * vel.X + vel.Y * vel.Y + vel.Z * vel.Z);
        }
        if (K::ReadKerfurState(actor, state, spooky, face)) k.state = state;
        void* target = K::ReadTargetActor(actor);
        k.target = !target ? Target::None : target == AD::Me() ? Target::Host : Target::Other;
        if (E::ReadMoveStatus(actor, &status)) k.path = status;
    });
}

DriveRead ReadDrive() {
    DriveRead d;
    OnGt([&] {
        void* atv = AD::Atv(g_key);
        FRotator rot{};
        if (!atv || !ue_wrap::atv::GetRootTransform(atv, d.root, rot)) return;
        const FVector up = E::GetActorUpVector(atv);
        d.ok = up.X != 0.f || up.Y != 0.f || up.Z != 0.f;   // a unit vector is never zero: a zero is an unread one
        d.upZ = up.Z;
        d.yaw = rot.Yaw;
        FVector lin{}, ang{};
        if (E::GetActorRootPhysicsVelocity(atv, lin, ang)) d.speed = std::sqrt(lin.X * lin.X + lin.Y * lin.Y + lin.Z * lin.Z);
        d.fuel = ue_wrap::atv::GetFuel(atv);
        d.brake = ue_wrap::atv::GetBrake(atv);
        d.turbo = ue_wrap::atv::GetTurbo(atv);
        coop::atv_sync::TableView tv;
        if (coop::atv_sync::ReadTable(g_key, tv)) {
            d.occupant = tv.occupantSlot;
            d.syncer = tv.syncerSlot;
        }
    });
    const KerfurRead kerfur = ReadKerfur(g_eid);
    d.kerfurOn = kerfur.parentIsAtv;
    d.kerfur.sitarrowCm = kerfur.sitarrowCm;
    ReadKerfurDrive(d.kerfur);
    return d;
}

// The status of the path following as the line says it: Idle and Moving by name, anything else (waiting for a path, paused)
// or unread as `?`.
const char* PathName(int status) { return status == 0 ? "idle" : status == 3 ? "moving" : "?"; }

const char* TargetName(Target t) {
    return t == Target::None ? "none" : t == Target::Host ? "host" : t == Target::Other ? "other" : "?";
}

std::string DriveText(const DriveRead& d) {
    char buf[416];
    std::snprintf(buf, sizeof(buf),
                  "root=(%.0f,%.0f,%.0f) yaw=%.0f upZ=%.2f v=%.0f fuel=%.1f brake=%d turbo=%d seat=%d/%d kerfur=%s kd=%.0f "
                  "kmode=%d kv=%.0f kstate=%d ktarget=%s kpath=%s",
                  static_cast<double>(d.root.X), static_cast<double>(d.root.Y), static_cast<double>(d.root.Z),
                  static_cast<double>(d.yaw), static_cast<double>(d.upZ), static_cast<double>(d.speed), static_cast<double>(d.fuel), d.brake ? 1 : 0,
                  d.turbo ? 1 : 0, d.occupant, d.syncer, d.kerfurOn ? "on" : "off", static_cast<double>(d.kerfur.sitarrowCm),
                  d.kerfur.mode, static_cast<double>(d.kerfur.speed), d.kerfur.state, TargetName(d.kerfur.target),
                  PathName(d.kerfur.path));
    return buf;
}

// Reads the drive leg's state, says it, and returns its text.
std::string SayDrive() {
    const std::string text = DriveText(ReadDrive());
    UE_LOGI("atv-riders: step 7 drive (host): %s", text.c_str());
    return text;
}

}  // namespace

// ---- the helpers the late steps share ----

std::string Num(const char* fmt, float v) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), fmt, static_cast<double>(v));
    return buf;
}

// The record's two seat bytes: who held the seat and who holds it.
uint8_t OccBefore(const DE::Rec& r) { return static_cast<uint8_t>((r.i0 >> 8) & 0xFF); }
uint8_t OccAfter(const DE::Rec& r) { return static_cast<uint8_t>(r.i0 & 0xFF); }

// A seat change of this ATV that was announced to no subscriber: the red `seat_announce_off`, said on every peer's records.
bool SeatSilent(const std::vector<DE::Rec>& seats) {
    const int32_t tag = Tag();
    if (!Find(seats, [&](const DE::Rec& r) { return r.i3 == tag && r.i2 == 0; })) return false;
    Fail(1, "seat_announce_off", "a seat change of the ATV was announced to no subscriber");
    return true;
}

// The ATV's seat is gone from this peer's table: the entry is erased or its actor dead.
bool EntryGone() {
    bool gone = false;
    OnGt([&] { gone = AD::Atv(g_key) == nullptr; });
    return gone;
}

// The body of `slot` is released and shows it: its seat pose off, its collision on.
bool ReleasedRec(const DE::Rec& r, uint8_t slot) {
    return r.i0 == slot && r.i1 == 0 && r.i2 == Tag() && r.f1 < 0.5f && r.f2 > 0.5f;
}

// The local player's seat verb, tried until it finishes. `invited` is read in the call the seat finishes in, before any stamp
// of the host can seat a mirror; -1 when the verb did not seat the player.
bool SitLocal(int& invited) {
    atv_seat::SitTry t;
    invited = -1;
    PS::WaitFor("the seat verb finishes", 40, [&] {
        bool done = false;
        OnGt([&] {
            void* atv = AD::Atv(g_key);
            void* me = AD::Me();
            done = atv && me && atv_seat::TrySit(me, atv, t);
            if (done && coop::atv_sync::ActorSeatedBy(coop::players::Registry::Get().LocalPeerId()) == atv) {
                invited = InvitedMirrors(atv);
                SayLocalSeatOffset();
            }
        });
        return done;
    }, false);
    return invited >= 0;
}

// ---- the reads ----

BodyRead ReadBody(uint8_t slot) {
    BodyRead b;
    OnGt([&] {
        coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(slot);
        void* body = p && p->valid() ? p->GetActor() : nullptr;
        if (!body) return;
        b.exists = true;
        b.ragdoll = p->IsRagdollDisplayed();
        void* atv = AD::Atv(g_key);
        void* parent = nullptr;
        b.parentIsAtv = atv && E::GetAttachParentActor(body, parent) && parent == atv;
        bool on = false;
        b.onAtv = ue_wrap::puppet::GetOnAtv(body, on) && on;
        b.collision = E::GetActorRootCollisionEnabled(body) > 0;
        FVector at{}, root{};
        FRotator rot{};
        if (atv && E::TryGetActorLocation(body, at) && ue_wrap::atv::GetRootTransform(atv, root, rot)) b.offsetCm = Dist(at, root);
    });
    return b;
}

KerfurRead ReadKerfur(uint32_t eid) {
    KerfurRead k;
    OnGt([&] {
        coop::element::Npc* npc = coop::element::NpcMirrors().Get(eid);
        void* actor = npc ? npc->LiveActor() : nullptr;
        if (!actor || !K::IsKerfurActor(actor)) return;
        k.exists = true;
        coop::mirror_defer::HideInfo hide{};
        k.hidden = coop::mirror_defer::HideInfoOf(eid, hide);
        void* atv = AD::Atv(g_key);
        void* parent = nullptr;
        k.parentIsAtv = atv && E::GetAttachParentActor(actor, parent) && parent == atv;
        FVector at{}, seat{}, root{};
        FRotator rot{};
        if (atv && E::TryGetActorLocation(actor, at) && ue_wrap::atv::GetSitArrowLocation(atv, seat)) k.sitarrowCm = Dist(at, seat);
        if (atv && k.sitarrowCm >= 0.f && ue_wrap::atv::GetRootTransform(atv, root, rot)) {
            k.off[0] = at.X - root.X, k.off[1] = at.Y - root.Y, k.off[2] = at.Z - root.Z;
            k.sitOff[0] = seat.X - root.X, k.sitOff[1] = seat.Y - root.Y, k.sitOff[2] = seat.Z - root.Z;
        }
    });
    return k;
}

bool KerfurMirrorUp(uint32_t eid) {
    bool up = false;
    OnGt([&] {
        if (eid != 0) {
            coop::element::Npc* npc = coop::element::NpcMirrors().Get(eid);
            void* actor = npc ? npc->LiveActor() : nullptr;
            up = actor && K::IsKerfurActor(actor);
            return;
        }
        std::vector<coop::element::Npc*> npcs;
        coop::element::NpcMirrors().Snapshot(npcs);
        for (coop::element::Npc* npc : npcs) {
            void* actor = npc ? npc->LiveActor() : nullptr;
            if (actor && K::IsKerfurActor(actor)) up = true;
        }
    });
    return up;
}

bool PuppetUp(uint8_t slot) {
    bool up = false;
    OnGt([&] {
        coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(slot);
        up = p && p->valid();
    });
    return up;
}

bool WorldUp() {
    return PollFn("the announced world", kJoinS, [] {
               bool ready = false;
               OnGt([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
               return ready ? 1 : 0;
           }) &&
           PollFn("the host's puppet", 60, [] { return PuppetUp(kSlotH) ? 1 : 0; });
}

// ---- the host: the fixture, steps 1, 2 and 4 ----

// The kerfur the host's seat edge invites: one spawned 200 cm beside the ATV (a runtime spawn, so a rejoined observer takes the
// reveal route), or for `join` and `bind_ride_off` the world's own (the adoption route). Its eid is posted, 0 for the world's.
bool HostFixture() {
    uint32_t eid = 0;
    const bool spawn = !coop::dev::atv_riders_arm::Is("join") && !coop::dev::atv_riders_arm::Is("bind_ride_off");
    if (spawn) {
        void* kerfur = nullptr;
        bool placed = false;
        OnGt([&] {
            FVector at{};
            FRotator rot{};
            void* atv = AD::Atv(g_key);
            placed = atv && ue_wrap::atv::GetRootTransform(atv, at, rot);
            if (placed) kerfur = coop::dev::spawn_npc::SpawnAt(L"kerfurOmega_C", {at.X + 200.f, at.Y, at.Z + 60.f});
        });
        if (!placed || !kerfur) {
            Invalid("the kerfur was not spawned beside the ATV");
            return false;
        }
        if (!PollFn("the kerfur's element id", kEventS, [&] {
                OnGt([&] { eid = coop::element::Registry::Get().EidForActor(kerfur); });
                return eid != coop::element::kInvalidId ? 1 : 0;
            }))
            return false;
        UE_LOGI("atv-riders: the host's kerfur %p is eid %u", kerfur, eid);
    } else {
        bool any = false;
        OnGt([&] { any = !K::AllKerfurs().empty(); });
        if (!any) {
            Invalid("the world holds no kerfur of its own (join and bind_ride_off take the save's)");
            return false;
        }
    }
    PS::Mark("kerfur-eid", std::to_string(eid));
    return true;
}

// Steps 1 (the body attached), 2 (the invite, the rider's fact and the stamp that carries it) and 4 (the re-sent block that
// carries it) on the host. Its facts: the driver's seat change, the body's record, the kerfur's own sit.
bool HostRide(const Cur& cur) {
    const int32_t tag = Tag();
    bool s1 = false, s2 = false, s4 = false, inviteChecked = false;
    uint32_t factOrd = 0, eid = 0;
    return PollFn("the driver's seat and the kerfur's ride on the host", kRideS, [&]() -> int {
        const auto seats = Recs(cur, DE::kSeatChange);
        if (SeatSilent(seats)) return -1;
        const auto bodies = Recs(cur, DE::kBodySeat);
        if (!s1) {
            if (Find(bodies, [&](const DE::Rec& r) { return r.i0 == kSlotC && r.i1 == 3; })) {
                Fail(1, "", "the seat owner could not seat the driver's body");
                return -1;
            }
            if (const DE::Rec* a = Find(bodies, [&](const DE::Rec& r) { return r.i0 == kSlotC && r.i1 == 1; })) {
                if (a->f1 < 0.5f) {
                    Fail(1, "", "the driver's body was attached with no seat pose");
                    return -1;
                }
                if (!ReadBody(kSlotC).parentIsAtv) {
                    Fail(1, "body_attach_off", "the driver's body was seated and its attach parent is not the ATV");
                    return -1;
                }
                g_attachCm = a->f0;
                if (!JudgeRemoteSeat(kSlotC, 1)) return -1;
                s1 = true;
                StepPass(1, "the driver's body is attached to the ATV with the seat pose");
            }
        }
        if (!inviteChecked)
            if (const DE::Rec* seat = Find(seats, [&](const DE::Rec& r) { return r.i3 == tag && OccAfter(r) == kSlotC; })) {
                inviteChecked = true;
                const auto invites = Recs(cur, DE::kInvite);
                if (!Find(invites, [&](const DE::Rec& r) { return r.i0 == tag && r.i1 == 1 && r.ord < seat->ord; })) {
                    Fail(2, "invite_off", "the driver's seat change invited no kerfur");
                    return -1;
                }
            }
        const auto facts = Recs(cur, DE::kRiderFact);
        if (!factOrd)
            if (const DE::Rec* f = Find(facts, [&](const DE::Rec& r) { return r.i0 == tag && r.i2 == 1; })) {
                factOrd = f->ord;
                eid = static_cast<uint32_t>(f->i1);
                g_eid = eid;
                PS::Mark("rider-sat", std::to_string(eid));
                UE_LOGI("atv-riders: the host's kerfur eid %u sat on the ATV", eid);
            }
        if (factOrd) {
            for (const DE::Rec& r : Recs(cur, DE::kStampBuilt)) {
                if (r.i0 != tag || r.ord <= factOrd) continue;
                const bool resend = r.i2 == 1;
                if (r.i1 == 0) {
                    if (resend) Fail(4, "resend_rider_off", "the first re-sent block after the kerfur's seat carries no rider");
                    else Fail(2, "rider_stamp_off", "the first block built after the kerfur's seat carries no rider");
                    return -1;
                }
                if (static_cast<uint32_t>(r.i1) != eid) continue;
                if (resend) s4 = true;
                else s2 = true;
            }
            if (s2 && s4 && s1) {
                StepPass(2, "the invite ran, the kerfur sat and the host's block names it");
                StepPass(4, "the host's re-sent block names the kerfur");
                return 1;
            }
        }
        return 0;
    });
}

// Step 1 on the observer: the driver's body attached, no seat change announced to nobody.
bool ObserverSeat(const Cur& cur) {
    return PollFn("the driver's body on the observer", kRideS, [&]() -> int {
        if (SeatSilent(Recs(cur, DE::kSeatChange))) return -1;
        const auto bodies = Recs(cur, DE::kBodySeat);
        if (Find(bodies, [&](const DE::Rec& r) { return r.i0 == kSlotC && r.i1 == 3; })) {
            Fail(1, "", "the seat owner could not seat the driver's body");
            return -1;
        }
        const DE::Rec* a = Find(bodies, [&](const DE::Rec& r) { return r.i0 == kSlotC && r.i1 == 1; });
        if (!a) return 0;
        if (a->f1 < 0.5f) {
            Fail(1, "", "the driver's body was attached with no seat pose");
            return -1;
        }
        if (!ReadBody(kSlotC).parentIsAtv) {
            Fail(1, "body_attach_off", "the driver's body was seated and its attach parent is not the ATV");
            return -1;
        }
        g_attachCm = a->f0;
        if (!JudgeRemoteSeat(kSlotC, 1)) return -1;
        StepPass(1, "the driver's body is attached to the ATV with the seat pose");
        return 1;
    });
}

// The driver's seat, and step 2's local check at the call the seat finishes in: no mirror left invited and unseated, and the
// call the game's sit made on a mirror was refused.
bool DriverSit(const Cur& cur) {
    int invited = -1;
    if (!SitLocal(invited)) {
        Invalid("the seat verb did not seat the driver (see the [ATVP] lines)");
        return false;
    }
    if (invited > 0) {
        Fail(2, "mirror_invite_off", "the driver's own sit left " + std::to_string(invited) + " kerfur mirror(s) invited");
        return false;
    }
    if (!Find(Recs(cur, DE::kMirrorCancel), [](const DE::Rec& r) { return r.i0 == 1; })) {
        Fail(2, "mirror_invite_off", "the driver's own sit invited no mirror, and the gate refused none");
        return false;
    }
    StepPass(2, "the driver's own sit invited no kerfur mirror (its tryToOccupyCar was refused)");
    return true;
}

// Steps 2 (the ride) and 4 (the observer's first re-sent block with the rider) on a client, for the kerfur the host named.
bool ClientRide(Role me, const Cur& cur) {
    const int32_t tag = Tag();
    const uint32_t eid = g_eid;
    bool s2 = false, s4 = me != kObserver;
    return PollFn("the kerfur's ride on a client", kRideS, [&]() -> int {
        const auto seats = Recs(cur, DE::kSeatChange);
        if (SeatSilent(seats)) return -1;
        const auto rides = Recs(cur, DE::kRide);
        if (!s2 && Find(seats, [&](const DE::Rec& r) { return r.i3 == tag && static_cast<uint32_t>(r.i1) == eid; })) {
            const auto mine = [&](const DE::Rec& r) { return static_cast<uint32_t>(r.i0) == eid; };
            if (const DE::Rec* ok = Find(rides, [&](const DE::Rec& r) { return mine(r) && r.i1 == 1; })) {
                if (ok->f0 > kRigidCm) {
                    Fail(2, "", "the kerfur sits " + Num("%.0f", ok->f0) + " cm from the ATV's sitarrow");
                    return -1;
                }
                s2 = true;
                StepPass(2, "the kerfur mirror rides, " + Num("%.0f", ok->f0) + " cm from the ATV's sitarrow");
            } else if (Find(rides, [&](const DE::Rec& r) { return mine(r) && (r.i1 == 4 || r.i1 == 5); })) {
                Fail(2, "", "the kerfur mirror's ride was refused or did not attach");
                return -1;
            } else if (!Find(rides, mine)) {
                Fail(2, "mirror_ride_off", "the stamp named the kerfur and no ride was issued for it");
                return -1;
            }
        }
        if (!s4 && Find(Recs(cur, DE::kPoseApplied), [&](const DE::Rec& r) {
                return r.i0 == tag && r.i1 == 1 && r.f1 > 0.5f && static_cast<uint32_t>(r.f0) == eid;
            })) {
            s4 = true;
            StepPass(4, "the first block from the ATV's syncer carries the rider");
        }
        return s2 && s4 ? 1 : 0;
    });
}

// ---- step 3: the drive ----

// The host's ATV moves kDriveCm: it says `drive-done`; the driver lets the throttle go and says `drive-off`; the body is read.
bool HostDrive() {
    FVector start{};
    if (!ReadRoot(start)) {
        Invalid("the ATV's root does not read");
        return false;
    }
    uint32_t blockCursor = DE::Count(DE::kPoseApplied);   // the blocks of the drive only, each read once
    const int32_t tag = Tag();
    PS::Mark("h_drive_ready");
    bool done = false;
    float movedCm = 0.f;
    return PollFn("the driven ATV's 600 cm and the throttle's release", kDriveS, [&]() -> int {
        std::string t;
        if (PS::Peek("drive-off", t)) {
            const BodyRead b = ReadBody(kSlotC);
            if (!b.parentIsAtv || std::fabs(b.offsetCm - g_attachCm) >= kRigidCm) {
                Fail(3, "", "the driver's body moved " + Num("%.0f", b.offsetCm - g_attachCm) + " cm from its seat (attached: " +
                                (b.parentIsAtv ? "yes" : "no") + ")");
                return -1;
            }
            StepPass(3, "the driver's body stayed on its seat through " + Num("%.0f", movedCm) + " cm");
            return 1;
        }
        if (done) return 0;
        bool fresh = false;
        DE::Rec r;
        while (DE::Next(DE::kPoseApplied, blockCursor, r)) fresh = fresh || (r.i0 == tag && r.i1 == 0);
        if (fresh) {
            FVector now{};
            if (ReadRoot(now)) movedCm = std::sqrt((now.X - start.X) * (now.X - start.X) + (now.Y - start.Y) * (now.Y - start.Y));
            if (movedCm >= kDriveCm) {
                done = true;
                PS::Mark("drive-done");
            }
        }
        return 0;
    });
}

bool WatchDrive() {
    std::string t;
    if (!AwaitMark("drive-off", t, kDriveS + kSignalS)) return false;
    const BodyRead b = ReadBody(kSlotC);
    if (!b.parentIsAtv || std::fabs(b.offsetCm - g_attachCm) >= kRigidCm) {
        Fail(3, "", "the driver's body moved " + Num("%.0f", b.offsetCm - g_attachCm) + " cm from its seat (attached: " +
                        (b.parentIsAtv ? "yes" : "no") + ")");
        return false;
    }
    const KerfurRead k = ReadKerfur(g_eid);
    if (!k.exists || !k.parentIsAtv || k.sitarrowCm < 0.f || k.sitarrowCm > kRigidCm) {
        Fail(3, "", "the riding kerfur is " + Num("%.0f", k.sitarrowCm) + " cm from the ATV's sitarrow (attached: " +
                        (k.parentIsAtv ? "yes" : "no") + ")");
        return false;
    }
    StepPass(3, "the driver's body and the kerfur stayed on their seats through the drive");
    return true;
}

bool DriverDrive() {
    std::string t, why;
    if (!AwaitMark("h_drive_ready", t, kSignalS) || !AwaitMark("o_drive_ready", t, kSignalS)) return false;
    if (!AD::DriveOn(g_key, why)) {
        Invalid(why);
        return false;
    }
    const bool arrived = AwaitMark("drive-done", t, kDriveS);
    AD::DriveOff(g_key);
    PS::Mark("drive-off");
    return arrived;
}

// Step 7's second leg on the host: it backs its own ATV kDriveCm (the root's distance, whatever the direction) away from where
// step 3 stopped it, with its kerfur riding, lets the reverse key go, reads where its kerfur stands (JudgeKerfurSeat) and says
// `drive7-done` for the clients' reads. An ATV that stands tipped, or that the throttle does not move kProbeCm in kProbeS
// seconds, ends the leg INVALID at once as wedged. In the control the kerfur is idle and off the ATV, and the leg ends on the
// 600 cm alone.
bool HostDriveSeated() {
    const DriveRead first = ReadDrive();
    if (!first.ok) {
        Invalid("the ATV's root or up vector does not read");
        return false;
    }
    UE_LOGI("atv-riders: step 7 drive (host): %s", DriveText(first).c_str());
    // A tipped ATV cannot drive: the leg would measure something other than the host's own drive.
    if (first.upZ < kWedgedUpZ) {
        Invalid("the ATV is wedged before the host's drive (upZ=" + Num("%.2f", first.upZ) + ", yaw " + Num("%.0f", first.yaw) + ")");
        return false;
    }
    const FVector start = first.root;
    std::string why;
    // The leg backs away: step 3's drive ends against what it struck, ahead of the ATV, so the reverse key has the lane clear.
    const auto driveStart = std::chrono::steady_clock::now();
    if (!DriveBack(g_key, why)) {
        Invalid(why + "; " + SayDrive());
        return false;
    }
    SayDrive();
    // The leg's line is said at each block of the key (the host's own built, or a client's applied) and at most once per
    // kDriveLogGapS of them, so a drive that does not move shows where it stands without a line per block.
    const int32_t tag = Tag();
    uint32_t builtCursor = DE::Count(DE::kStampBuilt), appliedCursor = DE::Count(DE::kPoseApplied);
    std::chrono::steady_clock::time_point lastSaid = std::chrono::steady_clock::now();
    float movedCm = 0.f;
    bool probed = false;
    const bool arrived = PollFn("the host's own 600 cm drive", kDriveS, [&]() -> int {
        bool block = false;
        DE::Rec r;
        while (DE::Next(DE::kStampBuilt, builtCursor, r)) block = block || r.i0 == tag;
        while (DE::Next(DE::kPoseApplied, appliedCursor, r)) block = block || r.i0 == tag;
        const auto now = std::chrono::steady_clock::now();
        if (block && now - lastSaid >= std::chrono::seconds(kDriveLogGapS)) {
            lastSaid = now;
            SayDrive();
        }
        FVector at{};
        if (ReadRoot(at)) movedCm = std::sqrt((at.X - start.X) * (at.X - start.X) + (at.Y - start.Y) * (at.Y - start.Y));
        if (movedCm >= kDriveCm) return 1;
        // The throttle's first seconds moved the root nowhere: the ATV is held (a wall, a rock), not slow; said at once.
        if (!probed && now - driveStart >= std::chrono::seconds(kProbeS)) {
            probed = true;
            if (movedCm < kProbeCm) {
                Invalid("the ATV is wedged before the host's drive: the first " + std::to_string(kProbeS) + " s of reverse moved the root " +
                        Num("%.0f", movedCm) + " cm; " + SayDrive());
                return -1;
            }
        }
        return 0;
    }, [] { return "at the bound: " + SayDrive(); });
    if (arrived) SayDrive();
    DriveBackOff(g_key);
    if (!arrived) return false;
    if (Control()) {
        // The control measures the drive with no kerfur on the ATV: a kerfur that rode anyway says nothing about it.
        if (ReadKerfur(g_eid).parentIsAtv) {
            Invalid("control: the host's kerfur rode the ATV although it was idle");
            return false;
        }
        StepPass(7, "control: the host drove " + Num("%.0f", kDriveCm) + " cm with its kerfur idle and off the ATV");
    } else if (!JudgeKerfurSeat('b', "at the end of its own 600 cm drive")) {
        return false;
    }
    PS::Mark("drive7-done");
    return true;
}

bool HostKerfurIdle() {
    bool ran = false;
    OnGt([&] {
        void* kerfur = KerfurActor();
        void* me = AD::Me();
        ran = kerfur && me && K::RunActionName(kerfur, me, L"idle");
    });
    if (!ran) {
        Invalid("control: the game's 'idle' verb did not run on the host's kerfur");
        return false;
    }
    // The verb is recorded by the radial's watch and run on the next net-pump tick (coop/creatures/kerfur_command), so the
    // state is read until it shows.
    if (!PollFn("the host's kerfur idle after the 'idle' verb", kEventS, [&]() -> int {
            KerfurDrive k;
            ReadKerfurDrive(k);
            return k.state == kStateIdle ? 1 : 0;
        }))
        return false;
    UE_LOGI("atv-riders: step 7 control: the host's kerfur eid %u took the game's 'idle' verb before the host sat", g_eid);
    return true;
}

// ---- step 5: the observer's second life ----

// The host's side: it waits the observer's leaving and return, and names the key and the rider again for the new life.
bool HostRejoin() {
    std::string t;
    if (!AwaitMark("o_leaving", t, kSignalS) || !AwaitMark("o2_up", t, kRejoinS)) return false;
    PS::Mark("atv-key2", Narrow(g_key));
    PS::Mark("rider-sat2", std::to_string(g_eid));
    return AwaitMark("o2_s5", t, kRejoinS);
}

// The observer's second life. Its birth and bind holds (coop/dev/atv_riders_join_hold) are on from the start; they are released by its
// first applied ATV stamp, so the stamp is applied first and the owners' birth and bind are what take it.
bool RejoinedObserver() {
    std::string t;
    if (!WorldUp()) return false;
    PS::Mark("o2_up");
    const Cur zero{};
    if (!PollFn("the first applied ATV stamp", kRejoinS, [&] {
            return Find(Recs(zero, DE::kSeatChange), [](const DE::Rec& r) { return OccAfter(r) == kSlotC; }) ? 1 : 0;
        }))
        return false;
    coop::dev::atv_riders_join_hold::ReleaseBirth();
    coop::dev::atv_riders_join_hold::ReleaseBind();
    if (!AwaitMark("atv-key2", t, kSignalS)) return false;
    g_key.assign(t.begin(), t.end());
    if (!AwaitMark("rider-sat2", t, kSignalS) || !ParseMark(t, "rider-sat2", g_eid)) return false;
    const int32_t tag = Tag();
    const uint32_t eid = g_eid;

    // The body: the stamp came first (a held record), and the birth attached it.
    if (!PollFn("the driver's body is born", kRejoinS, [] { return PuppetUp(kSlotC) ? 1 : 0; })) return false;
    const auto bodies = Recs(zero, DE::kBodySeat);
    const auto mineBody = [&](const DE::Rec& r) { return r.i0 == kSlotC && r.i2 == tag; };
    if (!Find(bodies, [&](const DE::Rec& r) { return mineBody(r) && r.i1 == 2; })) {
        Invalid("the stamp did not come before the driver's body (the birth hold did not hold)");
        return false;
    }
    const DE::Rec* attached = Find(bodies, [&](const DE::Rec& r) { return mineBody(r) && r.i1 == 1; });
    if (!attached || attached->f1 < 0.5f || !ReadBody(kSlotC).parentIsAtv) {
        Fail(5, "birth_seat_off", "the table named the driver's seat and its body, once born, is not attached to the ATV");
        return false;
    }
    g_attachCm = attached->f0;
    if (!JudgeRemoteSeat(kSlotC, 5)) return false;
    StepPass(5, "the driver's body, born after the stamp, is attached with the seat pose");

    // The kerfur: decided at the bind when the mirror is shown there, else at its reveal.
    KerfurRead k;
    if (!PollFn("the kerfur mirror's bind", kRejoinS, [&] {
            k = ReadKerfur(eid);
            return k.exists ? 1 : 0;
        }))
        return false;
    const auto rideOf = [&](int code) {
        return [eid, code](const DE::Rec& r) { return static_cast<uint32_t>(r.i0) == eid && r.i1 == code; };
    };
    const bool hiddenAtBind = k.hidden;   // hidden at its bind: the ride waits for the reveal
    const char* route = hiddenAtBind ? "reveal" : "bind";
    if (k.hidden && !PollFn("the kerfur mirror's reveal", kRejoinS, [&] {
            k = ReadKerfur(eid);
            return !k.hidden ? 1 : 0;
        }))
        return false;
    const auto rides = Recs(zero, DE::kRide);
    const bool wasHidden = Find(rides, rideOf(3)) != nullptr;
    // A red proves something only on the route it disables: the reveal's needs a mirror held hidden, the bind's one shown at its
    // bind. On the other route the ride is seated anyway and the step would pass with the red armed.
    const bool redReveal = coop::dev::atv_riders_arm::Is("reveal_ride_off");
    const bool redBind = coop::dev::atv_riders_arm::Is("bind_ride_off");
    if (redReveal && !wasHidden) {
        Invalid("reveal_ride_off route not reached (the mirror was not held hidden: it was seated at its bind)");
        return false;
    }
    if (redBind && hiddenAtBind) {
        Invalid("bind_ride_off route not reached (the mirror was hidden at its bind: the ride is the reveal's)");
        return false;
    }
    if (!Find(rides, rideOf(1))) {
        Fail(5, redReveal ? "reveal_ride_off" : redBind ? "bind_ride_off" : "",
             std::string("the stamp named the kerfur and its mirror, shown after the ") + route + ", was not seated");
        return false;
    }
    StepPass(5, std::string("the kerfur mirror rides after its ") + route);
    PS::Mark("o2_s5");
    return true;
}

}  // namespace coop::dev::atv_riders_drill::detail
