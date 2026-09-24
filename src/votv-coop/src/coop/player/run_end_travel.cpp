// coop/player/run_end_travel.cpp -- which trip to the main menu is the game ending the run.
// The shape is in the header; the census and the verdicts are here, beside the code that runs
// them.

// The nine sites that name "menu" with a literal, read off the cooked blueprints -- a FLOOR,
// not a count, because eight more pass the level at runtime and one of those is a level-placed
// instance variable no cook census can read, which is why this seam judges the name it is
// actually called with: `mainPlayer_C`'s uber (the death chain,
// the only one that sets `dead`); `ui_menu_C` (the player's own quit); `ui_disclaimer_C` twice
// (before any session); and five in-world run-endings that end a run without ever touching the
// flag -- `gameover_C` (spawned by `theEvil_C` and `ui_badSun_C`), `npc_angryErieFlesh_C`,
// `birch_C`, `SKEL_C` and `tutorWall_spikes_C`. The five are what the field reported as "a death
// sent everyone to the menu": to a player they are a black screen and ten seconds, exactly like
// dying.

#include "coop/player/run_end_travel.h"

#include "coop/net/session.h"
#include "coop/player/death_revive.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"

#include <windows.h>

#include <atomic>
#include <string>

namespace coop::player::run_end_travel {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

// The single travel author in the game: lib_C::loadLevel. A NAME watch rather than an exact one,
// because `lib_C` is a BlueprintFunctionLibrary whose class loads with the first level and whose
// UFunction address is not stable across a level reload; the name matches whatever declares it.
constexpr const wchar_t* kLoadLevelName = L"loadLevel";
constexpr int            kLoadLevelTag  = 0x52554E45;  // 'RUNE'

// The level every run-ending travel names.
constexpr const wchar_t* kMenuLevel = L"menu";

// The class that owns the travel author, and the discriminator against its namesake.
constexpr const wchar_t* kTravelAuthorClass = L"lib_C";

// The one author allowed to reach the menu from inside a session: the pause menu's own quit.
// The discrimination has to happen here because the author is a PARAMETER of `loadLevel` -- all
// 26 sites pass `this` as `__WorldContext` -- and it is gone one hop later, where `transition`
// calls `OpenLevel` with the gamemode.
constexpr const wchar_t* kQuitAuthorClass = L"ui_menu_C";

std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<bool> g_watchInstalled{false};
// The watch is REGISTERED (above) and then goes LIVE when the gate resolves its name. One-way: a
// resolved watch does not come undone. Atomic because Install may be called off the game thread.
std::atomic<bool> g_seamLive{false};

// NEITHER AUTHOR CLASS IS RESOLVED AHEAD OF THE QUESTION, and that is the whole of this seam's
// readiness. Both are compared BY NAME against an object that is in front of us -- the function's
// outer, the travel's author -- so the class it names is loaded by the very fact that we are
// being asked about it. The pointer version had to find `ui_menu_C` before it could judge
// anything, which is a widget class that loads late: measured at 500 ms after a CLIENT's pawn
// first ticked and 344 ms after a host's, a span in which a death was not armed and
// the pump's flee ended the session for everyone. It also had to be thrown away per session and
// re-found, because a blueprint class dies with its world and its address is recycled -- a stale
// one refused the player's own quit, which is the one outcome this seam must never produce. A
// name cannot go stale and needs no cache, and the cost moves from a throttled full-array scan
// per session to one name compare per `loadLevel` call, of which a whole run sees about two.

std::atomic<unsigned long long> g_seen{0};
std::atomic<unsigned long long> g_menuSeen{0};
std::atomic<unsigned long long> g_cancelled{0};

// Rate latch for the sub-level line: a trigger volume can fire its travel more than once, and this
// seam does not act on those at all, so they are logged as a trail rather than per call.
unsigned long long g_subLevelLogged = 0;

// One-shot warnings, so a permanent shortfall says so once instead of once per travel.
bool g_saidNoRevive = false;
bool g_saidNoParams = false;

// Does `obj`'s class chain reach a class of this NAME? The quit menu is one exact class today;
// the walk costs nothing on a travel and survives a recook that subclasses it. By name because
// the alternative is holding the class, and this seam holds nothing (see the note above the
// counters).
bool IsOrDerivesFromNamed(void* obj, const wchar_t* className) {
    if (!obj || !className) return false;
    void* c = R::ClassOf(obj);
    for (int hops = 0; c && hops < 16; ++hops) {
        if (R::NameEquals(R::NameOf(c), className)) return true;
        c = R::SuperStructOf(c);
    }
    return false;
}

// The verdict, at the body's entry, on the game thread (the gate skips and counts any off-thread
// match). Every early Run is the fail-closed direction the death arc chose: a player who reaches
// the menu is recoverable, a player stranded in a world we refused to leave is not.
sg::Verdict OnLoadLevelPre(const sg::Call& call) {
    g_seen.fetch_add(1, std::memory_order_relaxed);
    if (!call.locals || !call.function) return sg::Verdict::Run;

    // TWO blueprints declare a function called `loadLevel`: lib_C's, the game's only travel
    // author, and waterVolume_basementFlooder_C's, which takes a float and floods a basement. A
    // name watch sees both -- measured on the first run of this seam, which logged the second one
    // as a resolve failure -- so the OWNER decides, every call, by its own name. That also settles
    // the recycled-address question the header raises about a name watch: lib_C declares exactly
    // one loadLevel, so owner plus name identify the function whatever its address, and the cached
    // offsets cannot be carried into a stranger's frame.
    void* owner = R::OuterOf(call.function);
    if (!owner || !R::NameEquals(R::NameOf(owner), kTravelAuthorClass)) return sg::Verdict::Run;

    static void*   sFn = nullptr;
    static int32_t sLevelOff = -1;
    static int32_t sAuthorOff = -1;
    if (call.function != sFn) {
        sFn = call.function;
        sLevelOff  = R::FindParamOffset(call.function, L"level");
        sAuthorOff = R::FindParamOffset(call.function, L"__WorldContext");
        if ((sLevelOff < 0 || sAuthorOff < 0) && !g_saidNoParams) {
            // The travel author's own parameters going missing is a game update renaming them,
            // and every menu travel would pass unjudged -- the pre-fix behaviour, silently.
            g_saidNoParams = true;
            UE_LOGE("run_end_travel: %ls::loadLevel's parameters did not resolve (level=%d "
                    "__WorldContext=%d) -- every menu travel now passes through unjudged",
                    kTravelAuthorClass, sLevelOff, sAuthorOff);
        }
    }
    if (sLevelOff < 0 || sAuthorOff < 0) return sg::Verdict::Run;

    const R::FName level = *reinterpret_cast<const R::FName*>(call.locals + sLevelOff);
    void* author = *reinterpret_cast<void* const*>(call.locals + sAuthorOff);

    if (!R::NameEquals(level, kMenuLevel)) {
        // A sub-level travel (sl_*, the map transitions). It is not this seam's to refuse -- and
        // it will still end the session for the peer that takes it, which is a separate open
        // question, so the trail is left deliberately.
        const unsigned long long n = ++g_subLevelLogged;
        if (n <= 8 || (n % 50) == 0) {
            UE_LOGI("run_end_travel: lib_C::loadLevel(\"%ls\") authored by %ls (#%llu) -- not the "
                    "menu, so not judged here; a sub-level travel still ends this peer's session",
                    R::ToString(level).c_str(), R::ClassNameOf(author).c_str(), n);
        }
        return sg::Verdict::Run;
    }
    g_menuSeen.fetch_add(1, std::memory_order_relaxed);

    const Judgement v = JudgeMenuTravel(author);
    if (v != Judgement::Cancel) {
        // Only the two that say something about this seam are worth a line per travel: a solo
        // travel is the common case and says nothing, and the two shortfalls latch below.
        if (v == Judgement::RunPlayerAsked) {
            UE_LOGI("run_end_travel: allowed lib_C::loadLevel(\"menu\") authored by %ls -- the "
                    "player asked to leave", R::ClassNameOf(author).c_str());
        } else if (v == Judgement::RunNoRevive && !g_saidNoRevive) {
            g_saidNoRevive = true;
            UE_LOGW("run_end_travel: a run-ending travel authored by %ls is passing through -- the "
                    "revive is not available, and refusing a travel we cannot answer strands the "
                    "player. net_pump's flee handles a death; a catch ending ends the run as it "
                    "did before this seam.", R::ClassNameOf(author).c_str());
        }
        return sg::Verdict::Run;
    }

    const std::wstring cls = R::ClassNameOf(author);
    const bool isLocalPawn = author && author == coop::players::Registry::Get().Local();
    g_cancelled.fetch_add(1, std::memory_order_relaxed);
    UE_LOGW("run_end_travel: REFUSED lib_C::loadLevel(\"menu\") authored by %ls%s -- the game is "
            "ending the run and this is a coop session, so the world is kept and the player is "
            "revived in place", cls.c_str(),
            isLocalPawn ? " = THE LOCAL PLAYER (the death chain)"
                        : " (a run-ending that never touches `dead`)");
    coop::death_revive::NoteRunEndCancelled(author, cls.c_str(), isLocalPawn);
    return sg::Verdict::Cancel;
}

}  // namespace

