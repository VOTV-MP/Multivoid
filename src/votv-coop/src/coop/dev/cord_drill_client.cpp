// coop/dev/cord_drill_client.cpp -- see coop/dev/cord_drill.h: the client's half. It says it is ready, takes the
// host's word (the makers' and sockets' keys, or in a rejoin the expectation line), acts in its own arms through its
// player's hand, and closes an arm when its copies show the arm's links and the host has said the same.

#include "cord_drill_detail.h"  // co-located private header (src tree, not include/)

#include "coop/interactables/cord_sync.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_key_index.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady

#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace coop::dev::cord_drill::detail {
namespace {

namespace CS = coop::cord_sync;
namespace E = ue_wrap::engine;

constexpr uint64_t kFindEveryMs = 250;   // the cast's props are found by key; a miss catches the object index up

enum class Ph : uint8_t { Wait, Branch, Resolve, Stage, Arm, Joiner, Done };

struct Client {
    Ph          ph = Ph::Wait;
    int         arm = 0;
    int         sub = 0;
    uint64_t    phaseMs = 0, armMs = 0, nextFindMs = 0;
    bool        armOpen = false, closable = false, seen = false, beforeSaid = false;
    bool        walking = false;   // this arm's walk is in flight: the director's deadline bounds it, not kArmFailMs
    std::string nonce;
    std::string keys[kMakers], sockKeys[kSocks];
    Hand        hand;
};
Client g_c;

void ArmName(char (&out)[16], int arm) { std::snprintf(out, sizeof(out), "arm%d", arm); }

void Fail(const char* arm, const char* why) {
    FailLine("client", arm, why);
    g_c.hand.Cancel();
    g_c.ph = Ph::Done;
}
void FailArm(const char* why) {
    char name[16];
    ArmName(name, g_c.arm);
    Fail(name, why);
}

Hand::Plan PlanFor(Hand::Mode mode, void* cord) {
    Hand::Plan plan;
    plan.mode = mode;
    plan.who = "client";
    plan.cord = cord;
    return plan;
}

// ---- finding the cast by the host's keys ----

// Every maker and the first `socks` sockets, found by the keys the host named; true once all are here. A miss looks
// again after kFindEveryMs: the lookup catches the object index up.
bool Resolve(int socks) {
    Cast& c = TheCast();
    const uint64_t now = NowMs();
    const bool due = now >= g_c.nextFindMs;
    if (due) g_c.nextFindMs = now + kFindEveryMs;
    bool all = true;
    for (int m = 0; m < kMakers; ++m) {
        if (!c.maker[m].Get() && due) {
            if (void* p = coop::prop_key_index::Find(Wide(g_c.keys[m]))) c.maker[m].Set(p);
        }
        all = all && c.maker[m].Get() != nullptr;
    }
    for (int k = 0; k < socks; ++k) {
        if (!c.sock[k].Get() && due) {
            if (void* s = CS::LevelSocket(Wide(g_c.sockKeys[k]))) c.sock[k].Set(s);
        }
        all = all && c.sock[k].Get() != nullptr;
    }
    return all;
}

// ---- the phases ----

void ClientWait() {
    if (!coop::net_pump::HasAnnouncedWorldReady() ||
        coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
        return;
    g_c.nonce = std::to_string(NowMs());
    Say("cord client ready " + g_c.nonce);
    g_c.ph = Ph::Branch;
    g_c.phaseMs = NowMs();
}

// The host's first word is the keys (a run's first life) or the expectation line (the rejoin).
void ClientBranch() {
    std::string keys, socks, expect;
    if (Rest("cord keys ", &keys) && Rest("cord socks ", &socks)) {
        g_c.keys[0] = Token(keys, "A=");
        g_c.keys[1] = Token(keys, "B=");
        for (int k = 0; k < kSocks; ++k) g_c.sockKeys[k] = Token(socks, std::to_string(k + 1).append("=").c_str());
        g_c.ph = Ph::Resolve;
    } else if (Rest("cord expect ", &expect)) {
        g_c.keys[0] = Token(expect, "a=");
        g_c.keys[1] = Token(expect, "b=");
        g_c.sockKeys[0] = Token(expect, "s1=");
        g_c.ph = Ph::Joiner;
    } else {
        if (NowMs() - g_c.phaseMs > kStageFailMs) Fail("stage", "the host never named the makers");
        return;
    }
    for (const std::string& k : g_c.keys)
        if (k.empty()) return Fail("stage", "a maker's key is missing from the host's line");
    g_c.phaseMs = NowMs();
}

void ClientResolve(void* player) {
    if (!Resolve(kSocks)) {
        if (NowMs() - g_c.phaseMs > kStageFailMs) Fail("stage", "the makers or sockets were not found by their keys");
        return;
    }
    Hand::Plan plan = PlanFor(Hand::Mode::WalkOnly, nullptr);
    plan.walk = true;
    if (!StandOf(player, Sock(1), Peer::Client, plan.stand)) return Fail("stage", "socket 2's stand point is unread");
    g_c.hand.Start(plan);
    g_c.ph = Ph::Stage;
    g_c.phaseMs = NowMs();
}

void ClientStage(void* player) {
    const Hand::St st = g_c.hand.Tick(player);
    if (st == Hand::St::Stuck) return Fail("stage", g_c.hand.stuckWhat());
    if (st != Hand::St::Done) {
        if (NowMs() - g_c.phaseMs > kStageFailMs) Fail("stage", "the walk to socket 2 never ended");
        return;
    }
    Say("cord client staged");
    g_c.ph = Ph::Arm;
    g_c.arm = 1;
    g_c.armOpen = false;
}

// A step of the client's own hand: Stuck fails the arm; true once it is Done.
bool HandDone(void* player) {
    const Hand::St st = g_c.hand.Tick(player);
    if (st == Hand::St::Stuck) FailArm(g_c.hand.stuckWhat());
    return st == Hand::St::Done;
}

void BeginArm() {
    Client& c = g_c;
    c.armOpen = true;
    c.armMs = NowMs();
    c.sub = 0;
    c.closable = c.seen = false;
    c.walking = false;
}

// The arm's own actions; each sets `closable` once its last act has run and the host has said what it sees.
void ArmActs(void* player) {
    Client& c = g_c;
    switch (c.arm) {
    case 1:
        c.closable = true;
        return;
    case 2:   // the client plugs B into 2
        if (c.sub == 0) {
            Hand::Plan plan = PlanFor(Hand::Mode::Plug, CordOfMaker(1));
            plan.sock = 1;
            c.hand.Start(plan);
            c.sub = 1;
        } else if (c.sub == 1 && HandDone(player)) {
            if (LinkOf(1) != 2) return FailArm("the release did not plug B in 2 on this peer");
            UE_LOGI("[CORD] client claims sent=%u (the latest claim's seq)", CS::ReadForDrill().claimsSent);
            c.sub = 2;
        } else if (c.sub == 2) {
            c.closable = Heard(HostSeenLine(2));
        }
        return;
    case 3:   // the client drags B into 1, A's: the game's rule leaves B unplugged
        if (c.sub == 0) {
            Hand::Plan plan = PlanFor(Hand::Mode::Plug, CordOfMaker(1));
            plan.sock = 0;
            plan.walk = true;
            if (!StandOf(player, Sock(0), Peer::Client, plan.stand)) return FailArm("socket 1's stand point is unread");
            c.hand.Start(plan);
            c.walking = true;
            c.sub = 1;
        } else if (c.sub == 1 && HandDone(player)) {
            c.sub = 2;
        } else if (c.sub == 2) {
            c.closable = Heard(HostSeenLine(3));
        }
        return;
    case 4:   // the host holds A; the client drags its copy of A into 3
        if (c.sub == 0) {
            if (LinkOf(0) != 0) return;   // the host's grab reaches this copy first
            Hand::Plan plan = PlanFor(Hand::Mode::Plug, CordOfMaker(0));
            plan.sock = 2;
            plan.walk = true;
            if (!StandOf(player, Sock(2), Peer::Client, plan.stand)) return FailArm("socket 3's stand point is unread");
            c.hand.Start(plan);
            c.walking = true;
            c.sub = 1;
        } else if (c.sub == 1 && HandDone(player)) {
            if (LinkOf(0) != 3) return FailArm("the release did not plug the copy of A in 3");
            UE_LOGI("[CORD] client's copy of A is in 3; the host's answer is due");
            c.sub = 2;
        } else if (c.sub == 2) {
            if (LinkOf(0) != 0) return;   // the answer: the host's own link, A unplugged
            Say("cord client dragged 4");
            c.sub = 3;
        } else if (c.sub == 3) {
            c.closable = Heard("cord host released 4");
        }
        return;
    case 5:   // the client holds B; the host plugs its B into 2; the release in the air applies the host's link
        if (c.sub == 0) {
            c.hand.Start(PlanFor(Hand::Mode::Grab, CordOfMaker(1)));
            c.sub = 1;
        } else if (c.sub == 1) {
            const Hand::St st = c.hand.Tick(player);
            if (st == Hand::St::Stuck) return FailArm(c.hand.stuckWhat());
            if (st == Hand::St::Held) {
                Say("cord client held 5");
                c.sub = 2;
            }
        } else if (c.sub == 2) {
            if (!Heard("cord host plugged 5")) return;
            if (LinkOf(1) != 0) return FailArm("the host's link applied to B inside the client's drag");
            UE_LOGI("[CORD] client's copy of B stays in the drag, unplugged");
            c.hand.Resume(-1);
            c.sub = 3;
        } else if (c.sub == 3 && HandDone(player)) {
            c.sub = 4;
        } else if (c.sub == 4) {
            c.closable = Heard(HostSeenLine(5));
        }
        return;
    case 6:   // the client moves its own copy of A; the copy stretches and its break is refused
        if (c.sub == 0) {
            // A prop moved, not a player: the host does not move its copy, so no broadcast reaches this peer.
            ue_wrap::FVector at{};
            if (!E::TryGetActorLocation(Maker(0), at) ||
                !E::SetActorLocation(Maker(0), {at.X, at.Y, at.Z + kMakerRiseCm}))
                return FailArm("the scripted move of the copy of A did not run");
            UE_LOGI("[CORD] client moved its copy of A %.0f uu by a scripted actor move", kMakerRiseCm);
            c.sub = 1;
        } else {
            c.closable = CS::ReadForDrill().stretchRefused >= 1;
        }
        return;
    case 7:   // the host moves its copy of A: the host's break reaches this peer as an unplug
        c.closable = true;
        return;
    }
}

void ClientArm(void* player) {
    Client& c = g_c;
    if (!c.armOpen) {
        if (!Heard(ArmLine(c.arm))) return;
        BeginArm();
    }
    // A walk is its own phase: the director's deadline bounds it, and the arm's bound starts at the arrival. A walk that
    // failed stays `walking` so the hand's Stuck fails the arm below with the walk's own text.
    if (c.walking) {
        const Hand::St st = c.hand.state();
        if (st != Hand::St::Walk && st != Hand::St::Stuck) {
            c.walking = false;
            c.armMs = NowMs();
            Say(WalkedLine(c.arm));
        }
    }
    if (!c.walking && NowMs() - c.armMs > kArmFailMs) {
        char why[160];
        std::snprintf(why, sizeof(why), "not closed within %llu ms of its bound's start; stretchRefused=%u",
                      static_cast<unsigned long long>(kArmFailMs), CS::ReadForDrill().stretchRefused);
        return FailArm(why);
    }
    ArmActs(player);
    if (c.ph != Ph::Arm) return;
    if (c.closable && !c.seen && Matches(kExpect[c.arm])) {
        UE_LOGI("[CORD] client sees %s arm=%d", kExpect[c.arm].sees, c.arm);
        if (c.arm == 6) UE_LOGI("[CORD] client stretchRefused=%u", CS::ReadForDrill().stretchRefused);
        Say(ClientSeenLine(c.arm));
        c.seen = true;
    }
    if (!c.seen) return;
    if (c.arm < kArms) {
        ++c.arm;
        c.armOpen = false;
        return;
    }
    if (!Heard(ClientSeenLine(kArms))) return;   // the host has the last line before this peer leaves
    UE_LOGI("[CORD] client arms 1-7 PASS");
    c.ph = Ph::Done;
}

// The rejoined client: the copies named by the host's expectation line, each copy's link logged before the seed and
// compared after the first op 2.
void ClientJoiner() {
    Client& c = g_c;
    if (NowMs() - c.phaseMs > kArmFailMs) return Fail("joiner", "the copies did not show the host's links within the bound");
    if (!Resolve(1)) return;
    const CS::DrillView view = CS::ReadForDrill();
    if (!c.beforeSaid) {
        c.beforeSaid = true;
        UE_LOGI("[CORD] client before the seed: %s (seeded=%d)", LinkText().c_str(), view.seeded ? 1 : 0);
    }
    if (!view.seeded || !Matches(kJoinerExpect)) return;
    UE_LOGI("[CORD] client sees %s after the seed", kJoinerExpect.sees);
    Say("cord client joined");
    UE_LOGI("[CORD] client DONE -- PASS");
    c.ph = Ph::Done;
}

}  // namespace

void ClientTick(coop::net::Session*) {
    void* player = coop::players::Registry::Get().Local();
    if (!player) return;
    switch (g_c.ph) {
    case Ph::Wait:    return ClientWait();
    case Ph::Branch:  return ClientBranch();
    case Ph::Resolve: return ClientResolve(player);
    case Ph::Stage:   return ClientStage(player);
    case Ph::Arm:     return ClientArm(player);
    case Ph::Joiner:  return ClientJoiner();
    case Ph::Done:    return;
    }
}

void ClientReset() {
    g_c.hand.Cancel();
    g_c = Client{};
}

}  // namespace coop::dev::cord_drill::detail
