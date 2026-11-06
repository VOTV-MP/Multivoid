// harness/autotest/autotest_can_state.cpp -- TEST-ONLY: a canned food's lid and contents across two peers.
//
// The client meets the host, then the host spawns two cans and names them in a folder both peers read. Can A: the host
// runs Open, the client runs eat. Can B: the client runs Open, the host runs eat. Each step is one peer's reflected call
// of the can's own verb; each peer then waits for the can on ITS copy to differ from the step before and logs
// `can-drill: STATE <step> <can> opened= foods= type= mesh=`. The client hands its rows to the host, which compares each
// step's opened, foods and mesh with its own, and the records each peer published at the step (exactly one on the peer
// that changed the can, none on the other), and logs `can-drill: VERDICT PASS|FAIL`, then `can-drill: DONE`. A file per
// step orders the two peers. Gated by VOTVCOOP_RUN_CAN_STATE; the folder is VOTVCOOP_CAN_DIR. Every wait is on a game
// state or the other peer's signal, with a bound, never on a clock.

#include "harness/autotest.h"
#include "harness/autotest/meet.h"
#include "harness/autotest/peer_signal.h"

#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/join_membership_sweep.h"
#include "coop/props/prop_can_state.h"
#include "coop/props/prop_key_index.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace PS = harness::autotest::peer_signal;
using PS::Await;
using PS::AwaitText;
using PS::Mark;
using PS::WaitFor;

constexpr int kStepBoundSec = 30;   // a verb's effect, or the other peer's record, on this peer's copy

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

// One can as a peer reads it.
struct Row {
    bool        found = false;
    int         opened = 0;
    int32_t     foods = 0;
    int32_t     type = 0;
    std::string mesh = "?";
    unsigned    published = 0;   // records this peer published since the step before
    unsigned    landed = 0;      // records that landed on this peer since the step before
};

bool SameState(const Row& a, const Row& b) {
    return a.found == b.found && a.opened == b.opened && a.foods == b.foods && a.mesh == b.mesh;
}

// A step's row, as the peers log and compare it.
struct StepRow {
    std::string step, can;
    Row         row;
};
std::vector<StepRow> g_rows;

std::string Narrow(const std::wstring& w) { return std::string(w.begin(), w.end()); }

void* Find(const std::string& key) {
    void* a = nullptr;
    const std::wstring w(key.begin(), key.end());
    OnGameThread([&] { a = coop::prop_key_index::Find(w); });
    return a;
}

Row ReadRow(const std::string& key) {
    Row r;
    OnGameThread([&] {
        const std::wstring w(key.begin(), key.end());
        void* a = coop::prop_key_index::Find(w);
        if (!a) return;
        void* const cls = R::ClassOf(a);
        const auto* base = static_cast<const uint8_t*>(a);
        int32_t openedOff = -1;
        uint8_t openedMask = 0;
        const int32_t foodsOff = R::FindPropertyOffset(cls, L"foods");
        const int32_t typeOff = R::FindPropertyOffset(cls, L"Type");
        if (!R::FindBoolProperty(cls, L"opened", openedOff, openedMask) || foodsOff < 0 || typeOff < 0) return;
        void* mesh = ue_wrap::prop::GetStaticMesh(a);
        void* asset = nullptr;
        if (mesh) {
            const int32_t off = R::FindPropertyOffset(R::ClassOf(mesh), L"StaticMesh");
            if (off >= 0) asset = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(mesh) + off);
        }
        r.found = true;
        r.opened = (base[openedOff] & openedMask) ? 1 : 0;
        r.foods = *reinterpret_cast<const int32_t*>(base + foodsOff);
        r.type = *reinterpret_cast<const int32_t*>(base + typeOff);
        if (asset) r.mesh = Narrow(R::ToString(R::NameOf(asset)));
    });
    return r;
}

coop::props::prop_can_state::Counts g_counted;   // the counters at the step before

// Logs the can's state for the step, keeps the row for the verdict, and returns it. The row carries the records this
// peer published and received since the previous row: the step's own, since a step is one peer's one change.
Row LogState(const char* step, const char* can, const std::string& key) {
    Row r = ReadRow(key);
    if (!r.found) {
        UE_LOGW("can-drill: STATE %hs %hs -- no such can here", step, can);
        return r;
    }
    coop::props::prop_can_state::Counts now;
    OnGameThread([&] { now = coop::props::prop_can_state::CountsNow(); });
    r.published = now.published - g_counted.published;
    r.landed = now.landed - g_counted.landed;
    g_counted = now;
    UE_LOGI("can-drill: STATE %hs %hs opened=%d foods=%d type=%d mesh=%hs published=%u landed=%u", step, can, r.opened,
            r.foods, r.type, r.mesh.c_str(), r.published, r.landed);
    g_rows.push_back({step, can, r});
    return r;
}

// The can on this peer's copy has left the state `before`: the step's effect is here, whether the peer ran the verb or
// the other peer's record arrived. A timeout is an ABORT line.
bool WaitChanged(const char* what, const std::string& key, const Row& before) {
    return WaitFor(what, kStepBoundSec, [&] {
        const Row now = ReadRow(key);
        return now.found && !SameState(now, before);
    });
}

