// coop/dev/atv_riders_drill_late.cpp -- the steps of the ATV riders drill after the rejoin: the driver's leaving (6), its
// ragdoll while seated (8), the host's seat (7) and the ATV's destruction (9); see coop/dev/atv_riders_drill.h and the order in
// atv_riders_drill.cpp. Decided like the steps before them (atv_riders_drill_steps.cpp): on the records since the cursor taken
// before the act, and a read of the body or the kerfur at the deciding event.

#include "atv_riders_drill_detail.h"

#include "coop/dev/atv_riders_arm.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::dev::atv_riders_drill::detail {

namespace E = ue_wrap::engine;

// ---- step 6: the driver leaves ----

bool DriverLeave() {
    std::string why;
    if (!AD::Brake(g_key, why) || !AD::Leave(g_key, why)) {
        Invalid(why);
        return false;
    }
    return true;
}

bool LeaveHost(const Cur& cur) {
    const int32_t tag = Tag();
    bool pre = false, released = false, body = false;
    return PollFn("the driver's leaving on the host", kEventS, [&]() -> int {
        const auto seats = Recs(cur, DE::kSeatChange);
        if (SeatSilent(seats)) return -1;
        if (Find(seats, [&](const DE::Rec& r) { return r.i3 == tag && OccBefore(r) == kSlotC && OccAfter(r) != kSlotC; }) &&
            !body) {
            if (ReadBody(kSlotC).parentIsAtv) {
                Fail(6, "release_seat_off", "the driver's seat ended and its body is still attached to the ATV");
                return -1;
            }
        }
        pre = pre || Find(Recs(cur, DE::kRiderFact), [&](const DE::Rec& r) { return r.i0 == tag && r.i2 == 0; }) != nullptr;
        released = released || Find(Recs(cur, DE::kInvite), [&](const DE::Rec& r) { return r.i0 == tag && r.i1 == 0; }) != nullptr;
        for (const DE::Rec& r : Recs(cur, DE::kBodySeat))
            if (r.i0 == kSlotC && r.i1 == 0) {
                if (!ReleasedRec(r, kSlotC)) {
                    Fail(6, "", "the driver's body was released with its seat pose or collision wrong");
                    return -1;
                }
                body = true;
            }
        if (pre && released && body) {
            StepPass(6, "the kerfur got off, the kerfurs were released and the driver's body was released");
            return 1;
        }
        return 0;
    });
}

// On a client: the kerfur's ride ends and, on the observer, the driver's body is released; on the driver also its own dismount
// ran the game's getOffCar on the riding mirror and the gate cancelled it. The release is judged on the state the rider
// subscriber left when the stamp's apply cleared the rider (DE::kRideCleared), read at that event and never later: the eid has
// no ride row, its pose is not held by the ride owner, and the mirror is attached to nothing.
bool LeaveWatch(Role me, const Cur& cur) {
    const int32_t tag = Tag();
    const uint32_t eid = g_eid;
    bool off = false, body = me != kObserver, released = false;
    return PollFn("the driver's leaving on a client", kEventS, [&]() -> int {
        const auto seats = Recs(cur, DE::kSeatChange);
        if (SeatSilent(seats)) return -1;
        if (me == kObserver && !body &&
            Find(seats, [&](const DE::Rec& r) { return r.i3 == tag && OccBefore(r) == kSlotC && OccAfter(r) != kSlotC; }) &&
            ReadBody(kSlotC).parentIsAtv) {
            Fail(6, "release_seat_off", "the driver's seat ended and its body is still attached to the ATV");
            return -1;
        }
        if (!off && Find(seats, [&](const DE::Rec& r) {
                return r.i3 == tag && static_cast<uint32_t>(r.f0) == eid && r.i1 == 0;
            }) &&
            ReadKerfur(eid).parentIsAtv) {
            Fail(6, "release_ride_off", "the rider's seat ended and the kerfur mirror is still attached to the ATV");
            return -1;
        }
        const auto rides = Recs(cur, DE::kRide);
        if (!off)
            if (const DE::Rec* r = Find(rides, [&](const DE::Rec& x) {
                    return static_cast<uint32_t>(x.i0) == eid && x.i1 == 0;
                })) {
                if (me == kDriver && r->f1 < 0.5f) {
                    Fail(6, "getoff_cancel_off", "the driver's own dismount had already unseated the kerfur mirror");
                    return -1;
                }
                if (me == kDriver && !Find(Recs(cur, DE::kMirrorCancel), [](const DE::Rec& x) { return x.i0 == 0; })) {
                    Fail(6, "getoff_cancel_off", "the driver's own dismount ran getOffCar on the kerfur mirror, uncancelled");
                    return -1;
                }
                off = true;
            }
        if (!body)
            for (const DE::Rec& r : Recs(cur, DE::kBodySeat))
                if (r.i0 == kSlotC && r.i1 == 0) {
                    if (!ReleasedRec(r, kSlotC)) {
                        Fail(6, "", "the driver's body was released with its seat pose or collision wrong");
                        return -1;
                    }
                    body = true;
                }
        const auto cleared = Recs(cur, DE::kRideCleared);
        if (!released)
            if (const DE::Rec* r = Find(cleared, [&](const DE::Rec& x) {
                    return static_cast<uint32_t>(x.i0) == eid && x.i1 == tag;
                })) {
                released = true;
                if (r->i2 != 0) {
                    std::string held;
                    if (r->i2 & 1) held += "the ride row is kept";
                    if (r->i2 & 2) held += std::string(held.empty() ? "" : ", ") + "its pose is still held by the ride owner";
                    if (r->i2 & 4) held += std::string(held.empty() ? "" : ", ") + "the mirror is still attached to an actor";
                    Fail(6, "unseat_latch_off", "the rider's seat ended and the kerfur mirror is not released: " + held);
                    return -1;
                }
            }
        if (off && body && released) {
            StepPass(6, "the kerfur mirror is released: no ride row, its pose is the stream's, attached to nothing");
            return 1;
        }
        return 0;
    });
}

