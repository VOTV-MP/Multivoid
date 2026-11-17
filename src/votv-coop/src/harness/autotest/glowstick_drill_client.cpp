// harness/autotest/glowstick_drill_client.cpp -- the glowstick drill's client: the grab, the strike, the arms' client legs, the rejoined life.

#include "glowstick_drill.h"   // co-located private header (src tree, not include/)

namespace harness::autotest::glowstick_drill {

// ---- the client sticks ----

// The aimed grab of `g`, one tick per poll, until it says Grabbed or Failed.
bool Grab(const std::wstring& key) {
    std::unique_ptr<D::AimedGrab> grab;
    D::GrabState st = D::GrabState::Working;
    const char* why = "";
    // The PR's probe grabbed a glowstick that had lain on the floor four seconds since the host spawned it; a copy
    // just spawned is still falling or rolling and the use knocks it instead of taking it. The grab waits for the
    // glowstick to be at rest (an event: one second without a move of half a cm).
    FVector last{};
    int still = 0;
    if (!Wait("the glowstick at rest", 60, [&] {
            FVector p{};
            OnGameThread([&] { p = Where(ByKey(key)); });
            still = Dist(p, last) < 0.5f ? still + 1 : 0;
            last = p;
            return still >= 10;
        }))
        return false;
    // The grab is the player's trace, 200 cm (armLength): a glowstick farther than kReachCm from the camera is not
    // grabbed, and the run says so with the distance instead of trying.
    float reach = -1.f;
    OnGameThread([&] { reach = Dist(E::GetCameraLocation(), Where(ByKey(key))); });
    GLOG("the glowstick '%ls' is %.0f cm from the camera at the grab, at rest at (%.1f,%.1f,%.1f)", key.c_str(), reach, last.X,
            last.Y, last.Z);
    if (reach < 0.f || reach > kReachCm) {
        GiveUp(RV::Outcome::Invalid, "the glowstick '%ls' is %.0f cm from the client's camera, past the %.0f cm the grab takes", key.c_str(),
               reach, kReachCm);
        return false;
    }
    FVector before{};   // where it lay at the last poll before the chain ran
    if (!Wait("the aimed grab", 90, [&] {
            OnGameThread([&] {
                void* me = coop::players::Registry::Get().Local();
                void* g = ByKey(key);
                if (!me || !g) {
                    st = D::GrabState::Failed;
                    why = "the player or the glowstick is gone";
                    return;
                }
                if (!grab) grab = std::make_unique<D::AimedGrab>(me, g, ue_wrap::prop::GetStaticMesh(g));
                if (!grab->ChainPre()) before = Where(g);
                st = grab->Tick();
                if (st == D::GrabState::Failed) {
                    why = grab->Why();
                    const FVector cam = E::GetCameraLocation(), after = Where(g);
                    GLOGW("the grab of '%ls' failed -- aimed %d after %d poses, chain %d%d%d, the glowstick %.0f cm from the camera, "
                            "the player's trace hits %p (the glowstick is %p), the hand holds %p; it lay at (%.1f,%.1f,%.1f) before the chain "
                            "and is at (%.1f,%.1f,%.1f) now, %.1f cm moved", key.c_str(), grab->Aimed() ? 1 : 0, grab->AimPoses(),
                            grab->ChainPre() ? 1 : 0, grab->ChainUse() ? 1 : 0, grab->ChainPost() ? 1 : 0, Dist(cam, after),
                            E::ReadMainPlayerHitActor(me), g, D::Grabbing(me), before.X, before.Y, before.Z, after.X, after.Y, after.Z,
                            Dist(before, after));
                }
            });
            return st != D::GrabState::Working;
        }))
        return false;
    if (st == D::GrabState::Failed) {
        GiveUp(RV::Outcome::Invalid, "the client could not grab '%ls': %s", key.c_str(), why);
        return false;
    }
    return true;
}

bool StrikeFrom(void* g, void* me);

// One step of the held glowstick over `base` (the point of the surface it is held over): its tip, tip down, a uu lower
// than the step before; then what its tick runs on a strike. True once it has struck.
bool StepAndStrike(void* g, const FVector& base, int step) {
    void* me = coop::players::Registry::Get().Local();
    if (!me || !g) return false;
    E::SetActorLocation(g, {base.X, base.Y, base.Z + kHoldAboveCm - static_cast<float>(step)});
    E::SetActorRotation(g, {0.f, 0.f, 180.f});
    return StrikeFrom(g, me);
}

// What a strike does: the glowstick's own `ray`, and on a hit what its tick runs (the player's dropGrabObject, then
// stickTo with the ray's hit). True once stickTo ran.
bool StrikeFrom(void* g, void* me) {
    ue_wrap::ParamFrame ray(R::FindDispatchFunctionCached(R::ClassOf(g), L"ray"));
    if (!ray.valid() || !ue_wrap::Call(g, ray) || !ray.Get<bool>(L"ReturnValue")) return false;
    const int32_t size = R::FindParamSize(ray.function(), L"outHit");
    std::vector<uint8_t> hit(static_cast<size_t>(size > 0 ? size : 0), 0u);
    if (size <= 0 || !ray.GetRaw(L"outHit", hit.data(), size)) return false;
    D::CallOnPlayer(me, L"dropGrabObject");
    ue_wrap::ParamFrame to(R::FindDispatchFunctionCached(R::ClassOf(g), L"stickTo"));
    return to.valid() && to.SetRaw(L"hit", hit.data(), size) && to.Set(L"setFrame", false) && ue_wrap::Call(g, to);
}

// The glowstick's `sticking` arrow: the component its ray starts from and runs along.
void* ArrowOf(void* g) {
    const int32_t so = g ? R::FindPropertyOffset(R::ClassOf(g), L"sticking") : -1;
    void* arrow = nullptr;
    if (so >= 0) std::memcpy(&arrow, static_cast<const uint8_t*>(g) + so, sizeof(arrow));
    return arrow && R::IsLive(arrow) ? arrow : nullptr;
}

bool ArrowForward(void* arrow, FVector& out) {
    ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(arrow), L"GetForwardVector"));
    if (!f.valid() || !ue_wrap::Call(arrow, f)) return false;
    out = f.Get<FVector>(L"ReturnValue");
    return true;
}

