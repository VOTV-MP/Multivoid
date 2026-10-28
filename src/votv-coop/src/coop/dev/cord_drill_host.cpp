// coop/dev/cord_drill_host.cpp -- see coop/dev/cord_drill.h: the host's half. It picks the sockets and spawns the
// makers (the recipe's free LEVEL sockets with a clear line from a stand point), opens each arm on a chat edge, acts
// in its own arms, and closes an arm when its copies show the arm's links and the client has said the same.

#include "cord_drill_detail.h"  // co-located private header (src tree, not include/)

#include "coop/net/session.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/devices/cord.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_nav.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::dev::cord_drill::detail {
namespace {

namespace CD = ue_wrap::cord;
namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;

constexpr const wchar_t* kMakerCls = L"prop_coffeeMaker_C";

enum class Ph : uint8_t { Wait, Stage, Arm, Gap, Rejoin, Done };

struct Host {
    Ph          ph = Ph::Wait;
    int         arm = 0;
    int         sub = 0;
    uint64_t    phaseMs = 0, armMs = 0;
    bool        closable = false, seen = false, clientLeft = false;
    bool        clientWalking = false;   // the client walks in this arm: the arm's bound starts at its "walked" line
    std::string consumed;   // the nonce of the client's ready line the host acted on
    Hand        hand;
};
Host g_h;

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void ArmName(char (&out)[16], int arm) { std::snprintf(out, sizeof(out), "arm%d", arm); }

void Fail(const char* arm, const char* why) {
    FailLine("host", arm, why);
    g_h.hand.Cancel();
    g_h.ph = Ph::Done;
}
void FailArm(const char* why) {
    char name[16];
    ArmName(name, g_h.arm);
    Fail(name, why);
}

// ---- the cast: sockets with room in front, in a cluster ----

struct Cand {
    void* sock = nullptr;
    ue_wrap::FVector at{}, fwd{}, stand{};
    float d = 0.f;
    bool clear = false;
};
struct Pick {
    void* sock[kSocks] = {};
    ue_wrap::FVector makerAt[kMakers]{};
    int free = 0, lone = 0, clear = 0, sideBlocked = 0, alone = 0;
};
// Every live level socket's place, for the "lone" test (off: nearly every wall socket has a twin 10 uu away, run
// cord10; the hand's aim tells a pair apart, cord_drill_hand.cpp).
struct Census {
    std::vector<Cand> free;
    std::vector<std::pair<void*, ue_wrap::FVector>> all;
};

ue_wrap::FVector SideSpot(const Cand& c, float sign) {
    return {c.stand.X - c.fwd.Y * kSideCm * sign, c.stand.Y + c.fwd.X * kSideCm * sign, c.at.Z};
}

// The free LEVEL sockets (no cord, no parent: a generator's child socket sits behind its own mesh) that stand LONE (no
// other cordSocket_C within kLoneCm) and that a standing player reaches: the stand point out along the plug arrow
// projected onto the navmesh, an eye there at the host's own eye height above its nav point, the socket within
// kTraceReachCm of that eye and a clear line from the eye to its face (run cord3: the use trace ends 200 uu from the
// camera; run cord4: a stand 100 uu out lay on a desk). Each peer stands kPeerSideCm to its own side of that point and
// both stands must pass the test. Socket 1 is the nearest to the host's player whose nominal stand has a clear line
// to a maker spot on each side and two more reachable sockets within kClusterCm of it.
bool Choose(void* player, Pick& out) {
    ue_wrap::FVector me{};
    void* cls = ue_wrap::object_index::ClassByName(L"cordSocket_C");
    if (!cls || !player || !E::TryGetActorLocation(player, me)) return false;
    Census census;
    ue_wrap::object_index::ForEachInstance(cls, [](void* p, void* o, int32_t) {
        if (!R::IsLive(o) || R::NameStartsWith(R::NameOf(o), L"Default__")) return;
        Census& cs = *static_cast<Census*>(p);
        Cand c;
        c.sock = o;
        if (!SocketGeometry(o, c.at, c.fwd)) return;
        cs.all.emplace_back(o, c.at);
        if (FieldOf(o, L"cord") || FieldOf(o, L"parent")) return;
        cs.free.push_back(c);
    }, &census);
    out.free = static_cast<int>(census.free.size());
    std::vector<Cand> cands;
    for (const Cand& c : census.free) {
        bool lone = true;
        for (const auto& other : census.all)
            if (other.first != c.sock && Dist(c.at, other.second) < kLoneCm) { lone = false; break; }
        if (lone) cands.push_back(c);
    }
    out.lone = static_cast<int>(cands.size());
    const ue_wrap::FVector navBox{50.f, 50.f, 250.f};
    ue_wrap::FVector meNav{};
    if (!E::ProjectToNav(player, me, navBox, &meNav)) return false;
    const float eyeUp = E::GetCameraLocation().Z - meNav.Z;
    for (Cand& c : cands) {
        if (!NominalStand(player, c.at, c.fwd, c.stand)) continue;
        const ue_wrap::FVector face{c.at.X + c.fwd.X * 12.f, c.at.Y + c.fwd.Y * 12.f, c.at.Z};
        // each peer has its own stand beside the nominal one: the socket is reachable only when both are
        c.clear = true;
        for (const Peer who : {Peer::Host, Peer::Client}) {
            ue_wrap::FVector stand{}, nav{};
            const bool placed = PeerStand(player, c.at, c.fwd, c.stand, who, stand, nav);
            const ue_wrap::FVector eye{nav.X, nav.Y, nav.Z + eyeUp};
            if (!placed || Dist(eye, c.at) > kTraceReachCm || ue_wrap::trace::LineBlockedStatDyn(player, eye, face) != 0) {
                c.clear = false;
                break;
            }
        }
        c.d = Dist(c.at, me);
        if (c.clear) ++out.clear;
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d < b.d; });
    for (const Cand& first : cands) {
        if (!first.clear) continue;
        // the stand is a nav point on the floor; the maker lines start at knee height, never inside the floor
        const ue_wrap::FVector knee{first.stand.X, first.stand.Y, first.stand.Z + kKneeCm};
        const ue_wrap::FVector spotA = SideSpot(first, 1.f), spotB = SideSpot(first, -1.f);
        if (ue_wrap::trace::LineBlockedStatDyn(player, knee, spotA) != 0 ||
            ue_wrap::trace::LineBlockedStatDyn(player, knee, spotB) != 0) {
            ++out.sideBlocked;
            continue;
        }
        std::vector<std::pair<float, const Cand*>> nearby;
        for (const Cand& o : cands) {
            const float d = Dist(first.stand, o.stand);
            if (&o != &first && o.clear && d <= kClusterCm) nearby.emplace_back(d, &o);
        }
        if (nearby.size() < 2) { ++out.alone; continue; }
        std::sort(nearby.begin(), nearby.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        out.sock[0] = first.sock;
        out.sock[1] = nearby[0].second->sock;
        out.sock[2] = nearby[1].second->sock;
        out.makerAt[0] = spotA;
        out.makerAt[1] = spotB;
        return true;
    }
    return false;
}

std::wstring KeyOfSock(void* s) { return ue_wrap::prop::GetActorSaveKeyString(s); }

bool SpawnCast(void* player) {
    Pick pick;
    const bool found = Choose(player, pick);
    UE_LOGI("[CORD] host setup: free sockets=%d lone=%d reachable=%d side-blocked=%d without-two-near=%d cluster=%s",
            pick.free, pick.lone, pick.clear, pick.sideBlocked, pick.alone, found ? "found" : "none");
    if (!found) { Fail("stage", "no cluster of three free wall sockets a standing player reaches"); return false; }
    for (int k = 0; k < kSocks; ++k)
        UE_LOGI("[CORD] host chose socket %d: %s key=%s", k + 1, Narrow(R::ToString(R::NameOf(pick.sock[k]))).c_str(),
                Narrow(KeyOfSock(pick.sock[k])).c_str());
    void* cls = R::FindClass(kMakerCls);
    if (!cls) { Fail("stage", "the coffee maker class does not resolve"); return false; }
    Cast& c = TheCast();
    for (int m = 0; m < kMakers; ++m) {
        void* maker = E::SpawnActor(cls, pick.makerAt[m]);
        const std::wstring key = maker ? ue_wrap::prop::GetKeyString(maker) : std::wstring();
        if (key.empty() || key == L"None") { Fail("stage", "a maker did not spawn with a key"); return false; }
        c.maker[m].Set(maker);
        c.makerAt[m] = pick.makerAt[m];
        c.makerKey[m] = key;
    }
    for (int k = 0; k < kSocks; ++k) {
        c.sock[k].Set(pick.sock[k]);
        c.sockKey[k] = KeyOfSock(pick.sock[k]);
        if (c.sockKey[k].empty() || c.sockKey[k] == L"None") { Fail("stage", "a socket has no key"); return false; }
    }
    Say("cord keys A=" + Narrow(c.makerKey[0]) + " B=" + Narrow(c.makerKey[1]));
    Say("cord socks 1=" + Narrow(c.sockKey[0]) + " 2=" + Narrow(c.sockKey[1]) + " 3=" + Narrow(c.sockKey[2]));
    return true;
}

// ---- the phases ----

bool FreshReady() {
    std::string nonce;
    if (!Rest("cord client ready ", &nonce) || nonce == g_h.consumed) return false;
    g_h.consumed = nonce;
    return true;
}

void HostWait(coop::net::Session* s, void* player) {
    if (!s->IsSlotWorldReady(1) || !FreshReady()) return;
    if (!SpawnCast(player)) return;
    Hand::Plan plan;
    plan.mode = Hand::Mode::WalkOnly;
    plan.who = "host";
    plan.walk = true;
    if (!StandOf(player, Sock(0), Peer::Host, plan.stand)) return Fail("stage", "socket 1's stand point is unread");
    g_h.hand.Start(plan);
    g_h.ph = Ph::Stage;
    g_h.phaseMs = NowMs();
}

void OpenArm(int n) {
    g_h.ph = Ph::Arm;
    g_h.arm = n;
    g_h.sub = 0;
    g_h.armMs = NowMs();
    g_h.closable = g_h.seen = false;
    g_h.clientWalking = n == 3;
    if (n != 4) Say(ArmLine(n));   // arm 4 opens once the host holds A
    Hand::Plan plan;
    plan.who = "host";
    plan.walk = false;
    if (n == 1) {
        plan.mode = Hand::Mode::Plug;
        plan.cord = CordOfMaker(0);
        plan.sock = 0;
        g_h.hand.Start(plan);
    } else if (n == 4) {
        plan.mode = Hand::Mode::Grab;
        plan.cord = CordOfMaker(0);
        g_h.hand.Start(plan);
    }
}

void HostStage(void* player) {
    const Hand::St st = g_h.hand.Tick(player);
    if (st == Hand::St::Stuck) return Fail("stage", g_h.hand.stuckWhat());
    if (st == Hand::St::Done && Heard("cord client staged")) return OpenArm(1);
    if (NowMs() - g_h.phaseMs > kStageFailMs) Fail("stage", "the peers were not both at their first sockets in time");
}

// The arm's own actions; each sets `closable` once its last act has run.
void ArmActs(void* player) {
    Host& h = g_h;
    switch (h.arm) {
    case 1: {
        const Hand::St st = h.hand.Tick(player);
        if (st == Hand::St::Stuck) return FailArm(h.hand.stuckWhat());
        if (st == Hand::St::Done) h.closable = true;
        return;
    }
    case 2:
    case 3:
        h.closable = true;
        return;
    case 4: {
        if (h.sub == 0) {   // the host grabs A and holds it, then says so
            const Hand::St st = h.hand.Tick(player);
            if (st == Hand::St::Stuck) return FailArm(h.hand.stuckWhat());
            if (st == Hand::St::Held) {
                Say(ArmLine(4));
                h.armMs = NowMs();
                h.clientWalking = true;
                h.sub = 1;
            }
        } else if (h.sub == 1) {   // the client dragged its copy of A into 3: the host's own link was its answer
            if (!Heard("cord client dragged 4")) return;
            if (LinkOf(0) != 0) return FailArm("the client's claim was applied against the host's own drag of A");
            h.hand.Resume(0);
            h.sub = 2;
        } else if (h.sub == 2) {
            const Hand::St st = h.hand.Tick(player);
            if (st == Hand::St::Stuck) return FailArm(h.hand.stuckWhat());
            if (st == Hand::St::Done) {
                Say("cord host released 4");
                h.closable = true;
                h.sub = 3;
            }
        }
        return;
    }
    case 5:
        if (h.sub != 0 || !Heard("cord client held 5")) return;
        // The host's own B goes into 2 by a scripted Connect while the client's hand is on its copy.
        if (!CD::Connect(CordOfMaker(1), Sock(1))) return FailArm("the scripted Connect of B did not run");
        UE_LOGI("[CORD] host plugged B in 2 by a scripted connect");
        Say("cord host plugged 5");
        h.closable = true;
        h.sub = 1;
        return;
    case 6:   // the client moves its own copy of A; the host's copy stays and still shows A in 1
        h.closable = true;
        return;
    case 7: {
        if (h.sub != 0) return;
        // A prop moved, not a player: A's cord stretches past its length and breaks on the host's own tick.
        ue_wrap::FVector at{};
        if (!E::TryGetActorLocation(Maker(0), at) ||
            !E::SetActorLocation(Maker(0), {at.X, at.Y, at.Z + kMakerRiseCm}))
            return FailArm("the scripted move of A did not run");
        UE_LOGI("[CORD] host moved A %.0f uu by a scripted actor move", kMakerRiseCm);
        h.closable = true;
        h.sub = 1;
        return;
    }
    }
}

void HostArm(void* player) {
    Host& h = g_h;
    // The client's walk is its own phase, bounded by the director's deadline; the arm's bound starts when it arrived.
    if (h.clientWalking && Heard(WalkedLine(h.arm))) {
        h.clientWalking = false;
        h.armMs = NowMs();
    }
    if (h.clientWalking) {
        if (NowMs() - h.armMs > kStageFailMs) return FailArm("walk: the client never reported arriving at its stand");
    } else if (NowMs() - h.armMs > kArmFailMs) {
        char why[160];
        std::snprintf(why, sizeof(why), "not closed within %llu ms of its bound's start",
                      static_cast<unsigned long long>(kArmFailMs));
        return FailArm(why);
    }
    ArmActs(player);
    if (h.ph != Ph::Arm) return;
    if (h.closable && !h.seen && Matches(kExpect[h.arm])) {
        UE_LOGI("[CORD] host sees %s arm=%d", kExpect[h.arm].sees, h.arm);
        Say(HostSeenLine(h.arm));
        h.seen = true;
    }
    if (!h.seen || !Heard(ClientSeenLine(h.arm))) return;
    if (h.arm < kArms) OpenArm(h.arm + 1);
    else h.ph = Ph::Gap;
}

// The client has left: the joiner is owed the seed's plug and unplug entries. A is put back where it was spawned (arm
// 7 moved it past its cord's length), plugged into 1 by a scripted Connect, and B unplugged by a scripted Disconnect.
void HostGap() {
    if (!g_h.clientLeft) return;
    void* a = CordOfMaker(0);
    void* b = CordOfMaker(1);
    if (!a || !b || !Sock(0)) return Fail("gap", "the cast is gone");
    E::SetActorLocation(Maker(0), TheCast().makerAt[0]);
    if (!CD::Connect(a, Sock(0)) || !CD::Disconnect(b) || !Matches(kJoinerExpect))
        return Fail("gap", "the scripted links did not take");
    UE_LOGI("[CORD] host set %s for the joiner", kJoinerExpect.sees);
    g_h.ph = Ph::Rejoin;
    g_h.phaseMs = NowMs();
}

void HostRejoin(coop::net::Session* s) {
    if (!s->IsSlotWorldReady(1) || !FreshReady()) return;
    const Cast& c = TheCast();
    Say("cord expect a=" + Narrow(c.makerKey[0]) + " b=" + Narrow(c.makerKey[1]) + " s1=" + Narrow(c.sockKey[0]) +
        " want A in 1, B unplugged");
    g_h.ph = Ph::Done;
}

}  // namespace

void HostTick(coop::net::Session* s) {
    void* player = coop::players::Registry::Get().Local();
    if (!player) return;
    switch (g_h.ph) {
    case Ph::Wait:   return HostWait(s, player);
    case Ph::Stage:  return HostStage(player);
    case Ph::Arm:    return HostArm(player);
    case Ph::Gap:    return HostGap();
    case Ph::Rejoin: return HostRejoin(s);
    case Ph::Done:   return;
    }
}

bool HostOnDisconnect() {
    // The arms are done (the host only waits to read the client's last line) and the client left: the host carries on
    // to the rejoin.
    const bool done = g_h.ph == Ph::Gap || (g_h.ph == Ph::Arm && g_h.arm == kArms && g_h.seen);
    if (done) {
        g_h.ph = Ph::Gap;
        g_h.clientLeft = true;
        return true;
    }
    g_h.hand.Cancel();
    g_h = Host{};
    return false;
}

}  // namespace coop::dev::cord_drill::detail
