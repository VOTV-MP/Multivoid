// coop/player/level_visit.cpp -- see coop/player/level_visit.h.

#include "coop/player/level_visit.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/death_revive.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_events.h"
#include "coop/player/puppet_hide.h"

#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/active_events.h"
#include "ue_wrap/world/level_instance.h"
#include "ue_wrap/world/world_singleton.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <string>
#include <unordered_set>
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
// The game's own clip check, a Blueprint function on the game mode: a name watch on the class that owns it.
UE_FACT_FN(kRowCanBackrooms, "level_visit", L"mainGamemode_C", L"canBackrooms", Declared);
constexpr int kTagCanBackrooms = 0x4C564342;  // 'LVCB'

// The maps the clip picks (mainPlayer_C's ReceiveTick); the tutorial's own pick is not a session's.
constexpr const wchar_t* kMaps[] = {L"sl_backroomsObsolete", L"untitled_untitled", L"untitled_219"};
// Where a visited map is put: far below the world, where nothing of the world reaches.
constexpr ue_wrap::FVector kOrigin{0.f, 0.f, -60000.f};
// A visitor this far from the map's origin, above it, came back by another way (the usual revive).
constexpr float kBackCm = 40000.f;
constexpr ULONGLONG kLoadBoundMs = 180000;

enum class Phase : uint8_t { None, Loading, In };

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watching = false, g_quitHooked = false;
int32_t g_quitCtxOff = -1;  // QuitGame's WorldContextObject in its parameters

// Game thread: the visit.
Phase g_phase = Phase::None;
// A trip the loadLevel watch let through, until the posted task that starts its visit has run: the gap in which no phase
// says a visit is under way yet.
bool g_tripPending = false;
std::wstring g_map, g_lastEnd;
void* g_streaming = nullptr;
void* g_level = nullptr;
void* g_controller = nullptr;  // this machine's player controller, kept at the trip's start for the visit's end
// The world's own actors (its persistent level's) when the visit began: what a map's script spawns lands in that level
// too, far below the world, and is told apart from them at the end and removed with the map.
std::unordered_set<void*> g_worldActors;
// The visitor's own item in hand during the visit (one of the world's), and the one to give back after the end: the
// game's death drops what the player holds where it dies, which is in the map.
void* g_carried = nullptr;
void* g_carriedClass = nullptr;  // its class: the game's drop replaces the held actor with a new one of it
void* g_return = nullptr;
ULONGLONG g_returnSinceMs = 0;
ue_wrap::FVector g_home{};
ULONGLONG g_loadAtMs = 0;
std::wstring g_slotName;   // the game instance's slot name before the map's script blanked it
std::wstring g_pass;       // and its NewVar_6, which the clip writes for untitled_219
uint8_t g_gameMode = 0;    // and its game mode byte
bool g_haveGameMode = false;
// Raised by a script callback, run by the next tick: the visit's end, and whether a revive is owed.
std::atomic<bool> g_endAsked{false};
std::atomic<bool> g_endIsDeath{false};
std::wstring g_endBy;  // which calls asked for the end, for its log line
// Until when the game's clip check is answered no after a visit: the view's jump back out of it is not a clip.
std::atomic<ULONGLONG> g_quietUntilMs{0};
constexpr ULONGLONG kQuietMs = 5000;

// A visit is under way from the moment its trip is let through, before and after its phase says so.
bool VisitUnderway() { return g_phase != Phase::None || g_tripPending; }

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

// This machine's own player: the pawn the game's clip may send on a trip. Every other mainPlayer_C is a peer's copy or a
// map's placed player, and none of them is a trip of this machine's.
bool OwnPlayer(void* obj) { return obj && obj == Local(); }
bool IsPlayerPawn(void* obj) {
    return obj && R::NameEquals(R::NameOf(R::ClassOf(obj)), ue_wrap::profile::name::MainPlayerClass);
}

// `pc` possesses `pawn` (AController::Possess): the pawn it had is let go, and the view follows the new one.
bool Possess(void* pc, void* pawn) {
    void* fn = pc && R::IsLive(pc) ? R::FindDispatchFunctionCached(R::ClassOf(pc), L"Possess") : nullptr;
    ue_wrap::ParamFrame pf(fn);
    return fn && pf.valid() && pawn && pf.Set<void*>(L"InPawn", pawn) && ue_wrap::Call(pc, pf);
}

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

