// coop/world/event_fire_sync.cpp -- see coop/world/event_fire_sync.h. The bytecode facts this
// module stands on: the save slot's settime iterates allEvents, skips rows in passEvents, and
// on a clock-cross fire calls the eventer's runEvent for the row and appends it to passEvents,
// so a runEvent whose caller is settime is exactly a scheduler fire (the host observation seam),
// and an empty allEvents kills the walk (the client suppression seam); runEvent is the only
// function of that name, called from settime, from the eventer itself and from the game's own
// event menu, which appends nothing; the gamemode's boot marks the rows before a new game's start
// day passed without firing them, and rebuilds allEvents from the events table on every world
// load, so the zeroed count self-heals and a client-written save cannot be poisoned; and the only
// special trigger the table uses is the prank roll, host-local RNG, so the wire carries no
// special field.

#include "coop/world/event_fire_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/world/time_sync.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/world/world_singleton.h"
#include "ue_wrap/world/daynightcycle.h"

#include <atomic>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_set>

namespace coop::event_fire_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace sg = ue_wrap::script_gate;

std::atomic<coop::net::Session*> g_session{nullptr};

// Resolution, on demand, game thread: the three classes come from the object index, which answers at
// once whether or not they are loaded; their members resolve once all three are, on the first pass or
// never (a renamed symbol on a future game version), so a failed pass latches loudly.
int32_t g_offSaveSlot = -1;           // mainGamemode.saveSlot (UsaveSlot_C*)
int32_t g_offEventer = -1;            // mainGamemode.eventer  (Atrigger_eventer_C*)
int32_t g_offPassEvents = -1;         // saveSlot.passEvents (TArray<FName>)
int32_t g_offAllEvents = -1;          // saveSlot.allEvents  (TArray<FName>)
void* g_runEventFn = nullptr;         // runEvent(FName event, FName special)
void* g_runSpecialEventFn = nullptr;  // runSpecialEvent(FName eventName1) -> bool
void* g_settimeFn = nullptr;          // saveSlot.settime, the scheduler's walk
int32_t g_offEventParam = -1;         // runEvent's `event` in its parameter frame
bool g_resolved = false;
bool g_resolveFailed = false;

// The host's watch on runEvent, registered once per process.
constexpr int kTagEventFire = 0x45564652;  // 'EVFR'
constexpr const wchar_t* kRunEvent = L"runEvent";  // one pointer: the gate matches a name watch by it
bool g_watchInstalled = false;
bool g_watchLive = false;

// Client suppression and replay state, game thread.
int g_zeroedAllEventsNum = 0;         // what we zeroed (restore on disconnect); 0 = nothing zeroed
void* g_zeroedSaveSlot = nullptr;
int32_t g_zeroedSaveSlotIdx = -1;
struct PendingFire {
    FireKind kind;
    std::string name;
    // The active override: the host registry says this row is in flight, so bypass the
    // passEvents dedupe (a mid-event joiner's blob carries the row as completed history while
    // the event is still running).
    bool activeOverride = false;
};
std::deque<PendingFire> g_pending;    // replays waiting for the eventer (join window)
// EventFire is pre-world-sendable, so a joiner can queue fires for its whole load window. The
// cap is the table plus specials plus margin; duplicates are skipped at queue time, so it is
// effectively unreachable.
constexpr size_t kMaxPending = 128;
std::unordered_set<std::string> g_replayed;  // rows replayed this session (dedupe)
std::atomic<unsigned> g_replays{0};          // fires replayed natively (ReplayCount)

// The UE4 array header; 8-byte FName elements for the two arrays touched.
struct RawArray {
    R::FName* Data;
    int32_t Num;
    int32_t Max;
};