// ---- step 8: the driver ragdolls while seated ----

// The driver sits again; once the host and the observer show the body seated it ragdolls, and it gets up when both have shown
// the flop.
bool DriverRagdoll() {
    std::string t;
    int invited = -1;
    if (!SitLocal(invited)) {
        Invalid("the driver could not sit again for the ragdoll");
        return false;
    }
    if (!AwaitMark("h_s8seat", t, kEventS) || !AwaitMark("o_s8seat", t, kEventS)) return false;
    bool ok = false;
    OnGt([&] { ok = E::SetMainPlayerRagdollMode(AD::Me(), true, false, false); });
    if (!ok) {
        Invalid("the ragdoll call did not run on the driver");
        return false;
    }
    if (!AwaitMark("h_flop", t, kEventS * 2) || !AwaitMark("o_flop", t, kEventS * 2)) return false;
    OnGt([&] { E::ForceMainPlayerGetUp(AD::Me()); });
    return true;
}

// The host and the observer: the re-sat body, then the flop and its stop. At the stop the body must be released, by the seat's
// end when it came before the flop or by the stop's restore when it came during it; the order is logged.
bool RagdollWatch(Role me, const Cur& cur) {
    const int32_t tag = Tag();
    const std::string who = me == kHost ? "h" : "o";
    if (!PollFn("the driver's body, seated again", kRideS, [&]() -> int {
            if (SeatSilent(Recs(cur, DE::kSeatChange))) return -1;
            if (!Find(Recs(cur, DE::kBodySeat), [&](const DE::Rec& r) { return r.i0 == kSlotC && r.i1 == 1 && r.i2 == tag; }))
                return 0;
            if (!ReadBody(kSlotC).parentIsAtv) {
                Fail(8, "", "the driver, seated again, has no body attached to the ATV");
                return -1;
            }
            OnGt([] { SayRemoteSeatOffset(kSlotC); });
            return 1;
        }))
        return false;
    const Cur c8 = Snap();
    PS::Mark((who + "_s8seat").c_str());

    bool endFirst = false;
    const bool redRestore = coop::dev::atv_riders_arm::Is("ragdoll_restore_off");
    if (!PollFn("the driver's flop", kEventS * 2, [&]() -> int {
            if (!ReadBody(kSlotC).ragdoll) return 0;
            endFirst = Find(Recs(c8, DE::kSeatChange), [&](const DE::Rec& r) {
                           return r.i3 == tag && OccBefore(r) == kSlotC && OccAfter(r) != kSlotC;
                       }) != nullptr;
            UE_LOGI("atv-riders: step 8 order: the driver's seat end arrived %s the flop", endFirst ? "before" : "during");
            return 1;
        }))
        return false;
    PS::Mark((who + "_flop").c_str());

    return PollFn("the flop's stop", kEventS * 2, [&]() -> int {
        const BodyRead b = ReadBody(kSlotC);
        if (b.ragdoll) return 0;
        // The seat's end decides the order; a stop before it is not yet the moment to judge.
        if (!Find(Recs(c8, DE::kSeatChange), [&](const DE::Rec& r) {
                return r.i3 == tag && OccBefore(r) == kSlotC && OccAfter(r) != kSlotC;
            }))
            return 0;
        // The red's mechanism only leaves the body seated when the seat end arrives during the flop; after it the seat's own
        // end has released the body, so a PASS would prove nothing about the stop.
        if (redRestore && endFirst) {
            Invalid("ragdoll_restore_off route not reached (the seat ended before the flop)");
            return -1;
        }
        const bool released = Find(Recs(c8, DE::kBodySeat), [&](const DE::Rec& r) { return ReleasedRec(r, kSlotC); }) != nullptr;
        if (!released || b.parentIsAtv || b.onAtv || !b.collision) {
            Fail(8, "ragdoll_restore_off", std::string("the flop stopped with the body not restored (the seat end came ") +
                                               (endFirst ? "before" : "during") + " the flop)");
            return -1;
        }
        StepPass(8, std::string("the body was restored at the flop's stop (the seat end came ") + (endFirst ? "before" : "during") +
                        " the flop)");
        return 1;
    });
}

