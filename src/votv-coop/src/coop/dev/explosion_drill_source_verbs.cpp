// coop/dev/explosion_drill_source_verbs.cpp -- see coop/dev/explosion_drill_internal.h.
//
// The verbs of the source arms' held legs: the guard that keeps the acting player and its guns whole, the hand
// reads and the game's own hand verbs (playerHandUse_RMB / LMB, the player's throw), the aim, and the waits on a
// projectile's birth. explosion_drill_source.cpp walks the legs and calls these in their order.

#include "coop/dev/explosion_drill_internal.h"

#include "explosion_drill_source.h"  // the source arms' shared types (co-located private header)

#include "drive_drill_verbs.h"  // Throw: the player's own throw (co-located private header)

#include "coop/dev/director/director.h"  // LookAt
#include "coop/dev/drill_hand.h"
#include "coop/dev/drill_hold.h"
#include "coop/player/players_registry.h"
#include "coop/world/explosion_sync.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/world/explosion.h"
#include "ue_wrap/world/world_singleton.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::dev::explosion_drill::detail {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace ES = coop::explosion_sync;
namespace DH = coop::dev::drill_hold;

namespace {

constexpr float kAimTolDeg = 5.f;

float AngDiff(float a, float b) {
    float d = std::fmod(a - b, 360.f);
    if (d > 180.f) d -= 360.f;
    if (d < -180.f) d += 360.f;
    return std::fabs(d);
}

bool ReadInt(void* obj, const wchar_t* name, int& out) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off < 0) return false;
    out = *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(obj) + off);
    return true;
}

// ---- what keeps the acting player and its guns whole ----
//
// Blueprint mainPlayer: Add Player Damage and kill() return while startInvinc holds (Add Player Damage also on
// immortal, dreaming and dead). startInvinc starts true; its one writer is a Delay(5) after begin-play that clears it,
// once, so false is its steady value and the arm puts back false, never a true it found inside that window. The arm
// sets it on the acting player, so a pipe bomb's own blast (radius 1500) leaves the legs after it a live player.
// Blueprint prop_funGun, playerHoldPost: in the story game modes a gun that the date and
// gamemode.unlockFungunsInStory do not allow calls the player's forceDrop() with the hint "the g*ns are prohibited!",
// so the gun is dropped as a world prop in the tick it was taken. The arm sets the gamemode's unlockFungunsInStory (the
// cheat menu's own switch) for the same span and puts back the value it found. Each source leg's start and each
// request arm's first verb re-assert both, so a Delay(5) that landed before them costs no leg.

constexpr int kInvincBit = 1, kUnlockBit = 2;

// The prefix of the guard's lines (coop/dev/drill_hold): the drill's tag and the role the arm runs.
const char* GuardTag() {
    static char tag[64];
    std::snprintf(tag, sizeof(tag), "[EXPLOSION-DRILL] source role=%s", RoleName(g_src.host));
    return tag;
}

// mainPlayer's own call of a held prop's hand verb: the Blueprint function of that name, with the player.
bool CallHandVerb(void* held, void* player, const wchar_t* verb) {
    void* fn = held ? R::FindDispatchFunctionCached(R::ClassOf(held), verb) : nullptr;
    ue_wrap::ParamFrame f(fn);
    return fn && f.valid() && f.Set<void*>(L"player", player) && ue_wrap::Call(held, f);
}

}  // namespace

// A reflected bool written in place.
bool WriteBool(void* obj, const wchar_t* name, bool value) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask)) return false;
    uint8_t* b = static_cast<uint8_t*>(obj) + off;
    *b = value ? static_cast<uint8_t>(*b | mask) : static_cast<uint8_t>(*b & ~mask);
    return true;
}

// A reflected int field written in place (the Blueprint's own member, the way its functions write it).
bool WriteInt(void* obj, const wchar_t* name, int value) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off < 0) return false;
    *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(obj) + off) = value;
    return true;
}

// The local hand's actor, when it is live and of class `cls`.
void* HeldOf(void* player, const wchar_t* cls) {
    E::MainPlayerGrabState gs{};
    if (!E::ReadMainPlayerGrabState(player, gs)) return nullptr;
    void* h = gs.holdingActor;
    return h && R::IsLive(h) && R::NameEquals(R::NameOf(R::ClassOf(h)), cls) ? h : nullptr;
}

// The class name of what the hand holds, or "none"; ASCII, for a log line.
std::string HeldClassName(void* player) {
    E::MainPlayerGrabState gs{};
    if (!E::ReadMainPlayerGrabState(player, gs) || !gs.holdingActor || !R::IsLive(gs.holdingActor)) return "none";
    const std::wstring w = R::ClassNameOf(gs.holdingActor);
    return std::string(w.begin(), w.end());
}

