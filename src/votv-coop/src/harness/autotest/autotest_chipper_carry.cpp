// harness/autotest/autotest_chipper_carry.cpp -- TEST-ONLY: what the chipper feed drill's client does with the paper it holds.
//
// The product under test is the HOST's response once a client-driven copy is in its chipper's box; the client's drive stream
// carries whatever pose the client's copy has. So the client walks to a NavMesh stand (the director moves players, nobody
// teleports), and then places its held paper by writing the prop's world transform, as a thin sheet laid into the thin slot
// the eat box is: at the box's centre while it stays held (held, from a stand within reach of the box), or 30 cm off the
// slab's top face along its own thinnest axis (the side that points up, where the opening faces), or on the first line clear
// into the slab from a hemisphere above it when the machine's body covers that, thrown back along that line at the centre in
// the same tick by the game's own throw (thrown, from a stand outside the machine's bounds: the write needs
// no reach, and the NavMesh does not know the drill's chipper, so a stand beside it could be inside it). A prop in the hand's
// throw stays driven by this client as its syncer through the flight, which is the case the host's flip is for; a prop merely
// let go is host-simulated and the game's own intake takes it. The client then waits for the host's outcome as an event.

#include "chipper_drill_common.h"  // co-located private header

#include "coop/dev/director/routes.h"

#include <algorithm>
#include <chrono>
#include <vector>

namespace harness::autotest::chipper_drill {

constexpr float kHoldCapCm = 145.f;    // the longest hold a stand may need (the grab's own cap is 150)
constexpr float kRingMinCm = 30.f;     // the NavMesh is sampled on rings round the box, from here
constexpr float kRingMaxCm = 420.f;    // to here
constexpr float kRingStepCm = 15.f;
constexpr int kRingBearings = 24;
constexpr float kDropAboveCm = 30.f;   // a throw starts this far off the slab's top face, along its own axis
constexpr float kHemiElevDeg[] = {60.f, 45.f, 30.f};   // the hemisphere of lines tried when the slab's own normal is blocked
constexpr int kHemiBearings = 12;
constexpr float kHemiRadiusCm = 70.f;   // how far from the box's centre those lines start
constexpr int kPlaceTickMs = 16;       // the paper's pose is written every tick the player holds it
constexpr int kMarkTicks = 5;          // writes before a held placement is reported to the host
constexpr float kThrowCmS = 200.f;     // the speed the paper is thrown down into the slot at
constexpr float kClearCm = 40.f;      // a drop's stand lies this far outside the machine's bounds, flat
constexpr int kBoxSettleMs = 1000;     // how long after a throw the box is read again, to say whether it moved
constexpr float kBoxMovedCm = 5.f;     // the box counts as moved past this
constexpr int kWaitSec = 60;           // how long the paper waits for the host's outcome
constexpr float kGrabLenMin = 50.f;    // the game's clamp of the scroll wheel's own variable
constexpr float kGrabLenMax = 150.f;
constexpr float kSnagCm = 250.f;       // a paper hanging farther than this from the camera is caught on the machine



// The player's grabLen, the scroll wheel's own variable (mainPlayer): the hold point moves with it.
void WriteGrabLen(void* player, float v) {
    const int32_t off = ue_wrap::reflected_offset::MainPlayer_grabLen();
    if (off >= 0) *reinterpret_cast<float*>(static_cast<uint8_t*>(player) + off) = std::fmin(std::fmax(v, kGrabLenMin), kGrabLenMax);
}

// A stand the NavMesh offers: a point of the mesh the walker can arrive at, how far it lies from the box (flat), and the hold
// length that puts the hold point on the target from there (the camera's distance to it).
struct Stand {
    FVector at;          // at the player's own height, as a walk's goal is given
    float horizCm;
    float holdLen;
};

// CLIENT: the points of the level's NavMesh on rings round `centre`, nearest ring first, each with the hold length to
// `target` from there. The NavMesh hugs the machine's mesh, not its bounding box, so these are where a player can really
// stand. Empty when the player or the mesh under it does not read.
std::vector<Stand> NavStands(const FVector& centre, const FVector& target) {
    std::vector<Stand> out;
    OnGameThread([&] {
        void* me = coop::players::Registry::Get().Local();
        FVector ml{}, floorHere{};
        if (!me || !E::TryGetActorLocation(me, ml) || !E::ProjectToNav(me, ml, {60.f, 60.f, 250.f}, &floorHere)) return;
        const float camAbove = E::GetCameraLocation().Z - floorHere.Z;
        const float bodyAbove = ml.Z - floorHere.Z;
        for (float r = kRingMinCm; r <= kRingMaxCm; r += kRingStepCm) {
            for (int k = 0; k < kRingBearings; ++k) {
                const float a = 6.28318531f * static_cast<float>(k) / static_cast<float>(kRingBearings);
                const FVector want{centre.X + r * std::cos(a), centre.Y + r * std::sin(a), floorHere.Z};
                FVector nav{};
                if (!E::ProjectToNav(me, want, {10.f, 10.f, 60.f}, &nav)) continue;
                if (std::fabs(nav.X - want.X) > 8.f || std::fabs(nav.Y - want.Y) > 8.f) continue;   // the point itself is off the mesh
                Stand s;
                s.at = FVector{nav.X, nav.Y, nav.Z + bodyAbove};
                s.horizCm = r;
                const float hz = nav.Z + camAbove - target.Z;
                s.holdLen = std::sqrt(r * r + hz * hz);
                out.push_back(s);
            }
        }
    });
    return out;
}

// CLIENT: whether a NavMesh route from where the player stands reaches `to`.
bool Reachable(const FVector& to) {
    bool ok = false;
    OnGameThread([&] {
        void* me = coop::players::Registry::Get().Local();
        FVector ml{};
        ok = me && E::TryGetActorLocation(me, ml) && coop::director::RouteInLegs(me, ml, to, 40.f);
    });
    return ok;
}

// CLIENT: how far the paper hangs from the camera, -1 when it does not read. A paper the machine caught on the way hangs far
// from the hand.
float PaperFromHand(const std::wstring& paperKey) {
    float d = -1.f;
    OnGameThread([&] {
        FVector pl{};
        if (void* p = ByKey(paperKey)) {
            if (E::TryGetActorLocation(p, pl)) d = Dist(pl, E::GetCameraLocation());
        }
    });
    return d;
}

// The held prop thrown as the fire key's grab branch does (throwShit on the grab's component with the velocity given, the prop's
// thrown, dropGrabObject): the game's own throw, the way coop/dev/container_intent_drill_throw_act.cpp's ThrowGrabBranch calls
// it, which sits in another drill's private namespace. Game thread.
bool ThrowGrab(void* player, void* item, const FVector& vel) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(player), L"grabbing_component");
    void* comp = off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(player) + off) : nullptr;
    void* shitFn = R::FindDispatchFunctionCached(R::ClassOf(player), L"throwShit");
    ue_wrap::ParamFrame shit(shitFn);
    if (!comp || !R::IsLive(comp) || !shitFn || !shit.valid() || !shit.Set<void*>(L"InputPin", comp) ||
        !shit.Set<FVector>(L"NewVel", vel) || !ue_wrap::Call(player, shit))
        return false;
    coop::director::CallWithPlayer(item, L"thrown", player);
    return coop::director::CallOnPlayer(player, L"dropGrabObject");
}