// ---- step 7: the host sits ----

// The reading of one peer's kerfur, as the text a mark and a FAIL line carry.
static std::string Reading(const KerfurRead& k, bool withSitarrow) {
    char buf[320];
    int n = std::snprintf(buf, sizeof(buf), "%.1f cm from sitarrow, offset from the ATV root (%.0f, %.0f, %.0f)",
                          static_cast<double>(k.sitarrowCm), static_cast<double>(k.off[0]), static_cast<double>(k.off[1]),
                          static_cast<double>(k.off[2]));
    if (withSitarrow && n > 0 && static_cast<size_t>(n) < sizeof(buf))
        std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n),
                      ", the sitarrow component's own offset from the root (%.0f, %.0f, %.0f)", static_cast<double>(k.sitOff[0]),
                      static_cast<double>(k.sitOff[1]), static_cast<double>(k.sitOff[2]));
    return buf;
}

bool JudgeKerfurSeat(char leg, const char* when) {
    const uint32_t eid = g_eid;
    const KerfurRead k = ReadKerfur(eid);
    if (!k.exists || k.sitarrowCm < 0.f) {
        Invalid(std::string("step 7: the kerfur eid ") + std::to_string(eid) + " does not read on this peer (" + when + ")");
        return false;
    }
    const std::string reading = Reading(k, MyRole() == kHost);
    UE_LOGI("atv-riders: step 7 (%s) on the %s: the kerfur eid %u is %s", when, RoleName(MyRole()), eid, reading.c_str());
    const std::string tag = std::string("s7") + leg + "_";
    PS::Mark((tag + RoleName(MyRole())).c_str(), reading);
    if (k.sitarrowCm <= kRigidCm) {
        StepPass(7, std::string("the kerfur is within ") + Num("%.0f", kRigidCm) + " cm of sitarrow " + when + ": " + reading);
        return true;
    }
    // The readings the other peers have already left (no waiting: a FAIL known is said at once).
    std::string others;
    for (int r = 0; r < kRoles; ++r) {
        std::string t;
        if (r != MyRole() && PS::Peek((tag + RoleName(static_cast<Role>(r))).c_str(), t))
            others += std::string("; the ") + RoleName(static_cast<Role>(r)) + " read " + t;
    }
    Fail(7, "host_kerfur_off_seat", std::string(when) + ": the kerfur eid " + std::to_string(eid) + " is " + reading + others);
    return false;
}

// The host sits. The kerfur it seats is one that came off a client's ride (step 6's release, and the driver's second seat of step
// 8), so the host first reads that the kerfur no longer rides; then its own game seats it, and the kerfur's place is read at the
// host's next block for the key and at the end of a 600 cm drive of the host's own.
bool HostSits(const Cur& cur) {
    if (!PollFn("the host's kerfur, off the ATV", kEventS, [&]() -> int {
            const KerfurRead k = ReadKerfur(g_eid);
            return k.exists && !k.parentIsAtv ? 1 : 0;
        }))
        return false;
    const bool control = Control();
    if (control && !HostKerfurIdle()) return false;
    int invited = -1;
    if (!SitLocal(invited)) {
        Invalid("the host's seat verb did not seat the host");
        return false;
    }
    // The control's kerfur takes no seat: the host's drive is read with no kerfur on the ATV.
    if (control) return HostDriveSeated();
    const int32_t tag = Tag();
    uint32_t factOrd = 0;
    if (!PollFn("the host's kerfur's seat", kRideS, [&]() -> int {
            const auto facts = Recs(cur, DE::kRiderFact);
            const DE::Rec* f = Find(facts, [&](const DE::Rec& r) { return r.i0 == tag && r.i2 == 1; });
            if (!f) return 0;
            factOrd = f->ord;
            g_eid = static_cast<uint32_t>(f->i1);
            PS::Mark("rider-sat3", std::to_string(g_eid));
            UE_LOGI("atv-riders: the host's kerfur eid %u sat on the host's seat", g_eid);
            return 1;
        }))
        return false;
    if (!PollFn("the host's next block after its kerfur's seat", kEventS, [&]() -> int {
            return Find(Recs(cur, DE::kStampBuilt), [&](const DE::Rec& r) { return r.i0 == tag && r.ord > factOrd; }) ? 1 : 0;
        }))
        return false;
    return JudgeKerfurSeat('a', "at the host's next block after its kerfur's seat") && HostDriveSeated();
}