// The replay policy, the dupe matrix: every row's concrete output and which lane already
// carries it. The default is no replay. Replay only rows whose effect is a deterministic
// level, save or cosmetic flip that no existing lane delivers; replaying a lane-covered row
// would double-deliver. Keyed by name only: the few names living in both dispatchers have
// the same verdict either way, and the replay call still uses the received dispatch kind.
const char* const kReplayRows[] = {
    // Story and save flips (level-placed triggers; no lane):
    "treehouse_0", "treehouse_1", "treehouse_2", "treehouse_3", "treehouse_4", "treehouse_5",
    "break_RomeoSierra", "break_Victor", "break_Victor2",
    "obelisk",
    // Force-object appends (a save array the client's own dish scan reads; no lane):
    "looker_0-1", "looker_1-1", "looker_2-1", "looker_3-1", "looker_4-1",
    "arirSignal", "arirSpk", "picSignal", "peace",
    "arirSat_0", "arirSat_1", "arirSat_2", "piramid_sig",
    // Cosmetic or sound with no lane (the solar row's lights-dark converges with the light lane,
    // the same resulting state, echo-suppressed by its last-known prime):
    "solar", "call0",
    // Trigger-box scare arms, per-viewer scares by design; arming both sides is the correct coop
    // semantics (each player gets the scare on their own overlap):
    "toeStab", "falseEnter", "mann", "vent", "crys", "fakeGrays", "susArir",
    // Graffiti decal specials (a grime decal spawn; no lane):
    "arirGraff_0", "arirGraff_1", "arirGraff_2", "arirGraff_3",
    "arirGraff_4", "arirGraff_5", "arirGraff_6",
};

struct NoReplayRow { const char* name; const char* lane; };
const NoReplayRow kNoReplayRows[] = {
    // Outputs already ride a lane (a replay is a double delivery):
    { "starRain", "event_cue lane (cue 0)" },
    { "arirFollower", "npc lane" },
    // The swarm's wisps ride the npc lane (the source-gated catch in npc_world_enum); a replay
    // would arm the client's own swarm trigger and double-spawn client-local creatures on top of
    // the mirrors:
    { "wisps", "npc lane (EX-catch; event-swarm wisp_C mirrored)" },
    // The pyramid's path is host-random (wander and chase timers), so a replay armed the client's
    // own trigger box and a client walk-in spawned a divergent client-local pyramid with
    // unmirrored wisps. The arrival comes by mirror: the world-actor pose stream, npc-lane wisps
    // and the pyramid sync's brain suppression and gather relay.
    { "piramid", "piramid mirror lane (WA pose + piramid_sync brain/gather)" },
    // The ship: the trigger-box arm's overlap spawns the ship and the alarm lamp (and possibly
    // NPC leaves); replaying the arm would spawn them client-local. Host-only until the ship gets
    // a lane:
    { "arirShip", "actor spawn on armed overlap (no lane)" },
    { "earthTp", "SELF (pose stream)" },
    { "vehtp", "atv lane" },
    { "bedEvent", "sleep lane" },
    { "picnic", "prop lane" }, { "destroyPicnic", "prop lane" },
    { "enasus", "prop lane" }, { "enacros", "prop lane" },
    { "cookier", "prop lane (armed prop)" }, { "paperGray", "prop lane (armed prop)" },
    { "arirEgg", "prop lane (armed prop)" },
    { "console", "device lanes" }, { "lightswitch", "device lanes" },
    { "keypadGuess", "device lanes" }, { "atvExplode", "atv lane (trap flag)" },
    // Host-local by design:
    { "agrav", "physics divergence (by-design host-local)" },
    { "treehouseSleep", "per-player teleport" },
    // Creature and save-actor spawns, host-only until allowlisted. The vent crawler is
    // npc-allowlisted, so its no-replay reason is the mirrors:
    { "ventCrawler", "npc lane (allowlisted)" }, { "ventKnocker", "creature spawn (no lane yet)" },
    { "tentacleBalls", "creature spawn (no lane yet)" }, { "morningGay", "creature spawn (no lane yet)" },
    { "borgRozital", "creature spawn (no lane yet)" }, { "graysforest", "creature spawn (no lane yet)" },
    { "graystank", "creature spawn (no lane yet)" }, { "arirBuster", "creature spawn (no lane yet)" },
    { "eggvasion", "creature spawn (no lane yet)" }, { "boarwar", "creature spawn (no lane yet)" },
    { "soltoClean", "creature spawn (no lane yet)" },
    { "salt", "save-actor spawn (no lane)" }, { "rozitalHole", "save-actor spawn (no lane)" },
    { "dreambase", "save-actor spawn (no lane)" },
    { "fallbody_0", "dropper spawn (no lane)" }, { "fallbody_1", "dropper spawn (no lane)" },
    { "fallcar_0", "dropper spawn (no lane)" },
    // The prank layer (host-local RNG; thrown-prop outputs ride the prop lane):
    { "food", "prank special (prop lane)" }, { "drive", "prank special (prop lane)" },
    { "atvFuel", "prank special (prop lane)" }, { "atvFix", "prank special (prop lane)" },
    { "poisonFood", "prank special (prop lane)" }, { "expDrive", "prank special (prop lane)" },
    { "cookiebox", "prank special (prop lane)" }, { "trashPiles", "prank special (prop lane)" },
    { "vaccine", "prank special (prop lane)" }, { "oil", "prank special (prop lane)" },
    { "begos", "prank special (prop lane)" }, { "gascans", "prank special (prop lane)" },
    { "bombBox", "prank special (prop lane)" },
    { "rockThrow", "prank spawner (no lane)" }, { "hillRoller", "prank spawner (no lane)" },
    { "alienJump", "prank spawner (no lane)" }, { "trashBase", "prank spawner (no lane)" },
    { "alienSounds", "sound gap (future WorldSoundCue)" },
};