// An unknown author is REFUSED rather than allowed, and the direction is deliberate: being wrong
// here means the game did not end a run it wanted to end, and the player keeps playing with the
// pause menu still available; being wrong the other way is the session ending for everyone.
Judgement JudgeMenuTravel(void* author) {
    // Single player is untouched by this test, not by the watch's absence: the watch is registered
    // whenever the lane installs, and it refuses nothing without a live session.
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return Judgement::RunNoSession;
    // The player's own quit reads as a run-ending to everything except this test, and refusing
    // THAT traps them in the world.
    if (IsOrDerivesFromNamed(author, kQuitAuthorClass)) return Judgement::RunPlayerAsked;
    // Refusing a travel we cannot answer is the one outcome worse than the menu.
    if (!coop::death_revive::ReviveAvailable()) return Judgement::RunNoRevive;
    return Judgement::Cancel;
}

// Our own leave never reaches this seam: `net_pump`'s flee calls `mainGamemode_C::transition`
// directly (`ue_wrap::engine::ReturnToMainMenu`), one hop past `loadLevel`. A future lane that
// wants to leave through `loadLevel` needs an allow path here, not a special case at its call
// site.
void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    // THE WATCH'S NAME IS RESOLVED FROM HERE, not from Tick, for the reason this lane already
    // re-asserts the gate's enable rather than riding another consumer's: the resolve is shared,
    // and driving it only from Tick -- which is session-scoped -- left this watch's readiness in
    // the hands of whichever other consumer's tick happened to run. A name watch is inert until
    // the gate has turned the literal into an FName, so an unresolved one is a seam that cannot
    // intercept the travel it is supposed to judge. This runs on the harness's unconditional
    // per-frame game-thread tick, which runs at the menu too, costs one reflected call per frame
    // while the name is pending, and stops for good once the watch is live.
    if (!g_seamLive.load(std::memory_order_acquire)) {
        sg::ResolvePendingNames();
        if (sg::NameWatchLive(kLoadLevelName, kLoadLevelTag)) {
            g_seamLive.store(true, std::memory_order_release);
            UE_LOGI("run_end_travel: the watch on lib_C::%ls is LIVE -- from here a run-ending "
                    "travel is judged, so a death can be answered", kLoadLevelName);
        }
    }
    if (g_watchInstalled.load(std::memory_order_acquire)) return;
    // A name watch registers immediately and the gate resolves the name itself on the game
    // thread, so there is nothing to wait for and no retry throttle to own.
    if (sg::WatchName(kLoadLevelName, kLoadLevelTag, &OnLoadLevelPre, nullptr)) {
        g_watchInstalled.store(true, std::memory_order_release);
        UE_LOGI("run_end_travel: watching lib_C::%ls at the script-body gate -- the game's only "
                "travel author, and the only place a travel still carries its own author",
                kLoadLevelName);
    }
}