// Every actor of the world's own level (the level the local player is in): what the object index lists with that
// outer.
std::vector<void*> WorldLevelActors() {
    struct Ctx { void* level; std::vector<void*> out; } c{R::OuterOf(Local()), {}};
    if (!c.level) return {};
    OI::ForEachClass([](void* ctx, void* cls, void*) {
        OI::ForEachInstance(cls, [](void* ctx2, void* o, int32_t) {
            auto* k = static_cast<Ctx*>(ctx2);
            if (R::OuterOf(o) == k->level && R::IsLive(o)) k->out.push_back(o);
        }, ctx);
    }, &c);
    return c.out;
}

void* HeldActor(void* me) {
    static int32_t sOff = -2;
    if (sOff == -2) sOff = me ? R::FindPropertyOffset(R::ClassOf(me), L"holding_actor") : -2;
    void* h = (me && sOff >= 0) ? *reinterpret_cast<void* const*>(static_cast<const char*>(me) + sOff) : nullptr;
    return h && R::IsLive(h) ? h : nullptr;
}

bool InVisitedLevel(void* obj) { return obj && g_level && R::IsLive(obj) && R::OuterOf(obj) == g_level; }

// Whether `obj` belongs to the visited map, for the whole visit: once the level is known, its own objects; while it
// still loads (its BeginPlay and first ticks run inside the engine's add-to-world, before the tick below sees it), any
// object of a level other than the world's own.
bool FromVisitedMap(void* obj) {
    if (g_phase == Phase::None || !obj || !R::IsLive(obj)) return false;
    if (g_level) return InVisitedLevel(obj);
    void* level = R::OuterOf(obj);
    void* mine = R::OuterOf(Local());
    return level && mine && level != mine && R::NameEquals(R::NameOf(R::ClassOf(level)), L"Level");
}

// The host's world.backrooms, the session's value on every peer, kept as it changes (config::Subscribe): the clip check
// below asks for it on every tick, and a layered resolve reads the environment each time.
std::atomic<bool> g_closed{false};
void OnBackroomsRow() {
    const bool closed = !coop::config::ResolveFlag(coop::config_registry::rows::world_backrooms);
    if (g_closed.exchange(closed) != closed) UE_LOGI("level_visit: the host %hs the backrooms", closed ? "closed" : "opened");
}

// [dev] The drill's counts, and its red arms (each latched once).
std::atomic<uint32_t> g_tripsStarted{0}, g_clipsHeldClosed{0}, g_clipChecksAsked{0};
std::atomic<int> g_lastClipWhy{-1};
bool CloseOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::backrooms_close_off);
    return off;
}

// ---- the watches (game thread, inside the VM's body loop: decisions only, the work is the tick's) ----------------

// The game's own clip check, mainGamemode_C::canBackrooms(out return): mainPlayer_C's tick asks it every tick, and its
// answer gates the whole clip chain (black screen, teleport, save, then lib_C::loadLevel). It is answered no, in the out
// parameter, for this machine's own player while the host has closed the backrooms, a visit runs, or the quiet window
// after one is open (the view's jump into the map and back out would read as a clip); otherwise the game's own logic
// runs. A player that is not this machine's own (a copy, whose tick is off, or the visited map's placed player, which
// ticks and is the view target until it is taken) is always answered no: a trip is this machine's own player's alone.
// When the out parameter cannot be written the body runs and the clip is not held: said once.
bool AnswerNo(const sg::Call& call) {
    static void* sFn = nullptr;
    static int32_t sOff = -1;
    if (call.function != sFn) {
        sFn = call.function;
        sOff = R::FindParamOffset(call.function, L"return");
    }
    uint8_t* out = sOff >= 0 ? sg::OutParamPtr(call, sOff) : nullptr;
    if (!out) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            UE_LOGW("level_visit: canBackrooms's result cannot be written -- the clip is NOT held (said once)");
        }
        return false;
    }
    *out = 0;
    return true;
}