// The can's own verb by reflection, with this peer's player where the verb takes one.
void Verb(const std::string& key, const wchar_t* name) {
    void* a = Find(key);
    OnGameThread([&] {
        void* fn = a ? R::FindDispatchFunctionCached(R::ClassOf(a), name) : nullptr;
        if (!fn) {
            UE_LOGW("can-drill: %ls did not resolve", name);
            return;
        }
        ue_wrap::ParamFrame f(fn);
        if (R::FindParamOffset(fn, L"player") >= 0) f.Set<void*>(L"player", coop::players::Registry::Get().Local());
        const bool ok = ue_wrap::Call(a, f);
        UE_LOGI("can-drill: %ls ran on the can (dispatched=%d)", name, ok ? 1 : 0);
    });
}

bool BothCansHere(const std::string& keyA, const std::string& keyB) {
    return WaitFor("both cans here", kStepBoundSec, [&] { return Find(keyA) && Find(keyB); });
}

// The rows one peer saw, as one line: `step can opened foods published mesh`, rows apart by ';'.
std::string RowsText() {
    std::string out;
    for (const StepRow& s : g_rows) {
        if (!out.empty()) out += ';';
        out += s.step + ' ' + s.can + ' ' + std::to_string(s.row.opened) + ' ' + std::to_string(s.row.foods) + ' ' +
               std::to_string(s.row.published) + ' ' + s.row.mesh;
    }
    return out;
}

std::vector<StepRow> ParseRows(const std::string& text) {
    std::vector<StepRow> out;
    std::stringstream rows(text);
    std::string one;
    while (std::getline(rows, one, ';')) {
        std::stringstream f(one);
        StepRow s;
        s.row.found = true;
        if (f >> s.step >> s.can >> s.row.opened >> s.row.foods >> s.row.published >> s.row.mesh) out.push_back(s);
    }
    return out;
}

std::string Describe(const Row& r) {
    return "opened=" + std::to_string(r.opened) + " foods=" + std::to_string(r.foods) + " mesh=" + r.mesh +
           " published=" + std::to_string(r.published);
}

// Which peer changed the can at a step: 'h' the host, 'c' the client, 0 nobody (the cans' birth).
char ChangedBy(const std::string& step) {
    if (step == "1-host-opened" || step == "4-host-ate") return 'h';
    if (step == "2-client-ate" || step == "3-client-opened") return 'c';
    return 0;
}

// VOTVCOOP_CAN_RED_RECORDS=1: the count the host expects is one too many, so the verdict must fail -- a count that
// cannot fail proves nothing.
unsigned RedExtra() {
    char buf[8] = {};
    const DWORD n = ::GetEnvironmentVariableA("VOTVCOOP_CAN_RED_RECORDS", buf, sizeof(buf));
    return n > 0 && buf[0] == '1' ? 1u : 0u;
}

// One change is one record: the peer that changed the can published exactly one at the step, the other none.
bool RecordsOk(const StepRow& host, const StepRow& client) {
    const char by = ChangedBy(host.step);
    const unsigned red = RedExtra();
    const unsigned wantHost = (by == 'h' ? 1u : 0u) + red, wantClient = (by == 'c' ? 1u : 0u) + red;
    if (host.row.published == wantHost && client.row.published == wantClient) return true;
    UE_LOGW("can-drill: VERDICT FAIL records %hs/%hs published host=%u (want %u) client=%u (want %u)", host.step.c_str(),
            host.can.c_str(), host.row.published, wantHost, client.row.published, wantClient);
    return false;
}

// Every step the host logged must be in the client's rows with the same opened, foods and mesh, and cost one record.
void Judge(const std::string& clientText) {
    const std::vector<StepRow> client = ParseRows(clientText);
    for (const StepRow& h : g_rows) {
        const StepRow* c = nullptr;
        for (const StepRow& s : client)
            if (s.step == h.step && s.can == h.can) c = &s;
        if (!c) {
            UE_LOGW("can-drill: VERDICT FAIL %hs/%hs host=%hs client=none", h.step.c_str(), h.can.c_str(),
                    Describe(h.row).c_str());
            return;
        }
        if (!SameState(h.row, c->row)) {
            UE_LOGW("can-drill: VERDICT FAIL %hs/%hs host=%hs client=%hs", h.step.c_str(), h.can.c_str(),
                    Describe(h.row).c_str(), Describe(c->row).c_str());
            return;
        }
        if (!RecordsOk(h, *c)) return;
    }
    if (g_rows.size() != client.size()) {
        UE_LOGW("can-drill: VERDICT FAIL rows host=%zu client=%zu", g_rows.size(), client.size());
        return;
    }
    UE_LOGI("can-drill: VERDICT PASS");
}

