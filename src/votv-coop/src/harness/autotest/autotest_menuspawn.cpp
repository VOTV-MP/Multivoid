// harness/autotest/autotest_menuspawn.cpp -- the spawn-menu cross-peer drill
// (VOTVCOOP_RUN_MENUSPAWN_DRILL=1), both peers. The interface and the description live in
// harness/autotest.h.
//
// The client drives the spawn menu's OWN spawn(FName) -- the function a click on a catalog slot
// calls (ui_spawnmenu::spawn[13] -> gamemode->spawnPropThroughGamemode(n, a, 1, ...)), verified on
// the current pak -- so the birth the drill measures is byte-for-byte the birth a player's click
// produces. The catalog row is read off the game's own propProcessor::propsNames, so both peers
// name the same row without a hardcoded catalog string.
//
// Each peer diffs its own world: the set of live prop-lineage actors carrying that row name, before
// and after. The client's own set must gain one (the spawn happened at all); the host's is the
// measurement -- it gains one only if the client's birth crossed.

#include "harness/autotest.h"

#include "coop/config/config.h"          // ReadEnv -- the early arm's switch
#include "coop/player/players_registry.h"
#include "coop/props/join_membership_sweep.h"  // the predicate the divergence sweep dooms by
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/field_io.h"      // TArrayView -- the catalog array's shape
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace harness::autotest {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;

// The ceiling on the one wait this drill cannot express as a readiness line: the host waiting for
// a prop that -- before the fix -- never arrives. A negative result IS the measurement, so the
// wait has to end by itself; it is reported with the verdict rather than left to the rig's budget.
constexpr int kCrossCeilingMs = 20000;
// The host's wait for somebody to measure: a seated peer. Long, because it covers the client's
// whole boot, download and world load, and it is the rig that kills a peer that never arrives.
constexpr int kPeerCeilingMs = 300000;

// One task on the game thread, waited for.
template <typename Fn>
void OnGameThread(Fn fn) {
    auto ran = std::make_shared<std::atomic<bool>>(false);
    GT::Post([fn, ran] { fn(); ran->store(true); });
    while (!ran->load()) ::Sleep(5);
}

// VOTVCOOP_MENUSPAWN_EARLY=1: the client spawns as soon as its world stands, without waiting for
// its join to finish -- inside the load episode, the reconcile window and the armed divergence
// sweep. That is the mid-activity-join row this lane owes (principle 8): the load gates drop a
// birth before it is even queued, and the sweep dooms any live keyed prop the host's snapshot did
// not name. Without this arm the drill always lands after both and proves neither.
bool EarlyArm() {
    static const bool on = coop::config::ReadEnv("VOTVCOOP_MENUSPAWN_EARLY") == "1";
    return on;
}

// This peer's world is up and, on a client, its join is over (the worldrules probe's predicate --
// the moment a peer may be driven).
bool PeerIsReady() {
    void* player = coop::players::Registry::Get().Local();
    if (!player || !R::IsLive(player) || !ue_wrap::engine::GetController(player)) return false;
    if (!IsClientRole() || EarlyArm()) return true;
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

// Is any peer seated? The host's whole measurement is what a CLIENT's spawn does, so a host that
// started watching before anyone joined would spend its window on an empty lobby. Measured: the
// host reached its own readiness 59 s before the client reached the world at all.
bool APeerIsSeated() {
    coop::players::Registry& reg = coop::players::Registry::Get();
    for (uint8_t slot = 1; slot < coop::players::kMaxPeers; ++slot)
        if (reg.Puppet(slot)) return true;
    return false;
}

// The live (non-CDO) propProcessor_C: the actor that owns the spawn menu's catalog and the widget.
void* FindPropProcessor() {
    return R::FindObjectByClass(L"propProcessor_C");
}

// The catalog row the drill spawns: propsNames[0], the game's own first spawn-menu entry. Reading
// it beats a hardcoded name -- the datatable is inside the pak and a renamed row would silently
// turn the drill into a spawn of nothing.
bool ReadCatalogRow(void* processor, R::FName& row, std::wstring& rowText) {
    if (!processor) return false;
    static int32_t sOff = -2;
    if (sOff == -2) sOff = R::FindPropertyOffset(R::ClassOf(processor), L"propsNames");
    if (sOff < 0) return false;
    const auto* view = reinterpret_cast<const ue_wrap::field_io::TArrayView*>(
        reinterpret_cast<const uint8_t*>(processor) + sOff);
    if (!view->data || view->num <= 0) return false;
    row = *reinterpret_cast<const R::FName*>(view->data);
    if (row.ComparisonIndex == 0) return false;
    rowText = R::ToString(row);
    return true;
}

// Every live prop-lineage actor whose props-table row is `row`. The drill's before/after set.
// This walks the whole object array, so it is written to allocate nothing per object: the row is
// compared as an FName pair rather than through GetPropNameString, which renders the name and
// builds a std::wstring for every prop in the world on every poll.
void CollectRowActors(const R::FName& row, std::unordered_set<void*>& out) {
    out.clear();
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj || !R::IsLive(obj)) continue;
        if (!ue_wrap::prop::IsDescendantOfProp(obj)) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
        const R::FName nm = ue_wrap::prop::GetPropName(obj);
        if (nm.ComparisonIndex != row.ComparisonIndex || nm.Number != row.Number) continue;
        out.insert(obj);
    }
}