sg::Verdict OnCanBackroomsPre(const sg::Call& call) {
    if (!call.function || !call.callerObject || !InSession() || CloseOff()) return sg::Verdict::Run;
    if (!OwnPlayer(call.callerObject)) {
        static bool s_saidOther = false;
        if (!s_saidOther) {
            s_saidOther = true;
            UE_LOGI("level_visit: the game's clip check is answered no for a player that is not this machine's own "
                    "(said once)");
        }
        return AnswerNo(call) ? sg::Verdict::Cancel : sg::Verdict::Run;
    }
    const bool closed = g_closed.load();
    const bool visiting = VisitUnderway();
    const int why = closed ? 1 : visiting ? 2 : ::GetTickCount64() < g_quietUntilMs.load() ? 3 : 0;
    ++g_clipChecksAsked;
    g_lastClipWhy = why;
    static int s_said = 0;
    if (why != s_said) {
        s_said = why;
        if (why)
            UE_LOGI("level_visit: the game's clip check is answered no -- %hs",
                    why == 1 ? "the host has closed the backrooms" : why == 2 ? "a visit runs" : "a visit just ended");
    }
    if (!why || !AnswerNo(call)) return sg::Verdict::Run;
    if (closed) ++g_clipsHeldClosed;
    return sg::Verdict::Cancel;
}

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
    // A copy cannot author a trip (a copy is never the view target and its actor tick is off), but the visited map's
    // placed player can: it ticks the clip chain as the player does and holds the view until it is taken. So the author
    // is judged by ownership: a player that is not this machine's own is refused, and only its own player's call goes on.
    if (!OwnPlayer(author)) {
        if (!IsPlayerPawn(author)) return sg::Verdict::Run;
        static bool s_saidOther = false;
        if (!s_saidOther) {
            s_saidOther = true;
            UE_LOGW("level_visit: a trip to %ls authored by a player that is not this machine's own is refused (said "
                    "once)", R::ToString(level).c_str());
        }
        return sg::Verdict::Cancel;
    }
    // The game's own clip is not taken seated or riding (mainGamemode_C::canBackrooms); a trip asked so is not one.
    void* on = nullptr;
    void* parent = nullptr;
    if ((E::ReadMainPlayerSittingOn(author, on) && on) || (E::GetAttachParentActor(author, parent) && parent)) {
        UE_LOGW("level_visit: a trip to %ls while the player is seated or riding is refused -- the clip never sends "
                "one", R::ToString(level).c_str());
        return sg::Verdict::Cancel;
    }
    const std::wstring map = R::ToString(level);
    // The host's closed switch holds for a chain that passed the clip check before the row reached this machine too.
    if (g_closed.load() && !CloseOff()) {
        UE_LOGI("level_visit: a trip to %ls is refused -- the host has closed the backrooms", map.c_str());
        return sg::Verdict::Cancel;
    }
    // One visit at a time: a second call of the clip chain (or of a clip after the first is let through) is not a trip.
    if (g_phase != Phase::None || g_tripPending) {
        UE_LOGI("level_visit: a trip to %ls is refused -- a visit is already under way", map.c_str());
        return sg::Verdict::Cancel;
    }
    g_tripPending = true;
    ++g_tripsStarted;
    UE_LOGI("level_visit: the clip's trip to %ls is this player's alone -- the map is loaded below the world",
            map.c_str());
    ue_wrap::game_thread::Post([map] {
        struct Settled { ~Settled() { g_tripPending = false; } } settled;
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
        g_controller = E::GetController(me);  // the placed player takes it as its level begins play: kept now
        coop::death_revive::ClearBlackScreenNow();
        g_worldActors.clear();
        for (void* a : WorldLevelActors()) g_worldActors.insert(a);
        g_carried = HeldActor(me);
        g_carriedClass = g_carried ? R::ClassOf(g_carried) : nullptr;
        g_streaming = LI::Load(me, g_map, kOrigin);
        g_level = nullptr;
        g_loadAtMs = ::GetTickCount64();
        g_endAsked.store(false);
        g_phase = g_streaming ? Phase::Loading : Phase::None;
        if (!g_streaming) g_controller = nullptr;
        UE_LOGI("level_visit: %ls %hs", g_map.c_str(), g_streaming ? "loading" : "did NOT load -- the player stays");
    });
    return sg::Verdict::Cancel;
}

