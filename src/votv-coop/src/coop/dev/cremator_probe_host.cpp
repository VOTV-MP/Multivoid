// coop/dev/cremator_probe_host.cpp -- the host arm of the cremator probe, see coop/dev/cremator_probe.h.

#include "cremator_probe_detail.h"  // co-located private header (src tree, not include/)

#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace coop::dev::cremator_probe::detail {
namespace {

enum class Step : uint8_t { Watch, Run, Over };

constexpr uint8_t kClientSlot = 1;
constexpr float kInsideUu = 150.f;
constexpr int kStreakToStart = 2;
constexpr float kEndAfterS = 120.f;

Step g_step = Step::Watch;
ue_wrap::CachedObjRef g_cremator;
uint64_t g_nextSampleMs = 0;
uint64_t g_startMs = 0;
int g_streak = 0;
int g_prevAnim = -1;

const char* LifeName(coop::players::PuppetLife life) {
    return life == coop::players::PuppetLife::Alive ? "Alive"
         : life == coop::players::PuppetLife::Dead  ? "Dead"
                                                    : "None";
}

// One line from the host's copy of the machine and the client's puppet. Returns whether the puppet is gone or dead.
bool LogSample(float t, const char* edge) {
    auto& reg = coop::players::Registry::Get();
    auto* puppet = reg.Puppet(kClientSlot);
    const bool live = puppet && puppet->valid();
    const auto life = reg.LifeOf(kClientSlot);
    void* cremator = g_cremator.Get();
    void* door = ObjectProperty(cremator, L"swingerDoor");
    float windup = -1.f, health0 = kNoHealth;
    int actors = -1;
    ReadFloat(cremator, L"windup", windup);
    ReadMachine(cremator, actors, health0);
    UE_LOGI("cremator-probe: HOST T=%.0f active=%d anim=%d windup=%.3f closed=%d doorOpened=%d actors=%d "
            "health0=%.1f puppetLife=%s puppetLive=%d%s%s",
            t, Bit(cremator, L"isActive"), Bit(cremator, L"anim"), windup, Bit(cremator, L"isClosed"),
            Bit(door, L"opened"), actors, health0, LifeName(life), live ? 1 : 0, edge ? " edge=" : "",
            edge ? edge : "");
    return !live || life == coop::players::PuppetLife::Dead;
}

void Fail(const char* why) {
    UE_LOGW("cremator-probe: HOST FAIL %s", why);
    UE_LOGI("cremator-probe: HOST DONE");
    g_step = Step::Over;
}

void WatchTick(coop::net::Session& session, uint64_t now) {
    if (now < g_nextSampleMs) return;
    g_nextSampleMs = now + kSampleMs;

    auto* puppet = coop::players::Registry::Get().Puppet(kClientSlot);
    ue_wrap::FVector at{};
    if (!puppet || !puppet->valid() || !puppet->HasPose() || !session.IsSlotWorldReady(kClientSlot) ||
        !puppet->TryGetLocation(at)) {
        g_streak = 0;
        return;
    }
    const Nearest n = FindNearestFirebox(at);
    if (!n.actor || n.best > kInsideUu * kInsideUu) { g_streak = 0; return; }
    if (++g_streak < kStreakToStart) return;

    bool opened = false;
    if (!ShutDoor(n.actor, opened)) { Fail("the open door's close() did not run"); return; }
    if (!CallNoParams(n.actor, L"useLever")) { Fail("useLever did not run"); return; }
    g_cremator.Set(n.actor);
    g_startMs = now;
    g_nextSampleMs = now + kSampleMs;
    g_prevAnim = Bit(n.actor, L"anim");
    const std::wstring key = ue_wrap::prop::GetActorSaveKeyString(n.actor);
    UE_LOGI("cremator-probe: HOST LEVER key='%ls' doorWasOpened=%d puppetAt=(%.0f,%.0f,%.0f) dist=%.0f",
            key.c_str(), opened ? 1 : 0, at.X, at.Y, at.Z, std::sqrt(n.best));
    LogSample(0.f, "lever");
    g_step = Step::Run;
}

void RunTick(uint64_t now) {
    const float t = static_cast<float>(now - g_startMs) / 1000.f;
    // The handle's finish is the edge of `anim` going false; a 1 s sample can straddle it, so it is read every tick.
    const int anim = Bit(g_cremator.Get(), L"anim");
    if (g_prevAnim == 1 && anim == 0) LogSample(t, "handle-finish");
    g_prevAnim = anim;

    if (now < g_nextSampleMs) return;
    g_nextSampleMs = now + kSampleMs;
    const bool gone = LogSample(t, nullptr);
    if (gone || t >= kEndAfterS) {
        UE_LOGI("cremator-probe: HOST DONE");
        g_step = Step::Over;
    }
}

}  // namespace

void HostTick(coop::net::Session& session) {
    if (g_step == Step::Over) return;
    const uint64_t now = coop::net::NowMs();
    if (g_step == Step::Watch) WatchTick(session, now);
    else RunTick(now);
}

void HostReset() {
    g_step = Step::Watch;
    g_cremator.Reset();
    g_nextSampleMs = 0;
    g_startMs = 0;
    g_streak = 0;
    g_prevAnim = -1;
}

}  // namespace coop::dev::cremator_probe::detail