// The interaction rows: the row's entire effect is the prank special, host-local RNG.
bool IsPrankRow(const std::string& n) {
    return n.rfind("arirInteraction_", 0) == 0;
}

// The verdict: 1 replay, 0 known no-replay (the lane says why), -1 unknown (default no-replay).
int ReplayVerdict(const std::string& name, const char** laneOut) {
    for (const char* r : kReplayRows)
        if (name == r) return 1;
    for (const auto& nr : kNoReplayRows)
        if (name == nr.name) { *laneOut = nr.lane; return 0; }
    if (IsPrankRow(name)) { *laneOut = "prank special (host-local RNG)"; return 0; }
    return -1;
}

bool MembersMissing() {
    return g_offSaveSlot < 0 || g_offEventer < 0 || g_offPassEvents < 0 || g_offAllEvents < 0 ||
           !g_runEventFn || !g_runSpecialEventFn || !g_settimeFn || g_offEventParam < 0;
}

bool ResolvePass() {
    if (g_resolved) return true;
    if (g_resolveFailed) return false;
    namespace OI = ue_wrap::object_index;
    void* gmCls = OI::ClassByName(L"mainGamemode_C");
    void* ssCls = OI::ClassByName(L"saveSlot_C");
    void* evCls = OI::ClassByName(L"trigger_eventer_C");
    if (!gmCls || !ssCls || !evCls) return false;  // the world has not loaded them yet
    g_offSaveSlot = R::FindPropertyOffset(gmCls, L"saveSlot");
    g_offEventer = R::FindPropertyOffset(gmCls, L"eventer");
    g_offPassEvents = R::FindPropertyOffset(ssCls, L"passEvents");
    if (g_offAllEvents < 0) g_offAllEvents = R::FindPropertyOffset(ssCls, L"allEvents");
    g_runEventFn = R::FindFunction(evCls, L"runEvent");
    g_runSpecialEventFn = R::FindFunction(evCls, L"runSpecialEvent");
    g_settimeFn = R::FindFunction(ssCls, L"settime");
    g_offEventParam = g_runEventFn ? R::FindParamOffset(g_runEventFn, L"event") : -1;
    if (MembersMissing()) {
        g_resolveFailed = true;
        UE_LOGW("event_fire: resolution INCOMPLETE on loaded classes (saveSlot=0x%X eventer=0x%X "
                "passEvents=0x%X allEvents=0x%X runEvent=%s(event=0x%X) runSpecialEvent=%s settime=%s) -- "
                "latched OFF; game version mismatch?",
                g_offSaveSlot, g_offEventer, g_offPassEvents, g_offAllEvents, g_runEventFn ? "yes" : "NO",
                g_offEventParam, g_runSpecialEventFn ? "yes" : "NO", g_settimeFn ? "yes" : "NO");
        return false;
    }
    g_resolved = true;
    UE_LOGI("event_fire: resolved (saveSlot=0x%X passEvents=0x%X allEvents=0x%X eventer=0x%X "
            "runEvent=yes runSpecialEvent=yes settime=yes)",
            g_offSaveSlot, g_offPassEvents, g_offAllEvents, g_offEventer);
    return true;
}

void* SaveSlotOf(void* gm) {
    if (!gm || g_offSaveSlot < 0) return nullptr;
    void* ss = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(gm) + g_offSaveSlot);
    return (ss && R::IsLive(ss)) ? ss : nullptr;
}