// The rotation that lays the paper in the slot: the paper's thinnest axis along the box's thinnest, its widest along the
// box's widest, its middle along the box's middle. `ext` are the paper's half extents in its own axes (its world bounds at
// rotation zero); the frame is kept right-handed by flipping the thin axis, which a sheet does not mind.
ue_wrap::FRotator SlotRotation(const FVector& ext, const EatFrame& fr) {
    const float pe[3] = {ext.X, ext.Y, ext.Z}, be[3] = {fr.half.X, fr.half.Y, fr.half.Z};
    auto order = [](const float* v, int& thin, int& mid, int& wide) {
        thin = 0;
        wide = 0;
        for (int i = 1; i < 3; ++i) {
            if (v[i] < v[thin]) thin = i;
            if (v[i] > v[wide]) wide = i;
        }
        if (wide == thin) wide = (thin + 1) % 3;   // equal extents: any other axis
        mid = 3 - thin - wide;
    };
    int pt, pm, pw, bt, bm, bw;
    order(pe, pt, pm, pw);
    order(be, bt, bm, bw);
    float d[3][3];   // d[i]: the paper's own axis i, in the world
    std::memcpy(d[pt], fr.u[bt], sizeof(d[pt]));
    std::memcpy(d[pm], fr.u[bm], sizeof(d[pm]));
    std::memcpy(d[pw], fr.u[bw], sizeof(d[pw]));
    const float det = d[0][0] * (d[1][1] * d[2][2] - d[1][2] * d[2][1]) - d[0][1] * (d[1][0] * d[2][2] - d[1][2] * d[2][0]) +
                      d[0][2] * (d[1][0] * d[2][1] - d[1][1] * d[2][0]);
    if (det < 0.f)
        for (int j = 0; j < 3; ++j) d[pt][j] = -d[pt][j];
    const float deg = 57.2957795f;
    ue_wrap::FRotator r{};
    r.Pitch = std::asin(std::fmax(-1.f, std::fmin(1.f, d[0][2]))) * deg;
    r.Yaw = std::atan2(d[0][1], d[0][0]) * deg;
    r.Roll = std::atan2(-d[1][2], d[2][2]) * deg;
    return r;
}

