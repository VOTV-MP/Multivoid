// harness/autotest/autotest_glowstick.cpp -- TEST-ONLY: a glowstick stuck to a surface is stuck on every peer.
// The drill is in glowstick_drill.h (what it does, what it shares) and the glowstick_drill_*.cpp files; this file is the
// host's arms and the thread the dispatcher starts.

#include "harness/autotest.h"
#include "glowstick_drill.h"   // co-located private header (src tree, not include/)

namespace harness::autotest {
namespace glowstick_drill {

// The host's verdict, and its release of the client.
void Verdict(bool pass, const char* fmt, ...) {
    char reason[384] = {};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(reason, sizeof(reason), fmt, args);
    va_end(args);
    RV::Say(TagStr().c_str(), pass ? RV::Outcome::Pass : RV::Outcome::Fail, "%s", reason);
    PS::Mark("host_done");
    UE_LOGI("glowstick: DONE on host");
}

bool HostSpawns(std::wstring& key, std::wstring& boxKey, FVector& at, void*& box, FVector& floorSpot) {
    if (!disc_drill::HostWorldUp() || !PS::Await("client_ready", 480)) return false;
    if (!PS::WaitFor("the client's slot world-ready", 120, [] {
            bool ready = false;
            OnGameThread([&] { ready = harness::session_runtime::Session().IsSlotWorldReady(1); });
            return ready;
        }))
        return false;
    if (!Wait("the client's puppet with a pose", 240, [] {
            bool up = false;
            OnGameThread([&] {
                void* b = nullptr;
                FVector l{};
                up = ClientBody(b, l);
            });
            return up;
        }))
        return false;
    // Every arm: a box stands in front of the client and the glowstick rests on its top, so that the camera is within
    // the grab's reach of it (a glowstick on the floor is past it). A floor stick is made at the floor point beside.
    void* glow = nullptr;
    if (ArmIs("attached")) {
        // No glowstick rests on the box here. The box stands on the floor in front of the client and the glowstick lies on
        // the floor near it, as a player finds one: the client takes it into its hand and sticks it to the box's side.
        if (!SpawnAhead(kBoxPath, kBoxFarCm, 0.f, box, boxKey, &floorSpot) || !SpawnAhead(kGlowPath, kGlowNearCm, 0.f, glow, key, nullptr))
            return false;
        OnGameThread([&] { at = Where(glow); });
        GLOG("HOST spawned '%ls' on the floor at (%.0f,%.0f,%.0f) and the box '%ls' %.0f cm ahead of the client", key.c_str(),
                at.X, at.Y, at.Z, boxKey.c_str(), kBoxFarCm);
        PS::Mark("target", Narrow(key) + " " + Narrow(boxKey) + " " + Num3(at) + " " + Num3(floorSpot));
        return true;
    }
    if (!SpawnAhead(kBoxPath, kBoxAheadCm, ArmIs("world") ? -kSideCm : kSideCm, box, boxKey, &floorSpot)) return false;
    // A cylinder can roll off the box before it rests: the glowstick is spawned again until it rests on the top, three
    // times at most, so the client's grab is within reach of it.
    bool placed = false;
    for (int attempt = 0; attempt < 3 && !placed; ++attempt) {
        if (!SpawnOnTop(kGlowPath, box, glow, key)) return false;
        FVector last{};
        int still = 0;
        if (!Wait("the glowstick at rest on the box", 30, [&] {
                FVector p{};
                OnGameThread([&] { p = Where(glow); });
                still = Dist(p, last) < 0.5f ? still + 1 : 0;
                last = p;
                return still >= 10;
            }))
            return false;
        OnGameThread([&] {
            FVector o{}, e{};
            placed = E::GetActorBounds(box, true, o, e) && std::fabs(last.X - o.X) <= e.X + 5.f && std::fabs(last.Y - o.Y) <= e.Y + 5.f &&
                     last.Z >= o.Z + e.Z - 2.f;
            if (!placed) {
                GLOG("HOST: the glowstick rolled off the box to (%.0f,%.0f,%.0f); spawned again", last.X, last.Y, last.Z);
                ue_wrap::engine::DestroyActor(glow);
                glow = nullptr;
            }
        });
    }
    if (!placed) {
        GiveUp(RV::Outcome::Invalid, "the glowstick rolled off the box three times");
        return false;
    }
    OnGameThread([&] { at = Where(glow); });
    GLOG("HOST spawned '%ls' at (%.0f,%.0f,%.0f) on the box '%ls'; the floor point is (%.0f,%.0f,%.0f)", key.c_str(),
            at.X, at.Y, at.Z, boxKey.c_str(), floorSpot.X, floorSpot.Y, floorSpot.Z);
    PS::Mark("target", Narrow(key) + " " + Narrow(boxKey) + " " + Num3(at) + " " + Num3(floorSpot));
    return true;
}

struct Reported {
    FVector loc{}, glow{}, box{};
    std::string key;
};

bool ParseReported(const std::string& s, Reported& r) {
    std::istringstream in(s);
    return ParseNum3(in, r.loc) && ParseNum3(in, r.glow) && ParseNum3(in, r.box) && static_cast<bool>(in >> r.key);
}

// static / grab / attached: the stick the client made lands on the host (its hold ended, its park empty).
bool HostStickLanded(const std::wstring& key, Reported& rep, const char* what, const char* mark = "client_stuck") {
    // The client's hold is watched from before it grabs: it ends within a second of its stick, so a drive first looked
    // for once the client says it stuck has already come and gone. Decided when the client has said it stuck, its hold was
    // seen on the host and has ended, and the host's park holds nothing for the glowstick: what its record does is done.
    bool seenDriven = false;
    std::string text;
    if (!Wait("the client's stick, its hold ended on the host and its park empty", kStepSec, [&] {
            const HostRead r = ReadHost(key);
            if (r.driven) seenDriven = true;
            return PS::Peek(mark, text) && seenDriven && !r.driven && !r.unsettled;
        }))
        return false;
    if (!ParseReported(text, rep)) {
        GiveUp(RV::Outcome::Invalid, "the client's report did not parse ('%s')", text.c_str());
        return false;
    }
    const HostRead r = ReadHost(key);
    if (r.sticked != 1 || Dist(r.loc, rep.loc) > kSamePointCm) {
        Verdict(false, "%s: the client's stick at (%.0f,%.0f,%.0f) did not land on the host: sticked %d, its point (%.0f,%.0f,%.0f)", what,
                rep.loc.X, rep.loc.Y, rep.loc.Z, r.sticked, r.loc.X, r.loc.Y, r.loc.Z);
        return false;
    }
    OnGameThread([&] { LogConstraint("HOST copy", ByKey(key)); });
    return true;
}

void HostStatic(const std::wstring& key) {
    Reported rep;
    if (!HostStickLanded(key, rep, "static")) return;
    const GJ::dev::Counts j = Judged();
    Verdict(j.refused == 0, "the client's stick landed on the host stuck at its point (%.0f,%.0f,%.0f); the judge refused %u", rep.loc.X,
            rep.loc.Y, rep.loc.Z, j.refused);
}

void HostGrab(const std::wstring& key) {
    Reported rep;
    if (!HostStickLanded(key, rep, "grab")) return;
    PS::Mark("host_stuck");
    // The hold's begin on the host is its drive of that copy starting (the prelude has run by then).
    if (!Wait("the client's second hold begins on the host", kStepSec, [&] { return ReadHost(key).driven; })) return;
    const HostRead r = ReadHost(key);
    // Known at the hold's begin, said there: a copy still stuck then is the failure.
    if (r.sticked != 0) {
        Verdict(false, "the client took the stuck glowstick again: at that hold's begin the host's copy is still sticked=%d", r.sticked);
        return;
    }
    std::string text;
    if (!WaitText("client_regrabbed", text)) return;
    if (text != "0") {
        Verdict(false, "the client took the stuck glowstick again: its own copy sticked=%s after its grab", text.c_str());
        return;
    }
    // The client sticks it again after the regrab: the host's grab-time free record, parked on the grabbing client, must not
    // land on that stick (it is older), so the host's copy must end stuck at the second point.
    Reported again;
    if (!HostStickLanded(key, again, "grab re-stick", "client_restuck")) return;
    Verdict(true, "the client took the stuck glowstick again (the host's copy free at the hold's begin) and stuck it again at (%.0f,%.0f,%.0f): "
                  "the host's copy ended stuck there", again.loc.X, again.loc.Y, again.loc.Z);
}

// world: a stick to world geometry, no keyed prop: the host's copy lands static at the client's point.
void HostWorld(const std::wstring& key) {
    Reported rep;
    if (!HostStickLanded(key, rep, "world")) return;
    const HostRead r = ReadHost(key);
    if (rep.key != "-") {
        GiveUp(RV::Outcome::Invalid, "the client's strike hit the keyed prop '%s', not world geometry: the arm's floor point must hold none", rep.key.c_str());
        return;
    }
    const GJ::dev::Counts j = Judged();
    Verdict(r.stickStatic == 1 && r.key.empty() && j.refused == 0,
            "the client's stick to world geometry at (%.0f,%.0f,%.0f) landed on the host: static=%d, names no prop (key '%ls'); the judge refused %u",
            rep.loc.X, rep.loc.Y, rep.loc.Z, r.stickStatic, r.key.c_str(), j.refused);
}

void HostRefused(const std::wstring& key, bool hostile) {
    // The baseline before the client acts: its intent can be ruled on before its own signal reaches this peer.
    const GJ::dev::Counts j0 = Judged();
    std::string text;
    if (!WaitText("client_stuck", text)) return;
    if (!Wait("the judge's ruling on the client's stick", kStepSec, [&] { return Judged().judged > j0.judged; })) return;
    const GJ::dev::Counts j = Judged();
    if (j.refused == j0.refused) {
        Verdict(false, "the host took the client's %sstick: the judge refused nothing", hostile ? "far " : "");
        return;
    }
    if (j.corrected == j0.corrected) {
        Verdict(false, "the host refused the client's stick and sent the sender no record: its copy stays stuck");
        return;
    }
    std::string freed;
    if (!WaitText("client_freed", freed)) return;
    const HostRead r = ReadHost(key);
    Verdict(r.sticked != 1, "the host refused the client's %sstick and its own record freed the client's copy; the host's copy sticked=%d",
            hostile ? "far " : "", r.sticked);
}

// The box is pushed UP, not along the floor: a glowstick that lies on the box's top is carried along a floor push by
// friction whether a constraint holds it to the box or only to a point on it, and the arm could not tell them apart (the
// attach-key red passed). Lifted, a glowstick hung from the box rises with it; one held at a world point, or free, stays.
void HostAttached(const std::wstring& key, const std::wstring& boxKey, void* box) {
    Reported rep;
    if (!HostStickLanded(key, rep, "attached")) return;
    FVector g0{}, b0{};
    bool simulating = false, pushed = false;
    OnGameThread([&] {
        void* g = ByKey(key);
        g0 = Where(g);
        b0 = Where(box);
        // A push, not a teleport: the box's own velocity, with its simulation on.
        void* mesh = ue_wrap::prop::GetStaticMesh(box);
        simulating = E::IsComponentSimulatingPhysics(mesh);
        if (!simulating) E::SetComponentSimulatePhysics(mesh, true);
        pushed = E::SetComponentLinearVelocity(mesh, 0.f, 0.f, kLiftCmPerSec);
    });
    GLOG("HOST lifts the box '%ls' at %.0f cm/s (it simulated %d, the call ran %d)", boxKey.c_str(), kLiftCmPerSec,
            simulating ? 1 : 0, pushed ? 1 : 0);
    // The peak rise of each, sampled until the box is back at rest: at least a second on, then half a second without a move.
    // A lift that raised the box less than kMovedCm says INVALID with what it raised.
    FVector last = b0;
    int polls = 0, still = 0;
    float riseBox = 0.f, riseGlow = 0.f;
    if (!Wait("the lifted box comes back to rest", 60, [&] {
            FVector b{}, g{};
            OnGameThread([&] {
                b = Where(box);
                g = Where(ByKey(key));
            });
            if (b.Z - b0.Z > riseBox) riseBox = b.Z - b0.Z;
            if (g.Z - g0.Z > riseGlow) riseGlow = g.Z - g0.Z;
            still = Dist(b, last) < 0.5f ? still + 1 : 0;
            last = b;
            return ++polls >= 10 && still >= 5;
        }))
        return;
    GLOG("HOST the box '%ls' rose %.0f cm at its peak; its glowstick %.0f cm", boxKey.c_str(), riseBox, riseGlow);
    if (riseBox < kMovedCm) {
        // A box that did not rise has a reason, read from the host copy's stick state: a stick made static (to the world) or
        // attached to another prop than the box pins the box down, and that is the defect (the attach-key red), not a lift that
        // failed to run; a copy attached to the box that still did not rise is a lift that did not take.
        const HostRead st = ReadHost(key);
        if (st.stickStatic == 1 || st.key != boxKey) {
            Verdict(false, "the host's glowstick is stuck to the world, not the box (it held the box down: rose %.0f cm; static=%d, names '%ls')",
                    riseBox, st.stickStatic, st.key.c_str());
            return;
        }
        GiveUp(RV::Outcome::Invalid, "the lift at %.0f cm/s raised the box only %.0f cm (it simulated %d, the call ran %d)", kLiftCmPerSec,
               riseBox, simulating ? 1 : 0, pushed ? 1 : 0);
        return;
    }
    // A stick hung from the box rises with it, and the glowstick's origin swings about the stick point (35 cm): at least half
    // the box's rise follows it, where a stick held at a world point or a free glowstick rises by its swing alone.
    if (riseGlow < riseBox * kFollowShare) {
        Verdict(false, "the box rose %.0f cm and the host's glowstick %.0f cm: it did not follow", riseBox, riseGlow);
        return;
    }
    Verdict(true, "the lifted box rose %.0f cm and the host's glowstick, hung from it by the lane's landing, rose %.0f cm", riseBox, riseGlow);
}

void HostJoiner(const std::wstring& key) {
    Reported rep;
    if (!HostStickLanded(key, rep, "joiner")) return;
    std::string text;
    if (!PS::Await("client_done", 240)) return;
    const uint32_t life1Gen = harness::session_runtime::Session().peerGenerationForSlot(1);
    PS::Mark("host_ack");
    if (!PS::WaitFor("the client's life 1 gone from slot 1", 300, [life1Gen] {
            auto& s = harness::session_runtime::Session();
            return !s.IsSlotConnected(1) || s.peerGenerationForSlot(1) != life1Gen;
        }))
        return;
    if (!PS::WaitFor("the rejoined client's world ready", 400, [life1Gen] {
            bool ready = false;
            OnGameThread([&] {
                auto& s = harness::session_runtime::Session();
                ready = s.IsSlotWorldReady(1) && s.peerGenerationForSlot(1) != life1Gen;
            });
            return ready;
        }))
        return;
    PS::Mark("rejoin_target", Narrow(key) + " " + Num3(rep.loc));
    if (!WaitText("rejoin_done", text, kStepSec)) return;
    std::istringstream in(text);
    int sticked = -1;
    unsigned landed = 0, made = 0;
    FVector loc{};
    int held = -1;
    float dRest = 0.f, dFinal = 0.f, travelled = 0.f;
    in >> sticked >> landed >> made;
    ParseNum3(in, loc);
    in >> held >> dRest >> dFinal >> travelled;
    const bool pass = sticked == 1 && Dist(loc, rep.loc) <= kSamePointCm && held == 1;
    Verdict(pass, "the rejoined client's copy: sticked=%d, %u stuck record(s) landed (%u made by the lane), its point is %.0f cm from the client's; "
                  "nudged, it was %.1f cm from the stick point at rest and %.1f after, travelling up to %.1f cm: %s", sticked, landed, made, Dist(loc, rep.loc),
            dRest, dFinal, travelled, held == 1 ? "held by a constraint" : "free");
}

// joiner_window: the stick is made AFTER the rejoining client's save is cut and BEFORE its world is ready. The host sticks
// its own copy, the glowstick's own strike and stickTo as the client's life 1 does in the other arms; the product serves
// that stick to the joiner through the snapshot's record of the glowstick and the lane's landing of it (a stick the joiner's
// save did not carry). The joiner passes on the stick made by the lane: a stick its save carried is restored by the game.
void HostJoinerWindow(const std::wstring& key, const FVector& floorSpot) {
    if (!PS::Await("client_done", 240)) return;
    const uint32_t life1Gen = harness::session_runtime::Session().peerGenerationForSlot(1);
    PS::Mark("host_ack");
    if (!PS::WaitFor("the client's life 1 gone from slot 1", 300, [life1Gen] {
            auto& s = harness::session_runtime::Session();
            return !s.IsSlotConnected(1) || s.peerGenerationForSlot(1) != life1Gen;
        }))
        return;
    bool closed = false;
    if (!Wait("the rejoining client's save cut with its world not ready", 400, [&] {
            bool open = false;
            OnGameThread([&] {
                auto& s = harness::session_runtime::Session();
                if (s.peerGenerationForSlot(1) == life1Gen) return;
                closed = s.IsSlotWorldReady(1);
                open = !closed && coop::join_window_baseline::HasCapture(1);
            });
            return open || closed;
        }))
        return;
    if (closed) {
        GiveUp(RV::Outcome::Invalid, "the rejoining client's world was ready before the host's stick: the window closed");
        return;
    }
    GLOG("HOST the rejoining client's save is cut and its world is not ready -- the host's glowstick strikes");
    if (!Stick(key, floorSpot)) return;
    bool stillOpen = false;
    FVector loc{};
    OnGameThread([&] {
        stillOpen = !harness::session_runtime::Session().IsSlotWorldReady(1);
        loc = LocOf(ByKey(key));
    });
    GLOG("HOST stuck '%ls' at (%.0f,%.0f,%.0f) with the client's world %s", key.c_str(), loc.X, loc.Y, loc.Z,
            stillOpen ? "not ready" : "ready");
    if (!stillOpen) {
        GiveUp(RV::Outcome::Invalid, "the client's world was ready before the host's stick was made: the window closed");
        return;
    }
    PS::Mark("rejoin_target", Narrow(key) + " " + Num3(loc));
    std::string text;
    if (!WaitText("rejoin_done", text, kStepSec)) return;
    std::istringstream in(text);
    int sticked = -1;
    unsigned landed = 0, made = 0;
    FVector got{};
    int held = -1;
    float dRest = 0.f, dFinal = 0.f, travelled = 0.f;
    in >> sticked >> landed >> made;
    ParseNum3(in, got);
    in >> held >> dRest >> dFinal >> travelled;
    const bool pass = sticked == 1 && landed >= 1 && Dist(got, loc) <= kSamePointCm && held == 1;
    Verdict(pass, "the stick the host made in the join window: the joiner's copy sticked=%d, %u stuck record(s) landed (%u stick(s) made by the lane), its point is %.0f cm from the host's; "
                  "nudged, it was %.1f cm from the stick point at rest and %.1f after, travelling up to %.1f cm: %s", sticked, landed, made, Dist(got, loc),
            dRest, dFinal, travelled, held == 1 ? "held by a constraint" : "free");
}

void RunHost() {
    std::wstring key, boxKey;
    FVector at{};
    void* box = nullptr;
    FVector floorSpot{};
    if (!HostSpawns(key, boxKey, at, box, floorSpot)) return;
    if (ArmIs("static")) HostStatic(key);
    else if (ArmIs("attached")) HostAttached(key, boxKey, box);
    else if (ArmIs("world")) HostWorld(key);
    else if (ArmIs("grab")) HostGrab(key);
    else if (ArmIs("refused")) HostRefused(key, false);
    else if (ArmIs("hostile")) HostRefused(key, true);
    else if (ArmIs("joiner")) HostJoiner(key);
    else if (ArmIs("joiner_window")) HostJoinerWindow(key, floorSpot);
}

}  // namespace glowstick_drill

DWORD WINAPI GlowstickThread(LPVOID /*arg*/) {
    using namespace glowstick_drill;
    static const std::string tag = TagStr();   // Begin keeps the pointer
    if (!PS::Begin(tag.c_str())) return 0;
    if (!IsClientRole()) RunHost();
    else if ((ArmIs("joiner") || ArmIs("joiner_window")) && coop::config::ReadEnv("VOTVCOOP_GLOWSTICK_DRILL_REJOIN") == "1") RunRejoined();
    else RunClient();
    return 0;
}

}  // namespace harness::autotest