void* EventerOf(void* gm) {
    if (!gm || g_offEventer < 0) return nullptr;
    void* ev = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(gm) + g_offEventer);
    return (ev && R::IsLive(ev)) ? ev : nullptr;
}

RawArray* ArrayAt(void* obj, int32_t off) {
    if (!obj || off < 0) return nullptr;
    return reinterpret_cast<RawArray*>(reinterpret_cast<uint8_t*>(obj) + off);
}

std::string NarrowName(const R::FName& n) {
    const std::wstring w = R::ToString(n);
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s.push_back((c > 0 && c < 128) ? static_cast<char>(c) : '?');
    return s;
}

// The native fire, game thread. True iff the verb actually dispatched; callers gate the
// broadcast and the replayed set on it, since a failed host fire must not make clients replay
// an event the authority never executed, and a failed replay must not permanently consume
// the row.
bool NativeFire(FireKind kind, const std::wstring& eventName, const std::wstring& specialName) {
    void* eventer = EventerOf(ue_wrap::world_singleton::Gamemode());
    if (!eventer) {
        UE_LOGW("event_fire: no live trigger_eventer -- native fire dropped ('%ls')", eventName.c_str());
        return false;
    }
    if (kind == FireKind::SpecialEvent) {
        if (!g_runSpecialEventFn) { UE_LOGW("event_fire: runSpecialEvent unresolved"); return false; }
        ue_wrap::ParamFrame f(g_runSpecialEventFn);
        if (!f.valid()) return false;
        f.Set<R::FName>(L"eventName1", ue_wrap::fname_utils::StringToFName(eventName));
        if (ue_wrap::Call(eventer, f)) {
            UE_LOGI("event_fire: runSpecialEvent('%ls') dispatched", eventName.c_str());
            return true;
        }
        UE_LOGW("event_fire: runSpecialEvent('%ls') dispatch FAILED", eventName.c_str());
        return false;
    }
    if (!g_runEventFn) { UE_LOGW("event_fire: runEvent unresolved"); return false; }
    ue_wrap::ParamFrame f(g_runEventFn);
    if (!f.valid()) return false;
    f.Set<R::FName>(L"event", ue_wrap::fname_utils::StringToFName(eventName));
    f.Set<R::FName>(L"special", ue_wrap::fname_utils::StringToFName(specialName));
    if (ue_wrap::Call(eventer, f)) {
        UE_LOGI("event_fire: runEvent('%ls', special='%ls') dispatched",
                eventName.c_str(), specialName.c_str());
        return true;
    }
    UE_LOGW("event_fire: runEvent('%ls') dispatch FAILED", eventName.c_str());
    return false;
}

void Broadcast(FireKind kind, const std::string& name) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    coop::net::EventFirePayload p{};  // zero-init -> name[] pre-NUL-bound
    p.dispatch = static_cast<uint8_t>(kind);
    const size_t n = name.size() < sizeof(p.name) - 1 ? name.size() : sizeof(p.name) - 1;
    std::memcpy(p.name, name.c_str(), n);
    s->SendReliable(coop::net::ReliableKind::EventFire, &p, sizeof(p));
    UE_LOGI("event_fire: broadcast %s '%s'",
            kind == FireKind::SpecialEvent ? "runSpecialEvent" : "runEvent", name.c_str());
}

// HOST: a scheduler fire, seen as it happens -- runEvent entered from settime, the one call that
// fires a scheduled row. A dev fire reaches runEvent through our own ProcessEvent with no Blueprint
// caller and broadcasts at its own dispatch; the game's event menu calls runEvent with no settime
// either, and appends no row, as before.
sg::Verdict OnRunEventPre(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return sg::Verdict::Run;
    if (!ResolvePass() || call.function != g_runEventFn || call.callerFunction != g_settimeFn)
        return sg::Verdict::Run;
    const std::string name = NarrowName(*reinterpret_cast<const R::FName*>(call.locals + g_offEventParam));
    UE_LOGI("event_fire: host OBSERVED scheduler fire '%s' (settime -> runEvent)", name.c_str());
    Broadcast(FireKind::RunEvent, name);
    return sg::Verdict::Run;
}

