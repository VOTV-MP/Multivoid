// coop/dev/hook_retrieve_drill.cpp -- see coop/dev/hook_retrieve_drill.h.

#include "coop/dev/hook_retrieve_drill.h"

#include "coffee_drill_detail.h"  // co-located private header: SpawnProp (a prop_C named by a row)

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/director/aim_fan.h"
#include "coop/dev/director/director.h"  // StartBackgroundWalk: to the hook, wherever its cups came to rest
#include "coop/items/hook_anchor.h"
#include "coop/items/hook_retrieve.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady

#include "ue_wrap/actors/hook.h"
#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace coop::dev::hook_retrieve_drill {
namespace {

namespace E  = ue_wrap::engine;
namespace H  = ue_wrap::hook;
namespace PR = ue_wrap::prop;
namespace R  = ue_wrap::reflection;
namespace CD = coop::dev::coffee_drill::detail;

constexpr float    kAheadCm       = 120.f;   // before the player: inside the interaction reach
constexpr float    kApartCm       = 45.f;    // each cup this far to one side of the line ahead
constexpr float    kHeadUpCm      = 35.f;    // the head above its cup, so the trace meets the hook before the cup
constexpr uint16_t kSeq           = 0xFF01;  // the drill's hook in the host's own owner space, clear of a player's
constexpr uint64_t kSettleMs      = 3000;    // HOST: the cups' spawn to the tie (they fall to the floor first)
constexpr uint64_t kJoinArmMs     = 10000;   // HOST, join modes: its own world up to the cups
constexpr uint64_t kHostTakeMs    = 15000;   // HOST, host modes: the client's world ready to the host's walk
constexpr uint64_t kTakeBoundMs   = 120000;  // the tie (or the client's ready edge) to the hook gone
constexpr uint64_t kFindBoundMs   = 60000;   // CLIENT: world-ready to the anchored copy
constexpr uint64_t kReadyBoundMs  = 3000;    // the aim to the player's look settling on the hook
constexpr uint64_t kSeenBoundMs   = 15000;   // the press to the hook gone and the item in the inventory
constexpr uint64_t kCheckEveryMs  = 250;
constexpr float    kNearCm        = 3000.f;  // the cups roll where the floor slopes; the taker walks to them
constexpr float    kTeleportCm    = 1500.f;  // CLIENT, join modes: farther than this, it is put beside the hook
constexpr uint64_t kLandMs        = 8000;    // ...and waits this long, so the host has its puppet's new place
constexpr float    kWalkReachCm   = 120.f;
constexpr int      kWalkDeadlineS = 60;
constexpr int      kReaimMax      = 5;

// run: the client takes the hook, its copy an anchored mirror. join: the hook is tied before the client joins, so its
// copy is the one its save loaded. host / joinhost: the same two copies, and the host player takes the hook.
enum class Mode : uint8_t { Off, Run, Join, Host, JoinHost };
Mode TheMode() {
    static const Mode m = [] {
        const std::string v = coop::config::ResolveString(::coop::config_registry::rows::hook_retrieve_drill);
        if (v == "run") return Mode::Run;
        if (v == "join") return Mode::Join;
        if (v == "host") return Mode::Host;
        if (v == "joinhost") return Mode::JoinHost;
        return Mode::Off;
    }();
    return m;
}
bool JoinMode() { return TheMode() == Mode::Join || TheMode() == Mode::JoinHost; }
bool HostTakes() { return TheMode() == Mode::Host || TheMode() == Mode::JoinHost; }

enum class Step : uint8_t { Arm, Settle, Tie, Wait, Find, Land, Walk, Aim, Ready, Press, Seen, Watch, Done };
Step     g_step = Step::Arm;
uint64_t g_stepMs = 0;
uint64_t g_nextCheckMs = 0;
uint64_t g_armMs = 0;
uint64_t g_readyMs = 0;   // HOST, host modes: when the client's world came up
int      g_session = 1;
ue_wrap::CachedObjRef g_cupA, g_cupB, g_hook;
std::wstring g_key;
uint64_t g_granted0 = 0;
int      g_items0 = 0;
coop::director::AimFan g_fan;
int      g_reaims = 0;
std::shared_ptr<coop::director::BackgroundWalk> g_walk;

const char* Role(bool host) { return host ? "host" : "client"; }
void Next(Step s) { g_step = s; g_stepMs = ::GetTickCount64(); }
bool Expired(uint64_t bound) { return ::GetTickCount64() - g_stepMs > bound; }
void Cleanup() {
    for (auto* r : {&g_cupA, &g_cupB, &g_hook})
        if (void* a = r->Get()) E::DestroyActor(a);
}
void Fail(const char* what) {
    UE_LOGW("[HOOK-RETRIEVE-DRILL] FAIL in session %d: %s", g_session, what);
    Cleanup();
    g_step = Step::Done;
}
void Abandon(const char* why) {
    UE_LOGW("[HOOK-RETRIEVE-DRILL] ABANDONED in session %d: %s", g_session, why);
    Cleanup();
    g_step = Step::Done;
}

// The hook items in this player's inventory, by their record's class.
int HookItems() {
    ue_wrap::inventory::LivePersonalStore store;
    if (!ue_wrap::inventory::ReadLivePersonalStore(store)) return -1;
    int n = 0;
    for (const auto& r : store.records)
        if (r.className == L"prop_hook_C") ++n;
    return n;
}

void StartWalk(void* hook) {
    ue_wrap::FVector a{}, b{};
    if (!hook || !H::EndLocations(hook, a, b)) { Abandon("the hook's ends do not read"); return; }
    g_walk = coop::director::StartBackgroundWalk(a, kWalkReachCm, kWalkDeadlineS);
    Next(Step::Walk);
}

void Reaim() {
    if (++g_reaims > kReaimMax) { Abandon("the trace kept leaving the hook after the aim took it"); return; }
    g_fan = coop::director::AimFan();
    Next(Step::Aim);
}

// The taker's steps, either peer: walk to the head, turn the trace onto the hook, wait for the look to offer it (the
// pop-up), press E. False when the step is not one of these.
bool TakeTick(void* player, bool host) {
    switch (g_step) {
    case Step::Walk: {
        const int w = g_walk ? g_walk->state.load() : 2;
        if (w == 0) return true;
        g_walk.reset();
        if (w != 1) { Abandon("the walk to the hook did not arrive"); return true; }
        g_fan = coop::director::AimFan();
        Next(Step::Aim);
        return true;
    }
    case Step::Aim: {
        void* hook = g_hook.Get();
        ue_wrap::FVector a{}, b{};
        if (!hook || !H::EndLocations(hook, a, b)) { Abandon("the hook died or its ends do not read"); return true; }
        const bool aimed = E::ReadMainPlayerHitActor(player) == hook;
        const auto st = g_fan.Tick(player, a, aimed);
        if (st == coop::director::AimFan::State::Failed) {
            Fail("no pose of the fan put the player's trace on the hook -- no pop-up");
            return true;
        }
        if (st == coop::director::AimFan::State::Aimed) Next(Step::Ready);
        return true;
    }
    case Step::Ready: {
        void* hook = g_hook.Get();
        if (!hook) { Abandon("the hook died"); return true; }
        if (E::ReadMainPlayerHitActor(player) != hook) { Reaim(); return true; }
        if (E::MainPlayerHasSelectedAction(player)) {
            UE_LOGI("[HOOK-RETRIEVE-DRILL] %s: the look took the hook -- the pop-up offers its pickup", Role(host));
            Next(Step::Press);
            return true;
        }
        if (Expired(kReadyBoundMs)) Fail("the trace took the hook but the player's look never offered it");
        return true;
    }
    case Step::Press: {
        void* hook = g_hook.Get();
        if (!hook) { Abandon("the hook died"); return true; }
        if (E::ReadMainPlayerHitActor(player) != hook) { Reaim(); return true; }
        g_items0 = HookItems();
        g_granted0 = coop::hook_retrieve::GrantedCount();
        if (!E::CallMainPlayerUseSelectedAction(player)) { Abandon("useSelectedAction did not dispatch"); return true; }
        UE_LOGI("[HOOK-RETRIEVE-DRILL] %s pressed the hook (%d hook item(s) carried)", Role(host), g_items0);
        Next(Step::Seen);
        return true;
    }
    case Step::Seen: {
        const int items = HookItems();
        const bool grantFree = !host || coop::hook_retrieve::GrantedCount() == g_granted0;
        if (!g_hook.Alive() && items == g_items0 + 1 && grantFree) {
            UE_LOGI("[HOOK-RETRIEVE-DRILL] %s DONE in session %d: the hook went with this player's pickup and its "
                    "item reached the inventory (%d -> %d)", Role(host), g_session, g_items0, items);
            Cleanup();
            g_step = Step::Done;
            return true;
        }
        if (Expired(kSeenBoundMs)) {
            char why[160];
            std::snprintf(why, sizeof(why), "15 s after the press the hook %s and the inventory holds %d hook item(s), "
                          "was %d", g_hook.Alive() ? "still stands" : "went", items, g_items0);
            Fail(why);
        }
        return true;
    }
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
    if (HostTakes() && TakeTick(player, true)) return;
    switch (g_step) {
    case Step::Arm: {
        if (!H::EnsureResolved()) return;
        // Before whom the cups stand: the client's puppet, or in the join modes the host itself, before anyone joined.
        void* before = nullptr;
        if (JoinMode()) {
            if (!g_armMs) g_armMs = now;
            if (now - g_armMs < kJoinArmMs) return;
            before = player;
        } else {
            if (!s->IsSlotWorldReady(1)) return;
            before = coop::puppet_drive::Puppet(1).GetActor();
        }
        ue_wrap::FVector at{};
        if (!before || !R::IsLive(before) || !E::TryGetActorLocation(before, at)) return;
        const ue_wrap::FVector f = E::GetActorForwardVector(before);
        const ue_wrap::FVector right{-f.Y, f.X, 0.f};
        const ue_wrap::FVector mid{at.X + f.X * kAheadCm, at.Y + f.Y * kAheadCm, at.Z + 20.f};
        g_cupA.Set(CD::SpawnProp(L"prop_C", {mid.X - right.X * kApartCm, mid.Y - right.Y * kApartCm, mid.Z}, L"cup"));
        g_cupB.Set(CD::SpawnProp(L"prop_C", {mid.X + right.X * kApartCm, mid.Y + right.Y * kApartCm, mid.Z}, L"cup"));
        if (!g_cupA.Get() || !g_cupB.Get()) { Abandon("the cups did not spawn"); return; }
        UE_LOGI("[HOOK-RETRIEVE-DRILL] host: two cups spawned before %s", JoinMode() ? "the host" : "the client's puppet");
        Next(Step::Settle);
        return;
    }
    case Step::Settle:
        if (now - g_stepMs >= kSettleMs) Next(Step::Tie);
        return;
    case Step::Tie: {
        void* a = g_cupA.Get();
        void* b = g_cupB.Get();
        void* mesh = a ? PR::GetStaticMesh(a) : nullptr;
        ue_wrap::FVector at{};
        if (!a || !b || !mesh || !E::TryGetActorLocation(a, at)) { Abandon("a cup died or does not read"); return; }
        void* hook = H::SpawnCanonical(H::Kind::Hook, at, ue_wrap::FRotator{});
        if (!hook) { Abandon("the hook did not spawn"); return; }
        g_hook.Set(hook);
        if (!H::AttachHead(hook, a, mesh, {at.X, at.Y, at.Z + kHeadUpCm}, {0.f, 0.f, 1.f}, b, /*checkLen=*/false,
                           /*unfreezeFrozen=*/false) ||
            !H::WriteAttachedFlags(hook, true, true)) {
            Abandon("attach_a did not dispatch on the hook");
            return;
        }
        const auto r = coop::hook_anchor::SendCommit(kSeq, H::Kind::Hook, hook);
        if (r != coop::hook_anchor::CommitResult::AdoptedLocally) { Abandon("the hook lane did not adopt the hook"); return; }
        g_key = PR::GetActorSaveKeyString(hook);
        g_granted0 = coop::hook_retrieve::GrantedCount();
        UE_LOGI("[HOOK-RETRIEVE-DRILL] host: hook '%ls' tied between the cups and adopted -- the %s's to take",
                g_key.c_str(), HostTakes() ? "host" : "client");
        Next(HostTakes() ? Step::Wait : Step::Watch);
        return;
    }
    case Step::Wait:   // HOST takes: once the client has had its copy a while, the host walks to the hook
        if (!s->IsSlotWorldReady(1)) {
            g_readyMs = 0;
            if (Expired(kTakeBoundMs)) Abandon("the client's world never came up");
            return;
        }
        if (!g_readyMs) g_readyMs = now;
        if (now - g_readyMs < kHostTakeMs) return;
        UE_LOGI("[HOOK-RETRIEVE-DRILL] host: walking to its hook to take it");
        StartWalk(g_hook.Get());
        return;
    case Step::Watch: {   // the CLIENT takes: the host's hook goes by one granted pickup
        const uint64_t granted = coop::hook_retrieve::GrantedCount() - g_granted0;
        if (g_hook.Alive()) {
            if (Expired(kTakeBoundMs)) Fail("the hook still stands on the host 120 s after the tie");
            return;
        }
        if (granted != 1) {
            char why[120];
            std::snprintf(why, sizeof(why), "the hook went on the host with %llu granted pickups, not one",
                          static_cast<unsigned long long>(granted));
            Fail(why);
            return;
        }
        UE_LOGI("[HOOK-RETRIEVE-DRILL] host DONE in session %d: hook '%ls' taken by the client's pickup, granted once",
                g_session, g_key.c_str());
        Cleanup();
        g_step = Step::Done;
        return;
    }
    default:
        return;
    }
}

// A mirror of the lane carries skipSave; a hook the save loaded does not.
bool SkipSave(void* hook) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(hook), L"skipSave", off, mask) || off < 0) return false;
    return (static_cast<const uint8_t*>(hook)[off] & mask) != 0;
}

