// coop/props/prop_spawn_authoring.cpp -- see coop/props/prop_spawn_authoring.h for the design.

#include "coop/props/prop_spawn_authoring.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/props/prop_lifecycle.h"      // DestroyLocalProp -- a ghost the unscrew left, destroyed with no broadcast

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/fname_utils.h"     // StringToFName -- the two verb names, once
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"   // UE_ASSERT_GAME_THREAD
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"      // ClassByName -- the battery prop class
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/ufunction_hook.h"   // CurrentCallerFrame -- the calling Blueprint frame
#include "ue_wrap/facts/function_row.h"

namespace coop::prop_spawn_authoring {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace sg = ue_wrap::script_gate;

UE_FACT_FN(kRowSpawnPropThroughGamemode, "prop_spawn_authoring", L"mainGamemode_C",
           L"spawnPropThroughGamemode", Declared);

// The gate watch's tag. One consumer, one tag.
constexpr int kGateTag = 0x53504157;  // 'SPAW'

// Resolved once each, on the game thread. The two spawn verbs and the menu's own caller.
void* g_gamemodeSpawnFn = nullptr;   // mainGamemode_C::spawnPropThroughGamemode -- watched
void* g_menuSpawnFn     = nullptr;   // ui_spawnmenu_C::spawn -- the caller that makes it a player's
void* g_toolUberFn      = nullptr;   // tool_spawn_C::ExecuteUbergraph_tool_spawn -- the toolgun
void* g_sawUberFn       = nullptr;   // prop_hacksaw_C::ExecuteUbergraph_prop_hacksaw -- the hacksaw's cut
void* g_unscrewFn       = nullptr;   // ATV_C::unscrewPanel -- the battery an unscrew hands back (atv_gate's watch)
bool  g_unscrewBattery  = false;     // the unscrew now starting is the battery's branch
ue_wrap::CachedObjRef g_unscrewBorn;  // the battery prop that branch's body spawned on this client, until its POST takes it
ue_wrap::CachedObjRef g_batteryCls;   // prop_atvcarbattery_C, found again when a world reload has re-created it
void* g_pryEndFn        = nullptr;   // pryingCrowbar_C::End -- the crowbar a pry hands back
bool  g_watchInstalled  = false;

// The negative arm, and it is the half a run cannot otherwise see. Catching every menu spawn and
// rejecting every other caller of the same verb are two different claims, and only the second says
// the world's own spawns still stay local. So each side is counted and both are reported at
// teardown: a run whose `other` column is zero has not exercised the rejection, which is a fact
// about the run rather than about the filter.
unsigned g_verbFromMenu  = 0;   // spawnPropThroughGamemode called BY ui_spawnmenu_C::spawn
unsigned g_verbFromOther = 0;   // ... by anything else (lib_C::replaceProp, comp_physicsImpact, ...)

// The two names the lazy resolves match on, as FNames, so a per-birth test is two int compares
// and never a string build. Zeroed ComparisonIndex means "not yet converted".
R::FName g_menuSpawnName{0, 0};   // "spawn"
R::FName g_toolUberName{0, 0};    // "ExecuteUbergraph_tool_spawn"
R::FName g_sawUberName{0, 0};     // "ExecuteUbergraph_prop_hacksaw"
R::FName g_pryEndName{0, 0};      // "End"

// Is `fn` a function named `name` whose owner (a UFunction's Outer is its declaring class) is
// called `className`? Both compares are allocation-free: the FName pair is two ints, and the
// owner's name goes through the scratch-buffer compare rather than a std::wstring, which matters
// because the FName test does not always short-circuit -- a stranger class could declare a
// function called `spawn` too.
bool FunctionIs(void* fn, const R::FName& name, const wchar_t* className) {
    if (!fn || name.ComparisonIndex == 0) return false;
    const R::FName& fname = R::NameOf(fn);
    if (fname.ComparisonIndex != name.ComparisonIndex || fname.Number != name.Number) return false;
    void* owner = R::OuterOf(fn);
    return owner && R::NameEquals(R::NameOf(owner), className);
}

// The gate's pre. It does NOT open a bracket: the gate's own RAII scope around the body is the
// bracket, and `BirthIsPlayerAuthored` reads it. This callback only resolves the menu's spawn()
// pointer the first time the menu is the caller -- the widget class is loaded by definition there,
// since its bytecode is what is running -- and counts the two columns the run is judged on.
sg::Verdict OnGamemodeSpawnPre(const sg::Call& call) {
    if (!g_menuSpawnFn && FunctionIs(call.callerFunction, g_menuSpawnName, L"ui_spawnmenu_C"))
        g_menuSpawnFn = call.callerFunction;
    if (call.callerFunction && call.callerFunction == g_menuSpawnFn) {
        ++g_verbFromMenu;
    } else {
        ++g_verbFromOther;
        // Name each refused caller once, so a run says WHICH of the world's own spawn paths it
        // exercised rather than only that it exercised one. A fixed ring, not one slot: two
        // callers alternating past a single slot would render and log on every call.
        static void* s_said[4] = {};
        static int   s_next = 0;
        void* c = call.callerFunction;
        if (c) {
            bool seen = false;
            for (void* p : s_said) if (p == c) { seen = true; break; }
            if (!seen) {
                s_said[s_next] = c;
                s_next = (s_next + 1) % 4;
                UE_LOGI("prop_spawn_authoring: spawnPropThroughGamemode called by '%ls' -- not the "
                        "spawn menu, so the birth stays this peer's own",
                        R::ToString(R::NameOf(c)).c_str());
            }
        }
    }
    return sg::Verdict::Run;                    // observation only; the verb always runs
}

}  // namespace