// True iff the client's own passEvents already contains the row (the transferred save
// carried this fire; its world effects are already in the loaded state).
bool InClientPassEvents(const std::string& name) {
    RawArray* pass = ArrayAt(SaveSlotOf(ue_wrap::world_singleton::Gamemode()), g_offPassEvents);
    if (!pass || !pass->Data || pass->Num <= 0 || pass->Num > 100000) return false;
    std::wstring w(name.begin(), name.end());
    const R::FName want = ue_wrap::fname_utils::StringToFName(w);
    for (int32_t i = 0; i < pass->Num; ++i) {
        const R::FName& e = pass->Data[i];
        if (e.ComparisonIndex == want.ComparisonIndex && e.Number == want.Number) return true;
    }
    return false;
}

// The client replay executor, game thread. False if the eventer is not up yet (re-queue).
bool TryReplay(const PendingFire& pf) {
    if (!ResolvePass() || !EventerOf(ue_wrap::world_singleton::Gamemode())) return false;
    // Dedupe applies to one-shot scheduled rows only (the game's own passEvents semantics);
    // specials (graffiti, pranks the menu re-fires) are repeatable by design.
    if (pf.kind == FireKind::RunEvent) {
        if (g_replayed.count(pf.name)) {
            UE_LOGI("event_fire: '%s' already replayed this session -- skipping", pf.name.c_str());
            return true;
        }
        // A passEvents hit marks nothing: the skip must stay re-decidable, since marking here would
        // let a history-skipped fire permanently block a later in-flight override for the same row
        // (a fire landing between the joiner's connect and its blob capture rides both the wire and
        // the blob). A duplicate just rescans the array; passEvents never shrinks mid-session.
        if (!pf.activeOverride && InClientPassEvents(pf.name)) {
            UE_LOGI("event_fire: '%s' already in local passEvents (save carried it) -- skipping",
                    pf.name.c_str());
            return true;
        }
    }
    const std::wstring w(pf.name.begin(), pf.name.end());
    UE_LOGI("event_fire: client REPLAY %s '%s'%s",
            pf.kind == FireKind::SpecialEvent ? "runSpecialEvent" : "runEvent", pf.name.c_str(),
            pf.activeOverride ? " (in-flight active-override)" : "");
    // The special is always None: the only native special is the prank roll, host-local RNG, and
    // replaying it would roll a different prank here. Marked consumed only on a successful
    // dispatch (a frame or call failure is loud and must not eat the row).
    if (NativeFire(pf.kind, w, L"None")) {
        g_replays.fetch_add(1, std::memory_order_relaxed);
        if (pf.kind == FireKind::RunEvent) g_replayed.insert(pf.name);
    }
    return true;
}

// CLIENT: replay the queued fires in arrival order, stopping at the first the eventer cannot take
// yet. Run before a new fire is handled, and at the client's world-ready announce, by which time the
// gamemode's boot has set its eventer.
void DrainPending() {
    while (!g_pending.empty()) {
        if (!TryReplay(g_pending.front())) return;
        g_pending.pop_front();
    }
}

// CLIENT: the cycle is about to tick, and its settime walks the save's list of events on any clock
// change: hold the list empty before the tick's body runs, so no row is ever due on a client -- the
// first tick of a new world included, where the clock lane's pre-observer writes the host's newest
// sample into the same tick. Held on exactly the cycles the clock lane parks, by its own predicate.
bool g_tickObserved = false;  // the pre-observer is registered (once per process)
bool g_holdUnresolvable = false;  // the save slot's class has no allEvents; said once

