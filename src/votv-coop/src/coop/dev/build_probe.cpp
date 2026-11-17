// coop/dev/build_probe.cpp -- the building probe's arming, its world edge and its step sequence; see
// coop/dev/build_probe.h.

#include "coop/dev/build_probe.h"

#include "build_probe_detail.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_snapshot.h"
#include "coop/session/host_world_settle.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"

#include "harness/autotest/peer_signal.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/save_to_slot_hook.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/world/world_singleton.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>

namespace coop::dev::build_probe {
namespace detail {

namespace {
void Say(bool warn, const char* fmt, va_list ap) {
    char buf[480];
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    if (warn) UE_LOGW("%s: %s", kTag, buf);
    else UE_LOGI("%s: %s", kTag, buf);
}
}  // namespace

const char* RoleName(Role r) { return r == Role::Host ? "host" : r == Role::Client ? "client" : "solo"; }

void Info(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    Say(false, fmt, ap);
    va_end(ap);
}

void Warn(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    Say(true, fmt, ap);
    va_end(ap);
}

Res Invalid(const char* fmt, ...) {
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid, "%s", buf);
    return Res::Invalid;
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back(c < 128 ? static_cast<char>(c) : '?');
    return s;
}

std::string ClassOrNull(void* obj) {
    return obj && ue_wrap::reflection::IsLive(obj) ? Narrow(ue_wrap::reflection::ClassNameOf(obj)) : std::string("null");
}

}  // namespace detail

