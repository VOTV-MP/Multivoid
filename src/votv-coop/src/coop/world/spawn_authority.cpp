// coop/world/spawn_authority.cpp -- see the header. The rows rest on these bytecode facts: the
// mushroom master arms one looping spawn timer at begin-play, and its spawn mints spawner children
// with a lifespan, so refused children are reaped by the engine independently of the refused event;
// the mushroom spawner's own looping timer materialises the food cap and self-destroys, and with
// spawn refused the lifespan reaps it; the yellow-wisp ticker spawns at a navmesh random-walk point,
// not around the player, and its product is host-mirrored, so the client must not run its own
// spawner; the sky-wisp ticker spawns the sky wisps at absolute map coordinates, so the host rolls
// and clients mirror through the source-gated catch and the variant allowlist; the roach master's
// summon and its three looping timer entries fire independently of its tick. The tick rows: the
// insomniac and fossilhound tickers roll inside their tick with no delay chains or reap duties, so
// refusing the tick stops the roll and the product, and the products are host-mirrored; the roach
// master's tick drives roach movement, the food-eat mutation and crush traces, which a client running
// it would diverge, so it and its summoner are refused while the roach sync drives the client
// population. The jellyfish path's spawn makes seven fish inside its graph, and the host's are
// mirrored through the source-gated catch, so a client's own, its 18:00 roll's, is refused.

#include "coop/world/spawn_authority.h"

#include "coop/net/session.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"

#include <atomic>
#include <cstdint>
#include <iterator>

namespace coop::spawn_authority {
namespace {

namespace SG = ue_wrap::script_gate;

std::atomic<coop::net::Session*> g_session{nullptr};

// Refuse only while an active client session exists: the running flag flips true in the session
// start and false in the stop, which every disconnect path reaches; a bare role gate would bleed the
// refusal into single player after the session.
bool IsActiveClientSession() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->role() == coop::net::Role::Client;
}

// One row: the body a client must not run, named by its class and its function. The function is
// spelled as the live header dump spells it; names compare without case.
struct Row {
    const wchar_t* cls;
    const wchar_t* fn;
    const char* tag;
};
constexpr Row kRows[] = {
    {L"mushroomMaster_C",            L"Spawn",          "mushroomMaster.Spawn"},
    {L"mushroomSpawner_C",           L"Spawn",          "mushroomSpawner.Spawn"},
    // A late-game class: keyed by name, the watch holds from the class's first body, whenever it loads.
    {L"ticker_yellowWispSpawner_C",  L"ReceiveTick",    "yellowWispSpawner.ReceiveTick"},
    // Sky wisps: world-anchored, so the host rolls; the source-gated catch and the variant
    // allowlist mirror the products.
    {L"ticker_wispSpawner_C",        L"ReceiveTick",    "wispSpawner.ReceiveTick"},
    // The space jellyfish: the host's run is mirrored through the source-gated catch.
    {L"jellyfishPath_C",             L"spawn",          "jellyfishPath.spawn"},
    // The roach sim's entries: the ticker's cross-object call and the three looping timer
    // delegates. The roach sync drives the client population instead.
    {L"cockroachMaster_C",           L"summonRoach",    "cockroachMaster.summonRoach"},
    {L"cockroachMaster_C",           L"addRoachTimer",  "cockroachMaster.addRoachTimer"},
    {L"cockroachMaster_C",           L"spawnNestTimer", "cockroachMaster.spawnNestTimer"},
    {L"cockroachMaster_C",           L"CustomEvent",    "cockroachMaster.CustomEvent"},
    // The ticks.
    {L"ticker_insomniacSpawner_C",   L"ReceiveTick",    "insomniacSpawner.ReceiveTick"},
    {L"ticker_fossilhoundSpawner_C", L"ReceiveTick",    "fossilhoundSpawner.ReceiveTick"},
    {L"cockroachMaster_C",           L"ReceiveTick",    "cockroachMaster.ReceiveTick"},
    {L"ticker_roachSummoner_C",      L"ReceiveTick",    "roachSummoner.ReceiveTick"},
};
constexpr int kRowCount = static_cast<int>(std::size(kRows));
constexpr int kTagBase = 0x53410000;   // 'SA', then the row

// Refusals per row, for the log: the first and then each power of two, so a refused tick proves itself
// without a line a second. Game thread, as the gate's callbacks are.
std::uint64_t g_refused[kRowCount] = {};

SG::Verdict OnRowPre(const SG::Call& call) {
    if (!IsActiveClientSession()) return SG::Verdict::Run;
    const int row = call.tag - kTagBase;
    if (row < 0 || row >= kRowCount) return SG::Verdict::Run;
    const std::uint64_t n = ++g_refused[row];
    if ((n & (n - 1)) == 0)
        UE_LOGI("spawn_authority[%s]: client-refuse %p (call #%llu)", kRows[row].tag, call.object,
                static_cast<unsigned long long>(n));
    return SG::Verdict::Cancel;
}

std::atomic<bool> g_watched{false};

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_watched.exchange(true, std::memory_order_acq_rel)) return;
    int watched = 0;
    for (int i = 0; i < kRowCount; ++i) {
        if (SG::WatchClassName(kRows[i].cls, kRows[i].fn, kTagBase + i, &OnRowPre, nullptr)) {
            ++watched;
        } else {
            UE_LOGE("spawn_authority: the watch on %ls::%ls did not register -- a client runs it",
                    kRows[i].cls, kRows[i].fn);
        }
    }
    UE_LOGI("spawn_authority: %d/%d spawner watches registered by class and name, each live once the game "
            "thread resolves its names; refused only on an active client session", watched, kRowCount);
}

}  // namespace coop::spawn_authority