void RunHost() {
    if (!Await("client_ready", 400)) return;
    std::string keyA, keyB;
    OnGameThread([&] {
        void* cls = ue_wrap::asset_load::LoadObjectByPath(L"/Game/objects/prop_cannedFood_0.prop_cannedFood_0_C");
        void* local = coop::players::Registry::Get().Local();
        ue_wrap::FVector me{};
        if (!cls || !local || !E::TryGetActorLocation(local, me)) return;
        void* a = E::SpawnActor(cls, {me.X + 120.f, me.Y, me.Z + 40.f});
        void* b = E::SpawnActor(cls, {me.X + 120.f, me.Y + 80.f, me.Z + 40.f});
        const std::wstring ka = a ? ue_wrap::prop::GetInteractableKeyString(a) : L"";
        const std::wstring kb = b ? ue_wrap::prop::GetInteractableKeyString(b) : L"";
        keyA.assign(ka.begin(), ka.end());
        keyB.assign(kb.begin(), kb.end());
    });
    if (keyA.empty() || keyB.empty()) {
        UE_LOGW("can-drill: VERDICT ABORT -- the cans did not spawn");
        return;
    }
    if (!BothCansHere(keyA, keyB)) return;
    const Row bornA = LogState("0-born", "A", keyA);
    const Row bornB = LogState("0-born", "B", keyB);
    Mark("keys", keyA + " " + keyB);
    if (!Await("client_saw_born")) return;
    Verb(keyA, L"Open");   // A: the host opens
    if (!WaitChanged("the host's open on can A", keyA, bornA)) return;
    const Row openedA = LogState("1-host-opened", "A", keyA);
    Mark("host_opened");
    if (!Await("client_ate")) return;   // A: the client eats
    if (!WaitChanged("the client's eat on can A", keyA, openedA)) return;
    LogState("2-client-ate", "A", keyA);
    Mark("host_saw_eat");
    if (!Await("client_opened")) return;   // B: the client opens
    if (!WaitChanged("the client's open on can B", keyB, bornB)) return;
    const Row openedB = LogState("3-client-opened", "B", keyB);
    Verb(keyB, L"eat");   // B: the host eats
    if (!WaitChanged("the host's eat on can B", keyB, openedB)) return;
    LogState("4-host-ate", "B", keyB);
    Mark("host_ate");
    std::string clientRows;
    if (!AwaitText("client_rows", clientRows)) return;
    Judge(clientRows);
    UE_LOGI("can-drill: DONE");   // after the verdict: a run ends on DONE
}

// The world is up, the host's puppet is here, the join's load tail has quiesced (a client's change before it is not
// published, as prop_can_state holds), and the client stands beside the host, where the cans are spawned.
bool ClientReady() {
    if (!WaitFor("the world announced", 240, [&] {
            bool ready = false;
            OnGameThread([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
            return ready;
        }))
        return false;
    if (!WaitFor("the host's puppet", 60, [&] {
            bool have = false;
            OnGameThread([&] {
                coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(0);
                have = p && p->valid();
            });
            return have;
        }))
        return false;
    if (!WaitFor("the join's load tail to quiesce", 180, [&] {
            bool quiet = false;
            OnGameThread([&] { quiet = coop::join_membership_sweep::HasLoadTailQuiesced(); });
            return quiet;
        }))
        return false;
    return meet::WalkToHost("can-drill");
}

void RunClient() {
    if (!ClientReady()) return;
    Mark("client_ready");
    std::string keys;
    if (!AwaitText("keys", keys)) return;
    const size_t sp = keys.find(' ');
    const std::string keyA = keys.substr(0, sp), keyB = keys.substr(sp + 1);
    if (!BothCansHere(keyA, keyB)) return;
    const Row bornA = LogState("0-born", "A", keyA);
    const Row bornB = LogState("0-born", "B", keyB);
    Mark("client_saw_born");
    if (!Await("host_opened")) return;
    if (!WaitChanged("the host's open on can A", keyA, bornA)) return;
    const Row openedA = LogState("1-host-opened", "A", keyA);
    Verb(keyA, L"eat");
    if (!WaitChanged("the client's eat on can A", keyA, openedA)) return;
    LogState("2-client-ate", "A", keyA);
    Mark("client_ate");
    if (!Await("host_saw_eat")) return;
    Verb(keyB, L"Open");
    if (!WaitChanged("the client's open on can B", keyB, bornB)) return;
    const Row openedB = LogState("3-client-opened", "B", keyB);
    Mark("client_opened");
    if (!Await("host_ate")) return;
    if (!WaitChanged("the host's eat on can B", keyB, openedB)) return;
    LogState("4-host-ate", "B", keyB);
    Mark("client_rows", RowsText());
    UE_LOGI("can-drill: DONE");
}

}  // namespace

DWORD WINAPI CanStateThread(LPVOID /*arg*/) {
    if (!PS::Begin("can-drill")) return 0;
    // The step counters start from this drill, not from the session: a can published before it began is not a step's.
    OnGameThread([&] { g_counted = coop::props::prop_can_state::CountsNow(); });
    if (IsClientRole()) RunClient();
    else                RunHost();
    return 0;
}

}  // namespace harness::autotest
