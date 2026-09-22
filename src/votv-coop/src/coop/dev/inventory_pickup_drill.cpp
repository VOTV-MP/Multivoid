// coop/dev/inventory_pickup_drill.cpp -- see coop/dev/inventory_pickup_drill.h.

#include "coop/dev/inventory_pickup_drill.h"

#include "coop/config/config.h"
#include "coop/dev/director/director.h"
#include "record_digest.h"   // co-located private header (src tree, not include/)
#include "coop/player/players_registry.h"
#include "coop/net/session.h"
#include "coop/player/roster.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/actors/floppy_disc.h"
#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/desk/drive_chain.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_nav.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace coop::dev::inventory_pickup_drill {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

// The drill waits on readiness, not on a clock. A CLIENT acts once its join is over: it has announced
// world-ready and the join cover is down, which is when the host's snapshot has been applied (a
// measured run sat 39 s past that point on the old fixed wait). The HOST seeds once its own player
// stands and the save's objects have had time to spawn; in a rig run that is before the client's
// join capture, so the seeded drive and disc are in the world the joiner is handed.
constexpr int kClientSettleTicks = 120;  // ~2 s past the join's end, at the pump's ~60 Hz
constexpr int kHostSettleTicks   = 600;  // ~10 s past the host's player standing
constexpr float kDrillFood = 37.f, kDrillSleep = 61.f;
constexpr int kMaxTries    = 8;     // per wanted kind: a full inventory refuses everything, so never one dispatch per prop in the world

// The walk: the director's route over the NavMesh to a reachable point some way off, so the pose
// the profile records is somewhere the start point is not. A worker thread, since the director's
// Run blocks; every engine touch inside it is posted to the game thread by the director itself.
DWORD WINAPI WalkAwayThread(LPVOID /*arg*/) {
    namespace D = coop::director;
    auto goal = std::make_shared<D::DirectorGoal>();
    auto picked = std::make_shared<std::atomic<int>>(0);
    ue_wrap::game_thread::Post([goal, picked] {
        // A point the NavMesh can route to, eight ways round at 15 m: the route's own last point
        // is on the mesh by construction, which a computed offset is not. No pile is needed, so
        // the walk works from the start point as well as from the base.
        void* player = coop::players::Registry::Get().Local();
        if (!player) { picked->store(-1); return; }
        const ue_wrap::FVector me = E::GetActorLocation(player);
        for (int k = 0; k < 8; ++k) {
            const float a = static_cast<float>(k) * 0.785398f;
            const ue_wrap::FVector want{me.X + 1500.f * std::cos(a), me.Y + 1500.f * std::sin(a), me.Z};
            std::vector<ue_wrap::FVector> route;
            if (!E::FindNavPath(player, me, want, route) || route.size() < 2) continue;
            const ue_wrap::FVector end = route.back();
            const float dx = end.X - me.X, dy = end.Y - me.Y;
            if (dx * dx + dy * dy < 800.f * 800.f) continue;  // a route that ends at our feet
            goal->targetPos = end;
            picked->store(1);
            return;
        }
        picked->store(-1);
    });
    for (int waited = 0; picked->load() == 0 && waited < 4000; waited += 5) ::Sleep(5);
    if (picked->load() != 1) {
        UE_LOGW("[INV-PICKUP-DRILL] no route of 8 m or more from here -- the player stays put");
        UE_LOGI("[INV-PICKUP-DRILL] CLIENT LIFE DONE (no walk)");
        return 0;
    }
    goal->reachCm = 200.f;
    D::ControlManager mgr;
    D::AddWalkToProcesses(mgr, *goal);
    mgr.Run(*goal, /*maxSeconds=*/60);
    ue_wrap::game_thread::Post([goal] {
        void* player = coop::players::Registry::Get().Local();
        if (!player) return;
        const ue_wrap::FVector at = E::GetActorLocation(player);
        UE_LOGI("[INV-PICKUP-DRILL] walked (%hs) -- the player now stands at (%.0f, %.0f, %.0f) yaw=%.0f",
                goal->reached ? "reached" : goal->failReason, at.X, at.Y, at.Z,
                E::GetActorRotation(player).Yaw);
        // Everything this life set out to do is done; the profile that says so reaches the host on
        // the lane's next poll. A rig waits on this line, not on a number of seconds.
        UE_LOGI("[INV-PICKUP-DRILL] CLIENT LIFE DONE");
    });
    return 0;
}