void OnCycleTickPre(void* self, void* /*function*/, void* /*params*/) {
    if (!GT::IsGameThread() || !coop::time_sync::HoldsCycle(self)) return;
    void* ss = ue_wrap::daynightcycle::SaveSlotOfCycle(self);
    if (!ss) return;
    // The list's offset from the live slot's own class, a property lookup with no object-array walk,
    // so the first tick of a world is held even before the other members have resolved.
    if (g_offAllEvents < 0) {
        if (g_holdUnresolvable) return;
        g_offAllEvents = R::FindPropertyOffset(R::ClassOf(ss), L"allEvents");
        if (g_offAllEvents < 0) {
            g_holdUnresolvable = true;
            UE_LOGW("event_fire: the save slot has no allEvents -- a client's event walk cannot be held");
            return;
        }
    }
    RawArray* all = ArrayAt(ss, g_offAllEvents);
    if (!all || all->Num <= 0 || all->Num > 100000) return;  // 0 = already suppressed
    // A legal array state (empty with slack): data and capacity untouched, and the engine frees
    // the same allocation later. The gamemode's boot rebuilds allEvents from the table on every
    // world load, so this re-asserts after any reload and can never poison a save.
    g_zeroedAllEventsNum = all->Num;
    g_zeroedSaveSlot = ss;
    g_zeroedSaveSlotIdx = R::InternalIndexOf(ss);
    all->Num = 0;
    UE_LOGI("event_fire: client scheduler SUPPRESSED at the cycle's tick (allEvents %d -> 0; host is the only "
            "firer; restored on disconnect)", g_zeroedAllEventsNum);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    // Called every pump tick by the install fanout, which is also the retry until the cycle class
    // loads and until the gate has resolved the watch's name.
    if (!g_watchInstalled)
        g_watchInstalled = sg::WatchName(kRunEvent, kTagEventFire, &OnRunEventPre, nullptr);
    if (g_watchInstalled && !g_watchLive) {
        sg::ResolvePendingNames();
        if (sg::NameWatchLive(kRunEvent, kTagEventFire)) {
            g_watchLive = true;
            UE_LOGI("event_fire: the host's scheduler fires are seen at runEvent (a script-gate watch)");
        }
    }
    namespace DNC = ue_wrap::daynightcycle;
    if (g_tickObserved || !DNC::EnsureResolved()) return;
    void* fn = DNC::TickFunction();
    if (!fn) {
        UE_LOGW("event_fire: daynightCycle_C::ReceiveTick not found -- a client's event walk cannot be held");
        g_tickObserved = true;
        return;
    }
    if (!GT::RegisterPreObserver(fn, &OnCycleTickPre)) {
        static bool s_said = false;  // retried every pump tick; said once
        if (!s_said) UE_LOGW("event_fire: the cycle tick's pre-observer did not register (table full?) -- retrying");
        s_said = true;
        return;
    }
    g_tickObserved = true;
    UE_LOGI("event_fire: a client's event walk is held at the cycle's own tick (pre-observer on ReceiveTick)");
}

bool HostFire(FireKind kind, const std::wstring& eventName, const std::wstring& specialName) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (s && s->connected() && s->role() != coop::net::Role::Host) {
        UE_LOGW("event_fire: HostFire refused -- connected as a client (host is authoritative)");
        return false;
    }
    const std::wstring ev = eventName;
    const std::wstring sp = specialName.empty() ? L"None" : specialName;
    GT::Post([kind, ev, sp] {
        // Resolve here, not before the post: a solo host's dev menu has no session. The native fire
        // warns loudly if the world or the eventer is not up.
        if (!ResolvePass()) {
            UE_LOGW("event_fire: HostFire('%ls') -- the event classes are not loaded", ev.c_str());
            return;
        }
        if (!NativeFire(kind, ev, sp)) return;  // authority did not fire -> nothing to mirror
        // The dev seam: a direct runEvent has no settime caller, so the watch leaves it to this
        // broadcast. The wire carries the name only, never the special.
        std::string narrow;
        narrow.reserve(ev.size());
        for (wchar_t c : ev) narrow.push_back((c > 0 && c < 128) ? static_cast<char>(c) : '?');
        Broadcast(kind, narrow);
    });
    return true;
}

void OnReliable(const coop::net::EventFirePayload& payload) {
    if (!GT::IsGameThread()) { UE_LOGW("event_fire: OnReliable off-game-thread -- dropping"); return; }
    // NUL-bound the name (the payload crosses the trust boundary; the dispatcher length-checked
    // it).
    char buf[sizeof(payload.name) + 1] = {};
    std::memcpy(buf, payload.name, sizeof(payload.name));
    const std::string name(buf);
    if (name.empty() || payload.dispatch > static_cast<uint8_t>(FireKind::SpecialEvent)) {
        UE_LOGW("event_fire: OnReliable malformed (dispatch=%u, name='%s') -- dropping",
                payload.dispatch, name.c_str());
        return;
    }
    const FireKind kind = static_cast<FireKind>(payload.dispatch);
    const char* lane = nullptr;
    const int verdict = ReplayVerdict(name, &lane);
    if (verdict == 0) {
        UE_LOGI("event_fire: '%s' NOT replayed -- %s owns the outputs", name.c_str(), lane);
        return;
    }
    if (verdict < 0) {
        UE_LOGW("event_fire: '%s' not in the replay policy -- default NO-replay (newer host? "
                "add the row's verdict)", name.c_str());
        return;
    }
    DrainPending();
    PendingFire pf{ kind, name };
    if (g_pending.empty() && TryReplay(pf)) return;
    // One-shot rows dedupe at queue time too (a scheduler re-fire of a dev-fired row during the
    // same load window would otherwise queue twice; the replay would catch it later, but a
    // duplicate-free queue keeps the cap honest).
    if (kind == FireKind::RunEvent) {
        for (const auto& q : g_pending)
            if (q.kind == kind && q.name == name) return;
    }
    if (g_pending.size() >= kMaxPending) {
        UE_LOGW("event_fire: pending replay queue full (%zu) -- dropping '%s'",
                g_pending.size(), name.c_str());
        return;
    }
    UE_LOGI("event_fire: eventer not up yet -- queued '%s' (%zu pending)",
            name.c_str(), g_pending.size() + 1);
    g_pending.push_back(std::move(pf));
}