bool SpawnNativeBlast(ue_wrap::explosion::Cls c, const Vec& at, float damage, float force, std::string* why) {
    namespace X = ue_wrap::explosion;
    char what[48];
    if (!X::ResolveNow(c)) {
        std::snprintf(what, sizeof(what), "class %d", static_cast<int>(c));
        if (why) *why = what;
        return false;
    }
    const float loc[3] = {at.X, at.Y, at.Z}, rot[3] = {0.f, 0.f, 0.f}, scale[3] = {1.f, 1.f, 1.f};
    const X::Params p{damage, 1000.f, force, 0.f, 0.f, 0.f, 0.f, 0, true, false};
    if (!X::SpawnDeferred(c, loc, rot, scale, p, nullptr)) {
        std::snprintf(what, sizeof(what), "blast class %d did not spawn", static_cast<int>(c));
        if (why) *why = what;
        return false;
    }
    return true;
}

// Holds the switches; the name of the first member that cannot be read (said once per member), else null. The acting
// client holds both. A host that only hosts holds the gamemode's unlockFungunsInStory alone: a client's gun in hand
// runs on the host's world rule, and its own player is not the one acting.
const wchar_t* Guard(bool hostRole) {
    g_src.host = hostRole;  // the role the guard's own lines name, for a caller that does not tick the drill
    const wchar_t* bad = nullptr;
    const struct { int bit; const wchar_t* name; bool ok; } rows[] = {
        {kInvincBit, L"startInvinc",
         hostRole || DH::Hold(coop::players::Registry::Get().Local(), L"startInvinc", g_src.invincBefore, 0,
                              GuardTag())},
        {kUnlockBit, L"unlockFungunsInStory",
         DH::Hold(ue_wrap::world_singleton::Gamemode(), L"unlockFungunsInStory", g_src.unlockBefore, -1,
                  GuardTag())},
    };
    for (const auto& row : rows) {
        if (row.ok) continue;
        if (!bad) bad = row.name;
        if (g_src.unreadableSaid & row.bit) continue;
        g_src.unreadableSaid |= row.bit;
        Warn("source role=%s guard %ls is unreadable: its object or member did not resolve", RoleName(g_src.host),
             row.name);
    }
    return bad;
}

// What the last Guard held is put back, each member to the value it found; a member Guard did not hold is left alone,
// and a second call does nothing.
void Unguard() {
    if (g_src.invincBefore < 0 && g_src.unlockBefore < 0) return;  // nothing held: no player or gamemode lookup
    DH::Put(coop::players::Registry::Get().Local(), L"startInvinc", g_src.invincBefore, GuardTag());
    DH::Put(ue_wrap::world_singleton::Gamemode(), L"unlockFungunsInStory", g_src.unlockBefore, GuardTag());
}

bool BirthSeen(bool host) {
    const ES::Counters c = ES::GetCounters();
    return host ? c.captured > g_src.base.captured : c.dissolved > g_src.base.dissolved;
}

// ---- the held legs ----
//
// Blueprint prop_grenade, playerHandUse_RMB: its entry runs only at timer == 0, then arms the fuse (timer = 3,
// fuse()); the throw is mainPlayer's throwHoldingProp. Blueprint prop_pipebomb, playerHandUse_RMB: activate()
// (active = true, the six-second fuse); its playerHandUse_LMB calls throwHoldingProp. Blueprint prop_funGun,
// playerHandUse_LMB: sets hold, player and character, then firePrimary(), which returns while reload_primary > 0
// and, at ammo_primary > 0, fires through the gun's own fireBulletPrimary (the rpg's spawns prop_rocket, the sticky
// launcher's prop_stickybomb). A gun the hand just took has no ammo: its loadData writes ammo_primary from the hold
// record's ints, which the hand left empty. The leg therefore writes ammo_primary = maxMagazine_primary, the value
// the Blueprint's own reload writes, and logs it; reload_primary starts at 0, and the leg reads both and abandons
// when either would still stop the shot.
// Blueprint prop_funGun_stickyLauncher, playerHandUse_RMB: explode() on every bomb it holds.

void Act(const LegDef& d, void* player) {
    void* held = HeldOf(player, d.cls);
    if (!held) return Skip("the held actor is gone before its verb");
    Reading& r = g_src.r;
    g_src.base = ES::GetCounters();
    if (d.kind == Kind::Grenade) {
        if (!CallHandVerb(held, player, L"playerHandUse_RMB")) return Skip("playerHandUse_RMB did not run");
        g_src.verbAt = Clock::now();
        ReadFloat(held, L"timer", r.timer);
        if (!(r.timer > 0.f)) return Skip("the grenade stayed unarmed: timer is not above 0 after playerHandUse_RMB");
        return Enter(LStep::Act2);
    }
    if (d.kind == Kind::Pipebomb) {
        if (!CallHandVerb(held, player, L"playerHandUse_RMB")) return Skip("playerHandUse_RMB did not run");
        g_src.verbAt = Clock::now();
        bool active = false;
        if (ReadBool(held, L"active", active)) r.active = active ? 1 : 0;
        if (r.active == 0) return Skip("the pipe bomb stayed inactive after playerHandUse_RMB");
        return Enter(LStep::Act2);
    }
    int maxMagazine = -1;
    if (!ReadInt(held, L"maxMagazine_primary", maxMagazine) || maxMagazine <= 0)
        return Skip("maxMagazine_primary is unreadable or not above 0");
    if (!WriteInt(held, L"ammo_primary", maxMagazine)) return Skip("ammo_primary could not be written");
    Info("source leg=%s role=%s ammo set to %d (the reload's value)", d.name, RoleName(g_src.host), maxMagazine);
    ReadFloat(held, L"reload_primary", r.reload);
    ReadInt(held, L"ammo_primary", r.ammo);
    if (r.reload > 0.f) return Skip("reload_primary is above 0, so firePrimary returns");
    if (r.ammo == 0) return Skip("ammo_primary is 0, so firePrimary only clicks");
    g_src.verbAt = Clock::now();
    if (!CallHandVerb(held, player, L"playerHandUse_LMB")) return Skip("playerHandUse_LMB did not run");
    r.fired = true;
    Enter(IsSticky(d.kind) ? LStep::AwaitBomb : LStep::Birth);
}