// untitled_untitled's hook check (a 5 s timer into its event graph) ends the map when GetActorOfClass finds a grappling
// hook. In the game the map is the whole world, so a hook there is one the player brought; loaded in place, the world's
// own hooks are found too. Only a hook down in the map counts. Its graph's other ends are the guard (not taken: the
// visit sets NewVar_0) and its sequence's end after a 2400 s delay, so an End from that graph before then is the check.
constexpr ULONGLONG kUntitledSequenceMs = 2400000;
bool HookInMap() {
    void* cls = OI::ClassByName(L"prop_hook_C");
    bool found = false;
    if (cls)
        OI::ForEachInstance(cls, [](void* ctx, void* o, int32_t) {
            ue_wrap::FVector at{};
            if (R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__") && E::TryGetActorLocation(o, at) &&
                at.Z < kVisitFloorZ)
                *static_cast<bool*>(ctx) = true;
        }, &found);
    return found;
}

// Set when the hook check's End was let pass as no end: the QuitGame it calls next, in the same frame, is let pass too.
bool g_hookCheckQuit = false;

sg::Verdict OnEndPre(const sg::Call& call) {
    if (g_phase == Phase::None || !call.locals || !call.function) return sg::Verdict::Run;
    static int32_t sCtxOff = -1;
    if (sCtxOff < 0) sCtxOff = R::FindParamOffset(call.function, L"__WorldContext");
    void* ctx = sCtxOff >= 0 ? *reinterpret_cast<void* const*>(call.locals + sCtxOff) : nullptr;
    if (!FromVisitedMap(ctx) && !FromVisitedMap(call.callerObject)) return sg::Verdict::Run;
    if (::GetTickCount64() - g_loadAtMs < kUntitledSequenceMs && call.callerObject &&
        R::NameStartsWith(R::NameOf(R::ClassOf(call.callerObject)), L"untitled_untitled") &&
        !HookInMap()) {
        g_hookCheckQuit = true;
        static bool s_said = false;
        if (!s_said) {
            s_said = true;
            UE_LOGI("level_visit: untitled_untitled's hook check found a hook outside the map -- not an end "
                    "(said once)");
        }
        return sg::Verdict::Cancel;
    }
    if (!g_level) {
        static bool s_saidLoading = false;
        if (!s_saidLoading) {
            s_saidLoading = true;
            UE_LOGI("level_visit: the visited map's End during loading is cancelled");
        }
    }
    const std::wstring by = call.callerObject ? R::ClassNameOf(call.callerObject) : std::wstring(L"?");
    const std::wstring in = call.callerFunction ? R::ToString(R::NameOf(call.callerFunction)) : std::wstring(L"?");
    g_endBy += L", End from " + by + L"." + in + L" on " + (ctx ? R::ClassNameOf(ctx) : std::wstring(L"?"));
    g_endIsDeath.store(true);
    g_endAsked.store(true);
    return sg::Verdict::Cancel;
}

