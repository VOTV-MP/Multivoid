// coop/dev/nametag_drill.cpp -- see coop/dev/nametag_drill.h.

#include "coop/dev/nametag_drill.h"

#include "coffee_drill_detail.h"  // co-located private header: SpawnProp (a prop_C named by a row)

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/director/aim_fan.h"
#include "coop/dev/director/director.h"  // StartBackgroundWalk
#include "coop/dev/drill_hand.h"         // TakeIntoHandNow: the nametag into the hand
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_nametag.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady

#include "ue_wrap/actors/nametag.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <set>
#include <string>

namespace coop::dev::nametag_drill {
namespace {

namespace E  = ue_wrap::engine;
namespace NT = ue_wrap::nametag;
namespace PR = ue_wrap::prop;
namespace R  = ue_wrap::reflection;
namespace CD = coop::dev::coffee_drill::detail;

constexpr const wchar_t* kTagText   = L"mvDrillTag";
constexpr float    kAheadCm         = 120.f;
constexpr float    kBesideCm        = 1000.f;  // HOST: the client's puppet this near before the cup is spawned
constexpr float    kNearCm          = 3000.f;
constexpr float    kWalkReachCm     = 110.f;
constexpr int      kWalkDeadlineS   = 60;
constexpr uint64_t kHostWaitMs      = 15000;   // HOST, host mode: the client's world ready to the host's own tag
constexpr uint64_t kHandSettleMs    = 2500;    // the item in hand to the click, so the hand lane has shown it
constexpr uint64_t kEquipBoundMs    = 10000;
constexpr uint64_t kFindBoundMs     = 60000;
constexpr uint64_t kSeenBoundMs     = 15000;
constexpr uint64_t kWatchBoundMs    = 180000;
constexpr uint64_t kCheckEveryMs    = 250;

enum class Mode : uint8_t { Off, Run, Host };
Mode TheMode() {
    static const Mode m = [] {
        const std::string v = coop::config::ResolveString(::coop::config_registry::rows::nametag_drill);
        return v == "run" ? Mode::Run : v == "host" ? Mode::Host : Mode::Off;
    }();
    return m;
}

enum class Step : uint8_t { Arm, Wait, Find, Equip, Walk, Aim, Use, Seen, Watch, Done };
Step     g_step = Step::Arm;
uint64_t g_stepMs = 0;
uint64_t g_nextCheckMs = 0;
uint64_t g_readyMs = 0;
int      g_session = 1;
ue_wrap::CachedObjRef g_cup, g_item;
uint64_t g_applied0 = 0;
coop::director::AimFan g_fan;
std::shared_ptr<coop::director::BackgroundWalk> g_walk;

void Next(Step s) { g_step = s; g_stepMs = ::GetTickCount64(); }
bool Expired(uint64_t bound) { return ::GetTickCount64() - g_stepMs > bound; }
void End() {
    if (void* c = g_cup.Get(); c && coop::players::Registry::Get().LocalPeerId() == 0) E::DestroyActor(c);
    g_step = Step::Done;
}
void Fail(const char* what) { UE_LOGW("[NAMETAG-DRILL] FAIL in session %d: %s", g_session, what); End(); }
void Abandon(const char* why) { UE_LOGW("[NAMETAG-DRILL] ABANDONED in session %d: %s", g_session, why); End(); }
const char* Role(bool host) { return host ? "host" : "client"; }

bool CupTagged(void* cup) {
    std::wstring t;
    return cup && NT::ReadPropNametag(cup, t) && t == kTagText;
}

void* HeldNametag(void* player) {
    E::MainPlayerGrabState gs{};
    void* held = E::ReadMainPlayerGrabState(player, gs) ? gs.holdingActor : nullptr;
    return NT::IsNametagItem(held) ? held : nullptr;
}

// The drill's tag written on the item in hand, as the game's own entry would leave it.
bool WriteItemTag(void* item) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(item), L"tag");
    if (off < 0) return false;
    const R::FName n = ue_wrap::fname_utils::StringToFName(kTagText);
    std::memcpy(static_cast<uint8_t*>(item) + off, &n, sizeof(n));
    std::wstring back;
    return NT::ReadItemTag(item, back) && back == kTagText;
}