// Game thread: whether the line from `from` to `to` meets the eat box or nothing (not the machine's body, the player or the
// paper). `label` and the first hit (with its distance along the line) are appended to `say`.
bool LineIntoSlot(void* me, void* paper, void* eat, const FVector& from, const FVector& to, const char* label, std::string& say) {
    const uint8_t types[3] = {0, 1, 3};   // WorldStatic, WorldDynamic, PhysicsBody
    void* ignore[2] = {me, paper};
    ue_wrap::trace::Hit h;
    const bool traced = ue_wrap::trace::LineHitForObjects(me, from, to, types, 3, ignore, 2, &h);
    std::string what;
    if (!traced) what = "untraceable";
    else if (!h.blocked) what = "nothing";
    else if (h.component == eat) what = "the eat box";
    else what = Narrow(R::ClassNameOf(h.actor) + L"/" + R::ClassNameOf(h.component));
    char line[200];
    std::snprintf(line, sizeof(line), " [%s: first hit %s%s%.0f cm]", label, what.c_str(), h.blocked ? " at " : " ",
                  static_cast<double>(h.blocked ? Dist(h.point, from) : Dist(to, from)));
    say += line;
    return traced && (!h.blocked || h.component == eat);
}

// Game thread: the throw's start and direction for the slab `fr`, the first line that is clear into the eat box. The slab's
// opening is looked for along its own thinnest axis, on the side that points up, kDropAboveCm off the top face, at the centre
// and at offsets along its long axis; when the machine's body is in the way of all of those, along a hemisphere of directions
// above it (kHemiElevDeg x kHemiBearings, nearest the slab's own normal first), kHemiRadiusCm out. `say` lists every line's
// first hit. False when no line is clear.
bool FindThrowLine(void* me, void* paper, void* eat, const EatFrame& fr, FVector& from, FVector& dir, std::string& say) {
    int t = 0, w = 0;
    const float be[3] = {fr.half.X, fr.half.Y, fr.half.Z};
    for (int i = 1; i < 3; ++i) {
        if (be[i] < be[t]) t = i;
        if (be[i] > be[w]) w = i;
    }
    if (w == t) w = (t + 1) % 3;
    const float s = fr.u[t][2] >= 0.f ? 1.f : -1.f;
    const FVector normal{s * fr.u[t][0], s * fr.u[t][1], s * fr.u[t][2]};   // the slab's own axis, upward side
    const float reach = be[t] + kDropAboveCm;
    char label[48];
    static const float offsets[] = {0.f, 10.f, -10.f, 20.f, -20.f, 30.f, -30.f, 40.f, -40.f};
    for (const float off : offsets) {
        const FVector centre{fr.at.X + off * fr.u[w][0], fr.at.Y + off * fr.u[w][1], fr.at.Z + off * fr.u[w][2]};
        const FVector start{centre.X + normal.X * reach, centre.Y + normal.Y * reach, centre.Z + normal.Z * reach};
        std::snprintf(label, sizeof(label), "normal %+.0f cm along the long axis", static_cast<double>(off));
        if (LineIntoSlot(me, paper, eat, start, centre, label, say)) {
            from = start;
            dir = FVector{-normal.X, -normal.Y, -normal.Z};
            return true;
        }
    }
    struct Cand {
        FVector out;   // from the centre to the start
        float elev, bearing, align;
    };
    std::vector<Cand> cands;
    for (const float elev : kHemiElevDeg)
        for (int b = 0; b < kHemiBearings; ++b) {
            const float e = elev * 0.0174532925f, a = 6.28318531f * static_cast<float>(b) / static_cast<float>(kHemiBearings);
            const FVector o{std::cos(e) * std::cos(a), std::cos(e) * std::sin(a), std::sin(e)};
            cands.push_back(Cand{o, elev, a * 57.2957795f, o.X * normal.X + o.Y * normal.Y + o.Z * normal.Z});
        }
    std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.align > y.align; });
    for (const Cand& c : cands) {
        const FVector start{fr.at.X + c.out.X * kHemiRadiusCm, fr.at.Y + c.out.Y * kHemiRadiusCm, fr.at.Z + c.out.Z * kHemiRadiusCm};
        std::snprintf(label, sizeof(label), "elev %.0f bearing %.0f", static_cast<double>(c.elev), static_cast<double>(c.bearing));
        if (LineIntoSlot(me, paper, eat, start, fr.at, label, say)) {
            from = start;
            dir = FVector{-c.out.X, -c.out.Y, -c.out.Z};
            say += " [the hemisphere's first clear line]";
            return true;
        }
    }
    return false;
}

