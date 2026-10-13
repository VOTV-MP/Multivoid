// coop/dev/laptop_power_drill.cpp -- see coop/dev/laptop_power_drill.h.

#include "coop/dev/laptop_power_drill.h"

#include "coop/config/config.h"
#include "coop/interactables/laptop_sync.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady

#include "ue_wrap/core/log.h"
#include "ue_wrap/devices/laptop.h"

#include <chrono>
#include <cstdint>

namespace coop::dev::laptop_power_drill {
namespace {

namespace L  = ue_wrap::laptop;
namespace LS = coop::laptop_sync;
using Clock = std::chrono::steady_clock;

constexpr auto kStepBound = std::chrono::seconds(60);  // a toggle runs 4.5 to 7 s; a failure bound, never a wait

enum class ClientStep : uint8_t { Ready, Pressed, WaitHost, Pressed2, WaitHost2, Done };
enum class HostStep : uint8_t { Ready, WaitClient, Pressed, WaitClient2, Pressed2, WaitCrossing, Done };

ClientStep        g_client = ClientStep::Ready;
HostStep          g_host = HostStep::Ready;
Clock::time_point g_stepAt{};       // the current step's start; epoch until the first step is entered
bool              g_v0 = false;     // this peer's laptop at its start
uint32_t          g_ownBase = 0;    // the edges this peer had broadcast as its own at its start
uint32_t          g_appliedBase = 0;  // host: the client's edges it had applied at its start
int               g_slot = -1;      // host: the client it watches

bool Enabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::laptop_power_drill);
    return s;
}

void Enter(ClientStep c) { g_client = c; g_stepAt = Clock::now(); }
void Enter(HostStep h) { g_host = h; g_stepAt = Clock::now(); }
bool Expired() { return g_stepAt != Clock::time_point{} && Clock::now() - g_stepAt >= kStepBound; }
uint32_t Own() { return LS::OrganicEdgesSent() - g_ownBase; }

bool Powered() {
    L::PowerState ps;
    return L::EnsureResolved() && L::ReadPower(ps) && ps.powered;
}

// The blueprint's own press; true when its toggle started.
bool Press() {
    L::PowerState ps;
    return L::CallPowerToggle() && L::ReadPower(ps) && ps.anim;
}

// A power edge sent as the wire carries it, with no toggle here: the client's last two stand for two players' edges
// crossing, the second setting the host's target back while the first one's toggle still runs.
bool SendEdge(coop::net::Session& s, bool opened) {
    coop::net::LaptopStatePayload p{};
    p.op = 0;
    p.isOpened = opened ? 1 : 0;
    return s.SendReliableToSlot(0, coop::net::ReliableKind::LaptopState, &p, sizeof(p));
}

// Settled on `want`: no toggle runs, no target is pending and the lane's poll has seen the state.
bool SettledOn(bool want) {
    bool opened = false;
    return LS::Settled(opened) && opened == want;
}

// The first settled state with power, once this peer can start. A laptop that never settles in the step's bound, or
// settles with no power, abandons the drill.
enum class Begin : uint8_t { Wait, Go, Abandon };
Begin Start(const char* who) {
    if (g_stepAt == Clock::time_point{}) g_stepAt = Clock::now();
    bool opened = false;
    if (!LS::Settled(opened)) {
        if (!Expired()) return Begin::Wait;
        UE_LOGW("[LAPTOP-POWER-DRILL] ABANDONED: the %s's laptop did not settle within 60 s", who);
        return Begin::Abandon;
    }
    if (!Powered()) {
        UE_LOGW("[LAPTOP-POWER-DRILL] ABANDONED: the %s's laptop has no power, so its toggle cannot run", who);
        return Begin::Abandon;
    }
    g_v0 = opened;
    g_ownBase = LS::OrganicEdgesSent();
    return Begin::Go;
}

