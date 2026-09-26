// coop/dev/world_roll_drill.cpp -- see coop/dev/world_roll_drill.h.

#include "coop/dev/world_roll_drill.h"

#include "coop/config/config.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"  // coop::players::kMaxPeers
#include "coop/props/prop_snapshot.h"      // IsBracketClosed: a joiner's snapshot is over
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/world/world_actor_sync.h"  // IsMirroredActor: a host's fish, mirrored, told from this copy's own

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"  // GetStats: the gate's own count of the bodies it refused
#include "ue_wrap/core/types.h"
#include "ue_wrap/world/jellyfish_path.h"
#include "ue_wrap/world/skysphere.h"

#include <atomic>
#include <chrono>
#include <string>

namespace coop::dev::world_roll_drill {
namespace {

namespace SKY = ue_wrap::skysphere;
namespace JP  = ue_wrap::jellyfish_path;

enum class Arm { Off, EyeHost, EyeClient, EyeJoin, JellyHost, JellyClient, JellyJoin };
// Watch: the jellyfish arms follow the run after its start, the host to the path's end of it and the client
// to its mirrors leaving.
enum class Step { Wait, Watch, Done, Invalid };

// Game thread only, but for the session pointer the Install fanout stores.
std::atomic<coop::net::Session*> g_session{nullptr};
Step g_step = Step::Wait;
bool g_saidArm = false;
int  g_slot = -1;  // the host's client whose join armed it
// The client's eye arms: when its join was over, and how long the host's eye may take to arrive (the sky
// lane streams at 1 Hz).
std::chrono::steady_clock::time_point g_eyeSince{};
constexpr auto kEyeBound = std::chrono::seconds(20);
// The jellyfish arms: a run's seven fish, how long a client waits for them and for the run's end, and how often
// each peer says the fish's average place, to be matched by the log's time.
constexpr int  kJellyFish = 7;
constexpr auto kJellyArriveBound = std::chrono::seconds(20);
constexpr auto kJellyRunBound = std::chrono::seconds(600);
constexpr auto kJellyLogEvery = std::chrono::seconds(5);
std::chrono::steady_clock::time_point g_jellySince{};  // the client's join over; the host's spawn
std::chrono::steady_clock::time_point g_jellyAt{};     // the run seen under way on this peer
std::chrono::steady_clock::time_point g_nextJellyLog{};

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::world_roll_drill);
        return v == "eye_host"   ? Arm::EyeHost
             : v == "eye_client" ? Arm::EyeClient
             : v == "eye_join"   ? Arm::EyeJoin
             : v == "jelly_host"   ? Arm::JellyHost
             : v == "jelly_client" ? Arm::JellyClient
             : v == "jelly_join"   ? Arm::JellyJoin
                                 : Arm::Off;
    }();
    return a;
}

const char* ArmName() {
    switch (ArmOf()) {
    case Arm::EyeHost:   return "eye_host";
    case Arm::EyeClient: return "eye_client";
    case Arm::EyeJoin:   return "eye_join";
    case Arm::JellyHost:   return "jelly_host";
    case Arm::JellyClient: return "jelly_client";
    case Arm::JellyJoin:   return "jelly_join";
    default:             return "off";
    }
}

// What this peer does on its arm, for the first line.
const char* ArmPlan(bool host) {
    if (ArmOf() == Arm::EyeHost)
        return host ? "setting the sky's eye once a client's join is over" : "watching for the host's sky eye";
    if (ArmOf() == Arm::EyeJoin)
        return host ? "setting the sky's eye before any client's world is ready" : "watching for the host's standing eye";
    if (ArmOf() == Arm::JellyHost)
        return host ? "spawning the jellyfish once a client's join is over" : "watching for the host's jellyfish";
    if (ArmOf() == Arm::JellyClient)
        return host ? "watching; the client spawns its own jellyfish" : "spawning this copy's own jellyfish once joined";
    if (ArmOf() == Arm::JellyJoin)
        return host ? "spawning the jellyfish before any client's world is ready" : "watching for the host's running jellyfish";
    return host ? "watching; the client sets its own sky eye" : "setting this copy's own sky eye once joined";
}

void Invalid(char role, const char* why) {
    g_step = Step::Invalid;
    UE_LOGW("world_roll_drill: [%c] INVALID (arm %s) -- %s", role, ArmName(), why);
}

bool IsJellyArm() {
    return ArmOf() == Arm::JellyHost || ArmOf() == Arm::JellyClient || ArmOf() == Arm::JellyJoin;
}

long long SecondsSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t).count();
}

// The live fish on this peer: the lane's mirrors apart from this copy's own, and their average place.
struct FishCount {
    int mirrors = 0;
    int own = 0;
    ue_wrap::FVector sum{};
    ue_wrap::FVector Average() const {
        const int n = mirrors + own;
        return n ? ue_wrap::FVector{sum.X / n, sum.Y / n, sum.Z / n} : ue_wrap::FVector{};
    }
};