void ReplayInFlightRow(const std::string& rowName) {
    if (!GT::IsGameThread()) { UE_LOGW("event_fire: ReplayInFlightRow off-game-thread -- dropping"); return; }
    if (rowName.empty()) return;
    const char* lane = nullptr;
    const int verdict = ReplayVerdict(rowName, &lane);
    if (verdict == 0) {
        UE_LOGI("event_fire: in-flight '%s' NOT replayed -- %s owns the outputs (its join "
                "snapshot delivers current state)", rowName.c_str(), lane);
        return;
    }
    if (verdict < 0) {
        UE_LOGW("event_fire: in-flight '%s' not in the replay policy -- default NO-replay "
                "(add the row's verdict)", rowName.c_str());
        return;
    }
    DrainPending();
    PendingFire pf{ FireKind::RunEvent, rowName, /*activeOverride=*/true };
    if (g_pending.empty() && TryReplay(pf)) return;
    // The eventer is not up yet. If the row is already queued (a fire copy landed in the pre-world
    // window), upgrade it in place: two entries would double-dispatch, and the plain copy alone
    // could history-skip the in-flight replay.
    for (auto& q : g_pending) {
        if (q.kind == FireKind::RunEvent && q.name == rowName) {
            q.activeOverride = true;
            UE_LOGI("event_fire: in-flight '%s' already queued -- upgraded to active-override",
                    rowName.c_str());
            return;
        }
    }
    if (g_pending.size() >= kMaxPending) {
        UE_LOGW("event_fire: pending replay queue full (%zu) -- dropping in-flight '%s'",
                g_pending.size(), rowName.c_str());
        return;
    }
    UE_LOGI("event_fire: eventer not up yet -- queued in-flight '%s' (%zu pending)",
            rowName.c_str(), g_pending.size() + 1);
    g_pending.push_back(std::move(pf));
}

void OnClientWorldReady() {
    if (g_pending.empty()) return;
    const size_t before = g_pending.size();
    DrainPending();
    if (g_pending.empty())
        UE_LOGI("event_fire: world ready -- replayed the %zu queued fire(s)", before);
    else
        UE_LOGW("event_fire: world ready but the eventer is not up -- %zu of %zu queued fire(s) wait for "
                "the next fire", g_pending.size(), before);
}

void OnDisconnect() {
    // Restore the client's scheduler only if we zeroed this exact live save slot and nothing
    // repopulated it since (a boot rebuild leaves the count positive, and then the restore must
    // not run).
    if (g_zeroedAllEventsNum > 0 && g_zeroedSaveSlot &&
        R::IsLiveByIndex(g_zeroedSaveSlot, g_zeroedSaveSlotIdx)) {
        RawArray* all = ArrayAt(g_zeroedSaveSlot, g_offAllEvents);
        if (all && all->Num == 0 && all->Max >= g_zeroedAllEventsNum) {
            all->Num = g_zeroedAllEventsNum;
            UE_LOGI("event_fire: allEvents restored (0 -> %d) -- local scheduler resumes",
                    g_zeroedAllEventsNum);
        }
    }
    g_zeroedAllEventsNum = 0;
    g_zeroedSaveSlot = nullptr;
    g_zeroedSaveSlotIdx = -1;
    g_pending.clear();
    g_replayed.clear();
    g_session.store(nullptr, std::memory_order_release);
}

unsigned ReplayCount() { return g_replays.load(std::memory_order_relaxed); }

}  // namespace coop::event_fire_sync