sg::Verdict OnQuitGamePre(void* context, void* sourceObject, const uint8_t* parms) {
    if (g_phase == Phase::None) return sg::Verdict::Run;
    void* worldContext = (parms && g_quitCtxOff >= 0) ? *reinterpret_cast<void* const*>(parms + g_quitCtxOff) : nullptr;
    if (!FromVisitedMap(sourceObject) && !FromVisitedMap(context) && !FromVisitedMap(worldContext))
        return sg::Verdict::Run;
    if (g_hookCheckQuit) {
        g_hookCheckQuit = false;
        return sg::Verdict::Cancel;
    }
    if (!g_level) {
        static bool s_saidLoading = false;
        if (!s_saidLoading) {
            s_saidLoading = true;
            UE_LOGI("level_visit: the visited map's QuitGame during loading is cancelled");
        }
    }
    g_endBy += L", QuitGame from " + (sourceObject ? R::ClassNameOf(sourceObject) : std::wstring(L"?")) + L" on " +
               (worldContext ? R::ClassNameOf(worldContext) : std::wstring(L"?"));
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

// The world's sky: what lights or covers a level without a bound on its reach. A map travelled to is a world of its own
// with none of the world's; loaded in place, 600 m below, it would have all of it. The height fog's density grows
// without bound below its height and covers the map in its colour; the sun and the sky light light a maze the game
// leaves dark but for the player's lantern; the world's unbound post process sets the view's exposure and grading.
// The components are hidden and the volumes switched off for the visit on this machine, and put back after.
constexpr const wchar_t* kWorldSkyClasses[] = {L"ExponentialHeightFogComponent", L"DirectionalLightComponent",
                                               L"SkyLightComponent", L"SkyAtmosphereComponent",
                                               L"VolumetricCloudComponent"};
constexpr const wchar_t* kMapLightClasses[] = {L"PointLightComponent", L"SpotLightComponent", L"RectLightComponent",
                                               L"DirectionalLightComponent", L"SkyLightComponent",
                                               L"ExponentialHeightFogComponent"};
std::vector<void*> g_hiddenSky;
std::vector<void*> g_offVolumes;
ULONGLONG g_skyHeldAtMs = 0;

bool SetComponentVisible(void* comp, bool on) {
    void* fn = comp && R::IsLive(comp) ? R::FindDispatchFunctionCached(R::ClassOf(comp), L"SetVisibility") : nullptr;
    ue_wrap::ParamFrame pf(fn);
    return fn && pf.valid() && pf.Set<bool>(L"bNewVisibility", on) && pf.Set<bool>(L"bPropagateToChildren", false) &&
           ue_wrap::Call(comp, pf);
}

// Every live instance of the class named, split by whether its level (a component's actor's, or an actor's own) is
// the visited one.
void InstancesOf(const wchar_t* name, bool component, std::vector<void*>& world, std::vector<void*>& map) {
    void* cls = OI::ClassByName(name);
    if (!cls) return;
    struct Ctx { bool component; std::vector<void*>* world; std::vector<void*>* map; } c{component, &world, &map};
    OI::ForEachInstance(cls, [](void* ctx, void* o, int32_t) {
        auto* k = static_cast<Ctx*>(ctx);
        if (!R::IsLive(o) || R::NameStartsWith(R::NameOf(o), L"Default__")) return;
        void* level = k->component ? R::OuterOf(R::OuterOf(o)) : R::OuterOf(o);
        (level == g_level ? k->map : k->world)->push_back(o);
    }, &c);
}

// A post process volume's bitfield bool, read or written.
bool VolumeBit(void* v, const wchar_t* name, int set = -1) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(v), name, off, mask) || off < 0) return false;
    uint8_t& b = *(static_cast<uint8_t*>(v) + off);
    if (set >= 0) b = set ? (b | mask) : (b & ~mask);
    return (b & mask) != 0;
}

void HideWorldSky() {
    std::wstring said, map;
    for (const wchar_t* name : kWorldSkyClasses) {
        std::vector<void*> world, inMap;
        InstancesOf(name, true, world, inMap);
        int hidden = 0;
        for (void* c : world)
            if (SetComponentVisible(c, false)) { g_hiddenSky.push_back(c); ++hidden; }
        if (!world.empty()) said += L" " + std::wstring(name) + L"=" + std::to_wstring(hidden);
    }
    std::vector<void*> volumes, mapVolumes;
    InstancesOf(L"PostProcessVolume", false, volumes, mapVolumes);
    for (void* v : volumes)
        if (VolumeBit(v, L"bEnabled")) { VolumeBit(v, L"bEnabled", 0); g_offVolumes.push_back(v); }
    for (const wchar_t* name : kMapLightClasses) {
        std::vector<void*> world, inMap;
        InstancesOf(name, true, world, inMap);
        if (!inMap.empty()) map += L" " + std::wstring(name) + L"=" + std::to_wstring(inMap.size());
    }
    int unbound = 0;
    for (void* v : mapVolumes) unbound += VolumeBit(v, L"bUnbound") ? 1 : 0;
    g_skyHeldAtMs = ::GetTickCount64();
    UE_LOGI("level_visit: the world's sky hidden for the visit:%ls PostProcessVolume=%zu; the map's own:%ls "
            "PostProcessVolume=%zu (unbound %d)", said.c_str(), g_offVolumes.size(), map.empty() ? L" no light" : map.c_str(),
            mapVolumes.size(), unbound);
}

