// coop/world/weather_redsky.cpp -- see coop/world/weather_redsky.h.

#include "coop/world/weather_redsky.h"

#include "coop/element/element.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/red_sky.h"

#include <atomic>
#include <cstdint>

namespace coop::weather_redsky {
namespace {

namespace GT = ue_wrap::game_thread;
namespace RS = ue_wrap::red_sky;
namespace sg = ue_wrap::script_gate;

UE_FACT_FN(kRowSpawnRedSky, "weather_redsky", L"mainGamemode_C", L"spawnRedSky", Declared);

std::atomic<coop::net::Session*> g_session{nullptr};

// Game thread, but for the session pointer.
constexpr int kSpawnRedSkyTag = 0x52534b54;  // 'RSKT'
bool g_gateAsked = false;  // the watch was registered (or refused, said)
bool g_applying = false;   // the lane's own apply of the host's red sky runs
// This session's counts, said at its end.
uint64_t g_refused = 0;      // a client's own toggles refused
bool     g_saidRefused = false;
uint64_t g_sent = 0;         // the host's red sky sent as a toggle left it
uint64_t g_applied = 0;      // the host's red sky applied here
uint64_t g_applyFailed = 0;  // the host's red sky whose toggle here did not leave this copy's sky as the host's

// The lane's own apply, for the length of one toggle: the gate lets exactly this call run.
struct Applying {
    Applying() { g_applying = true; }
    ~Applying() { g_applying = false; }
    Applying(const Applying&) = delete;
    Applying& operator=(const Applying&) = delete;
};

// CLIENT, before the gamemode's toggle: the host's red sky owns both edges, and only the lane's apply runs. No
// world's load calls the toggle (the save holds no red sky), so a world this client has not announced ready is
// refused too: a clock sample crossing noon there would otherwise start a red sky the host never had.
sg::Verdict OnSpawnRedSkyPre(const sg::Call& /*call*/) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() == coop::net::Role::Host) return sg::Verdict::Run;
    if (g_applying) return sg::Verdict::Run;
    ++g_refused;
    if (!g_saidRefused) {
        g_saidRefused = true;
        UE_LOGI("weather: a client refused its own red sky toggle -- the host's red sky owns both edges (first "
                "refusal; the rest are counted)");
    }
    return sg::Verdict::Cancel;
}

// Whether any client's world is ready. The transport sends a loading client no edge (its pre-world gate), and that
// client gets the seed at its world-ready instead.
bool AnyClientWorldReady(coop::net::Session* s) {
    for (int slot = 1; slot < static_cast<int>(coop::players::kMaxPeers); ++slot)
        if (s->IsSlotWorldReady(slot)) return true;
    return false;
}

// HOST, after the toggle ran: its red sky as the body left it, to every world-ready client.
void OnSpawnRedSkyPost(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    bool red = false;
    if (!RS::Read(call.object, red)) {
        UE_LOGW("weather: the host's red sky toggle ran and its sky did not read -- nothing sent");
        return;
    }
    if (!AnyClientWorldReady(s)) {
        UE_LOGI("weather: the host's red sky toggle ran -- red %d; no client's world is ready, the seed carries it",
                red ? 1 : 0);
        return;
    }
    coop::net::RedSkyPayload p{};
    const coop::element::ElementId self = coop::players::Registry::Get().LocalPlayerElementId();
    p.senderElementId = self == coop::element::kInvalidId ? 0u : self;
    p.state = red ? 1 : 0;
    if (!s->SendReliable(coop::net::ReliableKind::RedSky, &p, sizeof(p))) {
        UE_LOGW("weather: the host's red sky %d was not sent -- SendReliable failed", red ? 1 : 0);
        return;
    }
    ++g_sent;
    UE_LOGI("weather: the host's red sky toggle ran -- sent red %d", red ? 1 : 0);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_gateAsked) return;
    g_gateAsked = true;
    if (!sg::WatchName(kRowSpawnRedSky, kSpawnRedSkyTag, &OnSpawnRedSkyPre, &OnSpawnRedSkyPost))
        UE_LOGE("weather: the script gate refused the watch on mainGamemode_C.spawnRedSky -- the host's red sky does "
                "not cross, and a client's own noon toggle runs");
}

bool LocalRedSkyActive() {
    bool red = false;
    return GT::IsGameThread() && RS::Read(nullptr, red) && red;
}

void Apply(const coop::net::RedSkyPayload& payload) {
    if (!GT::IsGameThread()) {
        UE_LOGW("weather: the host's red sky applied off the game thread -- dropping");
        return;
    }
    // From the host alone: the host's receive edge refuses a client's copy (client_authorship.h).
    const bool want = payload.state != 0;
    bool live = false;
    if (!RS::ReadLive(nullptr, live)) {
        UE_LOGI("weather: the host's red sky %d arrived before this client's gamemode -- its world-ready seed "
                "carries it", want ? 1 : 0);
        return;
    }
    // The toggle's own test: a live event is ended, none is started.
    if (live == want) return;
    bool ran = false;
    {
        Applying scope;
        ran = RS::CallSpawn();
    }
    bool red = !want;
    const bool read = RS::Read(nullptr, red);
    if (ran && read && red == want) {
        ++g_applied;
        UE_LOGI("weather: applied the host's red sky %d", want ? 1 : 0);
    } else if (g_applyFailed++ == 0) {
        UE_LOGW("weather: the host's red sky %d did not apply -- the toggle ran=%d, this copy reads %s (first "
                "failure; the rest are counted, and the next edge tries again)", want ? 1 : 0, ran ? 1 : 0,
                read ? (red ? "red" : "clear") : "nothing");
    }
}

void OnDisconnect() {
    if (g_refused || g_sent || g_applied || g_applyFailed)
        UE_LOGI("weather: red sky session end -- %llu of this client's own toggles refused, %llu sent, %llu applied, "
                "%llu did not apply", static_cast<unsigned long long>(g_refused),
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_applied),
                static_cast<unsigned long long>(g_applyFailed));
    g_refused = g_sent = g_applied = g_applyFailed = 0;
    g_saidRefused = false;
    g_applying = false;
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::weather_redsky