// The first actor of that row this peer did not have at baseline, or null.
void* FindNewRowActor(const R::FName& row, const std::unordered_set<void*>& before) {
    std::unordered_set<void*> now;
    CollectRowActors(row, now);
    for (void* a : now) if (!before.count(a)) return a;
    return nullptr;
}

// The NEGATIVE arm: lib_C::replaceProp(prop, row, delete=false) reaches the SAME gamemode spawn
// verb the menu does (lib::replaceProp -> spawnPropThroughGamemode) from a caller that is not the
// menu, and so stands in for every morph, spawner and impact the world runs for itself. Its birth
// must stay local. Catching the menu's spawn and refusing everyone else's are two claims, and a
// run that only spawns from the menu has tested one of them.
bool DriveNonMenuSpawn(void* targetProp, const R::FName& row) {
    if (!targetProp) return false;
    void* libCdo = R::FindClassDefaultObject(L"lib_C");
    if (!libCdo) return false;
    void* fn = R::FindFunction(R::ClassOf(libCdo), L"replaceProp");
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<void*>(L"prop", targetProp);
    f.SetRaw(L"new", &row, sizeof(row));
    f.Set<bool>(L"delete", false);          // leave the drill's own prop standing
    f.Set<void*>(L"playerHolding", nullptr);
    f.Set<void*>(L"__WorldContext", targetProp);
    return ue_wrap::Call(libCdo, f);
}

// The menu's own spawn(FName) and the widget it lives on: the click's function. The widget need
// not be OPEN -- spawn() traces from the camera and calls the gamemode, with no read of its own
// visibility (ui_spawnmenu::spawn, exprs [1]-[13]) -- but it does have to EXIST, and the
// propProcessor creates it during the world's own startup. So this is a readiness question as much
// as a call: measured, a client spawning as early as its world stands reached the catalog seven
// seconds before the widget.
bool ResolveMenuSpawn(void* processor, void*& widget, void*& fn) {
    widget = nullptr;
    fn = nullptr;
    if (!processor) return false;
    static int32_t sWidgetOff = -2;
    if (sWidgetOff == -2) sWidgetOff = R::FindPropertyOffset(R::ClassOf(processor), L"spawnmenu");
    if (sWidgetOff < 0) return false;
    widget = *reinterpret_cast<void* const*>(
        reinterpret_cast<const uint8_t*>(processor) + sWidgetOff);
    if (!widget || !R::IsLive(widget)) return false;
    fn = R::FindFunction(R::ClassOf(widget), L"spawn");
    return fn != nullptr;
}

bool DriveMenuSpawn(void* processor, const R::FName& row) {
    void* widget = nullptr;
    void* fn = nullptr;
    if (!ResolveMenuSpawn(processor, widget, fn)) return false;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid() || !f.SetRaw(L"name", &row, sizeof(row))) return false;
    return ue_wrap::Call(widget, f);
}

}  // namespace