// The glowstick turned so that its ray runs along `dir`: of the right-angle orientations, the one whose arrow reads closest
// to it (the arrow's axis in the actor's frame is the asset's, not assumed). False when none is within 0.98.
bool AimArrow(void* g, const FVector& dir) {
    void* arrow = ArrowOf(g);
    if (!arrow) return false;
    float best = -2.f;
    ue_wrap::FRotator bestR{};
    for (int p = 0; p < 4; ++p)
        for (int y = 0; y < 4; ++y)
            for (int r = 0; r < 4; ++r) {
                const ue_wrap::FRotator rot{90.f * p, 90.f * y, 90.f * r};
                FVector fwd{};
                if (!E::SetActorRotation(g, rot) || !ArrowForward(arrow, fwd)) continue;
                const float dot = fwd.X * dir.X + fwd.Y * dir.Y + fwd.Z * dir.Z;
                if (dot > best) {
                    best = dot;
                    bestR = rot;
                }
            }
    E::SetActorRotation(g, bestR);
    return best >= 0.98f;
}

// One step of the held glowstick at the box's side face that looks toward the player: its arrow turned into the box, its tip
// `14 - step` cm off the face at the face's mid height, a cm closer than the step before; then a strike.
bool StepAndStrikeSide(void* g, void* box, int step) {
    void* me = coop::players::Registry::Get().Local();
    FVector o{}, e{};
    if (!me || !g || !box || !E::GetActorBounds(box, true, o, e)) return false;
    const FVector cam = E::GetCameraLocation();
    FVector into{}, face{};
    if (std::fabs(cam.X - o.X) >= std::fabs(cam.Y - o.Y)) {
        const float s = cam.X > o.X ? 1.f : -1.f;
        into = {-s, 0.f, 0.f};
        face = {o.X + s * e.X, o.Y, o.Z};
    } else {
        const float s = cam.Y > o.Y ? 1.f : -1.f;
        into = {0.f, -s, 0.f};
        face = {o.X, o.Y + s * e.Y, o.Z};
    }
    void* arrow = ArrowOf(g);
    if (!arrow || !AimArrow(g, into)) return false;
    FVector arrowAt{}, actorAt{};
    if (!E::TryGetComponentLocation(arrow, arrowAt) || !E::TryGetActorLocation(g, actorAt)) return false;
    const float gap = 14.f - static_cast<float>(step);
    const FVector tip = {face.X - into.X * gap, face.Y - into.Y * gap, face.Z - into.Z * gap};
    E::SetActorLocation(g, {tip.X - (arrowAt.X - actorAt.X), tip.Y - (arrowAt.Y - actorAt.Y), tip.Z - (arrowAt.Z - actorAt.Z)});
    return StrikeFrom(g, me);
}