// CLIENT: the cups that stood before the host's was spawned, so the one it spawns is told apart.
std::set<void*> g_oldCups;

void SnapshotCups() {
    g_oldCups.clear();
    void* cls = ue_wrap::object_index::ClassByName(L"prop_C");
    if (!cls) return;
    ue_wrap::object_index::ForEachInstance(cls, [](void*, void* obj, int32_t) {
        if (PR::GetPropNameString(obj) == L"cup") g_oldCups.insert(obj);
    }, nullptr);
}

// The nearest keyed cup within reach of a walk that was not standing at the snapshot.
struct Near { ue_wrap::FVector from; void* best; float bestCm; };
void* NearestCup(void* player) {
    Near ctx{{}, nullptr, kNearCm};
    void* cls = ue_wrap::object_index::ClassByName(L"prop_C");
    if (!cls || !E::TryGetActorLocation(player, ctx.from)) return nullptr;
    ue_wrap::object_index::ForEachInstance(cls, [](void* c, void* obj, int32_t) {
        auto* n = static_cast<Near*>(c);
        ue_wrap::FVector at{};
        if (R::NameStartsWith(R::NameOf(obj), L"Default__") || PR::GetPropNameString(obj) != L"cup" ||
            g_oldCups.count(obj) || !E::TryGetActorLocation(obj, at))
            return;
        const std::wstring key = PR::GetInteractableKeyString(obj);
        if (key.empty() || key == L"None") return;
        const float cm = std::sqrt((at.X - n->from.X) * (at.X - n->from.X) + (at.Y - n->from.Y) * (at.Y - n->from.Y) +
                                   (at.Z - n->from.Z) * (at.Z - n->from.Z));
        if (cm < n->bestCm) { n->best = obj; n->bestCm = cm; }
    }, &ctx);
    return ctx.best;
}

// The walk to the cup, before the nametag is taken: a walk with an item in hand lets it go.
void StartWalk(bool host) {
    ue_wrap::FVector at{};
    void* cup = g_cup.Get();
    if (!cup || !E::TryGetActorLocation(cup, at)) { Abandon("the cup died"); return; }
    g_walk = coop::director::StartBackgroundWalk(at, kWalkReachCm, kWalkDeadlineS);
    UE_LOGI("[NAMETAG-DRILL] %s: walking to the cup", Role(host));
    Next(Step::Walk);
}