// The day-night cycle sets the sun and the sky as it runs; what it shows again is hidden again, once a second.
void HoldWorldSky() {
    if (::GetTickCount64() - g_skyHeldAtMs < 1000) return;
    g_skyHeldAtMs = ::GetTickCount64();
    for (void* c : g_hiddenSky)
        if (R::IsLive(c)) SetComponentVisible(c, false);
    for (void* v : g_offVolumes)
        if (R::IsLive(v) && VolumeBit(v, L"bEnabled")) VolumeBit(v, L"bEnabled", 0);
}

void ShowWorldSky() {
    for (void* c : g_hiddenSky)
        if (R::IsLive(c)) SetComponentVisible(c, true);
    for (void* v : g_offVolumes)
        if (R::IsLive(v)) VolumeBit(v, L"bEnabled", 1);
    g_hiddenSky.clear();
    g_offVolumes.clear();
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
    if (me && E::GetController(me) == nullptr && g_controller && R::IsLive(g_controller))
        UE_LOGW("level_visit: the player had no controller at the visit's end -- possessed again (ok=%d)",
                Possess(g_controller, me) ? 1 : 0);
    ShowWorldSky();
    // What the map's script spawned into the world's level goes with the map; the visitor, the copies of other
    // players and what the visitor holds stay. The visitor's own item, dropped down there, comes back: the actor it
    // held, or, when the game's death replaced that one with a new drop, the new one of its class.
    int removed = 0;
    void* held = HeldActor(me);
    const bool carriedGone = g_carried && !R::IsLive(g_carried);
    void* dropped = nullptr;
    for (void* a : WorldLevelActors()) {
        ue_wrap::FVector at{};
        void* parent = nullptr;
        if (a == me || a == held || g_worldActors.count(a) || !E::TryGetActorLocation(a, at) || at.Z >= kVisitFloorZ ||
            coop::players::Registry::Get().IsPuppetBody(a) ||
            (E::GetAttachParentActor(a, parent) && parent && (parent == me || parent == held)))
            continue;
        if (carriedGone && !dropped && R::ClassOf(a) == g_carriedClass) {
            dropped = a;
            continue;
        }
        if (E::DestroyActor(a)) ++removed;
    }
    ue_wrap::FVector carriedAt{};
    if (!carriedGone && g_carried && g_carried != held && E::TryGetActorLocation(g_carried, carriedAt) &&
        carriedAt.Z < kVisitFloorZ)
        dropped = g_carried;
    if (dropped) {
        E::TeleportTo(dropped, {g_home.X, g_home.Y, g_home.Z + 50.f}, ue_wrap::FRotator{});
        g_return = dropped;
        g_returnSinceMs = ::GetTickCount64();
    }
    UE_LOGI("level_visit: %d actor(s) the map's script spawned removed with it; the visitor's dropped item %hs",
            removed, dropped ? "comes back" : "-- none");
    g_worldActors.clear();
    g_carried = nullptr;
    g_carriedClass = nullptr;
    const bool unloaded = LI::Unload(g_streaming);
    const bool canRevive = revive && coop::death_revive::ReviveAvailable();
    if (canRevive) coop::death_revive::NoteRunEndCancelled(nullptr, L"the visit's end", false);
    else if (revive && me) Teleport(me, g_home);
    UE_LOGI("level_visit: %ls ended (%ls%ls) -- unloaded=%d, %hs", g_map.c_str(), why, g_endBy.c_str(),
            unloaded ? 1 : 0,
            canRevive ? "the player is revived in the world"
            : revive  ? "the player is put back where it clipped"
                      : "the player is already back");
    g_lastEnd = why;
    g_endBy.clear();
    g_controller = nullptr;
    g_quietUntilMs.store(::GetTickCount64() + kQuietMs);
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
// gone, so it is not a second player in this one. It possessed this machine's controller as its level began play
// (it is placed to be the map's player), so the controller goes back to this machine's player first.
ue_wrap::FVector TakePlacedPlayer(void* me) {
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
        void* pc = E::GetController(c.found);
        if (pc && me && E::GetController(me) == nullptr) {
            UE_LOGI("level_visit: the map's placed player had taken the controller -- given back (ok=%d)",
                    Possess(pc, me) ? 1 : 0);
        }
        void* destroy = R::FindDispatchFunctionCached(R::ClassOf(c.found), L"K2_DestroyActor");
        ue_wrap::ParamFrame d(destroy);
        if (destroy && d.valid()) ue_wrap::Call(c.found, d);
    }
    return at;
}