FishCount CountFish() {
    FishCount c;
    JP::ForEachFish([](void* ctx, void* fish, const ue_wrap::FVector& at) {
        FishCount& fc = *static_cast<FishCount*>(ctx);
        if (coop::world_actor_sync::IsMirroredActor(fish)) ++fc.mirrors; else ++fc.own;
        fc.sum.X += at.X;
        fc.sum.Y += at.Y;
        fc.sum.Z += at.Z;
    }, &c);
    return c;
}

// The fish's average place every kJellyLogEvery, on either peer, for the two logs to be matched by time.
void SayFish(char role, const FishCount& c) {
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextJellyLog) return;
    g_nextJellyLog = now + kJellyLogEvery;
    const ue_wrap::FVector a = c.Average();
    UE_LOGI("world_roll_drill: [%c] %s fish: mirrors %d own %d, average (%.0f, %.0f, %.0f)", role, ArmName(),
            c.mirrors, c.own, a.X, a.Y, a.Z);
}

// jelly_host and jelly_join, the host: the path's own spawn, as the 18:00 roll would call it; its run must be
// under way after the call.
void SpawnJelly(const char* when) {
    int before = -1, after = -1;
    JP::ReadListed(before);
    if (!JP::CallSpawn()) {
        Invalid('H', "the jellyfish path's spawn did not run");
        return;
    }
    bool active = false;
    JP::ReadActive(active);
    JP::ReadListed(after);
    if (!active) {
        Invalid('H', "the jellyfish path's run is not active after its spawn");
        return;
    }
    UE_LOGI("world_roll_drill: [H] arm %s -- %s; its path's spawn ran, the run is active and its list went %d -> %d",
            ArmName(), when, before, after);
    g_jellySince = g_jellyAt = std::chrono::steady_clock::now();
    g_nextJellyLog = {};
    g_step = Step::Watch;
}

// The host's run, watched to its end: the path destroys its seven at the spline's end and ends the run.
void WatchJellyRun() {
    bool active = true;
    if (!JP::ReadActive(active)) return;
    if (!active) {
        UE_LOGI("world_roll_drill: [H] %s: the path's run ended %lld s after its spawn", ArmName(),
                SecondsSince(g_jellySince));
        g_step = Step::Done;
        return;
    }
    SayFish('H', CountFish());
    if (std::chrono::steady_clock::now() - g_jellySince > kJellyRunBound) {
        UE_LOGI("world_roll_drill: [H] %s: the run is still active %lld s after its spawn; its end is not seen",
                ArmName(), SecondsSince(g_jellySince));
        g_step = Step::Done;
    }
}

void TickJellyHost(coop::net::Session* s) {
    if (g_step == Step::Watch) {
        WatchJellyRun();
        return;
    }
    if (g_step != Step::Wait) return;
    if (ArmOf() == Arm::JellyJoin) {
        // Before any client's world is ready, so a joiner meets the run under way. The path is waited for.
        if (!JP::Path()) return;
        for (int slot = 1; slot < static_cast<int>(coop::players::kMaxPeers); ++slot)
            if (s->IsSlotWorldReady(slot)) {
                Invalid('H', "a client's world was ready before this host could spawn its jellyfish");
                return;
            }
        SpawnJelly("no client's world is ready yet");
        return;
    }
    for (int slot = 1; slot < static_cast<int>(coop::players::kMaxPeers) && g_slot < 0; ++slot)
        if (s->IsSlotWorldReady(slot) && coop::prop_snapshot::IsBracketClosed(slot)) g_slot = slot;
    if (g_slot < 0) return;
    if (ArmOf() == Arm::JellyClient) {
        bool active = false;
        JP::ReadActive(active);
        UE_LOGI("world_roll_drill: [H] arm jelly_client -- slot %d's join is over; its path is left alone, its run "
                "active=%d", g_slot, active ? 1 : 0);
        g_step = Step::Done;
        return;
    }
    SpawnJelly("a client's join is over");
}