// --- Spawn-menu cross-peer drill (VOTVCOOP_RUN_MENUSPAWN_DRILL=1) --------------------
//
// See harness/autotest.h. Both peers take a baseline of the catalog row's actors once they are
// ready; the client then drives the menu's spawn and both report what their own world gained:
//   menuspawn: row='<name>' baseline=<n> (role=<host|client>)
//   menuspawn: CLIENT spawned key='<key>' loc=(x,y,z)
//   menuspawn: negative arm -- lib_C::replaceProp drove the same gamemode verb from a NON-menu caller
//   menuspawn: DONE role=<host|client> row='<name>' spawned=<0|1> crossed=<0|1> key='<key>' waited=<s>s
// The host's `crossed` is the measurement and the key in its DONE line is which birth crossed --
// the menu's, never the negative arm's. The client's `crossed` says only that the spawn happened.
// VOTVCOOP_MENUSPAWN_EARLY=1 moves the client's spawn inside its own join, where the load gates
// and the armed divergence sweep are what the birth has to survive.
void RunMenuSpawnDrill() {
    const bool isClient = IsClientRole();
    const char* role = isClient ? "client" : "host";
    UE_LOGI("menuspawn: drill start (%s, waiting for this peer to be ready)", role);
    for (int waited = 0;; waited += 250) {
        bool ready = false;
        OnGameThread([&ready] { ready = PeerIsReady(); });
        if (ready) break;
        // A phase that can never end is not a readiness wait: a peer that stands up without ever
        // getting a controller leaves this thread alive and silent, and the rig's process watch
        // sees a healthy process. So the wait dies, saying which predicate never came true.
        if (waited >= kPeerCeilingMs) {
            UE_LOGW("menuspawn: DONE role=%s row='' spawned=0 crossed=0 sweep-candidate=0 key='' "
                    "waited=%ds -- this peer never became ready (no local pawn with a controller, "
                    "or on a client the join never reached Idle)", role, waited / 1000);
            return;
        }
        ::Sleep(250);
    }

    // The row and the baseline, from this peer's own world. The drill's own instruments are part
    // of its readiness, not a one-shot read: the propProcessor builds propsNames from the
    // datatables and creates the menu widget during the world's own startup, so a peer can stand
    // in its world well before either exists. On the client the widget is waited for too, since
    // driving the menu is the whole point; the host only reads.
    std::wstring rowText;
    R::FName row{0, 0};
    std::unordered_set<void*> before;
    for (int waited = 0; rowText.empty(); waited += 250) {
        OnGameThread([&rowText, &row, &before, isClient] {
            void* processor = FindPropProcessor();
            R::FName r{0, 0};
            std::wstring text;
            if (!ReadCatalogRow(processor, r, text)) return;
            if (isClient) {
                void* w = nullptr;
                void* f = nullptr;
                if (!ResolveMenuSpawn(processor, w, f)) return;   // the widget is not up yet
            }
            row = r;
            rowText = text;
            CollectRowActors(row, before);
        });
        if (!rowText.empty()) break;
        if (waited >= kCrossCeilingMs) {
            UE_LOGW("menuspawn: DONE role=%s row='' spawned=0 crossed=0 key='' waited=%ds "
                    "-- the catalog or the spawn-menu widget never became readable", role,
                    waited / 1000);
            return;
        }
        ::Sleep(250);
    }
    UE_LOGI("menuspawn: row='%ls' baseline=%d (role=%s)", rowText.c_str(),
            static_cast<int>(before.size()), role);

    // The host's baseline is taken above, at its own readiness -- before any client exists, so
    // nothing the client does can land inside it -- and only THEN does it wait for a peer to
    // measure. The two are separate on purpose: baselining at the join would race the client's
    // spawn, and watching from the host's own readiness would spend the window on an empty lobby.
    if (!isClient) {
        bool seated = false;
        for (int waited = 0; !seated; waited += 250) {
            OnGameThread([&seated] { seated = APeerIsSeated(); });
            if (seated) break;
            if (waited >= kPeerCeilingMs) {
                UE_LOGW("menuspawn: DONE role=host row='%ls' spawned=0 crossed=0 key='' "
                        "waited=%ds -- no peer ever seated, nothing to measure",
                        rowText.c_str(), waited / 1000);
                return;
            }
            ::Sleep(250);
        }
        UE_LOGI("menuspawn: a peer is seated -- watching for the client's menu spawn to arrive");
    }

    // The host holds its baseline and watches; only the client spawns. The client's own spawn is
    // what starts the host's clock, so the host waits for the crossing from here.
    bool spawned = false;
    bool sweepCandidate = false;   // the mid-join verdict: a player-authored birth must never be one
    std::wstring spawnedKey;
    if (isClient) {
        bool drove = false;
        OnGameThread([&drove, &row] {
            void* processor = FindPropProcessor();
            drove = DriveMenuSpawn(processor, row);
        });
        if (!drove) UE_LOGW("menuspawn: the menu's spawn() could not be driven -- widget or "
                            "function unresolved");
        // The prop is finished inside that call, so it is already in the world; its Key is minted
        // in the same finish (lib_C::assignKey), so it reads immediately.
        OnGameThread([&spawned, &spawnedKey, &row, &before] {
            if (void* a = FindNewRowActor(row, before)) {
                spawned = true;
                spawnedKey = ue_wrap::prop::GetInteractableKeyString(a);
                ue_wrap::FVector l{};
                const bool lRead = ue_wrap::engine::TryGetActorLocation(a, l);
                UE_LOGI("menuspawn: CLIENT spawned key='%ls' loc=(%.1f,%.1f,%.1f)%s",
                        spawnedKey.c_str(), l.X, l.Y, l.Z, lRead ? "" : " (unread)");
            }
        });
        if (!spawned) UE_LOGW("menuspawn: CLIENT drove the menu spawn but no new '%ls' appeared "
                              "in its own world", rowText.c_str());
        // The mid-join half that a sweep cannot always be made to show: this rig's world holds so
        // few in-universe props that the sweep's half-of-the-world valve aborts it before it
        // adjudicates anything. So ask the predicate the sweep itself consults. This is a VERDICT,
        // not a note: a player-authored birth that reads as a divergence candidate is the defect,
        // whether or not a bracket happens to be open, since the record that spares it owes nothing
        // to a bracket. An earlier build printed exactly this as a benign aside and passed.
        if (spawned) {
            OnGameThread([&row, &before, &sweepCandidate] {
                if (void* a = FindNewRowActor(row, before)) {
                    sweepCandidate =
                        coop::join_membership_sweep::IsInDivergenceUniverseUnclaimed(a);
                    UE_LOGI("menuspawn: sweep-candidate=%d claim-tracking=%d -- a player-authored "
                            "birth must not be a divergence candidate, bracket or no bracket",
                            sweepCandidate ? 1 : 0,
                            coop::join_membership_sweep::IsClaimTrackingActive() ? 1 : 0);
                }
            });
            if (sweepCandidate)
                UE_LOGW("menuspawn: FAIL -- the birth is a divergence-sweep candidate; a sweep "
                        "firing now would destroy the player's own prop on the player's own machine");
        }
        // The negative arm, on the prop just spawned and with delete off, so it stays standing and
        // the host keeps looking for the key it was told about. The birth this makes reaches the
        // same gamemode verb from lib_C::replaceProp, and the admission must refuse it: the host's
        // DONE names the key it found, so a wrongly-admitted second birth shows up as the wrong key
        // there and as a second PLAYER-AUTHORED line here.
        if (spawned) {
            bool drove2 = false;
            OnGameThread([&drove2, &row, &before] {
                if (void* a = FindNewRowActor(row, before)) drove2 = DriveNonMenuSpawn(a, row);
            });
            UE_LOGI("menuspawn: negative arm -- lib_C::replaceProp drove the same gamemode verb "
                    "from a NON-menu caller (%s); its birth must stay local",
                    drove2 ? "ran" : "COULD NOT RUN");
        }
    }

    // Only the HOST waits: gaining the prop is the whole question there, and a ceiling ends it
    // either way. The client already knows -- it made the thing -- and re-scanning on the client
    // would answer with whichever new actor of the row it met first, which after the negative arm
    // is the replaceProp birth rather than the menu's, and report a key the host never saw.
    int waitedMs = 0;
    bool crossed = isClient && spawned;
    std::wstring crossedKey = spawnedKey;
    for (; !isClient;) {
        OnGameThread([&crossed, &crossedKey, &row, &before] {
            if (void* a = FindNewRowActor(row, before)) {
                crossed = true;
                crossedKey = ue_wrap::prop::GetInteractableKeyString(a);
            }
        });
        if (crossed || waitedMs >= kCrossCeilingMs) break;
        ::Sleep(250);
        waitedMs += 250;
    }
    UE_LOGI("menuspawn: DONE role=%s row='%ls' spawned=%d crossed=%d sweep-candidate=%d key='%ls' "
            "waited=%ds", role, rowText.c_str(), spawned ? 1 : 0, crossed ? 1 : 0,
            sweepCandidate ? 1 : 0, (crossed ? crossedKey : spawnedKey).c_str(), waitedMs / 1000);
}

DWORD WINAPI MenuSpawnDrillThread(LPVOID /*arg*/) {
    RunMenuSpawnDrill();
    return 0;
}

}  // namespace harness::autotest
