// coop/dev/atv_riders_drill_steps.cpp -- the reads and the steps of the ATV riders drill, each decided on the records of
// coop/interactables/atv_drill_events.h and on a read of the body or the kerfur at the decision's event; see
// coop/dev/atv_riders_drill.h and the order in atv_riders_drill.cpp. A step polls the rings from the cursor taken before its
// act, so a verdict is a function of what was recorded since, and the order of two records is their `ord`.

#include "atv_riders_drill_detail.h"

#include "atv_gate_drill_detail.h"   // co-located private header of the gate drill: ChooseAtv
#include "atv_seat.h"                // co-located private header: the game's own seat verb

#include "coop/creatures/npc_sync.h"
#include "coop/dev/atv_riders_arm.h"
#include "coop/dev/atv_riders_join_hold.h"
#include "coop/dev/spawn_npc.h"
#include "coop/element/mirror_defer.h"
#include "coop/element/mirror_managers.h"
#include "coop/element/registry.h"
#include "coop/interactables/atv_sync.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/save/save_transfer.h"
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

}  // namespace

bool ReadRoot(FVector& out) {
    bool ok = false;
    OnGt([&] {
        void* atv = AD::Atv(g_key);
        FRotator rot{};
        ok = atv && ue_wrap::atv::GetRootTransform(atv, out, rot);
    });
    return ok;
}

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

// The kerfur step 0 spawned, 0 when step 0 did not run.
static uint32_t g_firstJoinEid = 0;

// Spawns a kerfur 200 cm beside the ATV under g_key and waits for its element id.
static bool SpawnKerfurBesideAtv(uint32_t& eid) {
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
    return true;
}

// The kerfur the host's seat edge invites: step 0's when that ran, else one spawned beside the ATV. Its eid is posted.
bool HostFixture() {
    uint32_t eid = g_firstJoinEid;
    if (eid == 0 && !SpawnKerfurBesideAtv(eid)) return false;
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

// ---- step 0: the reveal route, on the driver's first join ----

// The host's side. The join window is read as the state the host keeps for the slot (the world taken, not yet ready): the
// save transfer announces it to no subscriber, so the drill thread polls it as the six join drills of coop/dev do.
bool HostFirstJoin() {
    if (!PollFn("the driver's join window (its world taken, not yet ready)", kJoinS, [&]() -> int {
            int state = 0;   // 1 the window is open, -1 the driver's world was ready before the host saw it open
            OnGt([&] {
                const coop::net::Session* s = coop::npc_sync::GetSession();
                if (!s) return;
                if (s->IsSlotWorldReady(kSlotC)) state = -1;
                else if (coop::save_transfer::WorldTakenFor(kSlotC)) state = 1;
            });
            if (state < 0) Invalid("step 0: the driver's world was ready before the host saw its join window");
            return state;
        }))
        return false;
    g_key = coop::dev::atv_gate_drill::detail::ChooseAtv();
    if (g_key.empty()) {
        Invalid("the world holds no ATV a route from the host reaches");
        return false;
    }
    UE_LOGI("atv-riders: step 0: the driver's join window is open, the host's ATV key='%ls'", g_key.c_str());
    uint32_t eid = 0;
    if (!SpawnKerfurBesideAtv(eid)) return false;
    g_firstJoinEid = eid;
    const Cur cur = Snap();
    int invited = -1;
    if (!SitLocal(invited)) {
        Invalid("step 0: the host's seat verb did not seat the host");
        return false;
    }
    const int32_t tag = Tag();
    if (!PollFn("the host's kerfur's seat in the join window", kRideS, [&]() -> int {
            const auto facts = Recs(cur, DE::kRiderFact);
            const DE::Rec* f = Find(facts, [&](const DE::Rec& r) { return r.i0 == tag && r.i2 == 1; });
            if (!f) return 0;
            if (static_cast<uint32_t>(f->i1) != eid) {
                Invalid("step 0: the host's seat took the kerfur eid " + std::to_string(f->i1) + ", not the spawned " +
                        std::to_string(eid));
                return -1;
            }
            return 1;
        }))
        return false;
    UE_LOGI("atv-riders: step 0: the host sits with its kerfur eid %u; waiting for the driver's reveal", eid);
    std::string t, why;
    if (!AwaitMark("s0_done", t, kJoinS, "the driver decides at its reveal")) return false;
    if (!AD::Brake(g_key, why) || !AD::Leave(g_key, why)) {
        Invalid(why);
        return false;
    }
    g_eid = eid;
    return PollFn("the host's kerfur, off the ATV", kEventS, [&]() -> int {
        const KerfurRead k = ReadKerfur(eid);
        return k.exists && !k.parentIsAtv ? 1 : 0;
    });
}

// The driver's side, decided at the reveal from its own ride records and reads: the host's kerfur is the one the ride owner has a
// record for. Hidden at its bind (read when first bound, or a held-hidden record) is the reveal route; a mirror bound shown is
// not reached.
bool DriverFirstJoin() {
    const Cur zero{};
    uint32_t eid = 0;
    KerfurRead k;
    if (!PollFn("the host's kerfur mirror and its ride row", kJoinS, [&]() -> int {
            if (!eid) {
                const auto rides = Recs(zero, DE::kRide);
                if (const DE::Rec* r = Find(rides, [](const DE::Rec& x) { return x.i1 >= 1 && x.i1 <= 3; }))
                    eid = static_cast<uint32_t>(r->i0);
            }
            if (!eid) return 0;
            k = ReadKerfur(eid);
            return k.exists ? 1 : 0;
        }))
        return false;
    const auto rideOf = [eid](int code) {
        return [eid, code](const DE::Rec& r) { return static_cast<uint32_t>(r.i0) == eid && r.i1 == code; };
    };
    if (!k.hidden && !Find(Recs(zero, DE::kRide), rideOf(3))) {
        Invalid("step 0: the reveal route not reached (the kerfur mirror bound after the lift)");
        return false;
    }
    if (k.hidden && !PollFn("the kerfur mirror's reveal", kJoinS, [&]() -> int {
            k = ReadKerfur(eid);
            return !k.hidden ? 1 : 0;
        }))
        return false;
    if (!Find(Recs(zero, DE::kRide), rideOf(1))) {
        Fail(0, coop::dev::atv_riders_arm::Is("reveal_ride_off") ? "reveal_ride_off" : "",
             "the kerfur mirror was shown, a ride row was stored for it and no ride was issued");
        return false;
    }
    StepPass(0, "the kerfur mirror, hidden at its bind, rides from the reveal");
    PS::Mark("s0_done");
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
