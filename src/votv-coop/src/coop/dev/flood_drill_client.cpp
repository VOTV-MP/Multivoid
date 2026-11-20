// coop/dev/flood_drill_client.cpp -- the client's steps of the flood drill: it reads its own pipes and level at each line the
// host says after a FloodState, walks by the director to the pipe and mends it with a toolbox in its hand (mend), or asks
// the host for a repair from where it stands (far); the host decides those. See coop/dev/flood_drill.h. The log, chat and
// verdict are flood_drill.cpp.

#include "flood_drill_detail.h"

#include "coop/dev/director/director.h"
#include "coop/dev/director/routes.h"
#include "coop/dev/drill_hand.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/world/flood_sync.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace coop::dev::flood_drill::detail {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace FS = coop::flood_sync::dev;
namespace HB = coop::hand_item;

namespace {

constexpr float kStandCm = 150.f;      // the director's stop at the pipe, flat; the host's reach is 400 from the pipe
constexpr int kWalkDeadlineS = 420;
constexpr float kFarMinCm = 800.f;     // the far arm's client stands at least twice the host's reach from the pipe
constexpr const wchar_t* kToolbox = L"prop_toolbox_C";

enum class CS {
    WaitReady,
    WaitState1, WaitState2,                 // burst, mend, far and the join arm's first life
    Walk, WaitHand, WaitMended, WaitFell,   // mend
    WaitHandFar,                            // far
    WaitOld,                                // mendold
    WaitJoinState                           // the join arm's second life
};
CS g_c = CS::WaitReady;
std::shared_ptr<coop::director::BackgroundWalk> g_walk;
float g_levelAtMend = 0.f;

void CGo(CS s) {
    g_c = s;
    Go();
}

// This copy's reading against the host's line `head`: false when the line is not here yet (a bound may have said INVALID)
// or this copy differs (said FAIL).
bool CheckLine(const char* head, const char* what, Reading* mine = nullptr) {
    std::string rest;
    if (!Rest(std::string(head) + " ", &rest)) {
        Late((std::string("the host's ") + head + " line never came").c_str());
        return false;
    }
    Reading host, here;
    if (!Parse(rest, host)) {
        Invalid("the host's state line could not be read");
        return false;
    }
    if (!Read(here)) return false;
    std::string why;
    if (!Matches(here, host, why)) {
        Fail("%s: %s", what, why.c_str());
        return false;
    }
    Fact("%s: pipes %s, level %.1f here, %.1f on the host", what, here.bits.c_str(), here.level, host.level);
    if (mine) *mine = here;
    return true;
}

bool OwnToolboxAnnounced() { return HB::OwnAnnounced(kToolbox); }

void StartWalk(void* me) {
    void* const pipe = FS::PipeByRank(0);
    ue_wrap::FVector at{}, from{};
    if (!pipe || !E::TryGetActorLocation(pipe, at) || !E::TryGetActorLocation(me, from)) return Invalid("the pipe's or the player's place is unread");
    if (!coop::director::RouteInLegs(me, from, at, kStandCm)) return Invalid("no NavMesh route reaches the pipe");
    Fact("walking to the pipe by the director");
    g_walk = coop::director::StartBackgroundWalk(at, kStandCm, kWalkDeadlineS);
    CGo(CS::Walk);
}

void StartFar(void* me) {
    void* const pipe = FS::PipeByRank(0);
    ue_wrap::FVector at{}, from{};
    if (!pipe || !E::TryGetActorLocation(pipe, at) || !E::TryGetActorLocation(me, from)) return Invalid("the pipe's or the player's place is unread");
    const float dx = at.X - from.X, dy = at.Y - from.Y, dz = at.Z - from.Z;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist < kFarMinCm) {
        char buf[120];
        std::snprintf(buf, sizeof(buf), "the client stands %.0f cm from the pipe: nothing is out of reach", dist);
        return Invalid(buf);
    }
    if (!coop::dev::drill_hand::TakeIntoHandNow(L"toolbox", kToolbox)) return Invalid("a toolbox could not be taken into the hand");
    Fact("%.0f cm from the pipe, a toolbox in the hand", dist);
    CGo(CS::WaitHandFar);
}

// The pipe's own toolboxFix with the hand's toolbox, as a finished repair runs it.
bool FixWithHand(void* pipe) {
    void* const box = HB::LocalHandActor();
    ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(pipe), L"toolboxFix"));
    return box && f.valid() && f.Set<void*>(L"toolbox", box) && ue_wrap::Call(pipe, f) && f.Get<bool>(L"return");
}