// The client, once its join is over. jelly_client: this copy's own spawn, as its 18:00 roll would call it, must be
// refused at the gate (its own count of refusals moves) and leave its path's list as it was. jelly_host and
// jelly_join: the host's seven must reach this copy as mirrors within kJellyArriveBound, with none of its own, and
// leave with the host's run; a run that outlives kJellyRunBound here is INCONCLUSIVE, its end not seen.
void TickJellyClient() {
    if (g_step == Step::Done || g_step == Step::Invalid) return;
    if (!coop::net_pump::HasAnnouncedWorldReady() ||
        coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (ArmOf() == Arm::JellyClient) {
        int before = -1, after = -1;
        if (!JP::ReadListed(before)) return;  // the path is not loaded yet
        const unsigned long long refusedBefore = ue_wrap::script_gate::GetStats().cancelled;
        if (!JP::CallSpawn()) {
            Invalid('C', "the jellyfish path's spawn did not run");
            return;
        }
        const bool refused = ue_wrap::script_gate::GetStats().cancelled > refusedBefore;
        bool active = false;
        JP::ReadListed(after);
        JP::ReadActive(active);
        if (refused && after == before)
            UE_LOGI("world_roll_drill: [C] jelly_client DONE -- this copy's own spawn was refused at the gate: its "
                    "path's list stays %d, active=%d -- PASS", after, active ? 1 : 0);
        else
            UE_LOGW("world_roll_drill: [C] jelly_client DONE -- this copy's own spawn ran (refused=%d): its path's "
                    "list went %d -> %d, active=%d -- FAIL", refused ? 1 : 0, before, after, active ? 1 : 0);
        g_step = Step::Done;
        return;
    }
    if (g_jellySince == std::chrono::steady_clock::time_point{}) g_jellySince = now;
    const FishCount c = CountFish();
    if (g_step == Step::Wait) {
        if (c.mirrors >= kJellyFish && c.own > 0) {
            UE_LOGW("world_roll_drill: [C] %s DONE -- the host's %d fish reached this copy as mirrors, but it has %d "
                    "of its own -- FAIL", ArmName(), c.mirrors, c.own);
            g_step = Step::Done;
        } else if (c.mirrors >= kJellyFish) {
            UE_LOGI("world_roll_drill: [C] %s: the host's %d fish reached this copy as mirrors %lld ms after its "
                    "join, none of its own", ArmName(), c.mirrors,
                    static_cast<long long>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - g_jellySince).count()));
            g_jellyAt = now;
            g_nextJellyLog = {};
            g_step = Step::Watch;
        } else if (now - g_jellySince > kJellyArriveBound) {
            UE_LOGW("world_roll_drill: [C] %s DONE -- mirrors %d, own %d, %lld s after its join -- FAIL", ArmName(),
                    c.mirrors, c.own, SecondsSince(g_jellySince));
            g_step = Step::Done;
        }
        return;
    }
    if (c.mirrors == 0) {
        UE_LOGI("world_roll_drill: [C] %s DONE -- the host's fish came as mirrors and left with its run %lld s "
                "later, own %d -- PASS", ArmName(), SecondsSince(g_jellyAt), c.own);
        g_step = Step::Done;
        return;
    }
    SayFish('C', c);
    if (now - g_jellyAt > kJellyRunBound) {
        UE_LOGW("world_roll_drill: [C] %s DONE -- the host's fish came as mirrors; the run outlived %lld s here "
                "(mirrors %d, own %d), its end not seen -- INCONCLUSIVE", ArmName(), SecondsSince(g_jellyAt),
                c.mirrors, c.own);
        g_step = Step::Done;
    }
}

// eye_join, the host: its eye is set before any client's world is ready, so a joiner meets it standing and only
// the snapshot at its world-ready, or the stream after it, can carry it (the save has no eye). The sky is waited for.
void SetEyeBeforeJoin(coop::net::Session* s) {
    bool eye = false;
    if (!SKY::ReadEye(eye)) return;
    for (int slot = 1; slot < static_cast<int>(coop::players::kMaxPeers); ++slot)
        if (s->IsSlotWorldReady(slot)) {
            Invalid('H', "a client's world was ready before this host could set its eye");
            return;
        }
    if (!SKY::CallSetEye(true)) {
        Invalid('H', "the sky's setEye did not run");
        return;
    }
    SKY::ReadEye(eye);
    UE_LOGI("world_roll_drill: [H] arm eye_join -- no client's world is ready yet; its setEye(true) ran, this host's "
            "eye reads %d", eye ? 1 : 0);
    g_step = Step::Done;
}

// The host, once a client's join is over (its slot world-ready, the join's bracket closed). eye_host: the
// host's own noon verb, as its roll would run it; eye_client: the host's eye is left alone, and said, so the
// client's verdict has the value its copy must keep.
void TickHost(coop::net::Session* s) {
    if (IsJellyArm()) {
        TickJellyHost(s);
        return;
    }
    if (g_step != Step::Wait) return;
    if (ArmOf() == Arm::EyeJoin) {
        SetEyeBeforeJoin(s);
        return;
    }
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

// The client, once its join is over. eye_host and eye_join: the host's eye must reach this copy within kEyeBound.
// eye_client:
// this copy's own setEye(true), as its noon roll would call it, must be refused at the gate, the copy's eye
// unchanged, since a client never rolls the sky; the gate's own count tells a refusal from a body that ran and
// changed nothing (the copy can already read 1, the host's noon having rolled it). A call that did not run, or a
// sky whose eye never reads, proves nothing, so either ends the arm invalid.
void TickClient() {
    if (IsJellyArm()) {
        TickJellyClient();
        return;
    }
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
    if (ArmOf() == Arm::EyeHost || ArmOf() == Arm::EyeJoin) {
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
    g_jellySince = g_jellyAt = g_nextJellyLog = {};
}

}  // namespace coop::dev::world_roll_drill