void Install(bool isClient) {
    UE_ASSERT_GAME_THREAD("prop_spawn_authoring::Install");
    // Only a CLIENT asks the question. The host's own births -- a menu spawn, a mushroom spawner,
    // an impact -- are all the shared world by definition, and its spawn watcher broadcasts them
    // without caring who asked, so watching the verb there would buy nothing and cost a lookup on
    // every call of it.
    if (!isClient) return;
    if (g_watchInstalled) return;

    // Throttle the class walks while the gamemode is unresolved (it loads on gameplay entry), the
    // shape prop_drop_intent::Install uses for the same reason.
    static int s_retry = 0;
    if (s_retry > 0) { --s_retry; return; }

    // The two literals as FNames. The conversion dispatches ProcessEvent, so it is game-thread
    // work done once here rather than per birth at the seam.
    if (g_menuSpawnName.ComparisonIndex == 0)
        g_menuSpawnName = ue_wrap::fname_utils::StringToFName(L"spawn");
    if (g_toolUberName.ComparisonIndex == 0)
        g_toolUberName = ue_wrap::fname_utils::StringToFName(L"ExecuteUbergraph_tool_spawn");
    if (g_sawUberName.ComparisonIndex == 0)
        g_sawUberName = ue_wrap::fname_utils::StringToFName(L"ExecuteUbergraph_prop_hacksaw");
    if (g_pryEndName.ComparisonIndex == 0)
        g_pryEndName = ue_wrap::fname_utils::StringToFName(L"End");

    ue_wrap::facts::Resolved gamemodeSpawn;
    if (!g_gamemodeSpawnFn) {
        void* gmCls = R::FindClass(L"mainGamemode_C");
        if (!gmCls) { s_retry = 60; return; }
        gamemodeSpawn = ue_wrap::facts::Resolve(kRowSpawnPropThroughGamemode);
        g_gamemodeSpawnFn = gamemodeSpawn.Fn();
        if (!g_gamemodeSpawnFn) {
            UE_LOGW("prop_spawn_authoring: mainGamemode_C::spawnPropThroughGamemode not found -- a "
                    "client's spawn-menu birth cannot be told from the world's own spawns, so it "
                    "will not cross");
            g_watchInstalled = true;   // permanent give-up; do not re-walk every tick
            return;
        }
    }
    if (sg::Watch(gamemodeSpawn, kGateTag, &OnGamemodeSpawnPre, nullptr)) {
        UE_LOGI("prop_spawn_authoring: watching mainGamemode_C::spawnPropThroughGamemode -- a "
                "spawn-menu birth is a player's, a replaceProp or impact birth is the world's");
    } else {
        UE_LOGW("prop_spawn_authoring: the script-gate watch FAILED (gate uninstalled or table "
                "full) -- a client's spawn-menu birth will not cross");
    }
    g_watchInstalled = true;
}