void PocketTheWants(void* player);
void HostSeedTheWants(void* player);
void KeyCensus(bool first);
void HostSaveTick(coop::net::Session& session);
void ClientWriteGateProbe();

}  // namespace

void Tick(coop::net::Session* session) {
    static const bool s_on = ::coop::config::ReadEnv("VOTVCOOP_INV_PICKUP_DRILL") == "1";
    if (!s_on || !session) return;
    static bool s_done = false;
    if (s_done) { KeyCensus(false); HostSaveTick(*session); return; }

    void* player = coop::players::Registry::Get().Local();
    if (!player || !R::IsLive(player) || !E::GetController(player)) return;
    static int s_ticks = 0;
    const bool host = coop::roster::LocalIsHost();
    if (!host && (!coop::net_pump::HasAnnouncedWorldReady() ||
                  coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)) {
        s_ticks = 0;  // the join is not over: the settle counts from its end
        return;
    }
    if (++s_ticks < (host ? kHostSettleTicks : kClientSettleTicks)) return;
    s_done = true;
    KeyCensus(true);  // what stands in the world BEFORE this peer acts

    // The host only makes sure the world HOLDS a drive with a signal and a disc with data; the
    // client's items are the ones under test.
    if (coop::roster::LocalIsHost()) { HostSeedTheWants(player); return; }

    PocketTheWants(player);
    ClientWriteGateProbe();

    // Vitals no fresh life and no host has: what a rejoin restores must be THESE numbers.
    namespace V = ue_wrap::vitals;
    const bool wrote = V::Write(V::Field::Food, kDrillFood) && V::Write(V::Field::Sleep, kDrillSleep);
    UE_LOGI("[INV-PICKUP-DRILL] vitals set to food=%.0f sleep=%.0f -> %hs", kDrillFood, kDrillSleep,
            wrote ? "written" : "WRITE FAILED");

    if (HANDLE t = ::CreateThread(nullptr, 0, &WalkAwayThread, nullptr, 0, nullptr)) ::CloseHandle(t);
}

