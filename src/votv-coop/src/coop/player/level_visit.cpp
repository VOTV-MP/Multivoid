// coop/player/level_visit.cpp -- see coop/player/level_visit.h.

#include "coop/player/level_visit.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/death_revive.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/active_events.h"
#include "ue_wrap/world/level_instance.h"
#include "ue_wrap/world/world_singleton.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <string>
#include <vector>

namespace coop::level_visit {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
namespace sg = ue_wrap::script_gate;
namespace LI = ue_wrap::level_instance;

// lib_C is a function library whose class and functions are new objects with each level: name watches.
UE_FACT_FN(kRowLoadLevel, "level_visit", nullptr, L"loadLevel", AnyClass);
UE_FACT_FN(kRowEnd, "level_visit", L"lib_C", L"End", Declared);
UE_FACT_FN(kRowQuitGame, "level_visit", L"KismetSystemLibrary", L"QuitGame", Declared);
constexpr int kTagLoadLevel = 0x4C564C56;  // 'LVLV'
constexpr int kTagEnd = 0x4C56454E;        // 'LVEN'

// The maps the clip picks (mainPlayer_C's ReceiveTick); the tutorial's own pick is not a session's.
constexpr const wchar_t* kMaps[] = {L"sl_backroomsObsolete", L"untitled_untitled", L"untitled_219"};
// Where a visited map is put: far below the world, where nothing of the world reaches.
constexpr ue_wrap::FVector kOrigin{0.f, 0.f, -60000.f};
// A copy below this height is a player in a visited map: hidden on every other machine.
constexpr float kAwayZ = -30000.f;
// A visitor this far from the map's origin, above it, came back by another way (the usual revive).
constexpr float kBackCm = 40000.f;
constexpr ULONGLONG kLoadBoundMs = 180000;

enum class Phase : uint8_t { None, Loading, In };

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watching = false, g_quitHooked = false;
int32_t g_quitCtxOff = -1;  // QuitGame's WorldContextObject in its parameters

// Game thread: the visit.
Phase g_phase = Phase::None;
std::wstring g_map, g_lastEnd;
void* g_streaming = nullptr;
void* g_level = nullptr;
ue_wrap::FVector g_home{};
ULONGLONG g_loadAtMs = 0;
std::wstring g_slotName;   // the game instance's slot name before the map's script blanked it
std::wstring g_pass;       // and its NewVar_6, which the clip writes for untitled_219
uint8_t g_gameMode = 0;    // and its game mode byte
bool g_haveGameMode = false;
// Raised by a script callback, run by the next tick: the visit's end, and whether a revive is owed.
std::atomic<bool> g_endAsked{false};
std::atomic<bool> g_endIsDeath{false};

bool InSession() {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    return s && s->running();
}

bool IsVisitMap(const R::FName& level) {
    for (const wchar_t* m : kMaps)
        if (R::NameEquals(level, m)) return true;
    return false;
}

void* Local() { return coop::players::Registry::Get().Local(); }

int32_t GiOffset(const wchar_t* name) {
    void* gi = ue_wrap::world_singleton::GameInstance();
    return gi ? R::FindPropertyOffset(R::ClassOf(gi), name) : -1;
}

void Teleport(void* player, const ue_wrap::FVector& at) {
    void* f = player ? R::FindFunction(R::ClassOf(player), L"teleportWObackrooms") : nullptr;
    ue_wrap::ParamFrame pf(f);
    ue_wrap::FTransform t{};
    t.TX = at.X; t.TY = at.Y; t.TZ = at.Z;
    if (f && pf.valid() && pf.Set<ue_wrap::FTransform>(L"newTransform", t)) ue_wrap::Call(player, pf);
}

bool InVisitedLevel(void* obj) { return obj && g_level && R::IsLive(obj) && R::OuterOf(obj) == g_level; }

// ---- the watches (game thread, inside the VM's body loop: decisions only, the work is the tick's) ----------------

sg::Verdict OnLoadLevelPre(const sg::Call& call) {
    if (!call.locals || !call.function || !InSession()) return sg::Verdict::Run;
    void* owner = R::OuterOf(call.function);
    if (!owner || !R::NameEquals(R::NameOf(owner), L"lib_C")) return sg::Verdict::Run;  // the basement flooder's
    static int32_t sLevelOff = -1, sAuthorOff = -1;
    if (sLevelOff < 0) sLevelOff = R::FindParamOffset(call.function, L"level");
    if (sAuthorOff < 0) sAuthorOff = R::FindParamOffset(call.function, L"__WorldContext");
    if (sLevelOff < 0 || sAuthorOff < 0) return sg::Verdict::Run;
    const R::FName level = *reinterpret_cast<const R::FName*>(call.locals + sLevelOff);
    if (!IsVisitMap(level)) return sg::Verdict::Run;
    void* author = *reinterpret_cast<void* const*>(call.locals + sAuthorOff);
    if (author && author != Local() && coop::players::Registry::Get().IsPuppet(author)) {
        UE_LOGW("level_visit: REFUSED a trip to %ls authored by a peer's copy -- the clip is that player's machine's",
                R::ToString(level).c_str());
        return sg::Verdict::Cancel;
    }
    if (author != Local() || g_phase != Phase::None) return sg::Verdict::Run;
    const std::wstring map = R::ToString(level);
    UE_LOGI("level_visit: the clip's trip to %ls is this player's alone -- the map is loaded below the world",
            map.c_str());
    ue_wrap::game_thread::Post([map] {
        if (g_phase != Phase::None) return;
        void* me = Local();
        if (!me) return;
        g_map = map;
        E::TryGetActorLocation(me, g_home);
        void* gi = ue_wrap::world_singleton::GameInstance();
        const int32_t offSlot = GiOffset(L"slotName"), offMode = GiOffset(L"gamemode"), offFlag = GiOffset(L"NewVar_0");
        g_slotName = (gi && offSlot >= 0) ? ue_wrap::field_io::ReadFStringAt(gi, offSlot) : std::wstring();
        g_haveGameMode = gi && offMode >= 0;
        if (g_haveGameMode) g_gameMode = *(static_cast<uint8_t*>(gi) + offMode);
        if (gi && offFlag >= 0) *(static_cast<uint8_t*>(gi) + offFlag) = 1;  // the clip's own flag, which a map checks
        // The string the clip writes when it picks untitled_219 (empty for the others), which that map checks.
        const int32_t offPass = GiOffset(L"NewVar_6");
        if (gi && offPass >= 0) {
            g_pass = ue_wrap::field_io::ReadFStringAt(gi, offPass);
            ue_wrap::field_io::WriteFStringField(gi, offPass, _wcsicmp(map.c_str(), L"untitled_219") == 0
                                                                 ? L"$TPzHL8tj^f@6DJ!JmqkMmhHQUPG*5rTfxWR"
                                                                 : L"");
        }
        coop::death_revive::ClearBlackScreenNow();
        g_streaming = LI::Load(me, g_map, kOrigin);
        g_level = nullptr;
        g_loadAtMs = ::GetTickCount64();
        g_endAsked.store(false);
        g_phase = g_streaming ? Phase::Loading : Phase::None;
        UE_LOGI("level_visit: %ls %hs", g_map.c_str(), g_streaming ? "loading" : "did NOT load -- the player stays");
    });
    return sg::Verdict::Cancel;
}

sg::Verdict OnEndPre(const sg::Call& call) {
    if (g_phase == Phase::None || !call.locals || !call.function) return sg::Verdict::Run;
    static int32_t sCtxOff = -1;
    if (sCtxOff < 0) sCtxOff = R::FindParamOffset(call.function, L"__WorldContext");
    void* ctx = sCtxOff >= 0 ? *reinterpret_cast<void* const*>(call.locals + sCtxOff) : nullptr;
    if (!InVisitedLevel(ctx) && !InVisitedLevel(call.callerObject)) return sg::Verdict::Run;
    g_endIsDeath.store(true);
    g_endAsked.store(true);
    return sg::Verdict::Cancel;
}

sg::Verdict OnQuitGamePre(void* context, void* sourceObject, const uint8_t* parms) {
    if (g_phase == Phase::None) return sg::Verdict::Run;
    void* worldContext = (parms && g_quitCtxOff >= 0) ? *reinterpret_cast<void* const*>(parms + g_quitCtxOff) : nullptr;
    if (!InVisitedLevel(sourceObject) && !InVisitedLevel(context) && !InVisitedLevel(worldContext))
        return sg::Verdict::Run;
    g_endIsDeath.store(true);
    g_endAsked.store(true);
    return sg::Verdict::Cancel;
}

// ---- the visit (game thread) ---------------------------------------------------------------------------------------

// The events the visited level raised (its script calls lib_C::setEvent(true) on itself), ended as the game ends one.
void EndLevelEvents() {
    ue_wrap::active_events::Senders snd{};
    if (!ue_wrap::active_events::ReadSenders(snd) || snd.num <= 0) return;
    std::vector<void*> ours;
    for (int32_t i = 0; i < snd.num; ++i)
        if (InVisitedLevel(snd.data[i])) ours.push_back(snd.data[i]);
    void* lib = R::FindClassDefaultObject(L"lib_C");
    void* fn = lib ? R::FindFunction(R::ClassOf(lib), L"setEvent") : nullptr;
    for (void* s : ours) {
        ue_wrap::ParamFrame pf(fn);
        if (fn && pf.valid() && pf.Set<bool>(L"isEventActive", false) &&
            pf.Set<bool>(L"deactivateAmbientTrack", false) && pf.Set<void*>(L"__WorldContext", s))
            ue_wrap::Call(lib, pf);
    }
}

void Finish(bool revive, const wchar_t* why) {
    if (g_phase == Phase::None) return;
    void* me = Local();
    EndLevelEvents();
    if (me) LI::DeactivateUntaggedReverb(me);
    void* gi = ue_wrap::world_singleton::GameInstance();
    const int32_t offSlot = GiOffset(L"slotName"), offMode = GiOffset(L"gamemode"), offFlag = GiOffset(L"NewVar_0");
    if (gi && offSlot >= 0 && ue_wrap::field_io::ReadFStringAt(gi, offSlot) != g_slotName)
        ue_wrap::field_io::WriteFStringField(gi, offSlot, g_slotName);
    if (gi && offMode >= 0 && g_haveGameMode) *(static_cast<uint8_t*>(gi) + offMode) = g_gameMode;
    if (gi && offFlag >= 0) *(static_cast<uint8_t*>(gi) + offFlag) = 0;
    const int32_t offPass = GiOffset(L"NewVar_6");
    if (gi && offPass >= 0) ue_wrap::field_io::WriteFStringField(gi, offPass, g_pass);
    const bool unloaded = LI::Unload(g_streaming);
    const bool canRevive = revive && coop::death_revive::ReviveAvailable();
    if (canRevive) coop::death_revive::NoteRunEndCancelled(nullptr, L"the visit's end", false);
    else if (revive && me) Teleport(me, g_home);
    UE_LOGI("level_visit: %ls ended (%ls) -- unloaded=%d, %hs", g_map.c_str(), why, unloaded ? 1 : 0,
            canRevive ? "the player is revived in the world" : revive ? "the player is put back where it clipped"
                                                                : "the player is already back");
    g_lastEnd = why;
    g_phase = Phase::None;
    g_streaming = nullptr;
    g_level = nullptr;
    g_endAsked.store(false);
}

// The copies of the world's own systems a map may carry: untitled_untitled's ATV and its map board are gone, so the
// ATV lane counts one ATV, and untitled_219's day-night cycle stops ticking, so its tick (which runs the world's
// time and its scheduled events) does not run them twice; that map's own timeline sets the cycle's day directly.
void HoldCycleTick() {
    void* cls = OI::ClassByName(L"daynightCycle_C");
    std::vector<void*> found;
    struct Ctx { void* level; std::vector<void*>* out; } c{g_level, &found};
    if (cls)
        OI::ForEachInstance(cls, [](void* ctx, void* o, int32_t) {
            auto* k = static_cast<Ctx*>(ctx);
            if (R::IsLive(o) && R::OuterOf(o) == k->level) k->out->push_back(o);
        }, &c);
    for (void* cycle : found) {
        void* fn = R::FindDispatchFunctionCached(R::ClassOf(cycle), L"SetActorTickEnabled");
        ue_wrap::ParamFrame pf(fn);
        if (fn && pf.valid() && pf.Set<bool>(L"bEnabled", false)) ue_wrap::Call(cycle, pf);
    }
}

void RemoveWorldDuplicates() {
    for (const wchar_t* name : {L"ATV_C", L"atvMap_C"}) {
        void* cls = OI::ClassByName(name);
        std::vector<void*> found;
        struct Ctx { void* level; std::vector<void*>* out; } c{g_level, &found};
        if (cls)
            OI::ForEachInstance(cls, [](void* ctx, void* o, int32_t) {
                auto* k = static_cast<Ctx*>(ctx);
                if (R::IsLive(o) && R::OuterOf(o) == k->level) k->out->push_back(o);
            }, &c);
        for (void* a : found) {
            void* destroy = R::FindDispatchFunctionCached(R::ClassOf(a), L"K2_DestroyActor");
            ue_wrap::ParamFrame d(destroy);
            if (destroy && d.valid()) ue_wrap::Call(a, d);
        }
    }
}

// The map's own placed player (each map is a world of its own and carries one): where its player starts, and then
// gone, so it is not a second player in this one.
ue_wrap::FVector TakePlacedPlayer() {
    ue_wrap::FVector at = kOrigin;
    void* cls = OI::ClassByName(L"mainPlayer_C");
    struct Ctx { void* level; void* found; } c{g_level, nullptr};
    if (cls)
        OI::ForEachInstance(cls, [](void* ctx, void* o, int32_t) {
            auto* k = static_cast<Ctx*>(ctx);
            if (!k->found && R::IsLive(o) && R::OuterOf(o) == k->level) k->found = o;
        }, &c);
    if (c.found) {
        E::TryGetActorLocation(c.found, at);
        void* destroy = R::FindDispatchFunctionCached(R::ClassOf(c.found), L"K2_DestroyActor");
        ue_wrap::ParamFrame d(destroy);
        if (destroy && d.valid()) ue_wrap::Call(c.found, d);
    }
    return at;
}

// Every copy below the world is a player in a visited map: hidden there, shown again once it is back.
void HideAway() {
    static ULONGLONG s_next = 0;
    static void* s_copy[coop::net::kMaxPeers] = {};
    static bool s_hidden[coop::net::kMaxPeers] = {};
    const ULONGLONG now = ::GetTickCount64();
    if (now < s_next) return;
    s_next = now + 1000;
    auto& reg = coop::players::Registry::Get();
    for (int slot = 0; slot < static_cast<int>(coop::net::kMaxPeers); ++slot) {
        coop::RemotePlayer* p = reg.Puppet(static_cast<uint8_t>(slot));
        void* actor = p ? p->actor() : nullptr;
        ue_wrap::FVector at{};
        const bool away = actor && E::TryGetActorLocation(actor, at) && at.Z < kAwayZ;
        if (actor == s_copy[slot] && away == s_hidden[slot]) continue;
        if (actor) E::SetActorHiddenInGame(actor, away);
        if (away != s_hidden[slot])
            UE_LOGI("level_visit: slot %d's copy %hs", slot, away ? "hidden -- that player is in another map"
                                                               : "shown -- that player is back");
        s_copy[slot] = actor;
        s_hidden[slot] = away;
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!g_watching && sg::IsInstalled() && sg::WatchName(kRowLoadLevel, kTagLoadLevel, &OnLoadLevelPre, nullptr) &&
        sg::WatchName(kRowEnd, kTagEnd, &OnEndPre, nullptr)) {
        g_watching = true;
        UE_LOGI("level_visit: watching lib_C::loadLevel and lib_C::End -- a clip's trip is the clipping player's "
                "alone");
    }
    if (!g_quitHooked) {
        const ue_wrap::facts::Resolved quit = ue_wrap::facts::Resolve(kRowQuitGame);
        if (quit.Fn()) g_quitCtxOff = R::FindParamOffset(quit.Fn(), L"WorldContextObject");
        if (quit.Fn() && ue_wrap::ufunction_hook::InstallPreHook(quit, &OnQuitGamePre, /*armed=*/true)) {
            g_quitHooked = true;
            UE_LOGI("level_visit: QuitGame from a visited map is refused");
        }
    }
}

void Tick() {
    if (!InSession()) return;
    if (!g_watching || !g_quitHooked) Install(g_session.load(std::memory_order_acquire));
    HideAway();
    if (g_phase == Phase::None) return;
    void* me = Local();
    if (g_endAsked.load()) return Finish(g_endIsDeath.load(), L"the map's own end");
    if (g_phase == Phase::Loading) {
        g_level = LI::LoadedLevel(g_streaming);
        static ULONGLONG s_said = 0;
        if (!g_level && ::GetTickCount64() - s_said > 5000) {
            s_said = ::GetTickCount64();
            UE_LOGI("level_visit: %ls still loading (%llu ms) -- %hs", g_map.c_str(), ::GetTickCount64() - g_loadAtMs,
                    LI::DescribeState(g_streaming).c_str());
        }
        if (!g_level) {
            if (::GetTickCount64() - g_loadAtMs > kLoadBoundMs) Finish(true, L"the map did not load");
            return;
        }
        const ue_wrap::FVector start = TakePlacedPlayer();
        RemoveWorldDuplicates();
        HoldCycleTick();
        if (me) Teleport(me, {start.X, start.Y, start.Z + 50.f});
        g_phase = Phase::In;
        UE_LOGI("level_visit: %ls loaded below the world; the player is in it at (%.0f,%.0f,%.0f)", g_map.c_str(),
                start.X, start.Y, start.Z);
        return;
    }
    ue_wrap::FVector at{};
    if (me && E::TryGetActorLocation(me, at) && at.Z > kAwayZ) {
        const float dx = at.X - kOrigin.X, dy = at.Y - kOrigin.Y, dz = at.Z - kOrigin.Z;
        if (std::sqrt(dx * dx + dy * dy + dz * dz) > kBackCm) Finish(false, L"the player came back another way");
    }
}

void OnDisconnect() {
    if (g_phase != Phase::None) {
        void* me = Local();
        Finish(false, L"the session ended");
        if (me) Teleport(me, g_home);
    }
}

bool InVisitedMap(void* actor) {
    if (g_phase == Phase::None || !actor) return false;
    void* me = Local();
    void* level = R::OuterOf(actor);
    return me && level && level != R::OuterOf(me);
}

bool Visiting() { return g_phase != Phase::None; }
std::wstring CurrentMap() { return g_phase != Phase::None ? g_map : std::wstring(); }
std::wstring LastEnd() { return g_lastEnd; }
void* CurrentLevel() { return g_phase == Phase::In ? g_level : nullptr; }

}  // namespace coop::level_visit
