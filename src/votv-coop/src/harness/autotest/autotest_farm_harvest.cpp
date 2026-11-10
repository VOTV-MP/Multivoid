// harness/autotest/autotest_farm_harvest.cpp -- TEST-ONLY: every farm plant, harvested by each peer.
//
// For each row of the game's list_farmPlants (or the rows VOTVCOOP_FARM_HARVEST_ROWS names, comma separated) the host
// plants two copies beside the players, of the class the row plants, and writes them ripe: grown, watered, the fruit grown,
// a bush's berries at the row's most. The host then takes one through the plant's own action (E, action 4) and the client the
// other; a row that drops nothing is the game's discard, taken with Alt held. Each peer says, for both plants, whether the
// plant still stands and its berry count, and how many of the row's drop items appeared in the world and in its own
// inventory. The host judges each row by its kind:
//   a crop (type 0, a drop): the plant gone on both peers, the harvester's yield inside the row's amount range;
//   a bush (type 1): the berries to none on both peers, and the harvester holding them;
//   a tree (type 2): the game's action takes nothing from a tree (its fruit hangs on it as props), so the row reports what
//                    each peer holds of its fruit and is not judged;
//   a plant that drops nothing: the discard ends it on both peers.
// Lines `farm-harvest: ROW ...` per row and `farm-harvest: VERDICT PASS|FAIL`. Gated by VOTVCOOP_RUN_FARM_HARVEST; the
// peers' shared folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"
#include "harness/autotest/meet.h"
#include "harness/autotest/peer_signal.h"
#include "plant_state_drill.h"   // co-located private header: OnGameThread, ByKey, Narrow, Widen

#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/host_world_settle.h"
#include "ue_wrap/actors/growing_plant.h"
#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/data_table.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace GP = ue_wrap::growing_plant;
namespace R  = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;
namespace E  = ue_wrap::engine;
namespace PS = harness::autotest::peer_signal;
using plant_drill::ByKey;
using plant_drill::Narrow;
using plant_drill::OnGameThread;
using plant_drill::Widen;

constexpr int kStep = 120;
constexpr DWORD kSettleMs = 3000;   // an action's spawns, the host's state and its sends

struct Row {
    std::string name, drop;
    int type = 0, amin = 0, amax = 0;
};

// The table's rows, as the game holds them. Game thread.
std::vector<Row> ReadRows() {
    std::vector<Row> out;
    namespace DT = ue_wrap::data_table;
    void* table = R::FindObject(L"list_farmPlants", L"DataTable");
    void* rs = table ? DT::RowStruct(table) : nullptr;
    std::vector<DT::RowRef> refs;
    if (!rs || !DT::Rows(table, refs, "farm_harvest")) return out;
    const int32_t oType = R::FindPropertyOffsetByPrefix(rs, L"plantType_");
    const int32_t oRange = R::FindPropertyOffsetByPrefix(rs, L"amount_range_");
    const int32_t oDrop = R::FindPropertyOffsetByPrefix(rs, L"drop_");
    if (oType < 0 || oRange < 0 || oDrop < 0) return out;
    for (const auto& r : refs) {
        Row row;
        row.name = Narrow(R::ToString(r.key));
        row.type = r.row[oType];
        int32_t range[2] = {};
        std::memcpy(range, r.row + oRange, sizeof(range));
        row.amin = range[0];
        row.amax = range[1];
        R::FName drop{};
        std::memcpy(&drop, r.row + oDrop, sizeof(drop));
        row.drop = Narrow(R::ToString(drop));
        out.push_back(row);
    }
    return out;
}

// The rows to run: VOTVCOOP_FARM_HARVEST_ROWS when set, else every row.
std::vector<Row> Chosen(const std::vector<Row>& all) {
    char buf[2048] = {};
    if (::GetEnvironmentVariableA("VOTVCOOP_FARM_HARVEST_ROWS", buf, sizeof(buf)) == 0) return all;
    std::vector<Row> out;
    std::stringstream ss(buf);
    std::string want;
    while (std::getline(ss, want, ','))
        for (const Row& r : all)
            if (_stricmp(r.name.c_str(), want.c_str()) == 0) out.push_back(r);
    return out;
}