// The tagger's steps, either peer: the nametag into the hand, the walk to the cup, the aim, the item's own left click.
// False when the step is not one of these.
bool TagTick(void* player, bool host) {
    switch (g_step) {
    case Step::Equip: {
        void* held = HeldNametag(player);
        if (!held) {
            if (::GetTickCount64() - g_stepMs > kEquipBoundMs) { Abandon("no nametag came into the hand"); return true; }
            static uint64_t lastMs = 0;
            if (::GetTickCount64() - lastMs > 1000) {
                lastMs = ::GetTickCount64();
                coop::dev::drill_hand::TakeIntoHandNow(L"nametag", L"prop_nametag_C");
            }
            return true;
        }
        if (!g_item.Get()) {
            if (!WriteItemTag(held)) { Abandon("the nametag's tag did not take the drill's text"); return true; }
            g_item.Set(held);
            g_stepMs = ::GetTickCount64();   // the settle runs from here
            return true;
        }
        if (::GetTickCount64() - g_stepMs < kHandSettleMs) return true;
        UE_LOGI("[NAMETAG-DRILL] %s: a nametag reading '%ls' in hand", Role(host), kTagText);
        Next(Step::Use);
        return true;
    }
    case Step::Walk: {
        const int w = g_walk ? g_walk->state.load() : 2;
        if (w == 0) return true;
        g_walk.reset();
        if (w != 1) { Abandon("the walk to the cup did not arrive"); return true; }
        Next(Step::Equip);
        return true;
    }
    case Step::Aim: {
        void* cup = g_cup.Get();
        ue_wrap::FVector at{};
        if (!cup || !E::TryGetActorLocation(cup, at)) { Abandon("the cup died"); return true; }
        const auto st = g_fan.Tick(player, at, E::ReadMainPlayerHitActor(player) == cup);
        if (st == coop::director::AimFan::State::Failed) { Abandon("no pose of the fan put the trace on the cup"); return true; }
        if (st == coop::director::AimFan::State::Aimed) Next(Step::Use);
        return true;
    }
    case Step::Use: {
        void* item = g_item.Get();
        if (!item) { Abandon("the nametag left the hand before the click"); return true; }
        // A held item's own use breaks the interaction trace the item reads, so the hit it acts on is written here,
        // on the cup's mesh, as a player's look onto the cup leaves it (ue_wrap/engine: WriteMainPlayerHitResult).
        void* cup = g_cup.Get();
        ue_wrap::FVector at{};
        if (!cup || !E::TryGetActorLocation(cup, at) ||
            !E::WriteMainPlayerHitResult(player, cup, PR::GetStaticMesh(cup), at)) {
            Abandon("the player's hit could not be put on the cup");
            return true;
        }
        g_applied0 = coop::prop_nametag::AppliedForClients();
        void* fn = R::FindDispatchFunctionCached(R::ClassOf(item), L"playerHandUse_LMB");
        ue_wrap::ParamFrame f(fn);
        if (!fn || !f.valid() || !f.Set<void*>(L"player", player) || !ue_wrap::Call(item, f)) {
            Abandon("the nametag's left click did not dispatch");
            return true;
        }
        UE_LOGI("[NAMETAG-DRILL] %s clicked the cup with the nametag", Role(host));
        Next(Step::Seen);
        return true;
    }
    case Step::Seen:
        if (CupTagged(g_cup.Get()) && !g_item.Alive()) {
            UE_LOGI("[NAMETAG-DRILL] %s DONE in session %d: its cup carries '%ls' and the nametag is spent", Role(host),
                    g_session, kTagText);
            End();
            return true;
        }
        if (Expired(kSeenBoundMs))
            Fail(CupTagged(g_cup.Get()) ? "the cup took the tag but the nametag in hand was not spent within 15 s"
                                        : "this peer's cup does not carry the tag 15 s after the click");
        return true;
    default:
        return false;
    }
}

void HostTick(coop::net::Session* s) {
    if (g_step == Step::Done) return;
    const uint64_t now = ::GetTickCount64();
    if (now < g_nextCheckMs) return;
    g_nextCheckMs = now + kCheckEveryMs;
    void* player = coop::players::Registry::Get().Local();
    if (!player) return;
    if (TheMode() == Mode::Host && TagTick(player, true)) return;
    switch (g_step) {
    case Step::Arm: {
        if (!s->IsSlotWorldReady(1)) return;
        void* puppet = coop::puppet_drive::Puppet(1).GetActor();
        ue_wrap::FVector at{}, me{};
        if (!puppet || !R::IsLive(puppet) || !E::TryGetActorLocation(puppet, at)) return;
        // The client comes to the host's floor first (ClientTick): a cup on a slope rolls away from the copy it shows.
        if (!E::TryGetActorLocation(player, me) || std::hypot(at.X - me.X, at.Y - me.Y) > kBesideCm) return;
        const ue_wrap::FVector f = E::GetActorForwardVector(puppet);
        g_cup.Set(CD::SpawnProp(L"prop_C", {at.X + f.X * kAheadCm, at.Y + f.Y * kAheadCm, at.Z + 20.f}, L"cup"));
        if (!g_cup.Get()) { Abandon("the cup did not spawn"); return; }
        g_applied0 = coop::prop_nametag::AppliedForClients();
        UE_LOGI("[NAMETAG-DRILL] host: a cup '%ls' spawned before the client's puppet",
                PR::GetInteractableKeyString(g_cup.Get()).c_str());
        g_readyMs = now;
        Next(TheMode() == Mode::Host ? Step::Wait : Step::Watch);
        return;
    }
    case Step::Wait:
        if (now - g_readyMs >= kHostWaitMs) StartWalk(true);
        return;
    case Step::Watch: {   // the CLIENT tags: the host's cup takes the tag, applied for a client once
        const uint64_t applied = coop::prop_nametag::AppliedForClients() - g_applied0;
        if (CupTagged(g_cup.Get()) && applied == 1) {
            UE_LOGI("[NAMETAG-DRILL] host DONE in session %d: its cup carries the client's '%ls', applied once",
                    g_session, kTagText);
            End();
            return;
        }
        if (Expired(kWatchBoundMs)) Fail("the host's cup does not carry the client's tag 180 s on");
        return;
    }
    default:
        return;
    }
}

