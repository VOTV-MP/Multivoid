// harness/autotest/autotest_ambient_field.cpp -- TEST-ONLY: one set of hexahive holes and beehive branches across two peers.
//
// A hexahive field stands on the host before the client joins, and the host ends one beehive branch once the joiner's world is
// taken. The joiner's branches from the save must bind to the host's by key with no copy beside them, every nest-holding branch
// must hold its nest, the census-end must end the loaded branch the host lacks, and the field of holes must be the host's. Then
// the host's own beehive body makes a branch whose nest and end reach the client, the nest's end by the prop lane. The client's
// hexahive and beehive rows refuse, read from the gate's counters. The host prints `ambient-field: VERDICT` over both peers'
// fails and the rejoined life's: the client's second life (VOTVCOOP_AMBIENT_FIELD_REJOIN=1, set by the rig for it) is a new
// load and a new census. Gated by VOTVCOOP_RUN_AMBIENT_FIELD; the folder is VOTVCOOP_CAN_DIR. Red arms:
// VOTVCOOP_AMBIENT_FIELD_RED.

#include "harness/autotest.h"
#include "harness/autotest/peer_signal.h"
#include "harness/session_runtime.h"

#include "coop/config/config.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/save/save_transfer.h"
#include "coop/session/net_pump.h"
#include "coop/world/ambient_fixture_sync.h"
#include "coop/world/spawn_authority.h"
#include "ue_wrap/actors/ambient_creature.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace AC = ue_wrap::ambient_creature;
namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;
namespace PS = harness::autotest::peer_signal;
using PS::Await;
using PS::AwaitText;
using PS::Mark;

int g_fails = 0;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

// A readiness wait on a state the game thread reads. A judged wait (`abort` false) ends in the caller's check.
template <class Pred>
bool WaitGT(const char* what, int seconds, Pred pred, bool abort = true) {
    return PS::WaitFor(what, seconds, [&] {
        bool ok = false;
        OnGameThread([&] { ok = pred(); });
        return ok;
    }, abort);
}

