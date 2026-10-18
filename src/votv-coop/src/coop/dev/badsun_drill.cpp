// coop/dev/badsun_drill.cpp -- see coop/dev/badsun_drill.h. The enum, Install, Tick and OnDisconnect, and the helpers
// every arm shares; the arms are badsun_drill_clock.cpp and
// badsun_drill_lane.cpp.

#include "coop/dev/badsun_drill.h"

#include "badsun_drill_shared.h"  // co-located private header (src tree, not include/)

#include "coop/config/config.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_snapshot.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace coop::dev::badsun_drill {

namespace detail {

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::badsun_drill);
        return v == "clock"        ? Arm::ClockArm
             : v == "midnight"     ? Arm::Midnight
             : v == "spawn"        ? Arm::Spawn
             : v == "cheat"        ? Arm::Cheat
             : v == "join"         ? Arm::Join
             : v == "end"          ? Arm::End
             : v == "absent_stale" ? Arm::AbsentStale
                                   : Arm::Off;
    }();
    return a;
}

bool IsHost(coop::net::Session* s) { return s->role() == coop::net::Role::Host; }
const char* RoleName(bool host) { return host ? "host" : "client"; }

void VerdictV(const char* arm, bool host, const char* word, const char* fmt, va_list ap) {
    char buf[320];
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    if (word[0] == 'P') UE_LOGI("[BADSUN-DRILL] %s %s %s %s", arm, RoleName(host), word, buf);
    else UE_LOGW("[BADSUN-DRILL] %s %s %s %s", arm, RoleName(host), word, buf);
}

bool JoinOver(coop::net::Session* s, bool host, int& slot) {
    if (host) {
        for (int i = 1; i < static_cast<int>(coop::players::kMaxPeers) && slot < 0; ++i)
            if (s->IsSlotWorldReady(i) && coop::prop_snapshot::IsBracketClosed(i)) slot = i;
        return slot >= 0;
    }
    if (!coop::net_pump::HasAnnouncedWorldReady() ||
        coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
        return false;
    return true;
}

}  // namespace detail

namespace {

std::atomic<coop::net::Session*> g_session{nullptr};

}  // namespace

bool IsEnabled() { return detail::ArmOf() != detail::Arm::Off; }

void Install(coop::net::Session* session) {
    if (!IsEnabled()) return;
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    if (!IsEnabled()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    const bool host = detail::IsHost(s);
    switch (detail::ArmOf()) {
    case detail::Arm::ClockArm: detail::ClockTick(s, host); return;
    default:                    detail::LaneTick(s, host); return;
    }
}

void OnDisconnect() {
    detail::ClockReset();
    detail::LaneReset();
}

}  // namespace coop::dev::badsun_drill