// The throw after the arming: the grenade's is the player's own throwHoldingProp, the pipe bomb's its own LMB.
void Act2(const LegDef& d, void* player) {
    Reading& r = g_src.r;
    if (d.kind == Kind::Grenade) {
        r.thrown = coop::dev::drive_drill_verbs::Throw(player);
    } else {
        void* held = HeldOf(player, d.cls);
        r.thrown = held && CallHandVerb(held, player, L"playerHandUse_LMB");
    }
    if (!r.thrown) return Skip("the throw did not run");
    Enter(LStep::Birth);
}

void AwaitBomb(const LegDef& d) {
    Reading& r = g_src.r;
    g_scan.clear();
    r.bombMax = (std::max)(r.bombMax, Count(kBombCls, true, &g_scan));
    if (r.bombMax > 0) {
        if (!g_scan.empty()) Track(g_scan[0]);
        return Enter(LStep::AwaitAttach);
    }
    if (!Late(kStepBound)) return;
    // The held sticky leg measures a shot that made no bomb as it is; the leg that moves the bomb's parent has
    // nothing to measure without one.
    if (d.kind == Kind::StickyParent) return Skip("no bomb stood after the shot");
    Enter(LStep::End);
}

// A sticky bomb attaches on its hit (Blueprint prop_stickybomb, the hit event attaches it to what it struck).
void AwaitAttach() {
    Reading& r = g_src.r;
    g_scan.clear();
    r.bombMax = (std::max)(r.bombMax, Count(kBombCls, true, &g_scan));
    if (g_scan.empty()) return Enter(LStep::End);  // the bomb is gone before the detonate
    void* parent = nullptr;
    if (E::GetAttachParentActor(g_scan[0], parent) && parent) {
        r.attached = 1;
        return Enter(LStep::Detonate);
    }
    if (Past(g_src.stepAt, kAttachBound)) {
        r.attached = 0;
        Enter(LStep::Detonate);
    }
}

void Detonate(void* player) {
    void* held = HeldOf(player, L"prop_funGun_stickyLauncher_C");
    if (!held) return Skip("the launcher is gone before its detonate");
    g_src.base = ES::GetCounters();
    g_src.verbAt = Clock::now();
    g_src.r.detonated = CallHandVerb(held, player, L"playerHandUse_RMB");
    if (!g_src.r.detonated) return Skip("playerHandUse_RMB did not run on the launcher");
    Enter(LStep::Birth);
}

void Birth(const LegDef& d, bool host) {
    Reading& r = g_src.r;
    if (d.kind == Kind::Rocket) {
        g_scan.clear();
        r.rocketMax = (std::max)(r.rocketMax, Count(kRocketCls, true, &g_scan));
        if (!r.track.Raw() && !g_scan.empty()) Track(g_scan[0]);
    }
    if (IsSticky(d.kind)) r.bombMax = (std::max)(r.bombMax, Count(kBombCls, true));
    if (BirthSeen(host)) {
        r.birth = true;
        r.ms = Since(g_src.verbAt);
        return Enter(LStep::End);
    }
    if (Past(g_src.verbAt, kBirthBound)) Enter(LStep::End);
}

void Aim(void* player) {
    const ue_wrap::FRotator want = coop::director::LookAt(E::GetCameraLocation(), g_src.aim);
    E::SetControlRotation(E::GetController(player), want);
    const ue_wrap::FRotator got = E::GetCameraRotation();
    if (AngDiff(got.Yaw, want.Yaw) < kAimTolDeg && AngDiff(got.Pitch, want.Pitch) < kAimTolDeg)
        return Enter(LStep::Act);
    if (Late(kStepBound)) Skip("the camera never reached the aim");
}

void BeginItem(const LegDef& d) {
    if (!ResolveClass(d.cls, d.path)) return Skip("the item's class is neither loaded nor loadable by path");
    if (!coop::dev::drill_hand::TakeIntoHandNow(d.row, d.cls)) return Skip("the hand could not take the item");
    Enter(LStep::Held);
}

}  // namespace coop::dev::explosion_drill::detail
