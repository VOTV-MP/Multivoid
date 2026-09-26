// coop/dev/world_roll_drill.cpp -- see coop/dev/world_roll_drill.h.

#include "coop/dev/world_roll_drill.h"

#include "coop/config/config.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"  // coop::players::kMaxPeers
#include "coop/props/prop_snapshot.h"      // IsBracketClosed: a joiner's snapshot is over
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"  // GetStats: the gate's own count of the bodies it refused
#include "ue_wrap/world/skysphere.h"

#include <atomic>
#include <chrono>
#include <string>

namespace coop::dev::world_roll_drill {
namespace {

namespace SKY = ue_wrap::skysphere;

enum class Arm { Off, EyeHost, EyeClient };
enum class Step { Wait, Done, Invalid };

// Game thread only, but for the session pointer the Install fanout stores.
std::atomic<coop::net::Session*> g_session{nullptr};
Step g_step = Step::Wait;
bool g_saidArm = false;
int  g_slot = -1;  // the host's client whose join armed it
// The client's eye arms: when its join was over, and how long the host's eye may take to arrive (the sky
// lane streams at 1 Hz).
std::chrono::steady_clock::time_point g_eyeSince{};
constexpr auto kEyeBound = std::chrono::seconds(20);

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::world_roll_drill);
        return v == "eye_host"   ? Arm::EyeHost
             : v == "eye_client" ? Arm::EyeClient
                                 : Arm::Off;
    }();
    return a;
}

const char* ArmName() {
    switch (ArmOf()) {
    case Arm::EyeHost:   return "eye_host";
    case Arm::EyeClient: return "eye_client";
    default:             return "off";
    }
}

// What this peer does on its arm, for the first line.
const char* ArmPlan(bool host) {
    if (ArmOf() == Arm::EyeHost)
        return host ? "setting the sky's eye once a client's join is over" : "watching for the host's sky eye";
    return host ? "watching; the client sets its own sky eye" : "setting this copy's own sky eye once joined";
}

void Invalid(char role, const char* why) {
    g_step = Step::Invalid;
    UE_LOGW("world_roll_drill: [%c] INVALID (arm %s) -- %s", role, ArmName(), why);
}

// The host, once a client's join is over (its slot world-ready, the join's bracket closed). eye_host: the
// host's own noon verb, as its roll would run it; eye_client: the host's eye is left alone, and said, so the
// client's verdict has the value its copy must keep.
void TickHost(coop::net::Session* s) {
    if (g_step != Step::Wait) return;
    for (int slot = 1; slot < static_cast<int>(coop::players::kMaxPeers) && g_slot < 0; ++slot)
        if (s->IsSlotWorldReady(slot) && coop::prop_snapshot::IsBracketClosed(slot)) g_slot = slot;
    if (g_slot < 0) return;
    if (ArmOf() == Arm::EyeHost && !SKY::CallSetEye(true)) {
        Invalid('H', "the sky's setEye did not run");
        return;
    }
    bool eye = false;
    SKY::ReadEye(eye);
    UE_LOGI("world_roll_drill: [H] arm %s -- slot %d's join is over; %s, this host's eye reads %d", ArmName(),
            g_slot, ArmOf() == Arm::EyeHost ? "its setEye(true) ran" : "its eye is left alone", eye ? 1 : 0);
    g_step = Step::Done;
}

// The client, once its join is over. eye_host: the host's eye must reach this copy within kEyeBound. eye_client:
// this copy's own setEye(true), as its noon roll would call it, must be refused at the gate, the copy's eye
// unchanged, since a client never rolls the sky; the gate's own count tells a refusal from a body that ran and
// changed nothing (the copy can already read 1, the host's noon having rolled it). A call that did not run, or a
// sky whose eye never reads, proves nothing, so either ends the arm invalid.
void TickClient() {
    if (g_step != Step::Wait) return;
    if (!coop::net_pump::HasAnnouncedWorldReady() ||
        coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (g_eyeSince == std::chrono::steady_clock::time_point{}) g_eyeSince = now;
    bool eye = false;
    if (!SKY::ReadEye(eye)) {
        if (now - g_eyeSince > kEyeBound) Invalid('C', "the sky's eye did not read");
        return;
    }
    if (ArmOf() == Arm::EyeHost) {
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_eyeSince).count();
        if (eye) {
            UE_LOGI("world_roll_drill: [C] %s DONE -- the host's eye reached this copy %lld ms after its join -- PASS",
                    ArmName(), ms);
            g_step = Step::Done;
        } else if (now - g_eyeSince > kEyeBound) {
            UE_LOGW("world_roll_drill: [C] %s DONE -- this copy's eye still reads 0 %lld s after its join -- FAIL",
                    ArmName(),
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(kEyeBound).count()));
            g_step = Step::Done;
        }
        return;
    }
    const unsigned long long refusedBefore = ue_wrap::script_gate::GetStats().cancelled;
    if (!SKY::CallSetEye(true)) {
        Invalid('C', "the sky's setEye did not run");
        return;
    }
    const bool refused = ue_wrap::script_gate::GetStats().cancelled > refusedBefore;
    bool after = false;
    SKY::ReadEye(after);
    if (refused && after == eye)
        UE_LOGI("world_roll_drill: [C] eye_client DONE -- this copy's own setEye(true) was refused at the gate: its "
                "eye reads %d, as before -- PASS", after ? 1 : 0);
    else
        UE_LOGW("world_roll_drill: [C] eye_client DONE -- this copy's own setEye(true) ran (refused=%d): its eye went "
                "%d -> %d -- FAIL", refused ? 1 : 0, eye ? 1 : 0, after ? 1 : 0);
    g_step = Step::Done;
}

}  // namespace

bool IsEnabled() { return ArmOf() != Arm::Off; }

void Install(coop::net::Session* session) {
    if (!IsEnabled()) return;
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    if (!IsEnabled()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    const bool host = s->role() == coop::net::Role::Host;
    if (!g_saidArm) {
        g_saidArm = true;
        UE_LOGI("world_roll_drill: [%c] arm %s -- %s", host ? 'H' : 'C', ArmName(), ArmPlan(host));
    }
    if (host) TickHost(s);
    else      TickClient();
}

void OnDisconnect() {
    g_step = Step::Wait;
    g_saidArm = false;
    g_slot = -1;
    g_eyeSince = {};
}

}  // namespace coop::dev::world_roll_drill