void ClientTick() {
    if (g_step == Step::Done) return;
    void* player = coop::players::Registry::Get().Local();
    if (!player) return;
    if (TheMode() == Mode::Run && TagTick(player, false)) return;
    switch (g_step) {
    case Step::Arm: {
        if (!coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
            return;
        // Beside the host, on its floor, so the cup it spawns before this player rests where both copies stand.
        coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(0);
        void* host = rp ? rp->GetActor() : nullptr;
        ue_wrap::FVector at{};
        if (!host || !R::IsLive(host) || !E::TryGetActorLocation(host, at)) return;
        if (std::fabs(at.X) + std::fabs(at.Y) < 1000.f) return;   // the puppet has not taken the host's place yet
        SnapshotCups();
        E::TeleportTo(player, {at.X + 250.f, at.Y, at.Z + 30.f}, ue_wrap::FRotator{});
        UE_LOGI("[NAMETAG-DRILL] client: put beside the host");
        Next(Step::Find);
        return;
    }
    case Step::Find: {
        if (::GetTickCount64() - g_stepMs < 3000) return;   // the host spawns the cup once it sees the puppet beside it
        void* cup = NearestCup(player);
        if (!cup) {
            if (Expired(kFindBoundMs)) Abandon("no cup stood near this client within 60 s");
            return;
        }
        g_cup.Set(cup);
        UE_LOGI("[NAMETAG-DRILL] client: the cup '%ls' is here", PR::GetInteractableKeyString(cup).c_str());
        if (TheMode() == Mode::Run) StartWalk(false);
        else Next(Step::Watch);
        return;
    }
    case Step::Watch:   // the HOST tags: this client's copy shows the host's tag
        if (CupTagged(g_cup.Get())) {
            UE_LOGI("[NAMETAG-DRILL] client DONE in session %d: its copy of the cup carries the host's '%ls'",
                    g_session, kTagText);
            End();
            return;
        }
        if (Expired(kWatchBoundMs)) Fail("this client's copy of the cup does not carry the host's tag 180 s on");
        return;
    default:
        return;
    }
}

}  // namespace

void Tick(coop::net::Session* s) {
    if (TheMode() == Mode::Off || !s) return;
    if (s->role() == coop::net::Role::Host) HostTick(s);
    else if (s->connected()) ClientTick();
}

void OnDisconnect() {
    if (TheMode() == Mode::Off) return;
    ++g_session;
    g_step = Step::Arm;
    g_stepMs = g_nextCheckMs = g_readyMs = 0;
    g_cup.Reset();
    g_item.Reset();
    g_applied0 = 0;
    g_oldCups.clear();
    if (g_walk) coop::director::CancelBackgroundWalk(*g_walk);
    g_walk.reset();
}

}  // namespace coop::dev::nametag_drill