// What each slot's last applied pose says: its player stands below the visit floor, in a visited map. The pose
// listener only records (coop/player/puppet_events.h); Tick hands the record to puppet_hide, which owns the flag.
// Recorded on every pose, so a copy born below, a respawned copy and a lost packet all follow the next pose.
bool g_below[coop::players::kMaxPeers] = {};

void OnPose(uint8_t slot, const coop::net::PoseSnapshot& pose) {
    if (slot < coop::players::kMaxPeers) g_below[slot] = pose.z < kVisitFloorZ;
}

// [dev red] level_visit_hide_off: the record never hides a copy.
bool HideOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::level_visit_hide_off);
    return off;
}

// The visit reason of every copy's hidden flag, from the record. The same answer again changes nothing there.
void ApplyVisitHide() {
    const bool off = HideOff();
    for (int slot = 0; slot < static_cast<int>(coop::players::kMaxPeers); ++slot)
        coop::puppet_hide::Set(static_cast<uint8_t>(slot), coop::puppet_hide::Reason::Visit, g_below[slot] && !off);
}


// The visitor's item that its death dropped in the map, back in its hand once it is revived (or at its feet).
void GiveBack() {
    if (!g_return) return;
    if (!R::IsLive(g_return)) {
        g_return = nullptr;
        return;
    }
    void* me = Local();
    float hp = 0.f, dead = 1.f;
    const bool alive = ue_wrap::vitals::Read(ue_wrap::vitals::Field::Health, &hp) && hp > 0.f &&
                       ue_wrap::vitals::Read(ue_wrap::vitals::Field::Dead, &dead) && dead == 0.f;
    const ULONGLONG waited = ::GetTickCount64() - g_returnSinceMs;
    if (!me || waited < 2000 || (!alive && waited < 60000)) return;
    bool collected = false;
    const bool inHand = alive && !HeldActor(me) && E::CallMainPlayerHoldObject(me, g_return, collected);
    ue_wrap::FVector at{};
    if (!inHand && E::TryGetActorLocation(me, at))
        E::TeleportTo(g_return, {at.X, at.Y, at.Z + 50.f}, ue_wrap::FRotator{});
    UE_LOGI("level_visit: the item the visitor dropped in the map is back %hs", inHand ? "in its hand" : "at its feet");
    g_return = nullptr;
}
}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    static bool s_following = false;
    if (!s_following) {
        s_following = true;
        coop::config::Subscribe(coop::config_registry::rows::world_backrooms, &OnBackroomsRow);
        OnBackroomsRow();
    }
    if (!g_watching && sg::IsInstalled() && sg::WatchName(kRowLoadLevel, kTagLoadLevel, &OnLoadLevelPre, nullptr) &&
        sg::WatchName(kRowEnd, kTagEnd, &OnEndPre, nullptr) &&
        sg::WatchName(kRowCanBackrooms, kTagCanBackrooms, &OnCanBackroomsPre, nullptr)) {
        g_watching = true;
        UE_LOGI("level_visit: watching lib_C::loadLevel, lib_C::End and the clip check -- a clip's trip is the clipping "
                "player's alone");
    }
    if (!g_quitHooked) {
        const ue_wrap::facts::Resolved quit = ue_wrap::facts::Resolve(kRowQuitGame);
        if (quit.Fn()) g_quitCtxOff = R::FindParamOffset(quit.Fn(), L"WorldContextObject");
        if (quit.Fn() && ue_wrap::ufunction_hook::InstallPreHook(quit, &OnQuitGamePre, /*armed=*/true)) {
            g_quitHooked = true;
            UE_LOGI("level_visit: QuitGame from a visited map is refused");
        }
    }
    static bool s_listening = false;
    if (!s_listening && !(s_listening = coop::puppet_events::AddPoseListener(&OnPose)))
        UE_LOGW("level_visit: the pose listener list is full -- a visitor's copy is not hidden");
}