// CLIENT: the anchored copy nearest this player: a hook carrying a save key, which no owner-phase mirror has.
struct Near { ue_wrap::FVector from; void* best; float bestCm; };
void* NearestAnchoredCopy(void* player, float withinCm) {
    Near ctx{{}, nullptr, withinCm};
    void* cls = ue_wrap::object_index::ClassByName(H::ClassNameOf(H::Kind::Hook));
    if (!cls || !E::TryGetActorLocation(player, ctx.from)) return nullptr;
    ue_wrap::object_index::ForEachInstance(cls, [](void* c, void* obj, int32_t) {
        auto* n = static_cast<Near*>(c);
        ue_wrap::FVector at{};
        if (R::NameStartsWith(R::NameOf(obj), L"Default__") || !E::TryGetActorLocation(obj, at)) return;
        const std::wstring key = PR::GetActorSaveKeyString(obj);
        if (key.empty() || key == L"None") return;
        const float cm = std::sqrt((at.X - n->from.X) * (at.X - n->from.X) + (at.Y - n->from.Y) * (at.Y - n->from.Y) +
                                   (at.Z - n->from.Z) * (at.Z - n->from.Z));
        if (cm < n->bestCm) { n->best = obj; n->bestCm = cm; }
    }, &ctx);
    return ctx.best;
}

