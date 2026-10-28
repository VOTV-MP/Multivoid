// coop/dev/coffee_drill_client.cpp -- see coop/dev/coffee_drill.h: the client's half. It says it is ready, takes the
// host's word (the devices' keys, or in a rejoin the expectation line), acts in its own arms through its player
// (the director walks it; the press is the game's own action verb), and closes an arm when its copies show the
// arm's state.

#include "coffee_drill_detail.h"  // co-located private header (src tree, not include/)
#include "cord_drill_detail.h"    // co-located private header: Wide, Token and the stand reads

#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/director/director.h"
#include "coop/interactables/coffee_state.h"
#include "coop/interactables/cord_sync.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_key_index.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady

#include "ue_wrap/core/log.h"
#include "ue_wrap/devices/appliance.h"
#include "ue_wrap/devices/coffee.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace coop::dev::coffee_drill::detail {
namespace {

namespace D = coop::director;
namespace E = ue_wrap::engine;
namespace CF = ue_wrap::coffee;
namespace CDD = coop::dev::cord_drill::detail;

constexpr uint64_t kFindEveryMs = 250;   // the devices and cups are found by key; a miss catches the object index up

enum class Ph : uint8_t { Wait, Branch, Resolve, Arm, Joiner, Done };

struct Client {
    Ph          ph = Ph::Wait;
    int         arm = 0;
    int         sub = 0;
    uint64_t    phaseMs = 0, armMs = 0, nextFindMs = 0;
    bool        armOpen = false, closable = false, seen = false;
    bool        pre = false;    // before the arrival: the director's deadline bounds the arm, not kArmFailMs
    bool        due = false;    // this tick may look a key up
    std::string nonce;
    std::string vendKey, makerKey, sockKey;
    std::wstring cupKey;
    ue_wrap::CachedObjRef cup;
    std::shared_ptr<D::BackgroundWalk> walk;
    std::unique_ptr<D::AimedGrab> grab;
    int         grabTries = 0;
    uint64_t    cancelled0 = 0;   // arm 4: the cancel counter when the cup was lifted
};
Client g_c;

void CancelWalk() {
    if (g_c.walk && g_c.walk->state.load() == 0) D::CancelBackgroundWalk(*g_c.walk);
    g_c.walk.reset();
}

void Fail(const char* arm, const char* why) {
    FailLine("client", arm, why);
    CancelWalk();
    g_c.grab.reset();
    g_c.ph = Ph::Done;
}
void FailArm(const char* why) {
    char name[16];
    std::snprintf(name, sizeof(name), "arm%d", g_c.arm);
    Fail(name, why);
}

// The verdict of arm 4 (the fall raised no hit) and arm 5 (the cup was never put over the maker): never a pass,
// never a fail of the lane.
void Inconclusive(int arm, const char* why) {
    UE_LOGW("[COFFEE] arm%d INCONCLUSIVE: %s", arm, why);
    Say("coffee client inconclusive " + std::to_string(arm));
    CancelWalk();
    g_c.ph = Ph::Done;
}

void StartWalk(const ue_wrap::FVector& to, float reachCm, bool carry = false) {
    ue_wrap::FVector me{};
    E::TryGetActorLocation(coop::players::Registry::Get().Local(), me);
    UE_LOGI("[COFFEE] client walk arm=%d target (%.0f,%.0f,%.0f) reach %.0f from (%.0f,%.0f,%.0f)", g_c.arm, to.X,
            to.Y, to.Z, reachCm, me.X, me.Y, me.Z);
    g_c.walk = D::StartBackgroundWalk(to, reachCm, kWalkDeadlineS, carry);
}
int WalkState() { return g_c.walk ? g_c.walk->state.load() : 2; }

// The client is where the arm needs it: its bound starts now, and the host is told.
void Arrived() {
    g_c.pre = false;
    g_c.armMs = NowMs();
    Say(AtLine(g_c.arm));
}

std::string Saw() {
    Flags f;
    char buf[200];
    const bool read = Maker() && ReadFlags(Maker(), f);
    std::snprintf(buf, sizeof(buf), "%s, points=%d, cup %s", read ? FlagsText(f).c_str() : "flags unread",
                  static_cast<int>(Points()), g_c.cup.Get() ? "lives" : "gone or unknown");
    return buf;
}

// Where the player, the maker and the cup are, for the line that ends arm 5 undecided.
std::string CarryPlaces() {
    ue_wrap::FVector me{}, mk{}, cp{};
    const bool haveMe = E::TryGetActorLocation(coop::players::Registry::Get().Local(), me);
    const bool haveMk = Maker() && E::TryGetActorLocation(Maker(), mk);
    const bool haveCp = g_c.cup.Get() && E::TryGetActorLocation(g_c.cup.Get(), cp);
    char buf[200];
    std::snprintf(buf, sizeof(buf), "player %s(%.0f,%.0f,%.0f) maker %s(%.0f,%.0f,%.0f) cup %s(%.0f,%.0f,%.0f)",
                  haveMe ? "" : "unread ", me.X, me.Y, me.Z, haveMk ? "" : "unread ", mk.X, mk.Y, mk.Z,
                  haveCp ? "" : "gone ", cp.X, cp.Y, cp.Z);
    return buf;
}

// ---- finding the devices by the host's keys ----

bool Resolve() {
    Cast& c = TheCast();
    if (g_c.due) {
        if (!c.vending.Get())
            if (void* p = coop::prop_key_index::Find(CDD::Wide(g_c.vendKey))) c.vending.Set(p);
        if (!c.maker.Get())
            if (void* p = coop::prop_key_index::Find(CDD::Wide(g_c.makerKey))) c.maker.Set(p);
        if (!c.sock.Get() && !g_c.sockKey.empty())
            if (void* s = coop::cord_sync::LevelSocket(CDD::Wide(g_c.sockKey))) c.sock.Set(s);
    }
    return c.vending.Get() && c.maker.Get() && (g_c.sockKey.empty() || c.sock.Get());
}

// ---- the phases ----

void ClientWait() {
    if (!coop::net_pump::HasAnnouncedWorldReady() ||
        coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
        return;
    g_c.nonce = std::to_string(NowMs());
    Say("coffee client ready " + g_c.nonce);
    g_c.ph = Ph::Branch;
    g_c.phaseMs = NowMs();
}

// The host's first word is the keys (a run's first life) or the expectation line (the rejoin).
void ClientBranch() {
    std::string keys, expect;
    if (Heard("coffee host invalid")) {
        UE_LOGW("[COFFEE] INVALID (client) -- the host found no maker spot on the vending floor");
        g_c.ph = Ph::Done;
        return;
    }
    if (Rest("coffee keys ", &keys)) {
        g_c.vendKey = CDD::Token(keys, "V=");
        g_c.makerKey = CDD::Token(keys, "M=");
        g_c.sockKey = CDD::Token(keys, "S=");
        g_c.ph = Ph::Resolve;
    } else if (Rest("coffee expect ", &expect)) {
        g_c.makerKey = CDD::Token(expect, "m=");
        g_c.ph = Ph::Joiner;
    } else {
        if (NowMs() - g_c.phaseMs > kStageFailMs) Fail("stage", "the host never named the devices");
        return;
    }
    if (g_c.makerKey.empty() || (g_c.ph == Ph::Resolve && (g_c.vendKey.empty() || g_c.sockKey.empty())))
        return Fail("stage", "a key is missing from the host's line");
    g_c.phaseMs = NowMs();
}

void ClientResolve() {
    if (!Resolve()) {
        if (NowMs() - g_c.phaseMs > kStageFailMs) Fail("stage", "the devices were not found by their keys");
        return;
    }
    Say("coffee client staged");
    g_c.ph = Ph::Arm;
    g_c.arm = 1;
    g_c.armOpen = false;
}

void BeginArm() {
    Client& c = g_c;
    c.armOpen = true;
    c.armMs = NowMs();
    c.sub = 0;
    c.closable = c.seen = false;
    c.pre = true;
    c.cup.Reset();
    c.cupKey.clear();
}

// A press of `device`, as the game's own verb dispatches it for this player.
bool Press(void* device, void* player, const char* what) {
    const bool ok = ue_wrap::appliance::CallAction(device, player, CF::kBrewAction);
    UE_LOGI("[COFFEE] client pressed the %s ok=%d", what, ok ? 1 : 0);
    return ok;
}

// The host's key for this arm's cup, read once from its line; false while the line has not come.
bool CupNamed(int n) {
    std::string rest;
    if (!Rest(CupPrefix(n), &rest)) return false;
    g_c.cupKey = CDD::Wide(rest);
    return true;
}

// Arm 1: walk to the vending machine, press it.
void ArmVending(void* player) {
    Client& c = g_c;
    if (c.sub == 0) {
        ue_wrap::FVector at{};
        if (!Vending() || !E::TryGetActorLocation(Vending(), at)) return FailArm("the vending machine's place is unread");
        StartWalk(at, kVendReachCm);
        c.sub = 1;
    } else if (c.sub == 1) {
        const int w = WalkState();
        if (w == 0) return;
        c.walk.reset();
        if (w == 2) return FailArm("walk: the director's walk to the vending machine failed or ran out its deadline");
        Arrived();
        if (!Press(Vending(), player, "vending machine")) return FailArm("the vending machine's action did not run");
        c.sub = 2;
    } else if (c.due && CupNamed(1)) {
        c.closable = LiveByKey(c.cupKey) != nullptr;
    }
}

// Arm 2: the host drops the cup and the bag; this peer reads what the host's records and destroys brought.
void ArmFill() {
    Client& c = g_c;
    std::string rest;
    if (!c.due || !Rest(kFillPrefix, &rest)) return;
    const std::wstring cup = CDD::Wide(CDD::Token(rest, "cup=")), bag = CDD::Wide(CDD::Token(rest, "bag="));
    const std::string amt = CDD::Token(rest, "amt=");
    if (amt.empty()) return FailArm("the host's fill line has no amount");
    const int32_t want = std::atoi(amt.c_str());
    int32_t have = -1;
    Flags f;
    void* b = LiveByKey(bag);
    if (b) CF::ReadBagAmounts(b, have);
    c.closable = Maker() && ReadFlags(Maker(), f) && FlagsAre(f, 1, 1, 0) && f.power == 1 && !LiveByKey(cup) && b &&
                 have == want;
}

// Arm 3: walk to the maker's stand, press it.
void ArmBrew(void* player) {
    Client& c = g_c;
    if (c.sub == 0) {
        ue_wrap::FVector stand{};
        if (!CDD::StandOf(player, Sock(), CDD::Peer::Client, stand)) return FailArm("the maker's stand point is unread");
        StartWalk(stand, CDD::kStandReachCm);
        c.sub = 1;
    } else if (c.sub == 1) {
        const int w = WalkState();
        if (w == 0) return;
        c.walk.reset();
        if (w == 2) return FailArm("walk: the director's walk to the maker failed or ran out its deadline");
        Arrived();
        if (!Press(Maker(), player, "coffee maker")) return FailArm("the maker's action did not run");
        c.sub = 2;
    } else if (c.due && CupNamed(3)) {
        Flags f;
        c.closable = Maker() && ReadFlags(Maker(), f) && FlagsAre(f, 0, 0, 0) && LiveByKey(c.cupKey) != nullptr;
    }
}

// Arm 4: lift this peer's own copy of the spare cup over its own copy of the maker, let it fall, and read what the
// lane does with the hit: it is cancelled and counted, and the maker stays empty.
void ArmCancel() {
    Client& c = g_c;
    const uint64_t now = NowMs();
    if (c.sub == 0) {
        if (CupNamed(4)) c.sub = 1;
        else if (now - c.armMs > kArmFailMs) FailArm("the host never named the arm's cup");
    } else if (c.sub == 1) {
        if (c.due)
            if (void* p = LiveByKey(c.cupKey)) { c.cup.Set(p); c.sub = 2; }
        if (c.sub == 1 && now - c.armMs > kArmFailMs) FailArm("the host's cup never reached this peer");
    } else if (c.sub == 2) {
        void* cup = c.cup.Get();
        if (!cup) return FailArm("the cup died before the lift");
        // A cup the host's drive still holds is posed by the host; once it is free it simulates here.
        if (HeldByDrive(cup)) {
            if (now - c.armMs > kArmFailMs) Inconclusive(4, "the cup never left the host's drive");
            return;
        }
        ue_wrap::FVector mp{};
        if (!Maker() || !E::TryGetActorLocation(Maker(), mp)) return FailArm("the maker's place is unread");
        c.cancelled0 = coop::coffee_state::ReadForDrill().cancelled;
        // A prop moved by a scripted actor move, not a player: the copy falls onto the maker's mesh.
        if (!E::SetActorLocation(cup, {mp.X, mp.Y, mp.Z + kAboveCm})) return FailArm("the scripted lift of the cup did not run");
        UE_LOGI("[COFFEE] client lifted its copy of the cup %.0f uu over its copy of the maker (cancelled so far %llu)",
                kAboveCm, static_cast<unsigned long long>(c.cancelled0));
        Arrived();
        c.sub = 3;
    } else if (c.sub == 3) {
        const auto view = coop::coffee_state::ReadForDrill();
        Flags f;
        const bool read = Maker() && ReadFlags(Maker(), f);
        if (read && f.cup == 1) return FailArm("this copy of the maker took the cup: its own run of the fill was not cancelled");
        if (!c.cup.Get()) return FailArm("this copy of the cup was destroyed: its own run of the fill was not cancelled");
        if (view.cancelled > c.cancelled0 && view.lastOtherKey == c.cupKey) {
            UE_LOGI("[COFFEE] client's hit on the maker by the cup was cancelled (cancelled %llu -> %llu)",
                    static_cast<unsigned long long>(c.cancelled0), static_cast<unsigned long long>(view.cancelled));
            Say("coffee client hit 4");
            c.sub = 4;
        }
    } else if (c.sub == 4) {
        Flags f;
        c.closable = Maker() && ReadFlags(Maker(), f) && FlagsAre(f, 0, 0, 0) && c.cup.Get();
    }
}

// Arm 5: walk to the spare cup, grab it, carry it against the maker.
void ArmCarry(void* player) {
    Client& c = g_c;
    const uint64_t now = NowMs();
    ue_wrap::FVector mp{};
    if (c.sub == 0) {
        if (CupNamed(5)) c.sub = 1;
        else if (now - c.armMs > kArmFailMs) FailArm("the host never named the arm's cup");
    } else if (c.sub == 1) {
        if (c.due)
            if (void* p = LiveByKey(c.cupKey)) { c.cup.Set(p); c.sub = 2; }
        if (c.sub == 1 && now - c.armMs > kArmFailMs) FailArm("the host's cup never reached this peer");
    } else if (c.sub == 2) {
        void* cup = c.cup.Get();
        ue_wrap::FVector at{};
        if (!cup) return FailArm("the cup died before the walk");
        if (HeldByDrive(cup)) {
            if (now - c.armMs > kArmFailMs) FailArm("the cup never left the host's drive");
            return;
        }
        if (!E::TryGetActorLocation(cup, at)) return FailArm("the cup's place is unread");
        StartWalk(at, kCupReachCm);
        c.sub = 3;
    } else if (c.sub == 3) {
        const int w = WalkState();
        if (w == 0) return;
        c.walk.reset();
        if (w == 2) return FailArm("walk: the director's walk to the cup failed or ran out its deadline");
        if (!c.cup.Get()) return FailArm("the cup died before the grab");
        Arrived();
        c.grab = std::make_unique<D::AimedGrab>(player, c.cup.Get());
        c.grabTries = 0;
        c.sub = 4;
    } else if (c.sub == 4) {
        const D::GrabState st = c.grab->Tick();
        if (st == D::GrabState::Working) return;
        if (st == D::GrabState::Failed) {
            UE_LOGI("[COFFEE] client grab try %d failed: %s", c.grabTries + 1, c.grab->Why());
            if (++c.grabTries >= 2) return FailArm(c.grab->Why());
            if (!c.cup.Get()) return FailArm("the cup died before the second grab");
            c.grab = std::make_unique<D::AimedGrab>(player, c.cup.Get());
            return;
        }
        c.grab.reset();
        if (!Maker() || !E::TryGetActorLocation(Maker(), mp)) return FailArm("the maker's place is unread");
        UE_LOGI("[COFFEE] client grabbed the cup; carrying it to the maker");
        StartWalk(mp, kMakerReachCm, true);
        c.sub = 5;
    } else if (c.sub == 5) {
        // The host's copy, driven by this hold, may touch the maker on the way: the fill then destroys the cup here.
        if (!c.cup.Get()) { CancelWalk(); c.sub = 7; return; }
        const int w = WalkState();
        if (w == 0) return;
        c.walk.reset();
        if (w == 2) return FailArm("walk: the carry to the maker failed or ran out its deadline");
        c.sub = 6;
    } else if (c.sub == 6) {
        void* cup = c.cup.Get();
        if (!cup) { c.sub = 7; return; }
        ue_wrap::FVector cp{}, me{};
        void* ctrl = E::GetController(player);
        if (!ctrl || !Maker() || !E::TryGetActorLocation(Maker(), mp) || !E::TryGetActorLocation(cup, cp) ||
            !E::TryGetActorLocation(player, me))
            return FailArm("no controller, maker, cup or player place to aim with");
        // The carry's reach is flat: the walker has arrived only when it stands at the maker's height too.
        if (std::fabs(me.Z - mp.Z) > kFeetCm) return;
        const ue_wrap::FVector over{mp.X, mp.Y, mp.Z + kAboveCm};
        E::SetControlRotation(ctrl, D::LookAt(E::GetCameraLocation(), over));
        const float up = cp.Z - mp.Z;
        if (std::hypot(cp.X - mp.X, cp.Y - mp.Y) > kOverMakerCm || up < kReleaseMinCm || up > kReleaseMaxCm) return;
        if (!D::CallOnPlayer(player, L"dropGrabObject")) return FailArm("dropGrabObject did not run");
        UE_LOGI("[COFFEE] client let go of the cup over the maker");
        c.sub = 7;
    } else if (c.due) {
        Flags f;
        c.closable = Maker() && ReadFlags(Maker(), f) && FlagsAre(f, 1, 0, 0) && !LiveByKey(c.cupKey);
    }
}

void ArmActs(void* player) {
    switch (g_c.arm) {
    case 1: return ArmVending(player);
    case 2: return ArmFill();
    case 3: return ArmBrew(player);
    case 4: return ArmCancel();
    case 5: return ArmCarry(player);
    }
}

void ClientArm(void* player) {
    Client& c = g_c;
    if (!c.armOpen) {
        if (!Heard(ArmLine(c.arm))) return;
        BeginArm();
    }
    const uint64_t now = NowMs();
    c.due = now >= c.nextFindMs;
    if (c.due) c.nextFindMs = now + kFindEveryMs;
    // Before its arrival a peer is walking or waiting for a milestone of its own, and the director's deadline bounds
    // that; the arm's bound starts at the arrival.
    if (c.pre) {
        if (now - c.armMs > kStageFailMs) return FailArm("walk: the arm's arrival never came");
    } else if (now - c.armMs > kArmFailMs) {
        std::string why = "not closed within " + std::to_string(kArmFailMs) + " ms of its bound's start: " + Saw();
        if (c.arm == 4) return Inconclusive(4, why.c_str());
        if (c.arm == 5 && (c.sub == 5 || c.sub == 6)) {
            // The carry never arrived at the maker's height, or the cup never came over it: the drill's own
            // geometry, not the lane's.
            why += "; " + CarryPlaces();
            return Inconclusive(5, why.c_str());
        }
        return FailArm(why.c_str());
    }
    ArmActs(player);
    if (c.ph != Ph::Arm) return;
    if (c.closable && !c.seen) {
        UE_LOGI("[COFFEE] client sees arm=%d: %s", c.arm, Saw().c_str());
        Say(ClientSeenLine(c.arm));
        c.seen = true;
    }
    if (!c.seen) return;
    if (c.arm < kArms) {
        ++c.arm;
        c.armOpen = false;
        return;
    }
    if (!Heard(HostSeenLine(kArms))) return;   // the host has the last line before this peer leaves
    UE_LOGI("[COFFEE] client arms 1-5 PASS");
    c.ph = Ph::Done;
}

// The rejoined client: the maker named by the host's expectation line, read once the seed has brought its record.
void ClientJoiner() {
    Client& c = g_c;
    const uint64_t now = NowMs();
    if (now - c.phaseMs > kArmFailMs) return Fail("joiner", "the maker did not show the host's state within the bound");
    c.due = now >= c.nextFindMs;
    if (c.due) c.nextFindMs = now + kFindEveryMs;
    if (!Maker() && c.due)
        if (void* p = coop::prop_key_index::Find(CDD::Wide(c.makerKey))) TheCast().maker.Set(p);
    Flags f;
    if (!Maker() || !ReadFlags(Maker(), f) || !FlagsAre(f, 1, 0, 0)) return;
    UE_LOGI("[COFFEE] client's maker reads the host's state after the join: %s", FlagsText(f).c_str());
    Say("coffee client joined");
    UE_LOGI("[COFFEE] client DONE -- PASS");
    c.ph = Ph::Done;
}

}  // namespace

void ClientTick(coop::net::Session*) {
    void* player = coop::players::Registry::Get().Local();
    if (!player) return;
    switch (g_c.ph) {
    case Ph::Wait:    return ClientWait();
    case Ph::Branch:  return ClientBranch();
    case Ph::Resolve:
        g_c.due = NowMs() >= g_c.nextFindMs;
        if (g_c.due) g_c.nextFindMs = NowMs() + kFindEveryMs;
        return ClientResolve();
    case Ph::Arm:     return ClientArm(player);
    case Ph::Joiner:  return ClientJoiner();
    case Ph::Done:    return;
    }
}

void ClientReset() {
    CancelWalk();
    g_c = Client{};
}

}  // namespace coop::dev::coffee_drill::detail