void OnPeerLeft(uint8_t slot) {
    if (slot < coop::players::kMaxPeers) g_below[slot] = false;
}

void Tick() {
    if (!InSession()) return;
    if (!g_watching || !g_quitHooked) Install(g_session.load(std::memory_order_acquire));
    ApplyVisitHide();
    if (g_phase == Phase::None) {
        GiveBack();
        return;
    }
    void* me = Local();
    g_hookCheckQuit = false;
    if (void* h = HeldActor(me); h && g_worldActors.count(h)) {
        g_carried = h;
        g_carriedClass = R::ClassOf(h);
    }
    // A map that ends before it has loaded ends a visit the player never began: it is still in the world.
    if (g_endAsked.load()) return Finish(g_phase == Phase::In && g_endIsDeath.load(), L"the map's own end");
    if (g_phase == Phase::Loading) {
        // Polled: the streaming level's own loaded event (ULevelStreaming's OnLevelLoaded / OnLevelShown) would announce
        // it, but no production code of ours binds an engine delegate (the one binder in the tree, coop/dev/
        // pause_menu_probe_bind, measures a bound pair and not the engine's own broadcast), so nothing announces the
        // load through our substrate yet. A delegate-binding substrate is its own package; the poll goes with it.
        g_level = LI::LoadedLevel(g_streaming);
        static ULONGLONG s_said = 0;
        if (!g_level && ::GetTickCount64() - s_said > 5000) {
            s_said = ::GetTickCount64();
            UE_LOGI("level_visit: %ls still loading (%llu ms) -- %hs", g_map.c_str(), ::GetTickCount64() - g_loadAtMs,
                    LI::DescribeState(g_streaming).c_str());
        }
        if (!g_level) {
            if (::GetTickCount64() - g_loadAtMs > kLoadBoundMs) Finish(false, L"the map did not load");
            return;
        }
        const ue_wrap::FVector start = TakePlacedPlayer(me);
        // The placed player began play as the controller's player and put up its own black screen, as a player who
        // starts a level does; the visitor did not start one.
        const bool cleared = coop::death_revive::ClearBlackScreenNow();
        UE_LOGI("level_visit: the black screen the map's player put up is cleared (%d)", cleared ? 1 : 0);
        RemoveWorldDuplicates();
        HoldCycleTick();
        HideWorldSky();
        if (me) Teleport(me, {start.X, start.Y, start.Z + 50.f});
        g_phase = Phase::In;
        UE_LOGI("level_visit: %ls loaded below the world; the player is in it at (%.0f,%.0f,%.0f)", g_map.c_str(),
                start.X, start.Y, start.Z);
        return;
    }
    HoldWorldSky();
    ue_wrap::FVector at{};
    if (me && E::TryGetActorLocation(me, at) && at.Z > kVisitFloorZ) {
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
    for (bool& b : g_below) b = false;
}

bool InVisitedMap(void* actor) {
    if (g_phase == Phase::None || !actor) return false;
    void* me = Local();
    void* level = R::OuterOf(actor);
    if (me && level && level != R::OuterOf(me)) return true;
    // What the map's own script spawns is in the persistent level, at the map's place far below the world.
    ue_wrap::FVector at{};
    return E::TryGetActorLocation(actor, at) && at.Z < kVisitFloorZ;
}

bool Visiting() { return VisitUnderway(); }
std::wstring CurrentMap() { return g_phase != Phase::None ? g_map : std::wstring(); }
std::wstring LastEnd() { return g_lastEnd; }
void* CurrentLevel() { return g_phase == Phase::In ? g_level : nullptr; }
bool ReturnPending() { return g_return != nullptr; }
DrillStats Stats() {
    return {g_tripsStarted.load(), g_clipsHeldClosed.load(), g_clipChecksAsked.load(), g_lastClipWhy.load()};
}

}  // namespace coop::level_visit