// The props in the world named `drop`, and the records of that name in this player's inventory. Game thread.
struct Count { int world = 0, inv = 0; };
Count CountDrop(const std::string& drop) {
    Count c;
    if (drop.empty() || drop == "None") return c;
    struct Ctx { std::wstring want; int n; } ctx{Widen(drop), 0};
    OI::ForEachClass([](void* p, void* cls, void* any) {
        if (!any || !ue_wrap::prop::IsDescendantOfProp(any)) return;
        OI::ForEachInstance(cls, [](void* q, void* obj, int32_t) {
            auto* x = static_cast<Ctx*>(q);
            if (R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__") &&
                _wcsicmp(ue_wrap::prop::GetPropNameString(obj).c_str(), x->want.c_str()) == 0)
                ++x->n;
        }, p);
    }, &ctx);
    c.world = ctx.n;
    ue_wrap::inventory::LivePersonalStore store;
    if (ue_wrap::inventory::ReadLivePersonalStore(store))
        for (const auto& rec : store.records) {
            std::wstring name;
            if (ue_wrap::save_record::RecordName(rec, name) && _wcsicmp(name.c_str(), ctx.want.c_str()) == 0) ++c.inv;
        }
    return c;
}

// A plant's state here: "gone", or its berry count. Game thread.
std::string StateOf(const std::string& key) {
    void* p = ByKey(key);
    GP::Values v;
    if (!p) return "gone";
    if (!GP::Read(p, v)) return "unread";
    char b[48];
    std::snprintf(b, sizeof(b), "stands(grow=%.2f,amount=%d)", v.grow, v.amount);
    return b;
}

// The plant's own action, as the player's E runs it, with Alt held for a discard. Game thread.
bool Take(const std::string& key, bool alt) {
    void* p = ByKey(key);
    void* me = coop::players::Registry::Get().Local();
    void* fn = p ? R::FindDispatchFunctionCached(R::ClassOf(p), L"actionOptionIndex") : nullptr;
    if (!fn || !me) return false;
    int32_t off = -1;
    uint8_t mask = 0;
    const bool altRead = R::FindBoolProperty(R::ClassOf(me), L"input_alt", off, mask);
    uint8_t* altByte = altRead ? static_cast<uint8_t*>(me) + off : nullptr;
    const uint8_t was = altByte ? *altByte : 0;
    if (altByte && alt) *altByte = static_cast<uint8_t>(*altByte | mask);
    ue_wrap::ParamFrame f(fn);
    const bool ok = f.valid() && f.Set<void*>(L"player", me) && f.Set<uint8_t>(L"action", 4) && ue_wrap::Call(p, f);
    if (altByte) *altByte = was;
    return ok;
}

// A ripe plant of `row`, beside this player. "" when it could not be made. Game thread.
std::string PlantRipe(const Row& row, float side) {
    void* me = coop::players::Registry::Get().Local();
    void* cls = GP::FarmRowPlantClass(Widen(row.name));
    ue_wrap::FVector at{};
    if (!me || !cls || !E::TryGetActorLocation(me, at)) return "";
    void* p = GP::Spawn(cls, L"", Widen(row.name), L"", true, {at.X + 250.f, at.Y + side, at.Z - 80.f}, {});
    return p ? Narrow(GP::Key(p)) : "";
}

bool WriteRipe(const std::string& key, const Row& row) {
    return PS::WaitFor("the plant to be listed", 15, [&] {
        bool wrote = false;
        OnGameThread([&] {
            void* p = ByKey(key);
            GP::Values v;
            if (!p || !GP::Read(p, v)) return;
            // The stolas herb's ripening strikes it with lightning and ends it unless its noLightning is set (its grown
            // override): set here, so the ripe herb stands to be harvested.
            int32_t off = -1;
            uint8_t mask = 0;
            if (R::FindBoolProperty(R::ClassOf(p), L"noLightning", off, mask))
                *(static_cast<uint8_t*>(p) + off) = static_cast<uint8_t>(*(static_cast<uint8_t*>(p) + off) | mask);
            v.grow = 1.f;
            v.water = 1000.f;
            v.fruitGrow = 1.f;
            v.dryness = 0.f;
            if (row.type == 1) v.amount = row.amax > 0 ? row.amax : 1;
            GP::Apply(p, GP::Row(p), v);
            wrote = true;
        });
        return wrote;
    }, false);
}

std::string Fmt(const char* f, ...) {
    char b[512];
    va_list a;
    va_start(a, f);
    std::vsnprintf(b, sizeof(b), f, a);
    va_end(a);
    return b;
}