// The held glowstick stuck to the box's side by its own ray, and its stick published. False: a verdict was said.
bool StickSide(const std::wstring& key, const std::wstring& boxKey) {
    int step = 0;
    const uint32_t published0 = GS::dev::Get().published;
    if (!Wait("the glowstick's ray strikes the box's side", 60, [&] {
            bool stuck = false;
            OnGameThread([&] {
                void* g = ByKey(key);
                if (Sticked(g) == 1) {
                    stuck = true;
                    return;
                }
                if (StepAndStrikeSide(g, ByKey(boxKey), step)) stuck = true;
                if (step < 14) ++step;
            });
            return stuck;
        }))
        return false;
    return Wait("the stick published", kEventSec, [&] {
        uint32_t n = 0;
        OnGameThread([&] { n = GS::dev::Get().published; });
        return n > published0;
    });
}

// The held glowstick stuck by its own ray over `base`, and its stick published (the lane's own event). False: a verdict was said.
bool Stick(const std::wstring& key, const FVector& base) {
    int step = 0;
    const uint32_t published0 = GS::dev::Get().published;
    if (!Wait("the glowstick's ray strikes", 60, [&] {
            bool stuck = false;
            OnGameThread([&] {
                void* g = ByKey(key);
                if (Sticked(g) == 1) {
                    stuck = true;
                    return;
                }
                if (StepAndStrike(g, base, step)) stuck = true;
                if (step < static_cast<int>(kHoldAboveCm)) ++step;
            });
            return stuck;
        }))
        return false;
    return Wait("the stick published", kEventSec, [&] {
        uint32_t n = 0;
        OnGameThread([&] { n = GS::dev::Get().published; });
        return n > published0;
    });
}

// ---- arms: the client ----

bool ClientSetup(std::wstring& key, std::wstring& boxKey, FVector& floorSpot) {
    if (!disc_drill::HostPuppetUp() || !disc_drill::ClientJoined()) return false;
    PS::Mark("client_ready");
    std::string text;
    if (!WaitText("target", text)) return false;
    std::istringstream in(text);
    std::string k, bk;
    FVector at{};
    if (!(in >> k >> bk) || !ParseNum3(in, at) || !ParseNum3(in, floorSpot)) {
        GiveUp(RV::Outcome::Invalid, "the host's target did not parse ('%s')", text.c_str());
        return false;
    }
    key = Widen(k);
    boxKey = Widen(bk);
    if (!Wait("the glowstick's copy binds here", kStepSec, [&] {
            bool here = false;
            OnGameThread([&] { here = ByKey(key) != nullptr && ByKey(boxKey) != nullptr; });
            return here;
        }))
        return false;
    return true;
}

// The surface the held glowstick is lowered over: the box's top for grab (the stick is then within reach of a
// second grab), else the floor point beside the box. Attached sticks to the box's side (StickSide).
FVector SurfaceBase(const std::wstring& key, const std::wstring& boxKey, const FVector& floorSpot) {
    (void)key;
    if (!ArmIs("grab")) return floorSpot;
    FVector b = floorSpot;
    OnGameThread([&] {
        FVector o{}, e{};
        if (void* box = ByKey(boxKey); box && E::GetActorBounds(box, true, o, e)) b = {o.X, o.Y, o.Z + e.Z};
    });
    return b;
}