// One placement write, on the game thread: the box is read live (the machine is a physics prop and can be pushed), the paper's
// world transform is set to the target, and the hand's hold point is aimed at it, so the handle pulls where the write put it.
// `drop` puts the target kDropAboveCm off the box's top face along the slab's own (thinnest) axis, on its upward side, and
// throws the paper in the same call back along that axis at kThrowCmS, so no hold can pull it back (the hold point is aimed only
// for a held placement). `gone` when the paper is gone, `lost` when it is no longer in the hand, `unread` when the box or the
// paper's shape do not read, `blocked` when the machine's body is in the way of every line tried, `threwNot` when the game's
// throw call failed. `trace` says what each line's first hit was.
struct Placed {
    bool gone = false, lost = false, unread = false, blocked = false, threwNot = false;
    FVector target{}, paperAt{}, boxAt{}, dir{};
    std::string trace;
    ue_wrap::FRotator rot{};
};
Placed PlaceOnce(const std::wstring& chipKey, const std::wstring& paperKey, bool drop) {
    Placed out;
    OnGameThread([&] {
        void* me = coop::players::Registry::Get().Local();
        void* p = ByKey(paperKey);
        void* eat = EatOf(ByKey(chipKey));
        if (me && !p) {
            out.gone = true;
            return;
        }
        if (!me || !eat || coop::director::Grabbing(me) != p) {
            out.lost = true;
            return;
        }
        EatFrame fr;
        // The paper's own half extents are its world bounds at rotation zero.
        FVector origin{}, ext{};
        if (!ReadEatFrame(eat, fr) || !E::SetActorRotation(p, ue_wrap::FRotator{}) || !E::GetActorBounds(p, false, origin, ext)) {
            out.unread = true;
            return;
        }
        out.boxAt = fr.at;
        out.target = fr.at;
        if (drop && !FindThrowLine(me, p, eat, fr, out.target, out.dir, out.trace)) {
            out.blocked = true;
            return;
        }
        out.rot = SlotRotation(ext, fr);
        E::SetActorLocation(p, out.target);
        E::SetActorRotation(p, out.rot);
        E::TryGetActorLocation(p, out.paperAt);
        const FVector cam = E::GetCameraLocation();
        E::SetControlRotation(E::GetController(me), coop::director::LookAt(cam, out.target));
        if (!drop) WriteGrabLen(me, Dist(cam, out.target));
        if (drop) {   // throwShit ADDS its velocity to the body's own, so the body's is cleared first
            if (void* mesh = ue_wrap::prop::GetStaticMesh(p)) {
                E::SetComponentLinearVelocity(mesh, 0.f, 0.f, 0.f);
                E::SetComponentAngularVelocity(mesh, 0.f, 0.f, 0.f);
            }
            out.threwNot = !ThrowGrab(me, p, FVector{out.dir.X * kThrowCmS, out.dir.Y * kThrowCmS, out.dir.Z * kThrowCmS});
        }
    });
    return out;
}

