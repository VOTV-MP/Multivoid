// harness/autotest/autotest_reloadchurn.cpp -- the re-load churn probe
// (VOTVCOOP_RUN_RELOAD_CHURN=1; "python tools/mp.py reloadchurn"). A client that joined, left to
// the menu and joined again died two seconds into the second world load: UEngine::LoadMap's
// CreateGameModeForURL reads AWorldSettings::DefaultGameMode off the world's
// PersistentLevel->WorldSettings, and that pointer was null (a shipping build compiles the check
// out and dereferences one frame up). Our DLL is on no frame of that stack and the tree never names
// either field, so the first arm is a negative control (solo, sessionless), since a coop rejoin and
// a plain second in-process map load produce the same report. Per cycle: settle in gameplay,
// census, travel to the menu with the layer live, census at the menu (the decisive frame: a
// gameplay world still resident there with a null WorldSettings is the crash, one load early), then
// re-load and census again. Every live UWorld is reported with its PersistentLevel, WorldSettings
// and DefaultGameMode chain, so the failing link is named. Diagnostic, not a shipping path.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/session/session_manager.h"
#include "harness/session_runtime.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cwchar>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace harness::autotest {
namespace {

namespace R  = ue_wrap::reflection;
namespace P  = ue_wrap::profile;
namespace GT = ue_wrap::game_thread;
namespace E  = ue_wrap::engine;

bool WaitDone(const std::shared_ptr<std::atomic<int>>& d, int timeoutMs) {
    for (int i = 0; i < timeoutMs / 5 && d->load() == 0; ++i) ::Sleep(5);
    return d->load() != 0;
}

int EnvInt(const char* key, int fallback) {
    const std::string v = coop::config::ReadEnv(key);
    return v.empty() ? fallback : atoi(v.c_str());
}

// The census: one line per live UWorld, naming every link of the chain the crash walked. Runs
// inline on the game thread. The offsets come from reflection, never from the crash's literals,
// so the code survives a recook; a missing property is reported rather than read at offset 0.
struct Offsets {
    int32_t persistentLevel = -1;
    int32_t worldSettings   = -1;
    int32_t defaultGameMode = -1;
    bool ok() const { return persistentLevel >= 0 && worldSettings >= 0 && defaultGameMode >= 0; }
};

Offsets ResolveOffsetsGT() {
    Offsets o;
    if (void* c = R::FindClass(P::name::WorldClass))
        o.persistentLevel = R::FindPropertyOffset(c, L"PersistentLevel");
    if (void* c = R::FindClass(L"Level"))
        o.worldSettings = R::FindPropertyOffset(c, L"WorldSettings");
    if (void* c = R::FindClass(L"WorldSettings"))
        o.defaultGameMode = R::FindPropertyOffset(c, L"DefaultGameMode");
    return o;
}

void* ReadPtr(void* base, int32_t off) {
    return (base && off >= 0) ? *reinterpret_cast<void**>(static_cast<char*>(base) + off) : nullptr;
}

// Where in its destruction a UObject stands, the term that separates the two readings of "dead
// but never purged". The engine tears an object down in two GC phases: ConditionalBeginDestroy
// sets RF_BeginDestroyed and queues the object; then IsReadyForFinishDestroy is polled, and
// only when it answers true does ConditionalFinishDestroy run and release the slot. So
// RF_BeginDestroyed set with RF_FinishDestroyed clear means phase 2 is stuck; neither set means
// GC never reached it, so something references it. PendingKill and Unreachable come from the
// GUObjectArray slot: PendingKill alone is "marked, not yet collected", Unreachable is "GC has
// claimed it".
std::wstring DestroyStage(void* obj) {
    if (!obj) return L"<null>";
    const int32_t objFlags = *reinterpret_cast<int32_t*>(
        static_cast<char*>(obj) + P::off::UObject_ObjectFlags);
    const int32_t intFlags = R::InternalFlagsOf(obj);
    std::wstring s;
    auto bit = [&s](int32_t have, int32_t mask, const wchar_t* name) {
        if (have & mask) { if (!s.empty()) s += L'|'; s += name; }
    };
    bit(intFlags, 0x20000000, L"PendingKill");
    bit(intFlags, 0x10000000, L"Unreachable");
    bit(intFlags, 0x40000000, L"RootSet");
    bit(intFlags, 0x01000000, L"ClusterRoot");
    bit(intFlags, 0x00800000, L"ReachableInCluster");
    bit(objFlags, 0x00008000, L"RF_BeginDestroyed");
    bit(objFlags, 0x00010000, L"RF_FinishDestroyed");
    if (s.empty()) s = L"<none>";
    wchar_t raw[64];
    swprintf(raw, 64, L" (rf=%08X int=%08X)", static_cast<unsigned>(objFlags),
             static_cast<unsigned>(intFlags));
    return s + raw;
}

// True iff the object sits in GC phase 2, begun and not finished: the crash's precondition,
// which the scenario greps for.
bool IsStuckInFinishDestroy(void* obj) {
    if (!obj) return false;
    const int32_t f = *reinterpret_cast<int32_t*>(
        static_cast<char*>(obj) + P::off::UObject_ObjectFlags);
    return (f & 0x00008000) != 0 && (f & 0x00010000) == 0;
}

// `tag` names when in the cycle this frame was taken, so a null pairs with its moment.
void CensusGT(const char* tag, int cycle, const Offsets& o) {
    if (!o.ok()) {
        UE_LOGW("reloadchurn[%s c%d]: OFFSETS UNRESOLVED (PersistentLevel=%d WorldSettings=%d "
                "DefaultGameMode=%d) -- census skipped, NOT a clean world",
                tag, cycle, o.persistentLevel, o.worldSettings, o.defaultGameMode);
        return;
    }
    const std::vector<void*> worlds = R::FindObjectsByClass(P::name::WorldClass);
    int nullWs = 0;
    int stuck = 0;
    size_t live = 0;
    // Every dead world and level, not just the last seen: reload churn produces two dead worlds at
    // once, and a rooted holder of the other one must not pass unseen.
    struct Terminal { void* obj; bool isWorld; };
    std::vector<Terminal> deadTerminals;
    int rootedWorst = 0;
    UE_LOGI("reloadchurn[%s c%d]: %zu UWorld object(s) in the array", tag, cycle, worlds.size());
    for (void* w : worlds) {
        if (!w) continue;
        // Dead worlds are printed too: a second open of the same map in one process reuses the
        // still-resident package, so the world whose WorldSettings the next LoadMap reads may be
        // one already kill-flagged at the menu, and a live-only census cannot see a reused corpse.
        const bool alive = R::IsLive(w);
        if (alive) ++live; else deadTerminals.push_back({w, true});
        const std::wstring wname = R::ToString(R::NameOf(w));
        void* lvl = ReadPtr(w, o.persistentLevel);
        void* ws  = ReadPtr(lvl, o.worldSettings);
        void* gm  = ReadPtr(ws, o.defaultGameMode);
        std::wstring lname = L"<none>";
        if (lvl && R::IsLive(lvl)) lname = R::ToString(R::NameOf(lvl));
        if (alive && lvl && !ws) ++nullWs;
        UE_LOGI("reloadchurn[%s c%d]:   world='%ls' @%p %s stage=%ls PersistentLevel=%p ('%ls')  "
                "WorldSettings=%p  DefaultGameMode=%p%s",
                tag, cycle, wname.c_str(), w, alive ? "LIVE" : "dead", DestroyStage(w).c_str(),
                lvl, lname.c_str(), ws, gm,
                (lvl && !ws) ? "   <<< WORLDSETTINGS IS NULL -- this world would CRASH LoadMap"
                             : "");
        if (!alive) stuck += IsStuckInFinishDestroy(w) ? 1 : 0;
    }
    // The levels and the WorldSettings actors, independently of any world. ULevel::WorldSettings is
    // a UPROPERTY, so if the WorldSettings actor is destroyed the GC nulls the reference, and the
    // coop layer destroys actors by the thousand: the one mechanism that could produce this fault
    // without the tree naming either field, so the census must be able to see it.
    const std::vector<void*> levels = R::FindObjectsByClass(L"Level");
    int orphanLevels = 0;
    for (void* l : levels) {
        if (!l) continue;
        void* ws = ReadPtr(l, o.worldSettings);
        const bool alive = R::IsLive(l);
        if (!alive) deadTerminals.push_back({l, false});
        if (!ws) ++orphanLevels;
        if (!ws || !alive) {
            UE_LOGI("reloadchurn[%s c%d]:   level @%p %s stage=%ls WorldSettings=%p%s",
                    tag, cycle, l, alive ? "LIVE" : "dead", DestroyStage(l).c_str(), ws,
                    ws ? "" : "   <<< a LEVEL with NO WorldSettings");
        }
    }
    const std::vector<void*> settings = R::FindObjectsByClass(L"WorldSettings");
    size_t wsLive = 0;
    for (void* a : settings) if (a && R::IsLive(a)) ++wsLive;

    // Who still reaches the dead world, by the Outer chain. Resolving each object's world through
    // its level cannot find a husk whose PersistentLevel is already null. The husk is PendingKill
    // and nothing else (not Unreachable, no RF_BeginDestroyed), so GC never claimed it and it comes
    // out reachable on every pass; the engine nulls strong property references to a PendingKill
    // object but not the structural ones (Outer, Class, Name), so the surviving path is an Outer
    // chain from something GC must keep, a root-set object, and our own layer roots runtime spawns.
    // So every object's Outer chain is followed, and every one passing through the dead world or
    // its level is named, plus every root-set object independently.
    if (!deadTerminals.empty()) {
        const int32_t n = R::NumObjects();
        std::vector<int> reached(deadTerminals.size(), 0);
        std::vector<int> rootedReached(deadTerminals.size(), 0);
        std::vector<std::vector<std::pair<std::wstring, int>>> byClass(deadTerminals.size());
        int rooted = 0, printed = 0;
        for (int32_t i = 0; i < n; ++i) {
            void* obj = R::ObjectAt(i);
            if (!obj) continue;
            bool isTerminal = false;
            for (const auto& t : deadTerminals) if (t.obj == obj) { isTerminal = true; break; }
            if (isTerminal) continue;
            // A bounded Outer walk. A PendingKill object is marked but not freed, so the read is
            // safe; the census runs inside a posted game-thread task, whose SEH wrapper absorbs a
            // fault on a slot caught mid-purge.
            void* chain[24] = {};
            int depth = 0;
            int hit = -1;
            for (void* o = R::OuterOf(obj); o && depth < 24; o = R::OuterOf(o)) {
                chain[depth++] = o;
                for (size_t t = 0; t < deadTerminals.size(); ++t)
                    if (deadTerminals[t].obj == o) { hit = static_cast<int>(t); break; }
                if (hit >= 0) break;
            }
            const bool isRoot = (R::InternalFlagsOf(obj) & 0x40000000) != 0;
            if (isRoot) ++rooted;
            if (hit < 0) continue;
            ++reached[hit];
            if (isRoot) ++rootedReached[hit];
            const std::wstring cn = R::ClassNameOf(obj);
            auto& tally = byClass[hit];
            bool found = false;
            for (auto& e : tally) if (e.first == cn) { ++e.second; found = true; break; }
            if (!found && tally.size() < 512) tally.emplace_back(cn, 1);
            // The rooted ones are printed in full: those are the candidate holders, and there
            // should be none. A non-rooted object in the chain is a passenger.
            if (isRoot && printed < 24) {
                ++printed;
                std::wstring path;
                for (int d = 0; d < depth; ++d) {
                    path += L" -> ";
                    path += R::ToString(R::NameOf(chain[d]));
                }
                UE_LOGW("reloadchurn[%s c%d]:   HOLDER (RootSet) '%ls' class=%ls @%p%ls",
                        tag, cycle, R::ToString(R::NameOf(obj)).c_str(), cn.c_str(), obj,
                        path.c_str());
            }
        }
        for (size_t t = 0; t < deadTerminals.size(); ++t) {
            auto& tally = byClass[t];
            std::sort(tally.begin(), tally.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
            std::wstring top;
            for (size_t i = 0; i < tally.size() && i < 8; ++i)
                top += tally[i].first + L"x" + std::to_wstring(tally[i].second) + L" ";
            // One line per dead object with its own root-set count, so the harness grades the
            // worst.
            UE_LOGI("reloadchurn[%s c%d]:   DEAD %ls @%p reached by %d object(s) via Outer "
                    "(%d of them RootSet); RootSet objects in array=%d; top: %ls",
                    tag, cycle, deadTerminals[t].isWorld ? L"world" : L"level",
                    deadTerminals[t].obj, reached[t], rootedReached[t], rooted, top.c_str());
            if (rootedReached[t] > rootedWorst) rootedWorst = rootedReached[t];
        }
    }

    // The headline the scenario greps: a non-zero count is the crash condition, observed without
    // dying of it.
    UE_LOGI("reloadchurn[%s c%d]: VERDICT nullWorldSettings=%d liveWorlds=%zu "
            "stuckInFinishDestroy=%d rootedHoldersOfDead=%d "
            "levels=%zu(%d with no WorldSettings) worldSettingsActors=%zu/%zu live",
            tag, cycle, nullWs, live, stuck, rootedWorst, levels.size(), orphanLevels, wsLive,
            settings.size());
}

void PostCensus(const char* tag, int cycle, const Offsets& o) {
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([done, tag, cycle, o] { CensusGT(tag, cycle, o); done->store(1); });
    WaitDone(done, 20000);
}

// Which world is current, through world_identity, never through a class search: after the
// client travelled out, the dead gameplay world sat unpurged and a class search kept returning
// it, so the probe read "never left gameplay" while the world had moved to the menu.
const wchar_t* KindName(ue_wrap::world_identity::WorldKind k) {
    using WK = ue_wrap::world_identity::WorldKind;
    return k == WK::Gameplay ? L"Gameplay" : (k == WK::Other ? L"Other" : L"Unknown");
}

// Wait for the world kind to hold. Unknown is never a match either way: it is the legitimate
// null window of a travel, and reading it as "left" or "arrived" is the bug the third value
// exists to prevent.
ue_wrap::world_identity::WorldKind WaitForWorldKind(
        ue_wrap::world_identity::WorldKind want, int seconds) {
    using WK = ue_wrap::world_identity::WorldKind;
    WK k = WK::Unknown;
    for (int i = 0; i < seconds; ++i) {
        k = ue_wrap::world_identity::CurrentWorldKind();
        if (k == want) return k;
        ::Sleep(1000);
    }
    return k;
}

// Force a collection, on the game thread. KismetSystemLibrary::CollectGarbage arms one for the
// end of the frame rather than running it inline, so the caller gives it a frame before the
// census. The arm that separates the two readings: if a forced collection clears the dead world
// and the rejoin survives, the coop travel leaves the collection unfinished and the next LoadMap
// adopts what it left; if the husk survives, something references it.
bool ForceGcGT() {
    void* cdo = R::FindClassDefaultObject(L"KismetSystemLibrary");
    if (!cdo) { UE_LOGW("reloadchurn: KismetSystemLibrary CDO not resolved"); return false; }
    void* fn = R::FindFunction(R::ClassOf(cdo), L"CollectGarbage");
    if (!fn) { UE_LOGW("reloadchurn: CollectGarbage not resolved"); return false; }
    ue_wrap::ParamFrame f(fn);
    return f.valid() && ue_wrap::Call(cdo, f);
}

// The game's own travel verb, inline on the game thread; a bare engine open does not travel
// here.
bool TransitionToMenuGT() {
    void* gm = R::FindObjectByClass(P::name::GamemodeClass);
    if (!gm || !R::IsLive(gm)) { UE_LOGW("reloadchurn: no live mainGamemode_C"); return false; }
    void* fn = R::FindFunction(R::ClassOf(gm), L"transition");
    if (!fn) { UE_LOGW("reloadchurn: mainGamemode_C::transition not resolved"); return false; }
    R::FName ln = ue_wrap::fname_utils::StringToFName(L"/Game/menu");
    ue_wrap::ParamFrame f(fn);
    if (!f.valid() || !f.SetRaw(L"LevelName", &ln, sizeof(ln))) return false;
    return ue_wrap::Call(gm, f);
}

// The live UWorld's address as an identity, proving a re-load happened: LoadStorySave answers
// false when it defers (it re-issues the open from the menu and lands a second later), so its
// bool cannot tell "refused" from "queued". A world pointer that changed is the event itself.
void* WorldPtrGT() { return ue_wrap::world_identity::CurrentWorld(); }

void RunProbe() {
    const int cycles = EnvInt("VOTVCOOP_RELOAD_CYCLES", 3);
    const int dwellS = EnvInt("VOTVCOOP_RELOAD_DWELL_S", 15);
    const int menuS  = EnvInt("VOTVCOOP_RELOAD_MENU_S", 12);
    // Which save to re-load: the host's slot from VOTVCOOP_SAVE; a fresh peer has none and starts
    // a New Game, which travels through the same LoadMap. The map load is the subject.
    const std::string  slot = coop::config::ReadEnv("VOTVCOOP_SAVE");
    const std::wstring wslot(slot.begin(), slot.end());

    // The coop arm: the solo control re-loads the map by itself; a client re-loads it by rejoining,
    // which drags the whole join (the save transfer, the mirror spawn, the sweeps, the roster
    // teardown) through the same LoadMap. The two arms differ in exactly that.
    const bool  rejoin = coop::config::ReadEnv("VOTVCOOP_RELOAD_REJOIN") == "1";
    const std::string peer = coop::config::ReadEnv("VOTVCOOP_NET_PEER");
    const std::string port = coop::config::ReadEnv("VOTVCOOP_NET_PORT");
    const std::string addr = peer.empty() ? std::string() : (peer + ":" + (port.empty() ? "47621" : port));
    if (rejoin && addr.empty()) {
        UE_LOGW("reloadchurn: REJOIN arm asked for but VOTVCOOP_NET_PEER is unset -- abort");
        UE_LOGI("reloadchurn: DONE");
        ue_wrap::log::Flush();
        return;
    }

    const std::string arm = rejoin ? ("REJOIN -> " + addr)
                                   : (slot.empty() ? std::string("SOLO <fresh new game>")
                                                   : ("SOLO save '" + slot + "'"));
    UE_LOGI("reloadchurn: === RE-LOAD CHURN probe START (cycles=%d dwell=%ds menu=%ds arm=%s) ===",
            cycles, dwellS, menuS, arm.c_str());

    Offsets offs;
    {
        auto done = std::make_shared<std::atomic<int>>(0);
        auto out  = std::make_shared<Offsets>();
        GT::Post([done, out] { *out = ResolveOffsetsGT(); done->store(1); });
        WaitDone(done, 20000);
        offs = *out;
    }
    UE_LOGI("reloadchurn: offsets PersistentLevel=+0x%X WorldSettings=+0x%X DefaultGameMode=+0x%X",
            offs.persistentLevel, offs.worldSettings, offs.defaultGameMode);

    if (rejoin) {
        // A client reaches gameplay only through the join, so waiting for the world without waiting
        // for the session would pass on a peer that never connected.
        bool up = false;
        for (int i = 0; i < 240; ++i) {
            if (harness::session_runtime::Session().running()) { up = true; break; }
            ::Sleep(1000);
        }
        if (!up) {
            UE_LOGW("reloadchurn: REJOIN arm -- no running session after 240 s, abort");
            UE_LOGI("reloadchurn: DONE");
            ue_wrap::log::Flush();
            return;
        }
        UE_LOGI("reloadchurn: REJOIN arm -- session LIVE, waiting for the transferred world");
    }

    using WK = ue_wrap::world_identity::WorldKind;
    WK w = WaitForWorldKind(WK::Gameplay, 240);
    if (w != WK::Gameplay) {
        UE_LOGW("reloadchurn: never reached gameplay (worldKind=%ls) -- abort", KindName(w));
        UE_LOGI("reloadchurn: DONE");
        ue_wrap::log::Flush();
        return;
    }

    for (int c = 1; c <= cycles; ++c) {
        UE_LOGI("reloadchurn: ---- cycle %d/%d: in gameplay (world @%p) ----",
                c, cycles, ue_wrap::world_identity::CurrentWorld());
        PostCensus("gameplay", c, offs);
        ::Sleep(static_cast<DWORD>(dwellS) * 1000);
        PostCensus("pre-travel", c, offs);

        // Exit to the menu with the layer live, the player's own exit rather than the death flee:
        // the flee's bypass keeps our layer dormant through the teardown, which is not the reported
        // state.
        {
            auto done = std::make_shared<std::atomic<int>>(0);
            auto ok   = std::make_shared<int>(0);
            GT::Post([done, ok] { if (TransitionToMenuGT()) *ok = 1; done->store(1); });
            WaitDone(done, 8000);
            UE_LOGI("reloadchurn: cycle %d transition(/Game/menu) dispatched=%d", c, *ok);
        }

        const WK m = WaitForWorldKind(WK::Other, 90);
        if (m != WK::Other) {
            UE_LOGW("reloadchurn: cycle %d never left gameplay (worldKind=%ls) -- abort",
                    c, KindName(m));
            break;
        }
        UE_LOGI("reloadchurn: cycle %d at the menu (world @%p)", c, ue_wrap::world_identity::CurrentWorld());

        // The decisive frame: a gameplay UWorld still resident here with a null WorldSettings
        // faults the next load before it renders anything, and this line says so while the process
        // can still print it.
        PostCensus("menu", c, offs);
        // Sampled across the dwell: the solo control purges the old world some seconds into the
        // menu while the coop arm still has it, and whether the husk ever goes away is what
        // separates "we re-load before the engine finished" from "something is holding it".
        for (int t = 0; t < menuS; t += 10) {
            ::Sleep(static_cast<DWORD>(menuS - t < 10 ? menuS - t : 10) * 1000);
            PostCensus("menu-dwell", c, offs);
        }
        PostCensus("menu-settled", c, offs);

        // The GC arm, off by default: the run that reproduces the crash must not be the run that
        // tries to prevent it.
        if (coop::config::ReadEnv("VOTVCOOP_RELOAD_GC") == "1") {
            auto done = std::make_shared<std::atomic<int>>(0);
            auto ok   = std::make_shared<int>(0);
            GT::Post([done, ok] { if (ForceGcGT()) *ok = 1; done->store(1); });
            WaitDone(done, 8000);
            UE_LOGI("reloadchurn: cycle %d forced CollectGarbage dispatched=%d", c, *ok);
            ::Sleep(4000);   // the collection is armed for the end of the frame
            PostCensus("post-gc", c, offs);
        }

        const void* menuWorld = WorldPtrGT();
        UE_LOGI("reloadchurn: cycle %d RE-LOADING the world (%s) -- menu world @%p",
                c, rejoin ? "by REJOINING the host" : "from the save", menuWorld);
        ue_wrap::log::Flush();   // survive a hard fault: the next line may never be written
        if (rejoin) {
            // ConnectDirect queues the start; the harness's own tick consumes it and runs the whole
            // join as the browser's Connect does. Called off the game thread: it only touches queue
            // state.
            const bool accepted = coop::session_manager::ConnectDirect(addr);
            UE_LOGI("reloadchurn: cycle %d ConnectDirect('%s') accepted=%d",
                    c, addr.c_str(), accepted ? 1 : 0);
            // Census repeatedly through the join, each flushed: the fatal LoadMap runs on the game
            // thread inside the join, so the best evidence is the last census that reached disk.
            for (int i = 0; i < 30; ++i) {
                if (ue_wrap::world_identity::CurrentWorldKind() == WK::Gameplay) break;
                PostCensus("joining", c, offs);
                ue_wrap::log::Flush();
                ::Sleep(2000);
            }
        } else {
            auto done = std::make_shared<std::atomic<int>>(0);
            auto ok   = std::make_shared<int>(0);
            GT::Post([done, ok, wslot] {
                E::ResetCachedSave();
                *ok = wslot.empty() ? (E::StartFreshGame(true) ? 1 : 0)
                                    : (E::LoadStorySave(wslot.c_str()) ? 1 : 0);
                done->store(1);
            });
            WaitDone(done, 30000);
            // Not a verdict term: false means deferred, not failed; the world-pointer change below
            // is the evidence.
            UE_LOGI("reloadchurn: cycle %d LoadStorySave returned %d (false == deferred, not failed)",
                    c, *ok);
        }

        w = WaitForWorldKind(WK::Gameplay, rejoin ? 240 : 120);
        if (w != WK::Gameplay) {
            UE_LOGW("reloadchurn: cycle %d re-load never reached gameplay (worldKind=%ls)",
                    c, KindName(w));
            PostCensus("reload-stuck", c, offs);
            break;
        }
        // The re-load is proven only by a different UWorld: a world with the gameplay name is also
        // what "the travel never happened" looks like.
        const void* newWorld = WorldPtrGT();
        if (newWorld == menuWorld) {
            UE_LOGW("reloadchurn: cycle %d world pointer UNCHANGED (@%p) -- no load actually ran",
                    c, newWorld);
            break;
        }
        UE_LOGI("reloadchurn: cycle %d SURVIVED the re-load (world @%p, menu world was @%p)",
                c, newWorld, menuWorld);
        PostCensus("reloaded", c, offs);
        ue_wrap::log::Flush();
    }

    UE_LOGI("reloadchurn: DONE");
    ue_wrap::log::Flush();
}

}  // namespace

void RunReloadChurnProbe() { RunProbe(); }
DWORD WINAPI ReloadChurnProbeThread(LPVOID) { RunReloadChurnProbe(); return 0; }

}  // namespace harness::autotest