void ClientReport(const std::wstring& key, const std::wstring& boxKey, const char* mark = "client_stuck") {
    FVector loc{}, glow{}, box{};
    std::wstring attach;
    OnGameThread([&] {
        void* g = ByKey(key);
        loc = LocOf(g);
        glow = Where(g);
        box = Where(ByKey(boxKey));
        attach = AttachKeyOf(g);
    });
    PS::Mark(mark, Num3(loc) + " " + Num3(glow) + " " + Num3(box) + " " + (attach.empty() ? "-" : Narrow(attach)));
    GLOG("CLIENT stuck '%ls' at (%.0f,%.0f,%.0f), attached to '%ls'", key.c_str(), loc.X, loc.Y, loc.Z, attach.c_str());
}

// A copy this peer holds, free: read at the event that says it must be.
int ClientCopySticked(const std::wstring& key) {
    int v = -1;
    OnGameThread([&] { v = Sticked(ByKey(key)); });
    return v;
}

// This life reports it is done with its part, waits for the host's ack, and leaves for the rejoin; its own done line is
// the rig's rejoin marker.
void LeaveForRejoin() {
    PS::Mark("client_done");
    PS::Await("host_ack", 60);
    OnGameThread([] {
        auto& s = harness::session_runtime::Session();
        if (s.running()) coop::net_pump::FleeAfterNativeMenuTravel(s, "glowstick drill: life 1 leaves for the rejoin");
    });
    PS::WaitFor("this life's session stopped", 15, [] { return !harness::session_runtime::Session().running(); }, false);
    UE_LOGI("glowstick: DONE on client");
}

void RunClient() {
    std::wstring key, boxKey;
    FVector floorSpot{};
    if (!ClientSetup(key, boxKey, floorSpot)) return;
    if (ArmIs("joiner_window")) {   // no stick in this life: the host makes it while the next life joins
        LeaveForRejoin();
        return;
    }
    if (!Grab(key)) return;
    if (ArmIs("attached") ? !StickSide(key, boxKey) : !Stick(key, SurfaceBase(key, boxKey, floorSpot))) return;
    ClientReport(key, boxKey);
    std::string text;
    if (ArmIs("grab")) {
        if (!WaitText("host_stuck", text) || !Grab(key)) return;
        PS::Mark("client_regrabbed", std::to_string(ClientCopySticked(key)));
        // The stick made again from the regrab: the host's grab-time free record, parked here while this player held the
        // glowstick, is older than it and must not undo it.
        if (!Stick(key, SurfaceBase(key, boxKey, floorSpot))) return;
        ClientReport(key, boxKey, "client_restuck");
    } else if (ArmIs("refused") || ArmIs("hostile")) {
        // The host's own record frees this copy: the event is the copy's stick going.
        if (!Wait("the host's record frees this copy", kStepSec, [&] { return ClientCopySticked(key) == 0; })) return;
        PS::Mark("client_freed");
    } else if (ArmIs("joiner")) {
        LeaveForRejoin();
        return;
    }
    PS::Await("host_done", kStepSec);
    UE_LOGI("glowstick: DONE on client");
}

// The constraint's own effect, read on this peer's copy: stuck fields say nothing of whether a constraint holds the
// body (a record's apply writes them), so the copy is nudged and watched. A glowstick hung from its point turns about it
// and stays the same distance from it (the radius of its constraint frame, about 35 cm); a free one travels. The
// nudge is a local impulse on the copy's own body: no stickTo, no release, so no record is published by it.
struct NudgeResult {
    int held = -1;
    float dRest = 0.f, dFinal = 0.f, maxExcursion = 0.f;
    int simulating = -1;
};