// CLIENT: places the held paper in the slot (or over it) and waits for the host's outcome. Done when the host says its part is
// over, Gone when the paper's destroy reaches this client (the host's chipper took it), Lost when the hand lost it before it
// was let go, NotEaten when the bound ran out. A held paper is written every tick while it waits; a thrown one is written once
// and thrown in that tick, by the game's own throw, down at the slot's centre, and left; the box is read at the throw and a second
// after it, against where it stood when the carry began (`boxStart`).
CarryResult Place(const std::wstring& chipKey, const std::wstring& paperKey, bool drop, const FVector& boxStart) {
    using Clk = std::chrono::steady_clock;
    Clk::time_point waitFrom{};
    bool marked = false, released = false, said = false, settled = false;
    for (int tick = 0;; ++tick) {
        std::string hostDone;
        if (PS::Peek("host_done", hostDone)) return CarryResult::Done;
        if (!released) {
            const Placed w = PlaceOnce(chipKey, paperKey, drop);
            if (w.gone) return CarryResult::Gone;
            if (w.lost) return CarryResult::Lost;
            if (w.threwNot) {
                UE_LOGI("chipper-feed: CLIENT the game's throw call (throwShit, thrown, dropGrabObject) failed");
                return CarryResult::NoSide;
            }
            if (w.blocked) {
                UE_LOGI("chipper-feed: CLIENT the machine's body is in the way of every line into the slot:%s", w.trace.c_str());
                return CarryResult::NoSide;
            }
            if (w.unread) {
                UE_LOGI("chipper-feed: CLIENT the box or the paper's shape does not read");
                return CarryResult::NoSide;
            }
            g_carryClosestCm = Dist(w.paperAt, w.boxAt);
            if (!said) {
                said = true;
                UE_LOGI("chipper-feed: CLIENT first write: the held paper's transform set to %.0f,%.0f,%.0f rot %.0f,%.0f,%.0f (the "
                        "%s; the box is at %.0f,%.0f,%.0f); it reads back at %.0f,%.0f,%.0f", w.target.X, w.target.Y, w.target.Z,
                        w.rot.Pitch, w.rot.Yaw, w.rot.Roll,
                        drop ? "throw start, on a line clear into the slab" : "box's centre", w.boxAt.X, w.boxAt.Y,
                        w.boxAt.Z, w.paperAt.X, w.paperAt.Y, w.paperAt.Z);
                if (drop)
                    UE_LOGI("chipper-feed: CLIENT the throw starts at %.0f,%.0f,%.0f and goes along %.2f,%.2f,%.2f at %.0f cm/s; the "
                            "line to the slab:%s", w.target.X, w.target.Y, w.target.Z, w.dir.X, w.dir.Y, w.dir.Z,
                            static_cast<double>(kThrowCmS), w.trace.c_str());
            }
            if (!drop && !marked && tick >= kMarkTicks) {
                marked = true;
                waitFrom = Clk::now();
                PS::Mark("inbox", "placed at the box's centre, held");
                UE_LOGI("chipper-feed: CLIENT the held paper is placed in the slot: held there, waiting for the host's outcome (%d s)",
                        kWaitSec);
            }
            if (drop) {
                released = true;
                waitFrom = Clk::now();
                PS::Mark("inbox", "thrown down into the slot");
                UE_LOGI("chipper-feed: CLIENT the paper is thrown down at the slot's centre at %.0f cm/s by the game's own throw, in "
                        "the tick it was written; the box at the throw is at %.0f,%.0f,%.0f, %.0f cm from where it stood when "
                        "the carry began; waiting for the host's outcome (%d s)", static_cast<double>(kThrowCmS), w.boxAt.X,
                        w.boxAt.Y, w.boxAt.Z, Dist(w.boxAt, boxStart), kWaitSec);
            }
        } else {
            bool gone = false;
            EatFrame box;
            const bool read = !settled && Clk::now() - waitFrom >= std::chrono::milliseconds(kBoxSettleMs);
            OnGameThread([&] {
                gone = !ByKey(paperKey);
                if (read) ReadEatFrame(EatOf(ByKey(chipKey)), box);
            });
            if (gone) return CarryResult::Gone;
            if (read) {
                settled = true;
                const float moved = Dist(box.at, boxStart);
                UE_LOGI("chipper-feed: CLIENT the box a second after the throw is at %.0f,%.0f,%.0f: %.0f cm from where it stood when "
                        "the carry began%s", box.at.X, box.at.Y, box.at.Z, moved, moved > kBoxMovedCm ? " -- THE BOX MOVED" : "");
            }
        }
        if ((marked || released) && Clk::now() - waitFrom > std::chrono::seconds(kWaitSec)) return CarryResult::NotEaten;
        ::Sleep(kPlaceTickMs);
    }
}