void ClientTick() {
    if (g_step == Step::Done) return;
    void* player = coop::players::Registry::Get().Local();
    if (!player) return;
    if (!HostTakes() && TakeTick(player, false)) return;
    switch (g_step) {
    case Step::Arm:
        if (!coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
            return;
        Next(Step::Find);
        return;
    case Step::Find: {
        void* hook = NearestAnchoredCopy(player, JoinMode() ? 1.0e9f : kNearCm);
        g_items0 = HookItems();
        if (!hook || g_items0 < 0) {
            if (Expired(kFindBoundMs)) Abandon("no anchored hook stood near this client within 60 s");
            return;
        }
        g_hook.Set(hook);
        g_key = PR::GetActorSaveKeyString(hook);
        ue_wrap::FVector a{}, b{}, me{};
        bool moved = false;
        if (JoinMode() && H::EndLocations(hook, a, b) && E::TryGetActorLocation(player, me) &&
            std::hypot(a.X - me.X, a.Y - me.Y) > kTeleportCm)
            moved = E::TeleportTo(player, {a.X + 150.f, a.Y, a.Z + 80.f}, ue_wrap::FRotator{});
        UE_LOGI("[HOOK-RETRIEVE-DRILL] client: the anchored copy of hook '%ls' is here (%s); %d hook item(s) carried",
                g_key.c_str(), SkipSave(hook) ? "a mirror" : "loaded by the save", g_items0);
        if (HostTakes()) { Next(Step::Watch); return; }
        if (moved) { Next(Step::Land); return; }
        StartWalk(hook);
        return;
    }
    case Step::Land:
        if (Expired(kLandMs)) StartWalk(g_hook.Get());
        return;
    case Step::Watch: {   // the HOST takes: this client's copy goes, and no item reaches this inventory
        if (g_hook.Alive()) {
            if (Expired(kTakeBoundMs)) Fail("this client's copy still stands 120 s on -- the host's pickup never reached it");
            return;
        }
        const int items = HookItems();
        if (items != g_items0) {
            char why[120];
            std::snprintf(why, sizeof(why), "the copy went, but this inventory's hook items moved %d -> %d", g_items0,
                          items);
            Fail(why);
            return;
        }
        UE_LOGI("[HOOK-RETRIEVE-DRILL] client DONE in session %d: this copy of hook '%ls' went with the host's pickup",
                g_session, g_key.c_str());
        g_step = Step::Done;
        return;
    }
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
    g_stepMs = g_nextCheckMs = g_armMs = g_readyMs = 0;
    g_cupA.Reset();
    g_cupB.Reset();
    g_hook.Reset();
    g_key.clear();
    g_granted0 = 0;
    g_items0 = 0;
    g_reaims = 0;
    if (g_walk) coop::director::CancelBackgroundWalk(*g_walk);
    g_walk.reset();
}

}  // namespace coop::dev::hook_retrieve_drill