void Check(bool ok, const char* what) {
    UE_LOGI("ambient-field: %hs %hs", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}

// A peer's fails count as it left it in its done signal; text that is no count is a fail.
int ParseFails(const std::string& text) {
    char* end = nullptr;
    const long n = std::strtol(text.c_str(), &end, 10);
    return end != text.c_str() && n >= 0 ? static_cast<int>(n) : 1;
}

void* FirstOf(const wchar_t* cls) {
    void* found = nullptr;
    if (void* c = OI::ClassByName(cls))
        OI::ForEachInstance(c, [](void* ctx, void* obj, int32_t) {
            if (R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) *static_cast<void**>(ctx) = obj;
        }, &found);
    return found;
}

size_t CountOf(const wchar_t* cls) {
    size_t n = 0;
    if (void* c = OI::ClassByName(cls))
        OI::ForEachInstance(c, [](void* ctx, void* obj, int32_t) {
            if (R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) ++*static_cast<size_t*>(ctx);
        }, &n);
    return n;
}

// The live actor of `cls` within `cm` of `at`, or null.
void* ActorNear(const wchar_t* cls, const ue_wrap::FVector& at, float cm) {
    struct Ctx {
        ue_wrap::FVector at;
        float cm;
        void* found;
    } ctx{at, cm, nullptr};
    if (void* c = OI::ClassByName(cls))
        OI::ForEachInstance(c, [](void* p, void* obj, int32_t) {
            auto* x = static_cast<Ctx*>(p);
            ue_wrap::FVector l{};
            if (!R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__") || !E::TryGetActorLocation(obj, l)) return;
            const float dx = l.X - x->at.X, dy = l.Y - x->at.Y, dz = l.Z - x->at.Z;
            if (dx * dx + dy * dy + dz * dz <= x->cm * x->cm) x->found = obj;
        }, &ctx);
    return ctx.found;
}

// One run of a spawner's own function.
bool RunSpawner(const wchar_t* cls, const wchar_t* function, bool withDelta) {
    void* ticker = FirstOf(cls);
    void* fn = ticker ? R::FindDispatchFunctionCached(R::ClassOf(ticker), function) : nullptr;
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    if (withDelta) f.Set<float>(L"DeltaSeconds", 0.016f);
    return ue_wrap::Call(ticker, f);
}

// ---- what the two peers compare ----

// A sum of every place: two lists hold the same places when the counts and these agree.
struct Field {
    size_t n = 0;
    double x = 0, y = 0, z = 0;
};

Field ReadField(bool branches) {
    Field f;
    std::vector<void*> v;
    if (branches) AC::ListBranches(v);
    else AC::ListHoles(v);
    for (void* a : v) {
        ue_wrap::FVector at{};
        if (!E::TryGetActorLocation(a, at)) continue;
        ++f.n;
        f.x += at.X;
        f.y += at.Y;
        f.z += at.Z;
    }
    return f;
}

bool SameField(const Field& a, const Field& b) {
    const double tol = 2.0 * static_cast<double>((std::max<size_t>)(a.n, 1));
    return a.n == b.n && std::fabs(a.x - b.x) < tol && std::fabs(a.y - b.y) < tol && std::fabs(a.z - b.z) < tol;
}

// The branches whose nest is live: a branch the save brought links its nest by key, and a copy is given its nest by the lane.
size_t BranchesWithNest() {
    std::vector<void*> v;
    AC::ListBranches(v);
    size_t n = 0;
    for (void* b : v) {
        std::wstring key;
        void* hive = nullptr;
        if (AC::ReadBranch(b, key, hive) && hive) ++n;
    }
    return n;
}

struct Audit {
    Field branches;
    size_t withNest = 0;
    Field holes;
};

Audit ReadAudit() {
    Audit a;
    a.branches = ReadField(true);
    a.withNest = BranchesWithNest();
    a.holes = ReadField(false);
    return a;
}

std::string Pack(const Audit& a) {
    char b[320];
    std::snprintf(b, sizeof(b), "%zu %.1f %.1f %.1f %zu %zu %.1f %.1f %.1f", a.branches.n, a.branches.x, a.branches.y, a.branches.z,
                  a.withNest, a.holes.n, a.holes.x, a.holes.y, a.holes.z);
    return b;
}

bool Unpack(const std::string& text, Audit& a) {
    std::istringstream in(text);
    return static_cast<bool>(in >> a.branches.n >> a.branches.x >> a.branches.y >> a.branches.z >> a.withNest >> a.holes.n >> a.holes.x >>
                             a.holes.y >> a.holes.z);
}

std::string Place(const ue_wrap::FVector& v) {
    char b[96];
    std::snprintf(b, sizeof(b), "%.1f %.1f %.1f", v.X, v.Y, v.Z);
    return b;
}

// ---- the host ----

// The last branch ends: a branch the host no longer has, which a joiner's load may still carry.
bool CutBranch() {
    std::vector<void*> v;
    AC::ListBranches(v);
    return !v.empty() && AC::Destroy(v.back());
}

void* NewBranch(const std::vector<void*>& before) {
    std::vector<void*> v;
    AC::ListBranches(v);
    for (void* a : v)
        if (std::find(before.begin(), before.end(), a) == before.end()) return a;
    return nullptr;
}

// A joiner's world is taken: from there the host's changes reach it only through the lanes' own replay, so a branch ended now is
// one the joiner's load carries and the host lacks. The world is the joiner's own when the slot's capture count passes
// `capturesBefore`, the count an earlier occupant left; the slot's taken flag alone still stands from that occupant.
void CutAfterWorldTaken(int budgetSeconds, const char* what, uint32_t capturesBefore) {
    WaitGT("the host took the joiner's world", budgetSeconds,
           [capturesBefore] { return coop::save_transfer::WorldCapturesFor(1) > capturesBefore; });
    bool cut = false;
    OnGameThread([&] { cut = CutBranch(); });
    Check(cut, what);
}

void HostBody(int& rejoinFails, int& clientFails) {
    const uint32_t life1Gen = harness::session_runtime::Session().peerGenerationForSlot(1);
    WaitGT("the host's player", 240, [] { return coop::players::Registry::Get().Local() != nullptr; });

    // A real hive's own begin play beside the base: its field of holes and its self-destroy, before anyone joins.
    OnGameThread([&] {
        ue_wrap::FVector me{};
        void* pawn = coop::players::Registry::Get().Local();
        void* cls = OI::ClassByName(L"hexahiveSPawner_C");
        if (!cls) cls = R::FindClass(L"hexahiveSPawner_C");
        if (cls && pawn && E::TryGetActorLocation(pawn, me)) E::SpawnActor(cls, {me.X + 1500.f, me.Y, me.Z + 200.f});
    });
    WaitGT("the hive's field stands and its spawner is gone", 30, [] { return ReadField(false).n > 0 && CountOf(L"hexahiveSPawner_C") == 0; },
           false);
    OnGameThread([&] {
        Check(ReadField(false).n > 0, "the hive's own body made a field of holes");
        Check(CountOf(L"hexahiveSPawner_C") == 0, "the hive's spawner destroyed itself");
    });

    CutAfterWorldTaken(480, "the host ended a branch after the joiner's world was taken", 0);
    uint32_t life1Captures = 0;
    OnGameThread([&] { life1Captures = coop::save_transfer::WorldCapturesFor(1); });
    if (!Await("client_in", 480)) return;
    Audit audit;
    OnGameThread([&] { audit = ReadAudit(); });
    Mark("join_a", Pack(audit));
    if (!Await("join_a_done", 240)) return;

    // A hole the host ends, and a branch its own beehive body makes. A hole's end is each viewer's, so the client keeps its hole.
    ue_wrap::FVector holeAt{};
    OnGameThread([&] {
        std::vector<void*> v;
        AC::ListHoles(v);
        if (!v.empty() && E::TryGetActorLocation(v.back(), holeAt)) AC::Destroy(v.back());
    });
    std::vector<void*> had;
    OnGameThread([&] { AC::ListBranches(had); });
    void* branch = nullptr;
    WaitGT("the beehive spawner's own body made a branch", 120, [&] {
        RunSpawner(L"ticker_beehiveSpawner_C", L"Spawn", false);
        return (branch = NewBranch(had)) != nullptr;
    }, false);
    Check(branch != nullptr, "the beehive spawner's own body made a branch");
    if (!branch) return;
    ue_wrap::FVector at{}, nestAt{};
    WaitGT("the branch has its nest", 30, [&] {
        std::wstring key;
        void* hive = nullptr;
        return AC::ReadBranch(branch, key, hive) && hive && E::TryGetActorLocation(hive, nestAt) && E::TryGetActorLocation(branch, at);
    }, false);
    OnGameThread([&] { Check(!(nestAt.X == 0.f && nestAt.Y == 0.f && nestAt.Z == 0.f), "the host's branch has its nest"); });
    Mark("branch", Place(at) + " " + Place(nestAt) + " " + Place(holeAt));
    if (!Await("branch_seen", 240)) return;
    OnGameThread([&] { AC::Destroy(branch); });
    Mark("branch_end");
    if (!Await("branch_end_done", 240)) return;

    // The client's first life ends on its done signal; the rig then kills it, and the host sees its slot's occupant change.
    std::string clientText;
    clientFails = AwaitText("client_done", clientText, 120) ? ParseFails(clientText) : 1;
    Mark("host_ack");
    if (!PS::WaitFor("the client's life 1 gone from slot 1", 300, [life1Gen] {
            auto& s = harness::session_runtime::Session();
            return !s.IsSlotConnected(1) || s.peerGenerationForSlot(1) != life1Gen;
        })) {
        rejoinFails = 1;
        return;
    }
    CutAfterWorldTaken(400, "the host ended a branch after the rejoiner's world was taken", life1Captures);
    if (!Await("rejoin_ready", 400)) return;
    Audit second;
    OnGameThread([&] { second = ReadAudit(); });
    Mark("join_b", Pack(second));
    std::string rejoinText;
    rejoinFails = AwaitText("rejoin_done", rejoinText, 400) ? ParseFails(rejoinText) : 1;   // no word is no pass
}

void RunHost() {
    int rejoinFails = 1, clientFails = 1;
    HostBody(rejoinFails, clientFails);
    if (g_fails == 0 && clientFails == 0 && rejoinFails == 0) UE_LOGI("ambient-field: VERDICT PASS host=%d client=%d rejoin=%d", g_fails, clientFails, rejoinFails);
    else UE_LOGW("ambient-field: VERDICT FAIL host=%d client=%d rejoin=%d", g_fails, clientFails, rejoinFails);
    UE_LOGI("ambient-field: DONE on host fails=%d", g_fails);
}

// ---- the client ----

bool WorldReady() {
    return WaitGT("this client's world", 240, [] {
        return coop::players::Registry::Get().Local() && coop::net_pump::HasAnnouncedWorldReady();
    });
}

// The hexahive and beehive rows refuse on this client: each spawner's body is run here, and its refusal count must rise.
void CheckRefusals() {
    struct Spawner {
        const wchar_t* cls;
        const wchar_t* function;
        bool           withDelta;
        const char*    tag;
    };
    const Spawner spawners[] = {
        {L"ticker_hexahiveSpawner_C", L"ReceiveTick", true, "hexahiveSpawner.ReceiveTick"},
        {L"ticker_beehiveSpawner_C", L"Spawn", false, "beehiveSpawner.spawn"},
    };
    for (const Spawner& s : spawners) {
        uint64_t before = 0, after = 0;
        OnGameThread([&] {
            before = coop::spawn_authority::RefusalCount(s.tag);
            RunSpawner(s.cls, s.function, s.withDelta);
            after = coop::spawn_authority::RefusalCount(s.tag);
        });
        char what[96];
        std::snprintf(what, sizeof(what), "the client's %s body is refused (%llu -> %llu)", s.tag, static_cast<unsigned long long>(before),
                      static_cast<unsigned long long>(after));
        Check(after > before, what);
    }
}

// The join, judged against what the host read once its replay was due: the census-end is the end of that replay.
void JoinAudit(const Audit& host) {
    WaitGT("the join's census-end", 180, [] { return coop::ambient_fixture_sync::Read().censusEnds >= 1; });
    Audit here;
    WaitGT("the join's branches and holes match the host's", 30, [&] {
        here = ReadAudit();
        return SameField(here.branches, host.branches) && SameField(here.holes, host.holes);
    }, false);
    UE_LOGI("ambient-field: branches here=%zu host=%zu, holes here=%zu host=%zu", here.branches.n, host.branches.n, here.holes.n, host.holes.n);
    Check(SameField(here.branches, host.branches), "a joiner has the host's branches, at its places");
    Check(SameField(here.holes, host.holes), "a joiner has the hive's field, every hole at the host's place");
    WaitGT("every nest-holding branch holds its nest", 30, [&] { return BranchesWithNest() == host.withNest; }, false);
    size_t withNest = 0;
    OnGameThread([&] {
        withNest = BranchesWithNest();
        const coop::ambient_fixture_sync::Counters c = coop::ambient_fixture_sync::Read();
        UE_LOGI("ambient-field: loaded=%llu bound=%llu copies=%llu census-ended=%llu nests-bound=%llu", static_cast<unsigned long long>(c.loaded),
                static_cast<unsigned long long>(c.bound), static_cast<unsigned long long>(c.branchCopies),
                static_cast<unsigned long long>(c.censusEnded), static_cast<unsigned long long>(c.nestsBound));
        Check(c.loaded > 0 && c.bound > 0, "the save's branches bound to the host's by key");
        Check(c.branchCopies == 0, "no copy stands beside a branch the load brought");
        Check(c.loaded == c.bound + c.censusEnded, "every loaded branch was bound or ended by the census");
        Check(c.censusEnded >= 1, "the census-end ended the loaded branch the host lacks");
    });
    Check(withNest == host.withNest, "every nest-holding branch of the host holds its nest here");
}

// Leaves as a player's quit does, network first, so the host sees slot 1 empty at once.
void LeaveForRejoin() {
    OnGameThread([] {
        auto& s = harness::session_runtime::Session();
        if (s.running()) coop::net_pump::FleeAfterNativeMenuTravel(s, "ambient field drill: life 1 leaves for the rejoin");
    });
    PS::WaitFor("this life's session stopped", 15, [] { return !harness::session_runtime::Session().running(); }, false);
}

bool ClientBody() {
    if (!WorldReady()) return false;
    CheckRefusals();
    Mark("client_in");

    std::string body;
    Audit host;
    if (!AwaitText("join_a", body, 240) || !Unpack(body, host)) return false;
    JoinAudit(host);
    Mark("join_a_done");

    // A branch the host's own body made: its place, its nest, and the nest's end by the prop lane alone.
    if (!AwaitText("branch", body, 240)) return false;
    std::istringstream in(body);
    ue_wrap::FVector at{}, nestAt{}, holeAt{};
    in >> at.X >> at.Y >> at.Z >> nestAt.X >> nestAt.Y >> nestAt.Z >> holeAt.X >> holeAt.Y >> holeAt.Z;
    WaitGT("the host's branch stands here with its nest", 60, [&] {
        void* b = ActorNear(L"beehiveBranch_C", at, 100.f);
        std::wstring key;
        void* hive = nullptr;
        return b && AC::ReadBranch(b, key, hive) && hive && ActorNear(L"prop_waspnest_bee_C", nestAt, 200.f) == hive;
    }, false);
    uint64_t dropped0 = 0;
    OnGameThread([&] {
        void* b = ActorNear(L"beehiveBranch_C", at, 100.f);
        std::wstring key;
        void* hive = nullptr;
        Check(b && AC::ReadBranch(b, key, hive) && hive && ActorNear(L"prop_waspnest_bee_C", nestAt, 200.f) == hive,
              "the host's branch stands here at its place, holding its nest");
        const coop::ambient_fixture_sync::Counters c = coop::ambient_fixture_sync::Read();
        Check(c.branchCopies >= 1 && c.nestsBound >= 1, "the lane made a copy of the branch and gave it the nest");
        dropped0 = c.hiveDropped + c.hiveGone;
        // The hole the host ended before the branch was made: the reliable lane keeps order, so an end sent for it has come.
        Check(ActorNear(L"susDirtHole_C", holeAt, 100.f) != nullptr, "a hole the host ended stands here: a hole's end is its viewer's");
    });
    Mark("branch_seen");

    if (!Await("branch_end", 240)) return false;
    WaitGT("the host's branch ended here", 30, [&] { return ActorNear(L"beehiveBranch_C", at, 100.f) == nullptr; }, false);
    OnGameThread([&] { Check(ActorNear(L"beehiveBranch_C", at, 100.f) == nullptr, "a branch that ended on the host ended here"); });
    OnGameThread([&] {
        // The host's branch end destroys the host's nest too, and the prop lane may carry that end here first: the copy's end
        // then finds no nest. Either way the copy must not destroy one itself.
        const coop::ambient_fixture_sync::Counters e = coop::ambient_fixture_sync::Read();
        Check(e.hiveDropped + e.hiveGone == dropped0 + 1, "the branch's end left its nest to the prop lane");
    });
    WaitGT("the nest ended by the prop lane", 30, [&] { return ActorNear(L"prop_waspnest_bee_C", nestAt, 200.f) == nullptr; }, false);
    OnGameThread([&] { Check(ActorNear(L"prop_waspnest_bee_C", nestAt, 200.f) == nullptr, "its nest ended here by the prop lane"); });
    Mark("branch_end_done");
    return true;
}

void RunClient() {
    const bool ran = ClientBody();
    Mark("client_done", std::to_string(ran ? g_fails : g_fails + 1));   // a life that stopped short is no pass
    // The DONE line is the rig's cue to kill this life: said once the host watches slot 1.
    Await("host_ack", 60);
    LeaveForRejoin();
    UE_LOGI("ambient-field: DONE on client fails=%d", g_fails);
}

// The second life: a new load, a new census, the host's branches bound by key again.
void RunRejoin() {
    bool ran = false;
    if (WorldReady()) {
        Mark("rejoin_ready");
        std::string body;
        Audit host;
        if (AwaitText("join_b", body, 240) && Unpack(body, host)) {
            JoinAudit(host);
            ran = true;
        }
    }
    Mark("rejoin_done", std::to_string(ran ? g_fails : g_fails + 1));
    UE_LOGI("ambient-field: REJOIN DONE fails=%d", g_fails);
}

}  // namespace

DWORD WINAPI AmbientFieldThread(LPVOID /*arg*/) {
    if (!PS::Begin("ambient-field")) return 0;
    if (!IsClientRole()) RunHost();
    else if (coop::config::ReadEnv("VOTVCOOP_AMBIENT_FIELD_REJOIN") == "1") RunRejoin();
    else RunClient();
    return 0;
}

}  // namespace harness::autotest
