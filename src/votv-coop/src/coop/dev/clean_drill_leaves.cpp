// coop/dev/clean_drill_leaves.cpp -- the clean drill's `leaves` arm; see coop/dev/clean_drill.h.
//
// The wind's timer has every autumnLeafSpawner_C spawn a fallen-leaves decal at a random spot on each machine, and the spill lane
// makes the host's the world's and removes a client's own. The arm runs that spawn on every peer at once (spawnLeaves, the
// spawner's own event, on every spawner, until the peer has a few new leaves), and each peer decides on its own events:
//   the HOST waits until the lane has taken what the hook saw of its leaves, says FAIL when the lane sent fewer than the host
//   grew, and marks where its leaves stand.
//   each CLIENT waits for the same moment on its side, then fails if a leaf its own calls made still stands; else it waits
//   until a leaf stands at each of the host's places.
// The host says PASS when every client's copy holds the host's leaves and none of its own. Three peers (mp.py --clients 2).

#include "clean_drill_detail.h"   // co-located private header (src tree, not include/)

#include "coop/config/config.h"
#include "coop/net/session.h"
#include "coop/world/spill_sync.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_set>
#include <vector>

namespace coop::dev::clean_drill::detail {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
using coop::rig_verdict::Outcome;

constexpr const char* kTag = "grime-leaves";
constexpr const wchar_t* kLeaf = L"grime_fallenLeaves_C";
constexpr int kMaxRounds = 60;    // rounds of spawnLeaves on every spawner (a spawn is a trace that may miss)
constexpr size_t kWant = 3;       // a peer stops when its calls made this many new leaves
constexpr size_t kMarked = 6;     // the host's leaves marked for the clients

enum class Lv { Arrive, Spawn, Settle, Match, AwaitResults, AwaitEnd, Done };
Lv g_lv = Lv::Arrive;

struct Own {
    void* actor;
    int32_t idx;
    FVector at;
};
std::vector<Own> g_own;     // the leaves this peer's own calls made
uint32_t g_sent0 = 0;       // the lane's sent count before the calls
std::vector<FVector> g_hostLeaves;

void Go(Lv p, const char* name) {
    g_lv = p;
    Enter(name);
}

std::unordered_set<void*> LeafSet() {
    std::unordered_set<void*> set;
    for (void* d : All(kLeaf)) set.insert(d);
    return set;
}

// The leaf decals that stand now and did not in `before`.
void CollectNew(const std::unordered_set<void*>& before) {
    g_own.clear();
    for (void* d : All(kLeaf)) {
        FVector at{};
        if (before.count(d) == 0 && E::TryGetActorLocation(d, at)) g_own.push_back(Own{d, R::InternalIndexOf(d), at});
    }
}

// The spawner's own event on the spawners, round after round, until this peer holds kWant new leaves: the leaves its calls made.
void SpawnLeaves() {
    g_own.clear();
    const std::unordered_set<void*> before = LeafSet();
    const std::vector<void*> spawners = All(L"autumnLeafSpawner_C");
    g_sent0 = coop::spill_sync::Leaves().sent;
    for (int r = 0; r < kMaxRounds && g_own.size() < kWant; ++r)
        for (void* sp : spawners) {
            CallNoArgs(sp, L"spawnLeaves");
            CollectNew(before);
            if (g_own.size() >= kWant) break;
        }
    UE_LOGI("[CLN] %zu spawner(s) made %zu new fallen leaf decal(s) here", spawners.size(), g_own.size());
}

std::string PackHostLeaves() {
    std::string s;
    char buf[80];
    for (size_t i = 0; i < g_own.size() && i < kMarked; ++i) {
        std::snprintf(buf, sizeof(buf), "%.3f,%.3f,%.3f;", g_own[i].at.X, g_own[i].at.Y, g_own[i].at.Z);
        s += buf;
    }
    return s;
}

bool UnpackHostLeaves(const std::string& text) {
    g_hostLeaves.clear();
    for (const std::string& item : Split(text, ';')) {
        if (item.empty()) continue;
        const std::vector<std::string> f = Split(item, ',');
        float x = 0.f, y = 0.f, z = 0.f;
        if (f.size() != 3 || !ToFloat(f[0], x) || !ToFloat(f[1], y) || !ToFloat(f[2], z)) return false;
        g_hostLeaves.push_back({x, y, z});
    }
    return !g_hostLeaves.empty();
}

}  // namespace