// The raw ask a client's own repair would send, from where this client stands.
bool SendRawAsk(coop::net::Session& s, void* pipe) {
    coop::net::FloodPayload p{};
    coop::net::WireKeyFromString(R::ToString(R::NameOf(pipe)), p.name);
    p.op = coop::net::kFloodOpFix;
    return s.SendReliable(coop::net::ReliableKind::FloodIntent, &p, sizeof(p));
}

void ClientStep(coop::net::Session& s) {
    void* const me = coop::players::Registry::Get().Local();
    if (!me || !R::IsLive(me)) return;
    if (g_c != CS::WaitReady && PeerFailed()) return;
    const Arm arm = ArmOf();
    switch (g_c) {
    case CS::WaitReady:
        if (!ClientMayStart()) {
            Late("the client's world never became ready", kJoinBoundMs);
            return;
        }
        Fact("in the host's world%s", RejoinedLife() ? " (the rejoined life)" : "");
        CGo(arm == Arm::Join && RejoinedLife() ? CS::WaitJoinState : CS::WaitState1);
        return;
    case CS::WaitJoinState:
        // The ready edge's replay came before the host's line, on the one ordered channel.
        if (!CheckLine("h-joinstate", "the joiner reads the host's flood at its world-ready")) return;
        Say("c-ok join");
        Finish();
        return;
    case CS::WaitState1:
        if (!CheckLine("h-state1", "the client reads the burst")) return;
        Say("c-ok 1");
        if (arm == Arm::Mend) StartWalk(me);
        else if (arm == Arm::Far) StartFar(me);
        else if (arm == Arm::MendOld) CGo(CS::WaitOld);
        else CGo(CS::WaitState2);
        return;
    case CS::WaitOld:
        if (!CheckLine("h-old", "the client reads the pipe the host mended")) return;
        Say("c-ok old");
        Finish();
        return;
    case CS::WaitState2:
        if (!CheckLine("h-state2", "the client reads the level's rise")) return;
        Say("c-ok 2");
        if (arm == Arm::Join) UE_LOGI("[FLOOD-DRILL] CLIENT LIFE DONE");
        Finish();
        return;
    case CS::Walk: {
        const int st = g_walk ? g_walk->state.load() : 2;
        if (st == 0) {
            Late("the walk to the pipe never ended", kWalkBoundMs);
            return;
        }
        if (st == 2) return Invalid("the walk to the pipe did not arrive");
        if (!coop::dev::drill_hand::TakeIntoHandNow(L"toolbox", kToolbox)) return Invalid("a toolbox could not be taken into the hand");
        CGo(CS::WaitHand);
        return;
    }
    case CS::WaitHand: {
        // The host has the hand first on the one ordered lane, so its judge finds the toolbox in it.
        if (!OwnToolboxAnnounced()) {
            Late("the hand's toolbox was never announced");
            return;
        }
        void* const pipe = FS::PipeByRank(0);
        if (!pipe || !FixWithHand(pipe)) return Invalid("the pipe's toolboxFix did not mend on this copy");
        Fact("the repair ran here and is asked of the host");
        CGo(CS::WaitMended);
        return;
    }
    case CS::WaitMended: {
        Reading mine;
        if (!CheckLine("h-mended", "the client reads the host's pipe mended", &mine)) return;
        g_levelAtMend = mine.level;
        CGo(CS::WaitFell);
        return;
    }
    case CS::WaitFell: {
        Reading mine;
        if (!CheckLine("h-fell", "the client reads the level's fall", &mine)) return;
        if (mine.level >= g_levelAtMend) {
            Fail("this copy's level read %.1f at the host's fall and %.1f at its mend: it did not fall", mine.level, g_levelAtMend);
            return;
        }
        Say("c-ok 4");
        Finish();
        return;
    }
    case CS::WaitHandFar: {
        if (!OwnToolboxAnnounced()) {
            Late("the hand's toolbox was never announced");
            return;
        }
        void* const pipe = FS::PipeByRank(0);
        if (!pipe || !SendRawAsk(s, pipe)) return Invalid("the raw repair ask could not be sent");
        Fact("a raw repair ask sent from out of reach");
        Finish();
        return;
    }
    }
}

}  // namespace

void ClientTick(coop::net::Session& s) { ClientStep(s); }

void ResetClient() {
    g_c = CS::WaitReady;
    g_walk.reset();
    g_levelAtMend = 0.f;
}

}  // namespace coop::dev::flood_drill::detail