namespace {

namespace DC = ue_wrap::drive_chain;
namespace FD = ue_wrap::floppy_disc;
namespace SR = ue_wrap::save_record;

// What the client should end up carrying, one of each. A plain item proves the profile returns a
// record at all; the other two HOLD DATA -- a drive its signal row, a disc its `data` strings and
// its read-write count -- and whether that data survives a rejoin is a question about VALUES,
// which the readout's `values` lines answer when the same key is read in both lives.
enum class Kind { Plain, Drive, Disc };
struct Want {
    const char* label;
    Kind kind;
    std::vector<const wchar_t*> classes;
};
const Want kWants[] = {
    {"a plain item", Kind::Plain,
     {L"prop_food_C", L"prop_crowbar_C", L"prop_cup_C", L"prop_battery_C"}},
    {"a drive holding a signal", Kind::Drive, {L"prop_drive_C"}},
    // The colours, not the base class and not the white zip drive: what the disc driver seeds.
    {"a disc holding data", Kind::Disc,
     {L"prop_floppyDisc_R_C", L"prop_floppyDisc_G_C", L"prop_floppyDisc_Y_C",
      L"prop_floppyDisc_Bl_C", L"prop_floppyDisc_B_C", L"prop_floppyDisc_O_C"}},
};

constexpr const wchar_t* kStampName = L"INV-PICKUP-DRILL";

bool HoldsData(Kind kind, void* actor) {
    if (kind == Kind::Drive) {
        ue_wrap::signal_dynamic::Row row;
        return DC::EnsureResolved() && DC::ReadDriveRow(actor, row) && row.size > 0.f;
    }
    if (kind == Kind::Disc) {
        FD::DiscContent c;
        return FD::EnsureResolved() && FD::ReadDiscContent(actor, c) && !c.data.empty();
    }
    return true;
}

// Fixed content, so two runs and two logs compare; a non-zero date, so no receiver stamps Now.
bool Stamp(Kind kind, void* actor) {
    if (kind == Kind::Drive) {
        ue_wrap::signal_dynamic::Row row;
        row.name = kStampName;
        row.id = L"INV-DRILL";
        row.object = L"selftest";
        row.signal = L"selftest";
        row.size = 10.f;
        row.decoded = 10.f;
        row.date = 638000000000000000LL;
        row.frequency = 2;
        row.quality = 3;
        row.hasData = true;
        return DC::EnsureResolved() && DC::WriteDriveRow(actor, row) && DC::CallDriveUpd(actor);
    }
    if (kind == Kind::Disc) {
        FD::DiscContent c;
        c.readWrites = 777;
        c.data.push_back(std::wstring(kStampName) + L"-row-0");
        c.data.push_back(std::wstring(kStampName) + L"-row-1");
        return FD::EnsureResolved() && FD::WriteDiscContent(actor, c);
    }
    return true;
}

std::vector<void*> LiveOf(const Want& w) {
    std::vector<void*> out;
    for (const wchar_t* cls : w.classes)
        for (void* a : R::FindObjectsByClass(cls))
            if (a && R::IsLive(a)) out.push_back(a);
    return out;
}

// HOST, once: a rig world may hold no loose drive and no disc at all, and one that holds them may
// hold them empty. The host is where a prop's content is authored, so it is stamped HERE and
// reaches the client the way any prop's save data does; the client stamps its own copy only when
// that did not happen, and says so.
void HostSeedTheWants(void* player) {
    const ue_wrap::FVector me = E::GetActorLocation(player);
    float step = 0.f;
    for (const Want& w : kWants) {
        if (w.kind == Kind::Plain) continue;
        std::vector<void*> have = LiveOf(w);
        void* pick = nullptr;
        for (void* a : have) if (HoldsData(w.kind, a)) { pick = a; break; }
        if (pick) {
            UE_LOGI("[INV-PICKUP-DRILL] HOST: the world already holds %hs: '%ls' key='%ls' (%zu of the kind)",
                    w.label, R::ClassNameOf(pick).c_str(),
                    ue_wrap::prop::GetInteractableKeyString(pick).c_str(), have.size());
            continue;
        }
        bool spawned = false;
        if (have.empty()) {
            void* cls = R::FindClass(w.classes.front());
            step += 40.f;
            void* born = cls ? E::SpawnActor(cls, {me.X + 60.f, me.Y + step, me.Z + 60.f}) : nullptr;
            if (!born) {
                UE_LOGE("[INV-PICKUP-DRILL] HOST: no %hs in the world and the spawn of '%ls' failed "
                        "(class=%p)", w.label, w.classes.front(), cls);
                continue;
            }
            have.push_back(born);
            spawned = true;
        }
        const bool ok = Stamp(w.kind, have.front());
        UE_LOGI("[INV-PICKUP-DRILL] HOST: %hs '%ls' key='%ls' for %hs -> stamp %hs",
                spawned ? "spawned" : "took the world's empty", R::ClassNameOf(have.front()).c_str(),
                ue_wrap::prop::GetInteractableKeyString(have.front()).c_str(), w.label,
                ok ? "written" : "WRITE FAILED");
    }
}

void PocketTheWants(void* player) {
    void* fn = R::FindFunction(R::ClassOf(player), L"putObjectInventory2");
    if (!fn) {
        UE_LOGE("[INV-PICKUP-DRILL] putObjectInventory2 did not resolve -- drill aborted");
        return;
    }
    // What is carried already, by key: a prop that stands in the world under a carried key is a
    // duplicate of a pocketed item, and pocketing it again would hide exactly that.
    ue_wrap::inventory::PlayerInventory mine;
    ue_wrap::inventory::ReadAll(mine);
    auto carriedKey = [&mine](const std::wstring& key) {
        for (const auto& r : mine.inventory) if (r.key == key) return true;
        return false;
    };
    const ue_wrap::FVector me = E::GetActorLocation(player);

    for (const Want& w : kWants) {
        // A rejoined life already carries what the first one pocketed: the kind is satisfied, and
        // a second pickup would only blur which record the two lives are compared on.
        bool carried = false;
        for (const auto& r : mine.inventory)
            for (const wchar_t* cls : w.classes) if (r.className == cls) carried = true;

        // Candidates the verb may still refuse (a filtered class, a full inventory), so they are
        // tried in order up to kMaxTries; one that holds data goes first.
        std::vector<void*> order;
        for (void* prop : LiveOf(w)) {
            const std::wstring key = ue_wrap::prop::GetInteractableKeyString(prop);
            if (carriedKey(key)) {
                // Where it sits: at the player's feet it was ejected from an inventory, at a map
                // location it came with the world.
                const ue_wrap::FVector at = E::GetActorLocation(prop);
                const float dx = at.X - me.X, dy = at.Y - me.Y, dz = at.Z - me.Z;
                UE_LOGW("[INV-PICKUP-DRILL] DUPLICATE IN THE WORLD: '%ls' key='%ls' stands at "
                        "(%.0f, %.0f, %.0f), %.0f cm away, and that key is already carried -- "
                        "left alone", R::ClassNameOf(prop).c_str(), key.c_str(), at.X, at.Y, at.Z,
                        std::sqrt(dx * dx + dy * dy + dz * dz));
                continue;
            }
            if (HoldsData(w.kind, prop)) order.insert(order.begin(), prop);
            else order.push_back(prop);
        }
        if (carried) {
            UE_LOGI("[INV-PICKUP-DRILL] %hs is already carried -- nothing pocketed for it", w.label);
            continue;
        }
        if (order.empty()) {
            UE_LOGW("[INV-PICKUP-DRILL] the world holds no candidate for %hs", w.label);
            continue;
        }

        bool took = false;
        int tries = 0;
        for (void* prop : order) {
            if (++tries > kMaxTries) break;
            const std::wstring cls = R::ClassNameOf(prop);
            const std::wstring key = ue_wrap::prop::GetInteractableKeyString(prop);
            if (!HoldsData(w.kind, prop)) {
                // The host stamps its own; an empty one HERE means that content did not reach
                // this peer's copy, which is a finding about the prop's data lane, not the profile.
                const bool ok = Stamp(w.kind, prop);
                UE_LOGW("[INV-PICKUP-DRILL] '%ls' key='%ls' stands here holding NO data -- stamped "
                        "on this peer -> %hs", cls.c_str(), key.c_str(), ok ? "written" : "WRITE FAILED");
            }
            // The record as the item itself serializes it, before the verb runs. The carried
            // record differs from it in the bool group by design: addObject appends the owner's
            // `fridge` flag to bools[0].
            // Deliberately NOT gated on prop_save_data::Covers. That gate answers "does the
            // live-prop record lane publish this class", and a food is claimed off that lane; the
            // pocket is a different custody with a different carrier, and this line is the readout
            // of what the pocket will take. Gating it here would blind the drill to the one lane it
            // is driving. It writes no field of the actor -- getData only builds a struct -- but it
            // is not free of consequence: the first capture for a class runs the codec's convention
            // check, which can eject that class from the record lane for the session. A drill can
            // therefore be the party that ejects one, and the warning will name this class.
            SR::SaveRecord before;
            if (SR::CaptureRecord(prop, before))
                UE_LOGI("[INV-PICKUP-DRILL] before the pickup '%ls' key='%ls': %hs", cls.c_str(),
                        key.c_str(), coop::dev::record_digest::ValuesOf(before).c_str());

            const ue_wrap::FVector at = E::GetActorLocation(prop);
            ue_wrap::ParamFrame f(fn);
            f.Set(L"InputPin", prop);
            f.Set(L"noNotify", true);
            const bool called = ue_wrap::Call(player, f);
            took = called && f.Get<bool>(L"return");
            UE_LOGI("[INV-PICKUP-DRILL] putObjectInventory2('%ls' key='%ls' at (%.0f, %.0f, %.0f)) "
                    "for %hs -> %hs", cls.c_str(), key.c_str(), at.X, at.Y, at.Z, w.label,
                    !called ? "CALL FAILED" : took ? "POCKETED" : "refused");
            if (took) break;
        }
        if (!took) UE_LOGW("[INV-PICKUP-DRILL] no candidate was accepted for %hs", w.label);
    }
}

// HOST, with VOTVCOOP_INV_PICKUP_DRILL_SAVE set: save the world through the game's own
// saveSlot_C::save, when a joiner's world comes up. The host writes each player's profile to disk
// only with its own world save, and a rig host never saves by itself, so without this the cut is
// never exercised. The moment is a joiner's arrival and not a minute counter, because that is when
// there is something to see: the join capture just gathered the world and set the profiles aside,
// so the save after a REJOIN is the one that cuts what the first life left. `1` writes the slot the
// host loaded, once per arrival. `quick` writes twice per arrival: the player's quicksave, which
// puts the live world in a NEW <slot>_SUB_<n> file, then the pause menu's plain save, which writes
// the main slot -- two files from one world, which is what a profile set that follows the written
// slot has to be seen doing. The test rig puts the loaded slot and its profile files back after the
// run and reports the subsaves a run made. The arrival is read off the session as a whole, which is
// enough for the two peers this drill is written for: a SECOND client arriving while the first is
// still there is no edge, and a three-peer drill would need the edge per slot.
constexpr int kSaveGapTicks = 300;  // ~5 s at the pump's ~60 Hz: after the arrival, and between the two
constexpr int kSaveRuns     = 6;

void HostSaveTick(coop::net::Session& session) {
    static const std::string s_mode = ::coop::config::ReadEnv("VOTVCOOP_INV_PICKUP_DRILL_SAVE");
    static const bool s_on = s_mode == "1" || s_mode == "quick";
    if (!s_on || !coop::roster::LocalIsHost()) return;
    static bool s_wasReady = false;
    static int s_left = 0, s_due = 0, s_runs = 0;
    const bool ready = session.AnyWorldReadyPeer();
    if (ready && !s_wasReady) { s_left = s_mode == "quick" ? 2 : 1; s_due = kSaveGapTicks; }
    s_wasReady = ready;
    if (s_left == 0 || s_runs >= kSaveRuns || --s_due > 0) return;
    s_due = kSaveGapTicks;
    const bool firstOfArrival = s_left == 2;
    --s_left;
    ++s_runs;
    void* slot = ue_wrap::inventory::ResolveSaveSlot();
    void* fn = slot ? R::FindFunction(R::ClassOf(slot), L"save") : nullptr;
    if (!fn) {
        UE_LOGE("[INV-PICKUP-DRILL] HOST save #%d: saveSlot_C::save did not resolve (slot=%p)", s_runs, slot);
        return;
    }
    // saveToSlot, quicksave first: (true, *) -> a new subsave; (false, true) -> the loaded slot as
    // named; (false, false) -> the main slot, also when a subsave is what was loaded.
    const bool quick = s_mode == "quick" && firstOfArrival;
    const bool plain = s_mode == "quick" && !quick;
    ue_wrap::ParamFrame f(fn);
    f.Set(L"quicksave", quick);
    f.Set(L"overwriteSubsave", !quick && !plain);
    f.Set(L"isForcedSave", true);  // save() hands saveToSlot `false` whatever this says
    const bool called = ue_wrap::Call(slot, f);
    UE_LOGI("[INV-PICKUP-DRILL] HOST save #%d through saveSlot_C::save (%hs) -> %hs", s_runs,
            quick ? "quicksave: a new subsave" : plain ? "plain: the main slot" : "the loaded slot",
            called ? "called" : "CALL FAILED");
    if (s_left == 0) UE_LOGI("[INV-PICKUP-DRILL] HOST SAVES DONE for this arrival (%d so far)", s_runs);
}

// CLIENT, with VOTVCOOP_INV_PICKUP_DRILL_SAVE set: ask the engine to write this world's save
// object, once. A client's save cycle is held off before it ever reaches the engine, so the gate on
// the engine's save function is never asked in a normal run and would rot unseen. The slot name is
// a scratch one the boot sweep removes, in case the gate ever lets it through.
void ClientWriteGateProbe() {
    static const bool s_on = !::coop::config::ReadEnv("VOTVCOOP_INV_PICKUP_DRILL_SAVE").empty();
    if (!s_on) return;
    void* slot = ue_wrap::inventory::ResolveSaveSlot();
    void* gsCdo = R::FindClassDefaultObject(L"GameplayStatics");
    void* fn = gsCdo ? R::FindFunction(R::ClassOf(gsCdo), L"SaveGameToSlot") : nullptr;
    if (!slot || !fn) {
        UE_LOGE("[INV-PICKUP-DRILL] CLIENT write gate probe: not resolvable (slot=%p fn=%p)", slot, fn);
        return;
    }
    std::wstring name = L"zcoop_gateprobe";
    R::FString fs{};
    fs.Data = name.data();
    fs.Num  = static_cast<int32_t>(name.size()) + 1;
    fs.Max  = fs.Num;
    ue_wrap::ParamFrame f(fn);
    f.Set<void*>(L"SaveGameObject", slot);
    f.SetRaw(L"SlotName", &fs, sizeof(fs));
    f.Set<int32_t>(L"UserIndex", 0);
    const bool called = ue_wrap::Call(gsCdo, f);
    const bool wrote = called && f.Get<uint8_t>(L"ReturnValue") != 0;
    UE_LOGI("[INV-PICKUP-DRILL] CLIENT write gate probe: SaveGameToSlot('%ls') -> %hs", name.c_str(),
            !called ? "CALL FAILED" : wrote ? "WRITTEN -- THE GATE LET A CLIENT WORLD SAVE THROUGH"
                                            : "refused (the gate held)");
}

// Which keys stand in the world under the wanted classes, on THIS peer, over time. The drill's
// pickups are destroys on every other peer, so a key that leaves one census and comes back in a
// later one -- or two live actors under one key -- is the duplicate a rejoiner then finds at the
// item's old spot, caught on the peer and at the moment it is born. About once a minute, twelve
// times, and a census that found nothing new prints nothing.
constexpr int kCensusTicks = 3600;
constexpr int kCensusRuns  = 12;
constexpr size_t kCensusLines = 16;

void KeyCensus(bool first) {
    static int s_ticks = 0, s_runs = 0;
    static std::map<std::wstring, std::vector<void*>> s_last;
    if (!first) {
        if (s_runs >= kCensusRuns || ++s_ticks < kCensusTicks) return;
        s_ticks = 0;
    }
    ++s_runs;
    std::map<std::wstring, std::vector<void*>> now;
    size_t actors = 0;
    for (const Want& w : kWants)
        for (void* a : LiveOf(w)) {
            now[R::ClassNameOf(a) + L"|" + ue_wrap::prop::GetInteractableKeyString(a)].push_back(a);
            ++actors;
        }
    size_t lines = 0;
    auto say = [&lines](const char* what, const std::wstring& id, const std::vector<void*>& at) {
        if (++lines > kCensusLines) return;
        std::string ptrs;
        for (void* a : at) {
            char buf[24];
            std::snprintf(buf, sizeof(buf), " %p", a);
            ptrs += buf;
        }
        UE_LOGI("[INV-PICKUP-DRILL] census: %hs '%ls' (%zu live:%hs)", what, id.c_str(), at.size(),
                ptrs.c_str());
    };
    for (const auto& [id, at] : now) {
        const auto was = s_last.find(id);
        if (at.size() > 1 && (first || was == s_last.end() || was->second.size() != at.size()))
            say("SEVERAL LIVE ACTORS UNDER ONE KEY", id, at);
        else if (!first && was == s_last.end())
            say("APPEARED", id, at);
        else if (!first && was->second != at)
            say("SAME KEY, ANOTHER ACTOR", id, at);
    }
    if (!first)
        for (const auto& [id, at] : s_last)
            if (!now.count(id)) say("GONE", id, at);
    if (first || lines)
        UE_LOGI("[INV-PICKUP-DRILL] census #%d: %zu live actors under %zu keys, %zu line(s)%hs",
                s_runs, actors, now.size(), lines, lines > kCensusLines ? " (capped)" : "");
    s_last = std::move(now);
}

}  // namespace

}  // namespace coop::dev::inventory_pickup_drill
