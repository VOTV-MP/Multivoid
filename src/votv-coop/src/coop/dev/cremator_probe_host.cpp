// coop/dev/cremator_probe_host.cpp -- the host arm of the cremator probe, see coop/dev/cremator_probe.h.
//
// M-b moves the target by the host's TeleportClient order -- the `/cremate` verb under test, not a drill's
// convenience teleport (the rule that a drill moves its peer with the director binds drills; the admin verb's own
// move is what is measured here). The landing is the cremator's own `burning` begin-overlap for the puppet's actor.

#include "cremator_probe_detail.h"  // co-located private header (src tree, not include/)

#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace coop::dev::cremator_probe::detail {
namespace {

namespace E = ue_wrap::engine;

// Watch: waiting for slot 1's join to settle. Landing: the teleport order is sent, waiting for the overlap.
enum class Step : uint8_t { Watch, Landing, Run, Over };

constexpr uint8_t kClientSlot = 1;
constexpr int kLandingMaxTicks = 600;
// The run ends on the machine's own state: the burn-health at <= 0 with the puppet still collected, the same
// actor held for kHeldSamples further 1 s samples. The cap only bounds a run that never got there (the
// burn-health drains to 0 in ~107 s).
constexpr int kHeldSamples = 10;
constexpr float kCapS = 300.f;

Step g_step = Step::Watch;
int g_heldSamples = -1;  // -1 until the burn-health first reads <= 0 with the puppet collected; then the samples since
ue_wrap::CachedObjRef g_cremator;
ue_wrap::FVector g_fireboxAt{};
uint64_t g_nextSampleMs = 0;
uint64_t g_startMs = 0;
int g_landingTicks = 0;
int g_prevAnim = -1;
void* g_puppetActor = nullptr;  // the puppet's engine actor at the landing; a different one is a respawn
std::atomic<int> g_landed{0};  // the begin-overlap saw the puppet enter the probe's cremator

const char* LifeName(coop::players::PuppetLife life) {
    return life == coop::players::PuppetLife::Alive ? "Alive"
         : life == coop::players::PuppetLife::Dead  ? "Dead"
                                                    : "None";
}

// One line from the host's copy of the machine and the client's puppet; the machine's reading is handed back.
void SampleMachine(float t, const char* edge, int& actors, float& health0) {
    auto& reg = coop::players::Registry::Get();
    auto* puppet = reg.Puppet(kClientSlot);
    const bool live = puppet && puppet->valid();
    const auto life = reg.LifeOf(kClientSlot);
    void* cremator = g_cremator.Get();
    void* door = ObjectProperty(cremator, L"swingerDoor");
    float windup = -1.f, gas = -1.f;
    actors = -1;
    health0 = kNoHealth;
    ReadFloat(cremator, L"windup", windup);
    ReadFloat(cremator, L"gasAmount", gas);
    ReadMachine(cremator, actors, health0);
    UE_LOGI("cremator-probe: HOST T=%.0f active=%d anim=%d windup=%.3f closed=%d doorOpened=%d actors=%d "
            "health0=%.1f gas=%.2f puppetLife=%s puppetLive=%d%s%s",
            t, Bit(cremator, L"isActive"), Bit(cremator, L"anim"), windup, Bit(cremator, L"isClosed"),
            Bit(door, L"opened"), actors, health0, gas, LifeName(life), live ? 1 : 0, edge ? " edge=" : "",
            edge ? edge : "");
}

// A line at an edge (the lever, the handle's finish, a respawn), whose reading nothing judges.
void LogEdge(float t, const char* edge) {
    int actors = -1;
    float health0 = kNoHealth;
    SampleMachine(t, edge, actors, health0);
}

// The run's verdict, then its end. The cremator's tick destroys a collected actor whose burn-health reached 0;
// the puppet's actor changing (or going) is that destroy and the respawn that follows it. PASS is the puppet
// still collected, with the same actor, kHeldSamples samples after the burn-health reached <= 0.
void VerdictPass() {
    float health0 = kNoHealth, gas = -1.f;
    int actors = -1;
    void* cremator = g_cremator.Get();
    ReadMachine(cremator, actors, health0);
    ReadFloat(cremator, L"gasAmount", gas);
    UE_LOGI("cremator-probe: HOST VERDICT PASS puppet kept, health0=%.1f gas=%.2f held=%d samples",
            health0, gas, g_heldSamples);
    UE_LOGI("cremator-probe: HOST DONE");
    g_step = Step::Over;
}

void VerdictFail(const char* why, float t) {
    UE_LOGI("cremator-probe: HOST VERDICT FAIL %s T=%.0f", why, t);
    UE_LOGI("cremator-probe: HOST DONE");
    g_step = Step::Over;
}

// Whether slot 1's puppet is still the actor recorded at the landing. A pointer compare, run every tick.
bool PuppetKept() {
    auto* puppet = coop::players::Registry::Get().Puppet(kClientSlot);
    return puppet && puppet->valid() && puppet->GetActor() == g_puppetActor;
}

void Fail(const char* why) {
    UE_LOGW("cremator-probe: HOST FAIL %s", why);
    UE_LOGI("cremator-probe: HOST DONE");
    g_step = Step::Over;
}

float Between(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Slot 1 is world-ready, its join traffic is delivered and its puppet stands in the world (the pause-quit
// drill's settled-host test, for the one slot). Then the host opens the nearest cremator's door and sends slot 1
// the firebox pose, as the admin verb does.
void WatchTick(coop::net::Session& session) {
    auto* puppet = coop::players::Registry::Get().Puppet(kClientSlot);
    ue_wrap::FVector at{};
    if (!puppet || !puppet->valid() || !session.IsSlotWorldReady(kClientSlot) ||
        !coop::prop_snapshot::IsBracketClosed(kClientSlot) ||
        coop::join_window_baseline::IsLateWindowOpen(kClientSlot) || !session.SlotReliableIdle(kClientSlot) ||
        !puppet->TryGetLocation(at))
        return;
    const Nearest n = FindNearestFirebox(at);
    if (!n.actor) { Fail("no cremator with a firebox is loaded"); return; }
    ue_wrap::FRotator rot{};
    if (!E::TryGetActorRotation(n.actor, rot)) { Fail("the cremator's rotation is unreadable"); return; }

    g_cremator.Set(n.actor);
    g_fireboxAt = n.at;
    bool opened = false;
    if (!OpenDoor(n.actor, opened)) { Fail("the door's open() did not run"); return; }
    UE_LOGI("cremator-probe: HOST DOOR opened=%d", opened ? 1 : 0);

    coop::net::TeleportClientPayload pose{};
    pose.locX = n.at.X; pose.locY = n.at.Y; pose.locZ = n.at.Z;
    pose.rotPitch = rot.Pitch; pose.rotYaw = rot.Yaw; pose.rotRoll = rot.Roll;
    if (!session.SendReliableToSlot(kClientSlot, coop::net::ReliableKind::TeleportClient, &pose, sizeof(pose))) {
        Fail("the teleport order was not sent");
        return;
    }
    g_landed.store(0, std::memory_order_release);
    g_landingTicks = 0;
    UE_LOGI("cremator-probe: HOST SENT teleport to=(%.0f,%.0f,%.0f)", n.at.X, n.at.Y, n.at.Z);
    g_step = Step::Landing;
}

// Waiting for the cremator's own begin-overlap; then shut the door, pull the lever and start the samples.
void LandingTick(uint64_t now) {
    ++g_landingTicks;
    auto* puppet = coop::players::Registry::Get().Puppet(kClientSlot);
    ue_wrap::FVector at{};
    const bool haveAt = puppet && puppet->valid() && puppet->TryGetLocation(at);
    if (!g_landed.load(std::memory_order_acquire)) {
        if (g_landingTicks < kLandingMaxTicks) return;
        char why[96];
        std::snprintf(why, sizeof(why), "landing dist=%.0f", haveAt ? Between(at, g_fireboxAt) : -1.f);
        Fail(why);
        return;
    }
    UE_LOGI("cremator-probe: HOST LANDED ticks=%d", g_landingTicks);
    g_puppetActor = puppet && puppet->valid() ? puppet->GetActor() : nullptr;
    if (!g_puppetActor) { Fail("the puppet's actor is gone at the landing"); return; }
    void* cremator = g_cremator.Get();
    bool opened = false;
    if (!ShutDoor(cremator, opened)) { Fail("the open door's close() did not run"); return; }
    if (!CallNoParams(cremator, L"useLever")) { Fail("useLever did not run"); return; }
    g_startMs = now;
    g_nextSampleMs = now + kSampleMs;
    g_prevAnim = Bit(cremator, L"anim");
    const std::wstring key = ue_wrap::prop::GetActorSaveKeyString(cremator);
    UE_LOGI("cremator-probe: HOST LEVER key='%ls' doorWasOpened=%d puppetAt=(%.0f,%.0f,%.0f) dist=%.0f",
            key.c_str(), opened ? 1 : 0, at.X, at.Y, at.Z, haveAt ? Between(at, g_fireboxAt) : -1.f);
    LogEdge(0.f, "lever");
    g_heldSamples = -1;
    g_step = Step::Run;
}

void RunTick(uint64_t now) {
    const float t = static_cast<float>(now - g_startMs) / 1000.f;
    if (!PuppetKept()) {
        LogEdge(t, "respawn");
        VerdictFail("puppet re-spawned", t);
        return;
    }
    // The handle's finish is the edge of `anim` going false; a 1 s sample can straddle it, so it is read every tick.
    const int anim = Bit(g_cremator.Get(), L"anim");
    if (g_prevAnim == 1 && anim == 0) LogEdge(t, "handle-finish");
    g_prevAnim = anim;

    if (now < g_nextSampleMs) return;
    g_nextSampleMs = now + kSampleMs;
    int actors = -1;
    float health0 = kNoHealth;
    SampleMachine(t, nullptr, actors, health0);
    // The burn-health reads kNoHealth when unreadable, so a drained reading is a real <= 0, not a missing one.
    if (actors >= 1 && health0 != kNoHealth && health0 <= 0.f) {
        ++g_heldSamples;  // the crossing sample is -1 -> 0; each one after it counts
        if (g_heldSamples >= kHeldSamples) { VerdictPass(); return; }
    } else {
        g_heldSamples = -1;
    }
    if (t >= kCapS) {
        char why[80];
        std::snprintf(why, sizeof(why), "the machine never held the puppet at health0<=0 (held=%d)", g_heldSamples);
        VerdictFail(why, t);
    }
}

}  // namespace

void HostOnOverlap(void* cremator, void* other) {
    if (g_step != Step::Landing || !cremator || cremator != g_cremator.Get() || !other) return;
    auto* puppet = coop::players::Registry::Get().Puppet(kClientSlot);
    if (puppet && puppet->valid() && other == puppet->GetActor()) g_landed.store(1, std::memory_order_release);
}

void HostTick(coop::net::Session& session) {
    if (g_step == Step::Over) return;
    const uint64_t now = coop::net::NowMs();
    if (g_step == Step::Watch) WatchTick(session);
    else if (g_step == Step::Landing) LandingTick(now);
    else RunTick(now);
}

void HostReset() {
    g_step = Step::Watch;
    g_cremator.Reset();
    g_fireboxAt = {};
    g_nextSampleMs = 0;
    g_startMs = 0;
    g_landingTicks = 0;
    g_prevAnim = -1;
    g_heldSamples = -1;
    g_puppetActor = nullptr;
    g_landed.store(0, std::memory_order_release);
}

}  // namespace coop::dev::cremator_probe::detail