namespace {

namespace PS = harness::autotest::peer_signal;
using detail::Res;
using detail::Role;

enum class Phase : uint8_t { WaitJoin, Legs, Brick, Finish, Done };

constexpr uint64_t kJoinBoundMs = 300000;   // the host: a client's join is over within this
constexpr uint64_t kSaveBoundMs = 60000;    // solo: the save's write is reported within this

Phase    g_phase = Phase::WaitJoin;
bool     g_armed = false;
int      g_generation = 0;
void*    g_censusWorld = nullptr;
uint64_t g_edgeMs = 0;
uint64_t g_phaseMs = 0;
bool     g_saveCalled = false, g_saveWritten = false;
uint32_t g_writes = 0;

bool On() {
    static const bool s = coop::config::ResolveEnum(::coop::config_registry::rows::build_probe) == "on";
    return s;
}

void OnWritten(void*, const wchar_t* slot) {
    ++g_writes;
    UE_LOGI("%s: the engine wrote a world save to slot %ls", detail::kTag, slot ? slot : L"?");
}

// The role this process has or will have: its running session's, else the one its net.role row names (a host's world
// loads before its session runs), else solo.
Role RoleOf(const coop::net::Session& session) {
    if (session.running()) return session.role() == coop::net::Role::Host ? Role::Host : Role::Client;
    static const std::string configured = coop::config::ResolveEnum(::coop::config_registry::rows::net_role);
    return configured == "host" ? Role::Host : configured == "client" ? Role::Client : Role::Solo;
}

void To(Phase p, uint64_t now) {
    g_phase = p;
    g_phaseMs = now;
}

// The edge a census waits for, per role: the host's settled world, the client's announced one, a solo world with
// no backlog in the object index and a local player.
bool EdgeReached(Role role) {
    if (role == Role::Host) return coop::host_world_settle::Settled();
    if (role == Role::Client) return coop::net_pump::HasAnnouncedWorldReady();
    return ue_wrap::object_index::Backlog() == 0 && coop::players::Registry::Get().Local() != nullptr;
}

void Finish(Role role) {
    detail::SayKeyIndexEnd();
    detail::Info("DONE role=%s worlds=%d", detail::RoleName(role), g_generation);
    UE_LOGI("%s: DONE", detail::kTag);
    UE_LOGI("rig: VERDICT %s DONE -- measured, nothing judged", detail::kTag);
    g_phase = Phase::Done;
}

// A leg that could not measure said INVALID: the probe ends there.
bool Ended(Res r) {
    if (r != Res::Invalid) return false;
    g_phase = Phase::Done;
    return true;
}

void HostStep(coop::net::Session& s, uint64_t now) {
    switch (g_phase) {
    case Phase::WaitJoin:
        if (g_generation == 0 || !detail::InstallWatches()) return;
        if (!(s.IsSlotWorldReady(1) && coop::prop_snapshot::IsBracketClosed(1))) {
            if (now - g_phaseMs >= kJoinBoundMs) {
                detail::Invalid("no client's join was over within %llu s", static_cast<unsigned long long>(kJoinBoundMs / 1000));
                g_phase = Phase::Done;
            }
            return;
        }
        detail::Info("host: slot 1's join is over; the damage legs begin");
        To(Phase::Legs, now);
        return;
    case Phase::Legs: {
        const Res r = detail::TickDamageLegs();
        if (!Ended(r) && r == Res::Done) To(Phase::Brick, now);
        return;
    }
    case Phase::Brick: {
        const Res r = detail::TickBrickHost();
        if (!Ended(r) && r == Res::Done) To(Phase::Finish, now);
        return;
    }
    case Phase::Finish:
        Finish(Role::Host);
        return;
    default:
        return;
    }
}

void ClientStep(uint64_t now) {
    switch (g_phase) {
    case Phase::WaitJoin:
        if (g_generation == 0 || !detail::InstallWatches() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
            return;
        detail::Info("client: joined; waiting for the host's wall");
        To(Phase::Brick, now);
        return;
    case Phase::Brick: {
        const Res r = detail::TickBrickClient();
        if (!Ended(r) && r == Res::Done) To(Phase::Finish, now);
        return;
    }
    case Phase::Finish:
        Finish(Role::Client);
        return;
    default:
        return;
    }
}

// A solo world: the game's own save at its first edge, then the second world's census (after the tree's reload scenario)
// ends the probe.
void SoloStep(uint64_t now) {
    if (g_generation >= 2) {
        if (g_phase != Phase::Done) Finish(Role::Solo);
        return;
    }
    if (g_generation != 1 || g_saveWritten) return;
    if (!g_saveCalled) {
        void* gm = ue_wrap::world_singleton::Gamemode();
        void* fn = gm ? ue_wrap::reflection::FindFunction(ue_wrap::reflection::ClassOf(gm), L"save") : nullptr;
        if (!fn) return;
        ue_wrap::ParamFrame f(fn);
        f.Set<bool>(L"quicksave", false);
        f.Set<bool>(L"bypassEvent", false);
        f.Set<bool>(L"overwriteSubsave", false);
        g_saveCalled = f.valid() && ue_wrap::Call(gm, f);
        detail::Info("solo: the game's save %s", g_saveCalled ? "called" : "did not run");
        g_phaseMs = now;
        return;
    }
    if (g_writes > 0) {
        g_saveWritten = true;
        detail::Info("solo: saved; the reload of the same save prints the second census");
    } else if (now - g_phaseMs >= kSaveBoundMs) {
        detail::Invalid("the save was called and no world save was written within %llu s",
                        static_cast<unsigned long long>(kSaveBoundMs / 1000));
        g_phase = Phase::Done;
    }
}

}  // namespace

void Tick(coop::net::Session& session) {
    if (!On() || g_phase == Phase::Done) return;
    void* world = ue_wrap::world_identity::CurrentWorld();
    if (!world || ue_wrap::world_identity::CurrentWorldKind() != ue_wrap::world_identity::WorldKind::Gameplay) return;
    const Role role = RoleOf(session);
    const uint64_t now = coop::net::NowMs();
    if (!g_armed) {
        g_armed = true;
        if (!PS::Begin(detail::kTag)) {   // says its own INVALID line
            g_phase = Phase::Done;
            return;
        }
        ue_wrap::save_to_slot_hook::AddWritten(&OnWritten);
        g_phaseMs = now;
        detail::Info("armed role=%s", detail::RoleName(role));
    }
    detail::InstallWatches();
    if (world != g_censusWorld && EdgeReached(role)) {
        g_censusWorld = world;
        ++g_generation;
        g_edgeMs = now;
        detail::RunCensus(role, g_generation, g_edgeMs);
        detail::SayKeyDiff(g_generation);
    }
    detail::PollKeyIndex(now);
    if (role == Role::Host) HostStep(session, now);
    else if (role == Role::Client) ClientStep(now);
    else SoloStep(now);
}

void OnDisconnect() {
    if (!On()) return;
    detail::ResetDamageLegs();
    detail::ResetBrick();
    g_phase = Phase::WaitJoin;
    g_armed = false;
    g_phaseMs = 0;
}

}  // namespace coop::dev::build_probe