CarryResult CarryToBox(const std::wstring& chipKey, const std::wstring& paperKey, bool held) {
    g_carryClosestCm = 1e9f;
    EatFrame fr;
    OnGameThread([&] { ReadEatFrame(EatOf(ByKey(chipKey)), fr); });
    if (!(fr.half.X > 0.f)) return CarryResult::Lost;
    UE_LOGI("chipper-feed: CLIENT the box: centre %.0f,%.0f,%.0f, half extents %.0f,%.0f,%.0f", fr.at.X, fr.at.Y, fr.at.Z,
            static_cast<double>(fr.half.X), static_cast<double>(fr.half.Y), static_cast<double>(fr.half.Z));
    const FVector over{fr.at.X, fr.at.Y, fr.at.Z + fr.half.Z + kDropAboveCm};
    std::vector<Stand> stands = NavStands(fr.at, over);
    std::vector<Stand> chosen;
    if (held) {   // a stand within the hold's reach of the point over the box, the one whose hold is shortest and a route reaches
        std::sort(stands.begin(), stands.end(), [](const Stand& a, const Stand& b) { return a.holdLen < b.holdLen; });
        for (const Stand& s : stands) {
            if (s.holdLen > kHoldCapCm) break;
            if (Reachable(s.at)) chosen.push_back(s);
            if (chosen.size() >= 3) break;
        }
        if (chosen.empty()) {
            UE_LOGI("chipper-feed: CLIENT no reachable NavMesh stand holds within %.0f of the point over the box", static_cast<double>(kHoldCapCm));
            return CarryResult::NoSide;
        }
    } else {   // a drop needs no reach: the nearest reachable stand outside the machine's bounds, kClearCm clear of them
        FVector bo{}, be{};
        bool read = false;
        OnGameThread([&] {
            void* chip = ByKey(chipKey);
            read = chip && E::GetActorBounds(chip, false, bo, be);
        });
        if (!read) {
            UE_LOGI("chipper-feed: CLIENT the chipper's bounds do not read");
            return CarryResult::NoSide;
        }
        UE_LOGI("chipper-feed: CLIENT the chipper's bounds: centre %.0f,%.0f,%.0f, half extents %.0f,%.0f,%.0f; the drop's stand lies "
                "%.0f cm outside them", bo.X, bo.Y, bo.Z, be.X, be.Y, be.Z, static_cast<double>(kClearCm));
        std::sort(stands.begin(), stands.end(), [](const Stand& a, const Stand& b) { return a.horizCm < b.horizCm; });
        for (const Stand& s : stands) {
            if (std::fabs(s.at.X - bo.X) <= be.X + kClearCm && std::fabs(s.at.Y - bo.Y) <= be.Y + kClearCm) continue;
            if (Reachable(s.at)) chosen.push_back(s);
            if (chosen.size() >= 3) break;
        }
        if (chosen.empty()) {
            UE_LOGI("chipper-feed: CLIENT no reachable NavMesh stand lies outside the chipper's bounds");
            return CarryResult::NoSide;
        }
    }
    for (size_t k = 0; k < chosen.size(); ++k) {
        const bool walked = WalkTo(chosen[k].at, 20.f, k == 0 ? 300 : 90, /*carry=*/true);
        UE_LOGI("chipper-feed: CLIENT stand %zu at %.0f,%.0f (%.0f cm from the box, hold length %.0f): the carried walk %s", k + 1,
                chosen[k].at.X, chosen[k].at.Y, static_cast<double>(chosen[k].horizCm), static_cast<double>(chosen[k].holdLen),
                walked ? "arrived" : "did not arrive");
        if (!walked) continue;
        if (PaperFromHand(paperKey) > kSnagCm) {
            UE_LOGI("chipper-feed: CLIENT stand %zu: the paper hangs %.0f cm from the hand, caught on the machine on the way", k + 1,
                    static_cast<double>(PaperFromHand(paperKey)));
            continue;
        }
        return Place(chipKey, paperKey, /*drop=*/!held, fr.at);
    }
    return CarryResult::NoSide;
}


// True once the actor named by `key` has been still for kRestReadings readings, 500 ms apart, under kRestCmS.
bool AtRest(const std::wstring& key, int boundSec) {
    FVector last{};
    bool have = false;
    int rest = 0;
    for (int i = 0; i < boundSec * 2; ++i) {
        FVector pos{};
        bool ok = false;
        OnGameThread([&] {
            if (void* a = ByKey(key)) ok = E::TryGetActorLocation(a, pos);
        });
        if (!ok) return false;
        if (have) rest = Dist(pos, last) / 0.5f < kRestCmS ? rest + 1 : 0;
        last = pos;
        have = true;
        if (rest >= kRestReadings) return true;
        ::Sleep(500);
    }
    return false;
}

}  // namespace harness::autotest::chipper_drill
