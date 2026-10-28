// coop/session/host_world_settle.cpp -- see coop/session/host_world_settle.h.

#include "coop/session/host_world_settle.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/session/rig_ready.h"
#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/prop_processor.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/world/world_singleton.h"

#include <cstdint>
#include <cstddef>
#include <string>

namespace coop::host_world_settle {
namespace {

namespace CI = ue_wrap::container_inventory;
namespace PP = ue_wrap::prop_processor;
namespace WI = ue_wrap::world_identity;
namespace R  = ue_wrap::reflection;

using ull = unsigned long long;

constexpr uint64_t kWalkPeriodMs = 1000;
constexpr uint64_t kNoProcessorWarnMs = 5000;  // from the world clock's start

// All state game-thread-only; no mutex.
bool     g_latched = false;
uint32_t g_latchGen = 0;
bool     g_readySaid = false;  // per session

uint32_t g_clockGen = 0;       // the world the clock below belongs to
uint64_t g_clockStartMs = 0;   // the first Tick that saw a gamemode; 0 = not started
uint64_t g_lastWalkMs = 0;
uint32_t g_noProcessorWarnGen = 0;
uint32_t g_listFullWarnGen = 0;

void* g_buf[kContainerCap];

// What the last walk saw, for the latch and ceiling lines.
struct WalkResult {
    bool    settled = false;
    int     passes = -1;
    size_t  containers = 0;
    size_t  initialized = 0;
    void*   firstUninitialized = nullptr;
};
WalkResult g_last;

WalkResult Walk(void* gamemode) {
    WalkResult w;
    void* const pp = PP::WorldPropProcessor();
    w.passes = PP::Passes(pp);
    const bool listResolved = CI::AllContainersResolved(gamemode);
    const uint32_t gen = WI::Generation();
    // Fail closed: a processor, a pass count or a container list that does not resolve is not settled.
    static bool s_saidFins = false, s_saidList = false;
    if (pp && w.passes < 0 && !s_saidFins) {
        s_saidFins = true;
        UE_LOGW("host_world_settle: propProcessor_C.fins did not resolve -- the world waits for the ceiling");
    }
    if (!listResolved && !s_saidList) {
        s_saidList = true;
        UE_LOGW("host_world_settle: the gamemode's allContainers did not resolve -- the world waits for the ceiling");
    }
    // A processor that is still absent well into the world's life is a defect worth a line, once a world.
    if (!pp && g_clockStartMs && coop::net::NowMs() - g_clockStartMs >= kNoProcessorWarnMs &&
        g_noProcessorWarnGen != gen) {
        g_noProcessorWarnGen = gen;
        UE_LOGW("host_world_settle: no propProcessor_C found %d s into the world -- it waits for the ceiling",
                static_cast<int>(kNoProcessorWarnMs / 1000));
    }
    if (!listResolved) { g_last = w; return w; }
    const size_t n = CI::AllContainers(gamemode, g_buf, kContainerCap);
    // A list that fills the buffer may be cut short: it cannot be judged, so the world is not settled.
    if (n >= kContainerCap) {
        if (g_listFullWarnGen != gen) {
            g_listFullWarnGen = gen;
            UE_LOGW("host_world_settle: the gamemode lists %zu or more containers, past the %zu the walk reads -- "
                    "the world waits for the ceiling", n, kContainerCap);
        }
        w.containers = n;
        g_last = w;
        return w;
    }
    w.containers = n;
    for (size_t i = 0; i < n; ++i) {
        if (CI::InventoryInitialized(g_buf[i])) ++w.initialized;
        else if (!w.firstUninitialized) w.firstUninitialized = g_buf[i];
    }
    // [dev] host_settle_ceiling_probe: the processor never reads finished, so the ceiling is what ends the wait.
    static const bool s_ceilingProbe =
        coop::config::ResolveFlag(::coop::config_registry::rows::host_settle_ceiling_probe);
    w.settled = !s_ceilingProbe && PP::Finished(pp) && w.initialized == n;
    g_last = w;
    return w;
}

void Latch(uint32_t gen, uint64_t now) {
    g_latched = true;
    g_latchGen = gen;
    UE_LOGI("host_world_settle: SETTLED gen=%u %llu ms after the world began (%zu containers, prop passes %d)",
            gen, static_cast<ull>(g_clockStartMs ? now - g_clockStartMs : 0), g_last.containers, g_last.passes);
}

void Advance(uint32_t gen) {
    if (g_clockGen != gen) {
        g_clockGen = gen;
        g_clockStartMs = 0;
        g_lastWalkMs = 0;
        g_last = WalkResult{};
    }
    const uint64_t now = coop::net::NowMs();
    // A world with nothing to start: the menu, or a build whose world cannot be named (the two exits
    // the save transfer's old test had).
    if (WI::Degraded() || WI::CurrentWorldKind() == WI::WorldKind::Other) { Latch(gen, now); return; }
    // One pass a second from here on, the gamemode's lookup and the processor's included: before the
    // world's clock starts there is nothing to read faster than that.
    if (g_lastWalkMs && now - g_lastWalkMs < kWalkPeriodMs) return;
    g_lastWalkMs = now;
    void* const gm = ue_wrap::world_singleton::Gamemode();
    if (!gm) return;
    if (!g_clockStartMs) g_clockStartMs = now;
    if (Walk(gm).settled) { Latch(gen, now); return; }
    if (now - g_clockStartMs < kCeilingMs) return;
    const std::wstring cls = g_last.firstUninitialized ? R::ClassNameOf(g_last.firstUninitialized) : std::wstring();
    UE_LOGW("host_world_settle: the world did not settle in %d s (prop passes %d, %zu of %zu containers "
            "initialized; first uninitialized class '%ls') -- admitting anyway",
            static_cast<int>(kCeilingMs / 1000), g_last.passes, g_last.initialized, g_last.containers,
            cls.c_str());
    Latch(gen, now);
}

}  // namespace

bool Settled() {
    static const bool red = coop::config::ResolveFlag(::coop::config_registry::rows::host_settle_red);
    if (g_latched && g_latchGen == WI::Generation()) return true;
    return red && ue_wrap::world_singleton::Gamemode() != nullptr;
}

uint32_t MsSinceWorldBegan() {
    if (g_clockGen != WI::Generation() || !g_clockStartMs) return 0;
    return static_cast<uint32_t>(coop::net::NowMs() - g_clockStartMs);
}

bool TrueSettledNow() {
    void* const gm = ue_wrap::world_singleton::Gamemode();
    return gm && Walk(gm).settled;
}

void Tick(coop::net::Session& session) {
    if (session.role() != coop::net::Role::Host) return;
    const uint32_t gen = WI::Generation();
    if (!(g_latched && g_latchGen == gen)) Advance(gen);
    if (!g_readySaid && Settled()) {
        g_readySaid = true;
        coop::rig_ready::Say("hosting");
    }
}

void OnSessionEnd() { g_readySaid = false; }

}  // namespace coop::host_world_settle