void TickLeaves(coop::net::Session& s) {
    if (g_lv == Lv::Done) return;
    if (g_lv != Lv::Arrive && Over() && g_lv < Lv::AwaitEnd) g_lv = Lv::AwaitEnd;   // another peer decided: stop the steps
    if (g_lv > Lv::Arrive && Overdue()) return;
    const bool host = IsHost();
    std::string text;

    switch (g_lv) {
    case Lv::Arrive: {
        // The lane's hook is installed and the world has a spawner to ask.
        const bool ready = coop::spill_sync::Leaves().watching && !All(L"autumnLeafSpawner_C").empty();
        const int a = Arrive(s, kTag, "lv_up", ready);
        if (a < 0) { g_lv = Lv::Done; return; }
        if (a == 0) return;
        Go(Lv::Spawn, "the leaf spawn");
        return;
    }
    case Lv::Spawn:
        SpawnLeaves();
        if (g_own.empty()) {
            Decide(Outcome::Invalid, "%d round(s) of spawnLeaves made no fallen leaf on the %s", kMaxRounds,
                   SlotName(MySlot()));
            g_lv = Lv::AwaitEnd;
            return;
        }
        Go(Lv::Settle, "the spill lane taking the leaves");
        return;
    case Lv::Settle: {
        // The lane drains what its hook saw in one tick: with nothing pending it has taken them all, or never saw them.
        if (coop::spill_sync::Leaves().pending != 0) return;
        {
            const coop::spill_sync::LeafTally lane = coop::spill_sync::Leaves();
            UE_LOGI("[CLN] the lane's most fallen leaves pending at once: %u (%u dropped past its cap)", lane.peak,
                    lane.dropped);
        }
        if (host) {
            const uint32_t taken = coop::spill_sync::Leaves().sent - g_sent0;
            if (taken < g_own.size()) {
                Decide(Outcome::Fail, "the spill lane sent %u of the %zu fallen leaves the host grew: the others are the host's "
                                      "and no peer holds them", taken, g_own.size());
                g_lv = Lv::AwaitEnd;
                return;
            }
            MarkShared("lv_host", PackHostLeaves());
            UE_LOGI("[CLN] the host grew %zu fallen leaves and the lane sent %u", g_own.size(), taken);
            Go(Lv::AwaitResults, "the clients' results");
            return;
        }
        std::string standing;
        int stood = 0;
        for (const Own& o : g_own) {
            if (!R::IsLiveByIndex(o.actor, o.idx)) continue;
            char buf[96];
            std::snprintf(buf, sizeof(buf), "(%.0f,%.0f,%.0f) ", o.at.X, o.at.Y, o.at.Z);
            standing += buf;
            ++stood;
        }
        if (stood > 0) {
            MarkMine("lv_res", "F " + std::to_string(stood) + " fallen leaf(s) of its own stand");
            Decide(Outcome::Fail, "%d of the %zu fallen leaves the %s grew stand on it alone: %s", stood, g_own.size(),
                   SlotName(MySlot()), standing.c_str());
            g_lv = Lv::AwaitEnd;
            return;
        }
        UE_LOGI("[CLN] the %s's own %zu fallen leaf(s) are gone", SlotName(MySlot()), g_own.size());
        Go(Lv::Match, "the host's leaves");
        return;
    }
    case Lv::Match: {
        if (g_hostLeaves.empty()) {
            if (!PeekShared("lv_host", text)) return;
            if (!UnpackHostLeaves(text)) {
                Decide(Outcome::Invalid, "the host's leaves could not be read: '%s'", text.c_str());
                g_lv = Lv::AwaitEnd;
                return;
            }
        }
        for (const FVector& at : g_hostLeaves)
            if (!DecalAt(kLeaf, at)) return;   // arrives on the wire; the phase's bound says INVALID if it never does
        UE_LOGI("[CLN] every one of the host's %zu fallen leaf(s) stands on the %s at its place", g_hostLeaves.size(),
                SlotName(MySlot()));
        MarkMine("lv_res", "P");
        Go(Lv::AwaitEnd, "the verdict");
        return;
    }
    case Lv::AwaitResults:
        for (int slot = 1; slot < kPeers; ++slot) {
            if (!PeekOf("lv_res", slot, text)) return;
            if (text[0] == 'F') {
                Decide(Outcome::Fail, "the %s: %s", SlotName(slot), text.c_str() + 2);
                g_lv = Lv::AwaitEnd;
                return;
            }
        }
        Decide(Outcome::Pass, "the host's %zu fallen leaf(s) stand on every client at the host's places, and none of a client's "
                              "own stands", g_own.size());
        g_lv = Lv::AwaitEnd;
        return;
    case Lv::AwaitEnd:
        if (!Over()) return;
        g_lv = Lv::Done;
        UE_LOGI("[CLN] DONE on the %s", SlotName(MySlot()));
        return;
    case Lv::Done:
        return;
    }
}