void ClientTick(coop::net::Session& s) {
    switch (g_client) {
    case ClientStep::Ready:
        if (!coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
            return;
        if (const Begin b = Start("client"); b != Begin::Go) {
            if (b == Begin::Abandon) Enter(ClientStep::Done);
            return;
        }
        if (!Press()) {
            UE_LOGW("[LAPTOP-POWER-DRILL] ABANDONED: the client's press did not start its laptop's toggle");
            Enter(ClientStep::Done);
            return;
        }
        UE_LOGI("[LAPTOP-POWER-DRILL] client pressed its laptop (from isOpened=%d)", g_v0 ? 1 : 0);
        Enter(ClientStep::Pressed);
        return;
    case ClientStep::Pressed:
        if (!SettledOn(!g_v0)) {
            if (Expired()) {
                UE_LOGW("[LAPTOP-POWER-DRILL] FAIL: the client's press did not settle within 60 s");
                Enter(ClientStep::Done);
            }
            return;
        }
        if (Own() != 1) {
            UE_LOGW("[LAPTOP-POWER-DRILL] FAIL: the client sent %u edge(s) as its own for its one press", Own());
            Enter(ClientStep::Done);
            return;
        }
        UE_LOGI("[LAPTOP-POWER-DRILL] client: its press settled and its one edge went; waiting for the host's");
        Enter(ClientStep::WaitHost);
        return;
    case ClientStep::WaitHost:
        if (!SettledOn(g_v0)) {
            if (Expired()) {
                UE_LOGW("[LAPTOP-POWER-DRILL] FAIL: the host's press did not reach this client within 60 s");
                Enter(ClientStep::Done);
            }
            return;
        }
        if (Own() != 1) {
            UE_LOGW("[LAPTOP-POWER-DRILL] FAIL: echoed-replay (the client sent the host's replayed edge as its own: "
                    "%u own edges where 1 was due)", Own());
            Enter(ClientStep::Done);
            return;
        }
        if (!Press()) {
            UE_LOGW("[LAPTOP-POWER-DRILL] ABANDONED: the client's second press did not start its laptop's toggle");
            Enter(ClientStep::Done);
            return;
        }
        UE_LOGI("[LAPTOP-POWER-DRILL] client: the host's press replayed here and sent nothing back; pressed again");
        Enter(ClientStep::Pressed2);
        return;
    case ClientStep::Pressed2:
        if (!SettledOn(!g_v0)) {
            if (Expired()) {
                UE_LOGW("[LAPTOP-POWER-DRILL] FAIL: the client's second press did not settle within 60 s");
                Enter(ClientStep::Done);
            }
            return;
        }
        if (Own() != 2) {
            UE_LOGW("[LAPTOP-POWER-DRILL] FAIL: the client's second press sent %u own edge(s) in all, where 2 were "
                    "due", Own());
            Enter(ClientStep::Done);
            return;
        }
        UE_LOGI("[LAPTOP-POWER-DRILL] client: its second press went; waiting for the host's second");
        Enter(ClientStep::WaitHost2);
        return;
    case ClientStep::WaitHost2:
        if (!SettledOn(g_v0)) {
            if (Expired()) {
                UE_LOGW("[LAPTOP-POWER-DRILL] FAIL: the host's second press did not reach this client within 60 s");
                Enter(ClientStep::Done);
            }
            return;
        }
        if (Own() != 2) {
            UE_LOGW("[LAPTOP-POWER-DRILL] FAIL: echoed-replay (the client sent the host's second replayed edge as its "
                    "own: %u own edges where 2 were due)", Own());
            Enter(ClientStep::Done);
            return;
        }
        if (!SendEdge(s, !g_v0) || !SendEdge(s, g_v0)) {
            UE_LOGW("[LAPTOP-POWER-DRILL] ABANDONED: the client's two crossing edges were not sent");
            Enter(ClientStep::Done);
            return;
        }
        UE_LOGI("[LAPTOP-POWER-DRILL] client DONE: two presses and two own edges, the host's replays silent; sent "
                "isOpened=%d then %d, the second setting the host's target back mid-toggle", g_v0 ? 0 : 1,
                g_v0 ? 1 : 0);
        Enter(ClientStep::Done);
        return;
    case ClientStep::Done:
        return;
    }
}

void HostTick(coop::net::Session& s) {
    switch (g_host) {
    case HostStep::Ready:
        for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers) && g_slot < 0; ++slot)
            if (s.IsSlotWorldReady(slot)) g_slot = slot;
        if (g_slot < 0) return;
        if (const Begin b = Start("host"); b != Begin::Go) {
            if (b == Begin::Abandon) Enter(HostStep::Done);
            return;
        }
        g_appliedBase = LS::ClientEdgesApplied(g_slot);
        UE_LOGI("[LAPTOP-POWER-DRILL] host ARMED on slot %d (isOpened=%d): waiting for the client's press", g_slot,
                g_v0 ? 1 : 0);
        Enter(HostStep::WaitClient);
        return;
    case HostStep::WaitClient:
        if (LS::ClientEdgesApplied(g_slot) < g_appliedBase + 1 || !SettledOn(!g_v0)) {
            if (Expired()) {
                UE_LOGW("[LAPTOP-POWER-DRILL] FAIL: the client's press did not settle on the host within 60 s");
                Enter(HostStep::Done);
            }
            return;
        }
        if (Own() != 0) {
            UE_LOGW("[LAPTOP-POWER-DRILL] FAIL: echoed-replay (the host sent its replay of the client's edge as its "
                    "own: %u own edges where 0 were due)", Own());
            Enter(HostStep::Done);
            return;
        }
        if (!Press()) {
            UE_LOGW("[LAPTOP-POWER-DRILL] ABANDONED: the host's press did not start its laptop's toggle");
            Enter(HostStep::Done);
            return;
        }
        UE_LOGI("[LAPTOP-POWER-DRILL] host: the client's press replayed here and sent nothing back; pressed");
        Enter(HostStep::Pressed);
        return;
    case HostStep::Pressed:
        if (!SettledOn(g_v0)) {
            if (Expired()) {
                UE_LOGW("[LAPTOP-POWER-DRILL] FAIL: the host's press did not settle within 60 s");
                Enter(HostStep::Done);
            }
            return;
        }
        if (Own() != 1) {
            UE_LOGW("[LAPTOP-POWER-DRILL] FAIL: the host sent %u edge(s) as its own for its one press", Own());
            Enter(HostStep::Done);
            return;
        }
        UE_LOGI("[LAPTOP-POWER-DRILL] host: its press settled and its one edge went; waiting for the client's second");
        Enter(HostStep::WaitClient2);
        return;
    case HostStep::WaitClient2:
        if (LS::ClientEdgesApplied(g_slot) < g_appliedBase + 2 || !SettledOn(!g_v0)) {
            if (Expired()) {
                UE_LOGW("[LAPTOP-POWER-DRILL] FAIL: the client's second press did not settle on the host within 60 s");
                Enter(HostStep::Done);
            }
            return;
        }
        if (Own() != 1) {
            UE_LOGW("[LAPTOP-POWER-DRILL] FAIL: echoed-replay (the host sent its replay of the client's second edge "
                    "as its own: %u own edges where 1 was due)", Own());
            Enter(HostStep::Done);
            return;
        }
        if (!Press()) {
            UE_LOGW("[LAPTOP-POWER-DRILL] ABANDONED: the host's second press did not start its laptop's toggle");
            Enter(HostStep::Done);
            return;
        }
        UE_LOGI("[LAPTOP-POWER-DRILL] host: the client's second press replayed here and sent nothing back; pressed "
                "again");
        Enter(HostStep::Pressed2);
        return;
    case HostStep::Pressed2:
        if (!SettledOn(g_v0)) {
            if (Expired()) {
                UE_LOGW("[LAPTOP-POWER-DRILL] FAIL: the host's second press did not settle within 60 s");
                Enter(HostStep::Done);
            }
            return;
        }
        if (Own() != 2) {
            UE_LOGW("[LAPTOP-POWER-DRILL] FAIL: the host sent %u edge(s) as its own for its two presses", Own());
            Enter(HostStep::Done);
            return;
        }
        UE_LOGI("[LAPTOP-POWER-DRILL] host: its second press settled; waiting for the client's two crossing edges");
        Enter(HostStep::WaitCrossing);
        return;
    case HostStep::WaitCrossing: {
        // The first edge starts a toggle away from V; the second sets the target back to V while it runs. The laptop
        // must come back to V once that toggle ends, with no edge of its own sent for either replay.
        bool opened = false;
        if (LS::ClientEdgesApplied(g_slot) < g_appliedBase + 4 || !LS::Settled(opened)) {
            if (Expired()) {
                UE_LOGW("[LAPTOP-POWER-DRILL] FAIL: the client's crossing edges did not settle on the host within "
                        "60 s");
                Enter(HostStep::Done);
            }
            return;
        }
        if (Own() != 2)
            UE_LOGW("[LAPTOP-POWER-DRILL] FAIL: echoed-replay (the host sent its replay of the crossing edges as its "
                    "own: %u own edges where 2 were due)", Own());
        else if (opened != g_v0)
            UE_LOGW("[LAPTOP-POWER-DRILL] FAIL: settled-off-target (the target set back mid-toggle was dropped: the "
                    "host's laptop rests at isOpened=%d, the last edge said %d)", opened ? 1 : 0, g_v0 ? 1 : 0);
        else
            UE_LOGI("[LAPTOP-POWER-DRILL] host DONE PASS: each press went out once as its presser's edge, each replay "
                    "of it sent nothing back, and a target set back mid-toggle held");
        Enter(HostStep::Done);
        return;
    }
    case HostStep::Done:
        return;
    }
}

}  // namespace

void Tick(coop::net::Session* s) {
    if (!Enabled() || !s || !s->connected()) return;
    if (s->role() == coop::net::Role::Host) HostTick(*s);
    else ClientTick(*s);
}

void OnDisconnect() {
    g_client = ClientStep::Ready;
    g_host = HostStep::Ready;
    g_stepAt = {};
    g_v0 = false;
    g_ownBase = 0;
    g_appliedBase = 0;
    g_slot = -1;
}

}  // namespace coop::dev::laptop_power_drill