bool BirthIsPlayerAuthored(void* born) {
    // The menu: is the gamemode's spawn verb running on this thread, entered from the menu's own
    // spawn()? The gate answers from the RAII scope it pushes around the body, so the window
    // cannot outlive the body on any exit path, and an inner watched body belonging to some other
    // consumer cannot hide it.
    if (g_menuSpawnFn && sg::IsBodyActive(g_gamemodeSpawnFn, g_menuSpawnFn)) return true;
    // The toolgun, via the calling frame the native seam already holds: its ubergraph finishes the
    // actor itself. The pointer is cached on first sight -- the class is loaded by definition,
    // since its bytecode is what is running.
    const void* caller = ue_wrap::ufunction_hook::CurrentCallerFrame().function;
    if (!caller) {
        static bool s_said = false;
        if (g_unscrewBattery && !s_said) {
            s_said = true;
            UE_LOGW("prop_spawn_authoring: a birth inside an ATV's battery unscrew has no calling frame -- the unscrew's own "
                    "battery prop is not recorded here (said once)");
        }
        return false;
    }
    if (caller == g_toolUberFn) return true;
    // A class a world reloaded is a new function, so a miss of the latch is asked by name again.
    if (FunctionIs(const_cast<void*>(caller), g_toolUberName, L"tool_spawn_C")) {
        g_toolUberFn = const_cast<void*>(caller);
        UE_LOGI("prop_spawn_authoring: the toolgun's ubergraph is the calling frame -- its births "
                "are a player's");
        return true;
    }
    // The hacksaw's cut: its tick spawns the scrap of the prop it saws, then destroys the prop. The saw runs only where
    // it is held, so the scrap is this player's doing and no other peer's game makes it; the prop's destroy crosses by
    // the destroy seam. It is a granted act's consequence: the client's saw asked the host first (coop/props/hacksaw_cut)
    // and runs this tick only after the grant. MTA would have the server make the product (a request judged and answered
    // by the server alone, reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:3338-3367); here the derivation
    // (the drop from the prop's material, the amount from its mesh volume) lives mid-ubergraph with no entry point, so
    // the saw that holds that state runs it and the host judges the births it sends. A class a world reloaded is a new
    // function, so a miss of the latch is asked by name again.
    if (caller == g_sawUberFn) return true;
    if (FunctionIs(const_cast<void*>(caller), g_sawUberName, L"prop_hacksaw_C")) {
        g_sawUberFn = const_cast<void*>(caller);
        UE_LOGI("prop_spawn_authoring: the hacksaw's ubergraph is the calling frame -- the scrap of its cut is a "
                "player's");
        return true;
    }
    // An ATV's battery unscrewed: its unscrewPanel (a custom event, so the calling frame is the ATV's whole ubergraph)
    // spawns the battery at the panel. That birth is no player's: the host spawns the battery every peer shares, so this
    // client's own prop is only recorded here, for the POST of that body to destroy. The birth stays an unauthored
    // place in the drop-intent queue, and its drain sends nothing for it: the destroyed actor is dead at the drain, and
    // a live one is no whitelisted birth.
    if (g_unscrewBattery && g_unscrewFn && born && R::IsLive(born) && sg::IsBodyActive(g_unscrewFn)) {
        if (!g_batteryCls.Alive()) g_batteryCls.Set(ue_wrap::object_index::ClassByName(L"prop_atvcarbattery_C"));
        if (g_batteryCls.Raw() && R::ClassOf(born) == g_batteryCls.Raw()) g_unscrewBorn.Set(born);
    }
    // A pry's end: the crowbar the player's own crowbar became when the pry began (its begin destroyed the held one)
    // comes back as a new crowbar, freed from the board, dropped at it, or put back in the hand. The pry runs only on
    // the prying player's game, so without this the crowbar is gone for every other player.
    // The drill's red arm: the pry's End is not admitted as a player's birth (a developer knob, off).
    static const bool s_pryUnadmitted =
        coop::config::ResolveFlag(coop::config_registry::rows::crowbarpry_authorship_off);
    if (caller == g_pryEndFn) return !s_pryUnadmitted;
    // A reloaded world re-creates pryingCrowbar_C, so a miss of the latch is asked by name again.
    if (FunctionIs(const_cast<void*>(caller), g_pryEndName, L"pryingCrowbar_C")) {
        g_pryEndFn = const_cast<void*>(caller);
        UE_LOGI("prop_spawn_authoring: a prying crowbar's End is the calling frame -- the crowbar a pry hands back is "
                "a player's");
        return !s_pryUnadmitted;
    }
    return false;
}

// The record an unscrew's POST never took (a fault or a cancel skips one): its battery prop is this client's ghost, so it is
// destroyed here as the POST would, with no destroy sent, and never just forgotten.
static void DestroyHeldRecord(const char* when) {
    void* const ghost = g_unscrewBorn.Get();
    g_unscrewBorn.Reset();
    if (!ghost) return;
    coop::prop_lifecycle::DestroyLocalProp(ghost, false);
    UE_LOGW("prop_spawn_authoring: the battery prop of an unscrew whose end never came is destroyed at %s", when);
}

void NoteAtvUnscrewBranch(void* unscrewFn, bool batteryBranch) {
    UE_ASSERT_GAME_THREAD("prop_spawn_authoring::NoteAtvUnscrewBranch");
    DestroyHeldRecord("the next unscrew");
    g_unscrewFn = unscrewFn;
    g_unscrewBattery = batteryBranch;
}

void* TakeAtvUnscrewBattery() {
    UE_ASSERT_GAME_THREAD("prop_spawn_authoring::TakeAtvUnscrewBattery");
    void* const born = g_unscrewBorn.Get();
    g_unscrewBorn.Reset();
    g_unscrewBattery = false;
    return born;
}

void Reset() {
    UE_ASSERT_GAME_THREAD("prop_spawn_authoring::Reset");
    if (g_watchInstalled)
        UE_LOGI("prop_spawn_authoring: VERDICT spawnPropThroughGamemode calls: from-the-menu=%u "
                "(crossed as intents) from-elsewhere=%u (stayed local)%s",
                g_verbFromMenu, g_verbFromOther,
                g_verbFromOther == 0 ? " -- this run never exercised the rejection" : "");
    g_verbFromMenu = 0;
    g_verbFromOther = 0;
    g_unscrewBattery = false;
    DestroyHeldRecord("the session's end");
    // The tally is per session; the watch and the resolved UFunctions are NOT. They name the
    // game's own classes, which outlive any session in this process, and the gate is already
    // session-gated, so a watch costs nothing while nobody is connected. Retiring and re-adding it
    // per session would also churn the gate's keyed slots, which a retire does not give back.
    // There is no bracket left to clear: the window is the gate's own scope now, and it unwound
    // with the body.
}

}  // namespace coop::prop_spawn_authoring