// ---- the `leaves_join` arm --------------------------------------------------------------------------------------------
// The late-join answer for the fallen leaves: a client that joins after they fell holds the host's at the host's places.
// Two peers, the host and the first client, in two lives of the client (mp.py --rejoin with --rejoin-marker "life 1 holds",
// and the second life run with the `leaves_rejoin` row):
//   life 1: the host grows its leaves as the `leaves` arm does (the lane must take them all) and marks where they stand,
//   again every so often, for the client to come. The client checks it holds them and says so; the rig then ends it.
//   life 2: the client joins the same world, which the host's replay fills; it needs no other peer's mark, only the host's
//   fresh one, and says once a leaf stands at each of the host's places. The host says PASS on that.
namespace {
enum class Lj { Arrive, Spawn, Settle, Serve, Match, AwaitEnd, Done };
Lj g_lj = Lj::Arrive;
uint32_t g_serveTicks = 0;

void GoJ(Lj p, const char* name) {
    g_lj = p;
    Enter(name);
}
}  // namespace

void TickLeavesJoin(coop::net::Session& s) {
    static const bool second = ::coop::config::ResolveFlag(::coop::config_registry::rows::leaves_rejoin);
    constexpr const char* kJoinTag = "grime-leaves-join";
    if (g_lj == Lj::Done) return;
    if (g_lj != Lj::Arrive && Over() && g_lj < Lj::AwaitEnd) g_lj = Lj::AwaitEnd;
    if (g_lj > Lj::Arrive && Overdue()) return;
    const bool host = IsHost();
    std::string text;

    switch (g_lj) {
    case Lj::Arrive: {
        SetPeers(2);
        const bool ready = coop::spill_sync::Leaves().watching && !All(L"autumnLeafSpawner_C").empty();
        const int a = Arrive(s, kJoinTag, "lj_up", ready, /*barrier=*/!second);
        if (a < 0) { g_lj = Lj::Done; return; }
        if (a == 0) return;
        GoJ(host ? Lj::Spawn : Lj::Match, host ? "the leaf spawn" : "the host's leaves");
        return;
    }
    case Lj::Spawn:
        SpawnLeaves();
        if (g_own.empty()) {
            Decide(Outcome::Invalid, "%d round(s) of spawnLeaves made no fallen leaf on the host", kMaxRounds);
            g_lj = Lj::AwaitEnd;
            return;
        }
        GoJ(Lj::Settle, "the spill lane taking the leaves");
        return;
    case Lj::Settle: {
        if (coop::spill_sync::Leaves().pending != 0) return;
        const uint32_t taken = coop::spill_sync::Leaves().sent - g_sent0;
        if (taken < g_own.size()) {
            Decide(Outcome::Fail, "the spill lane sent %u of the %zu fallen leaves the host grew", taken, g_own.size());
            g_lj = Lj::AwaitEnd;
            return;
        }
        UE_LOGI("[CLN] the host grew %zu fallen leaves and the lane sent %u: waiting for a client that joins after", g_own.size(),
                taken);
        GoJ(Lj::Serve, "the rejoined client's result");
        return;
    }
    case Lj::Serve:
        // The mark is written again every so often: a client that joins later reads only a mark written after its start.
        if (g_serveTicks++ % 30 == 0) MarkShared("lv_host", PackHostLeaves());
        if (PeekOf("lj_res", 1, text)) {
            Decide(Outcome::Pass, "the client that joined after the host's %zu fallen leaves fell holds them at the host's "
                                  "places", g_own.size());
            g_lj = Lj::AwaitEnd;
        }
        return;
    case Lj::Match: {
        if (g_hostLeaves.empty()) {
            if (!PeekShared("lv_host", text)) return;
            if (!UnpackHostLeaves(text)) {
                Decide(Outcome::Invalid, "the host's leaves could not be read: '%s'", text.c_str());
                g_lj = Lj::AwaitEnd;
                return;
            }
        }
        for (const FVector& at : g_hostLeaves)
            if (!DecalAt(kLeaf, at)) return;   // the host's replay brings them; the phase's bound says INVALID if it never does
        UE_LOGI("[CLN] leaves-join: life %d holds the host's %zu fallen leaf(s) at their places", second ? 2 : 1,
                g_hostLeaves.size());
        MarkMine(second ? "lj_res" : "lj_life1", "P");
        GoJ(Lj::AwaitEnd, "the verdict");
        return;
    }
    case Lj::AwaitEnd:
        if (!Over()) return;
        g_lj = Lj::Done;
        UE_LOGI("[CLN] DONE on the %s", SlotName(MySlot()));
        return;
    case Lj::Done:
        return;
    }
}

}  // namespace coop::dev::clean_drill::detail