// A client: the host's body is seated on this peer, and the kerfur rides again.
bool WatchHostSeat(Role me, const Cur& cur) {
    const int32_t tag = Tag();
    (void)me;
    if (!PollFn("the host's body", kRideS, [&]() -> int {
            if (SeatSilent(Recs(cur, DE::kSeatChange))) return -1;
            const auto bodies = Recs(cur, DE::kBodySeat);
            if (Find(bodies, [&](const DE::Rec& r) { return r.i0 == kSlotH && r.i1 == 3; })) {
                Fail(7, "", "the seat owner could not seat the host's body");
                return -1;
            }
            const DE::Rec* a = Find(bodies, [&](const DE::Rec& r) { return r.i0 == kSlotH && r.i1 == 1 && r.i2 == tag; });
            if (!a) return 0;
            const BodyRead b = ReadBody(kSlotH);
            if (a->f1 < 0.5f || !b.parentIsAtv) {
                Fail(7, "", "the host's body is not attached to the ATV with the seat pose");
                return -1;
            }
            if (!JudgeRemoteSeat(kSlotH, 7)) return -1;
            StepPass(7, "the host's body is attached to the ATV with the seat pose");
            return 1;
        }))
        return false;
    std::string t;
    // The control's host kerfur takes no seat: the clients wait for the end of the host's drive and read no kerfur.
    if (Control()) return AwaitMark("drive7-done", t, kDriveS + kSignalS);
    if (!AwaitMark("rider-sat3", t, kRideS) || !ParseMark(t, "rider-sat3", g_eid)) return false;
    const uint32_t eid = g_eid;
    if (!PollFn("the kerfur's ride on the host's seat", kRideS, [&]() -> int {
            return Find(Recs(cur, DE::kRide), [&](const DE::Rec& r) { return static_cast<uint32_t>(r.i0) == eid && r.i1 == 1; }) ? 1 : 0;
        }))
        return false;
    // The mirror stands on the seat where the host's kerfur does: read when the ride was applied, and again at the end of the
    // host's drive.
    return JudgeKerfurSeat('a', "where the ride reached this peer") && AwaitMark("drive7-done", t, kDriveS + kSignalS) &&
           JudgeKerfurSeat('b', "at the end of the host's 600 cm drive");
}

// ---- step 9: the ATV is destroyed ----

bool HostDestroys() {
    bool ok = false;
    OnGt([&] {
        void* atv = AD::Atv(g_key);
        ok = atv && E::DestroyActor(atv);
    });
    if (!ok) {
        Invalid("the host's ATV was not destroyed");
        return false;
    }
    PS::Mark("h_destroyed");
    UE_LOGI("atv-riders: the host's ATV is destroyed; its own kerfur is the game's and is not judged");
    return true;
}

// A client: when the ATV's entry is gone every seat and ride on it has ended. The gone state is read before the records, so the
// exit and what it released are already recorded when it reads true.
bool WatchExit(Role me, const Cur& cur) {
    std::string t;
    if (!AwaitMark("h_destroyed", t, kSignalS)) return false;
    const int32_t tag = Tag();
    const uint32_t eid = g_eid;
    const bool control = Control();   // no kerfur rides the host's ATV in the control, so no ride ends with it
    return PollFn("the ATV's exit", kEventS, [&]() -> int {
        const bool gone = EntryGone();
        const auto seats = Recs(cur, DE::kSeatChange);
        const bool exit = Find(seats, [&](const DE::Rec& r) { return r.i3 == tag && OccAfter(r) == 0xFF; }) != nullptr;
        const bool body = Find(Recs(cur, DE::kBodySeat), [&](const DE::Rec& r) { return ReleasedRec(r, kSlotH); }) != nullptr;
        const bool ride = control || Find(Recs(cur, DE::kRide), [&](const DE::Rec& r) {
                                         return static_cast<uint32_t>(r.i0) == eid && r.i1 == 0;
                                     }) != nullptr;
        if (exit && body && ride) {
            StepPass(9, std::string("the ATV's exit released the host's body and ended the kerfur's ride (") + RoleName(me) + ")");
            return 1;
        }
        if (!gone) return 0;
        if (!exit) Fail(9, "exit_seat_off", "the ATV's copy is gone and no seat change was announced for its exit");
        else Fail(9, "", std::string("the ATV's exit left the host's body ") + (body ? "released" : "seated") + " and the ride " +
                             (ride ? "ended" : "going"));
        return -1;
    });
}

}  // namespace coop::dev::atv_riders_drill::detail