void Tick() {
    // The gate's enable is a per-script-call cost on the VM's hottest loop, so it is scoped to a
    // live session -- which is also what the verdict is scoped to, so an enabled gate with nothing
    // to judge is pure waste. Measured: the first build enabled it unconditionally and the
    // sessionless death control caught it, reporting a solo player paying for a seam that can
    // never fire.
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) {
        coop::death_revive::NoteRunEndSeamReady(false);
        return;
    }
    // The revive's arm asks whether this seam can answer a death at all: published from here so
    // `death_revive` never has to name this module back. The readiness itself is settled in
    // Install, which runs before a session exists; this tick only publishes it.
    coop::death_revive::NoteRunEndSeamReady(g_seamLive.load(std::memory_order_acquire));
}

void OnSessionStart() {
    // Nothing world-scoped is carried across sessions, and this lane now holds nothing that is:
    // both authors are judged by name against the object in hand. What is reset is the counters
    // and the one-shot warnings. The watch itself is process-wide and outlives a session.
    g_seen.store(0, std::memory_order_relaxed);
    g_menuSeen.store(0, std::memory_order_relaxed);
    g_cancelled.store(0, std::memory_order_relaxed);
    g_subLevelLogged = 0;
    g_saidNoRevive = false;
    g_saidNoParams = false;
}

bool WatchInstalled() { return g_watchInstalled.load(std::memory_order_acquire); }
unsigned long long TravelsSeen() { return g_seen.load(std::memory_order_relaxed); }
unsigned long long MenuTravelsSeen() { return g_menuSeen.load(std::memory_order_relaxed); }
unsigned long long TravelsCancelled() { return g_cancelled.load(std::memory_order_relaxed); }

}  // namespace coop::player::run_end_travel