// One take on this peer: the counts before, the action, the counts after and the plant's state. "<ran> <state> <dWorld> <dInv>".
std::string TakeHere(const Row& row, const std::string& key) {
    Count before{}, after{};
    bool ran = false;
    OnGameThread([&] { before = CountDrop(row.drop); ran = Take(key, row.amax <= 0); });
    ::Sleep(kSettleMs);
    std::string state;
    OnGameThread([&] { after = CountDrop(row.drop); state = StateOf(key); });
    return Fmt("%d %s %d %d", ran ? 1 : 0, state.c_str(), after.world - before.world, after.inv - before.inv);
}

// What a row's take should leave, judged from both peers' words. "" when it held.
std::string Judge(const Row& row, const std::string& takerState, const std::string& otherState, int dWorld, int dInv,
                  int otherWorld) {
    if (row.type == 2) return "";   // a tree: reported, not judged
    const bool gone = takerState == "gone" && otherState == "gone";
    if (row.amax <= 0) return gone ? "" : "the discard left the plant standing";
    if (row.type == 1) {
        const bool empty = takerState.find("amount=0") != std::string::npos && otherState.find("amount=0") != std::string::npos;
        if (!empty) return "the berries were not taken on both peers";
        if (dInv + dWorld < 1) return "the harvester got no berries";
        return "";
    }
    if (!gone) return "the harvest left the plant standing on a peer";
    const int got = dWorld + dInv;
    if (got < row.amin || got > row.amax) return Fmt("the harvester got %d, outside %d..%d", got, row.amin, row.amax);
    if (dWorld > 0 && otherWorld != dWorld) return Fmt("%d dropped in the harvester's world, %d seen by the other peer", dWorld, otherWorld);
    return "";
}

void RunHost() {
    if (!PS::WaitFor("the host's player", 240, [] {
            void* p = nullptr;
            OnGameThread([&] { p = coop::players::Registry::Get().Local(); });
            return p != nullptr;
        }))
        return;
    if (!PS::WaitFor("the host's world settled", 120, [] {
            bool s = false;
            OnGameThread([&] { s = coop::host_world_settle::Settled() && OI::IsSeeded() && OI::Backlog() == 0; });
            return s;
        }))
        return;
    std::vector<Row> rows;
    if (!PS::WaitFor("list_farmPlants", 60, [&] { OnGameThread([&] { rows = Chosen(ReadRows()); }); return !rows.empty(); }))
        return;
    if (!PS::Await("client_ready", 480)) return;
    PS::Mark("rows", std::to_string(rows.size()));
    int fails = 0, judged = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& row = rows[i];
        std::string a, b;
        OnGameThread([&] { a = PlantRipe(row, -120.f); b = PlantRipe(row, 120.f); });
        if (a.empty() || b.empty() || !WriteRipe(a, row) || !WriteRipe(b, row)) {
            UE_LOGW("farm-harvest: ROW %s -- the host could not plant it", row.name.c_str());
            ++fails;
            PS::Mark(("row_" + std::to_string(i)).c_str(), "skip");
            continue;
        }
        ::Sleep(kSettleMs);   // the ripe state reaches the client, and a fruit plant grows its fruit
        PS::Mark(("row_" + std::to_string(i)).c_str(), a + " " + b);
        if (!PS::Await(("seen_" + std::to_string(i)).c_str(), kStep)) return;
        // The host's take of A.
        const std::string hostWord = TakeHere(row, a);
        Count beforeClient{};
        OnGameThread([&] { beforeClient = CountDrop(row.drop); });
        PS::Mark(("hosttook_" + std::to_string(i)).c_str(), hostWord);
        std::string clientWord;
        if (!PS::AwaitText(("clienttook_" + std::to_string(i)).c_str(), clientWord, kStep)) return;
        ::Sleep(1500);   // the client's state and its drops reach this peer
        std::string bHere;
        Count afterClient{};
        OnGameThread([&] { bHere = StateOf(b); afterClient = CountDrop(row.drop); });
        const int hostSawW = afterClient.world - beforeClient.world;
        // Host take: "<ran> <state> <dWorld> <dInv>"; client word: "<aState> <aWorldSeen> | <ran> <bState> <dWorld> <dInv>".
        int hRan = 0, hdW = 0, hdI = 0, cRan = 0, cdW = 0, cdI = 0, cSawW = 0;
        char hState[64] = {}, cAState[64] = {}, cBState[64] = {};
        std::sscanf(hostWord.c_str(), "%d %63s %d %d", &hRan, hState, &hdW, &hdI);
        std::sscanf(clientWord.c_str(), "%63s %d | %d %63s %d %d", cAState, &cSawW, &cRan, cBState, &cdW, &cdI);
        const std::string hostVerdict = Judge(row, hState, cAState, hdW, hdI, cSawW);
        const std::string clientVerdict = Judge(row, cBState, bHere, cdW, cdI, hostSawW);
        const bool judgedRow = row.type != 2;
        if (judgedRow) ++judged;
        const bool ok = hostVerdict.empty() && clientVerdict.empty() && hRan && cRan;
        if (judgedRow && !ok) ++fails;
        UE_LOGI("farm-harvest: ROW %s type=%d amount=%d..%d drop=%s | host take: ran=%d plant %s on host, %s on client; "
                "+%d world +%d inv, client saw +%d | client take: ran=%d plant %s on client, %s on host; +%d world +%d inv, host saw +%d "
                "-- %s%s%s", row.name.c_str(), row.type, row.amin, row.amax, row.drop.c_str(), hRan, hState, cAState, hdW,
                hdI, cSawW, cRan, cBState, bHere.c_str(), cdW, cdI, hostSawW, !judgedRow ? "REPORTED (a tree)" : ok ? "PASS" : "FAIL",
                hostVerdict.empty() ? "" : (" host: " + hostVerdict).c_str(),
                clientVerdict.empty() ? "" : (" client: " + clientVerdict).c_str());
        OnGameThread([&] {
            for (const std::string& k : {a, b})
                if (void* p = ByKey(k)) GP::Destroy(p);
        });
    }
    UE_LOGI("farm-harvest: VERDICT %s -- %d of %d judged rows failed (%zu rows run)", fails ? "FAIL" : "PASS", fails, judged,
            rows.size());
    PS::Mark("done");
}