NudgeResult NudgeCopy(const std::wstring& key, const FVector& stickPoint) {
    NudgeResult n;
    FVector p0{};
    OnGameThread([&] {
        void* g = ByKey(key);
        void* mesh = g ? ue_wrap::prop::GetStaticMesh(g) : nullptr;
        if (!mesh) return;
        p0 = Where(g);
        n.simulating = E::IsComponentSimulatingPhysics(mesh) ? 1 : 0;
        if (!n.simulating) E::SetComponentSimulatePhysics(mesh, true);
        n.dRest = Dist(p0, stickPoint);
        float dx = p0.X - stickPoint.X, dy = p0.Y - stickPoint.Y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len > 1.f) {
            dx /= len;
            dy /= len;
        } else {
            dx = 1.f;
            dy = 0.f;
        }
        E::SetComponentLinearVelocity(mesh, dx * kNudgeCmPerSec, dy * kNudgeCmPerSec, kNudgeUpCmPerSec);   // away from the point
    });
    FVector last = p0;
    int polls = 0, still = 0;
    if (!Wait("the nudged copy comes to rest", 60, [&] {
            FVector now{};
            OnGameThread([&] { now = Where(ByKey(key)); });
            if (Dist(now, p0) > n.maxExcursion) n.maxExcursion = Dist(now, p0);
            still = Dist(now, last) < 0.5f ? still + 1 : 0;
            last = now;
            return ++polls >= 10 && still >= 5;
        }))
        return n;
    n.dFinal = Dist(last, stickPoint);
    n.held = std::fabs(n.dFinal - n.dRest) <= kHeldCm ? 1 : 0;
    return n;
}

// The rejoined client: the host names the glowstick; this life's own copy must be made at the point by a landing.
void RunRejoined() {
    if (!disc_drill::HostPuppetUp()) return;
    std::string text;
    if (!WaitText("rejoin_target", text, 400)) return;
    std::istringstream in(text);
    std::string k;
    FVector point{};
    if (!(in >> k) || !ParseNum3(in, point)) {
        GiveUp(RV::Outcome::Invalid, "the host's rejoin target did not parse ('%s')", text.c_str());
        return;
    }
    const std::wstring key = Widen(k);
    // The event: this life's join is over (the world announced, the join's progress idle), its copy is bound and the park
    // holds nothing for it. The stick reaches this copy through the host's save, which the game's own load restores, and a
    // record that says what the copy already holds changes nothing: no landing is counted, so none is waited for.
    if (!disc_drill::ClientJoined() || !Wait("this life's copy bound with its park empty", kStepSec, [&] {
            bool ready = false;
            OnGameThread([&] { ready = ByKey(key) != nullptr && !coop::prop_save_data::dev::Unsettled(key); });
            return ready;
        }))
        return;
    GS::dev::Counts c;
    FVector loc{};
    int sticked = -1;
    OnGameThread([&] {
        c = GS::dev::Get();
        void* g = ByKey(key);
        sticked = Sticked(g);
        LogConstraint("REJOINED copy", g);
        loc = LocOf(g);
    });
    GLOG("REJOINED copy '%ls' sticked=%d loc (%.0f,%.0f,%.0f), landed %u, made %u", key.c_str(), sticked, loc.X, loc.Y,
            loc.Z, c.landed, c.made);
    // The nudge comes after every other read: it moves the copy, publishes nothing, and what the host judges is below.
    const NudgeResult nudge = NudgeCopy(key, loc);
    GLOG("REJOINED copy nudged (it simulated %d): %.1f cm from the stick point at rest, %.1f after, travelled up to %.1f cm -- %s",
            nudge.simulating, nudge.dRest, nudge.dFinal, nudge.maxExcursion, nudge.held == 1 ? "held by a constraint" : "free");
    PS::Mark("rejoin_done", std::to_string(sticked) + " " + std::to_string(c.landed) + " " + std::to_string(c.made) + " " + Num3(loc) + " " +
                                std::to_string(nudge.held) + " " + std::to_string(nudge.dRest) + " " + std::to_string(nudge.dFinal) + " " +
                                std::to_string(nudge.maxExcursion));
    UE_LOGI("glowstick: REJOIN DONE");
}


}  // namespace harness::autotest::glowstick_drill