void RunClient() {
    if (!PS::WaitFor("the host's puppet", 120, [] {
            bool have = false;
            OnGameThread([&] {
                coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(0);
                have = p && p->valid();
            });
            return have;
        }))
        return;
    if (!meet::WalkToHost("farm-harvest")) return;
    std::vector<Row> rows;
    if (!PS::WaitFor("list_farmPlants", 60, [&] { OnGameThread([&] { rows = Chosen(ReadRows()); }); return !rows.empty(); }))
        return;
    PS::Mark("client_ready");
    std::string n;
    if (!PS::AwaitText("rows", n, 480)) return;
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& row = rows[i];
        std::string keys;
        if (!PS::AwaitText(("row_" + std::to_string(i)).c_str(), keys, kStep)) return;
        if (keys == "skip") continue;
        std::istringstream in(keys);
        std::string a, b;
        in >> a >> b;
        PS::WaitFor("the ripe copies", 15, [&] {
            bool ok = false;
            OnGameThread([&] {
                void* pa = ByKey(a);
                void* pb = ByKey(b);
                GP::Values va, vb;
                ok = pa && pb && GP::Read(pa, va) && GP::Read(pb, vb) && va.grow >= 1.f && vb.grow >= 1.f;
            });
            return ok;
        }, false);
        Count before{};
        OnGameThread([&] { before = CountDrop(row.drop); });
        PS::Mark(("seen_" + std::to_string(i)).c_str());
        std::string hostWord;
        if (!PS::AwaitText(("hosttook_" + std::to_string(i)).c_str(), hostWord, kStep)) return;
        ::Sleep(1500);   // the host's state and its drops reach this peer
        std::string aState;
        Count after{};
        OnGameThread([&] { aState = StateOf(a); after = CountDrop(row.drop); });
        const std::string mine = TakeHere(row, b);
        PS::Mark(("clienttook_" + std::to_string(i)).c_str(),
                 Fmt("%s %d | %s", aState.c_str(), after.world - before.world, mine.c_str()));
    }
    PS::Await("done", 600);
    UE_LOGI("farm-harvest: DONE on client");
}

}  // namespace

DWORD WINAPI FarmHarvestThread(LPVOID /*arg*/) {
    if (!PS::Begin("farm-harvest")) return 0;
    if (IsClientRole()) RunClient();
    else RunHost();
    return 0;
}

}  // namespace harness::autotest
