// coop/dev/desk_ping_drill.cpp -- see coop/dev/desk_ping_drill.h.

#include "coop/dev/desk_ping_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/director/director.h"
#include "coop/dev/director/routes.h"
#include "coop/dev/game_window.h"
#include "coop/interactables/desk_ping_sync.h"     // CountsNow: what the lane did on this peer
#include "coop/interactables/device_occupancy.h"   // LocalHolds: the client entered the coordinates screen
#include "coop/interactables/signal_catch_sync.h"  // LocalCatchesRelayed: a catch the host counted as its own
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady

#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/desk/coords_panel.h"
#include "ue_wrap/desk/desk_ping.h"
#include "ue_wrap/desk/desk_press.h"
#include "ue_wrap/desk/space_renderer.h"
#include "ue_wrap/engine/engine_component.h"  // GetComponentLocation
#include "ue_wrap/world/profile.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace coop::dev::desk_ping_drill {
namespace {

namespace CD = ue_wrap::console_desk;
namespace CP = ue_wrap::coords_panel;
namespace DG = ue_wrap::desk_ping;
namespace DPR = ue_wrap::desk_press;
namespace E = ue_wrap::engine;
namespace GW = coop::dev::game_window;
namespace PS = coop::desk_ping_sync;
namespace R = ue_wrap::reflection;
namespace SR = ue_wrap::space_renderer;
namespace sg = ue_wrap::script_gate;

bool On() {
    static const bool on = coop::config::ResolveString(::coop::config_registry::rows::desk_ping_drill) == "run";
    return on;
}

// An equilateral triangle of circumradius kCircumradius: sides of 346, under the atlas's 740, and an inradius of 100,
// which paces a ping at about 18 s (its stages divide by the inradius over 100, plus 1).
constexpr float kCircumradius = 200.f;
// The least gap between the ping's inner circle and any other sky signal's circle, so the miss finds nothing and the
// catch finds the host's fixture alone.
constexpr float kClearance = 250.f;
// No route ends at the button, on the desk: the client walks to a point of a ring about it that one reaches.
constexpr int   kStandpoints = 6;
constexpr float kStandRingCm = 110.f;
constexpr float kStandReachCm = 60.f;
constexpr int   kWalkDeadlineS = 240;
// Failure bounds only: each step ends on the state it waits for.
constexpr uint64_t kStartBoundMs = 60000;
constexpr uint64_t kEnterBoundMs = 15000;
constexpr uint64_t kPlaceBoundMs = 45000;
constexpr uint64_t kPingStartBoundMs = 5000;
constexpr uint64_t kPingEndBoundMs = 60000;
constexpr uint64_t kVerdictBoundMs = 90000;
constexpr uint64_t kRelayBoundMs = 5000;  // the host's catch detector polls once a second
// A step that retries its reads does so this often, not every tick.
constexpr uint64_t kRetryMs = 500;
constexpr const wchar_t* kFixtureName = L"planeteater";

// ---- the census: this peer's gatherSignal bodies entered and run, and its log lines of a failed ping ----------------

struct Census {
    uint32_t entered = 0;  // gatherSignal bodies entered (a refused one included)
    uint32_t ran = 0;      // gatherSignal bodies run
    uint32_t failed = 0;   // a verdict's "Error [n] Ping failed" lines written into this desk's log
    uint32_t busy = 0;     // the host's refusals of a verdict while its desk was busy, as their line shows them
};
Census g_census;
constexpr int kTagVerdict = 0x44505230;  // 'DPR0'
constexpr int kTagLog = 0x44505231;      // 'DPR1'
bool g_watched[2] = {};
bool g_live = false;
int32_t g_lineOff = -1;  // writeToCoordLog_2's B

sg::Verdict OnVerdictPre(const sg::Call&) {
    ++g_census.entered;
    return sg::Verdict::Run;
}
void OnVerdictPost(const sg::Call&) { ++g_census.ran; }

void OnLogPost(const sg::Call& c) {
    if (g_lineOff < 0) g_lineOff = R::FindParamOffset(c.function, L"B");
    if (g_lineOff < 0 || !c.locals) return;
    const std::wstring line = ue_wrap::field_io::ReadFStringAt(c.locals, g_lineOff);
    if (line.find(L"Ping failed") == std::wstring::npos) return;
    if (line.find(L"Error [") != std::wstring::npos) ++g_census.failed;
    if (line.find(L"already pinging") != std::wstring::npos) ++g_census.busy;
}

bool g_censusDead = false;  // a watch the gate refused or that settled dead: the drill cannot count

bool CensusLive() {
    if (g_live || g_censusDead) return g_live;
    if (!g_watched[0])
        g_watched[0] = sg::WatchClassName(DG::kRendererClass, DG::kVerdict, kTagVerdict, &OnVerdictPre, &OnVerdictPost);
    if (!g_watched[1]) g_watched[1] = sg::WatchClassName(DG::kDeskClass, DG::kLogLine, kTagLog, nullptr, &OnLogPost);
    if (!g_watched[0] || !g_watched[1]) {
        g_censusDead = true;
        return false;
    }
    sg::ResolvePendingNames();
    const bool verdictLive = sg::ClassNameWatchLive(DG::kRendererClass, DG::kVerdict, kTagVerdict);
    const bool logLive = sg::ClassNameWatchLive(DG::kDeskClass, DG::kLogLine, kTagLog);
    g_live = verdictLive && logLive;
    g_censusDead = (!verdictLive && sg::ClassNameWatchSettled(DG::kRendererClass, DG::kVerdict, kTagVerdict)) ||
                   (!logLive && sg::ClassNameWatchSettled(DG::kDeskClass, DG::kLogLine, kTagLog));
    return g_live;
}

// ---- the triangle -------------------------------------------------------------------------------------------------

struct Triangle {
    float v[6] = {};  // three vertices, x then y
    float cx = 0.f, cy = 0.f;
};

Triangle TriangleAt(float cx, float cy) {
    Triangle t;
    t.cx = cx;
    t.cy = cy;
    for (int i = 0; i < 3; ++i) {
        const float a = 1.5707963f + 2.0943951f * static_cast<float>(i);  // 90, 210 and 330 degrees
        t.v[2 * i] = cx + kCircumradius * std::cos(a);
        t.v[2 * i + 1] = cy + kCircumradius * std::sin(a);
    }
    return t;
}

// The centre, over a grid of the area, whose inner circle clears every sky signal's circle by the most, apart from a
// centre used before; false when none clears kClearance.
bool PickCentre(bool haveAway, float awayX, float awayY, float& ox, float& oy) {
    float w = 0.f, h = 0.f;
    std::vector<SR::SignalRow> sky;
    if (!CP::ReadAreaSize(w, h) || !SR::ReadSignals(sky)) return false;
    float best = -1e30f;
    for (int i = 1; i < 16; ++i) {
        for (int j = 1; j < 16; ++j) {
            const float x = w * static_cast<float>(i) / 16.f, y = h * static_cast<float>(j) / 16.f;
            if (x < kCircumradius || y < kCircumradius || x > w - kCircumradius || y > h - kCircumradius) continue;
            if (haveAway && std::hypot(x - awayX, y - awayY) < 4.f * kCircumradius) continue;
            float clear = 1e30f;
            for (const SR::SignalRow& s : sky)
                clear = std::fmin(clear, std::hypot(s.x - x, s.y - y) - s.strength * 50.f - kCircumradius / 2.f);
            if (clear > best) {
                best = clear;
                ox = x;
                oy = y;
            }
        }
    }
    return best >= kClearance;
}

bool Commit(const Triangle& t) {
    CP::DishAim aim;
    if (!CP::ReadDishAim(aim)) return false;
    aim.viewX = t.cx;
    aim.viewY = t.cy;
    aim.c0X = t.v[0];
    aim.c0Y = t.v[1];
    aim.c1X = t.v[2];
    aim.c1Y = t.v[3];
    aim.c2X = t.v[4];
    aim.c2Y = t.v[5];
    return CP::WriteCursorOnly(t.cx, t.cy) && CP::WriteDishCommitted(aim);
}

bool SkyHasAt(float x, float y) {
    std::vector<SR::SignalRow> sky;
    if (!SR::ReadSignals(sky)) return false;
    for (const SR::SignalRow& s : sky)
        if (std::fabs(s.x - x) < 1.f && std::fabs(s.y - y) < 1.f) return true;
    return false;
}

// ---- the host -----------------------------------------------------------------------------------------------------

enum class HStep : uint8_t { WaitClient, Miss, Fixture, Catch, Done };
HStep    g_host = HStep::WaitClient;
uint64_t g_hostMs = 0;
uint64_t g_rolledMs = 0;  // the catch's verdict rolled; the relay follows within a detector poll
bool     g_hostPass = true;
Census   g_hostBefore;
PS::Counts g_hostCounts;
uint64_t g_hostCatches = 0;
uint64_t g_hostAttributed = 0;
int32_t  g_hostFound = 0;
CP::DishAim g_missAim;

void HostGo(HStep s) {
    g_host = s;
    g_hostMs = ::GetTickCount64();
    g_hostBefore = g_census;
    g_hostCounts = PS::CountsNow();
}

void HostAbandon(const char* why) {
    UE_LOGW("[DESK-PING-DRILL] host ABANDONED: %s", why);
    g_host = HStep::Done;
}

void HostTick(coop::net::Session* s) {
    const uint64_t now = ::GetTickCount64();
    const PS::Counts c = PS::CountsNow();
    switch (g_host) {
    case HStep::WaitClient:
        if (!s->AnyWorldReadyPeer() || !CensusLive()) {
            if (g_censusDead) { HostAbandon("the census watches did not go live"); return; }
            if (!g_hostMs) g_hostMs = now;
            if (now - g_hostMs > kStartBoundMs * 4) HostAbandon("no client's world became ready");
            return;
        }
        UE_LOGI("[DESK-PING-DRILL] host: the census is live");
        HostGo(HStep::Miss);
        return;
    case HStep::Miss: {
        if (c.rolled == g_hostCounts.rolled) return;  // the client's legs carry their own bounds
        const bool ok = c.primed == g_hostCounts.primed + 1 && c.caught == g_hostCounts.caught &&
                        g_census.ran == g_hostBefore.ran + 1;
        g_hostPass = g_hostPass && ok;
        UE_LOGI("[DESK-PING-DRILL] host miss %s: primed %u, rolled %u, caught %u; its gatherSignal ran %u",
                ok ? "PASS" : "FAIL", c.primed - g_hostCounts.primed, c.rolled - g_hostCounts.rolled,
                c.caught - g_hostCounts.caught, g_census.ran - g_hostBefore.ran);
        CP::ReadDishAim(g_missAim);
        HostGo(HStep::Fixture);
        return;
    }
    case HStep::Fixture: {
        // The catch's triangle, once the client's aim has moved from the miss's: a sky signal at its centre whose
        // circle is the ping's inner circle, so the verdict's overlap is whole.
        CP::DishAim aim;
        if (!CP::ReadDishAim(aim) || (aim.c0X == g_missAim.c0X && aim.c0Y == g_missAim.c0Y)) return;
        const float cx = (aim.c0X + aim.c1X + aim.c2X) / 3.f, cy = (aim.c0Y + aim.c1Y + aim.c2Y) / 3.f;
        std::vector<SR::SignalRow> sky;
        if (!SR::ReadSignals(sky)) { HostAbandon("the host's sky does not read"); return; }
        SR::SignalRow fix;
        fix.x = cx;
        fix.y = cy;
        fix.strength = (kCircumradius / 2.f) / 50.f;
        fix.frequency = 7.25f;
        fix.frequencySpread = 0.5f;
        fix.polaritySpread = 0.5f;
        fix.lifeTime = fix.maxLifetime = 200.f;
        fix.objectName = kFixtureName;
        sky.push_back(fix);
        SR::ApplyStats st;
        if (!SR::ApplySignalSet(sky, st) || st.added < 1) { HostAbandon("the fixture signal was not added"); return; }
        UE_LOGI("[DESK-PING-DRILL] host put '%ls' at (%.0f,%.0f) under the catch's triangle", kFixtureName, cx, cy);
        g_hostCatches = coop::signal_catch_sync::LocalCatchesRelayed();
        g_hostAttributed = coop::signal_catch_sync::AttributedCatchesRelayed();
        ue_wrap::profile::ReadSignalsFound(g_hostFound);
        g_rolledMs = 0;
        HostGo(HStep::Catch);
        return;
    }
    case HStep::Catch: {
        // The catch's verdict, the busy intent the client sends behind it, and the catch's relay as the client's,
        // which the detector makes at its next poll.
        if (c.rolled == g_hostCounts.rolled) return;
        if (!g_rolledMs) g_rolledMs = now;
        const uint64_t attributed = coop::signal_catch_sync::AttributedCatchesRelayed() - g_hostAttributed;
        if ((c.busy == g_hostCounts.busy || attributed == 0) && now - g_rolledMs <= kRelayBoundMs) return;
        int32_t found = 0;
        const bool foundRead = ue_wrap::profile::ReadSignalsFound(found);
        const uint64_t own = coop::signal_catch_sync::LocalCatchesRelayed() - g_hostCatches;
        const bool ok = c.primed == g_hostCounts.primed + 1 && c.caught == g_hostCounts.caught + 1 &&
                        g_census.ran == g_hostBefore.ran + 1 && foundRead && found == g_hostFound &&
                        attributed == 1 && own == 0 && c.busy == g_hostCounts.busy + 1;
        g_hostPass = g_hostPass && ok;
        UE_LOGI("[DESK-PING-DRILL] host catch %s: primed %u, rolled %u, caught %u; its gatherSignal ran %u; its own "
                "finds %d -> %d; catches relayed as the client's %llu, as its own %llu; busy refusals %u",
                ok ? "PASS" : "FAIL", c.primed - g_hostCounts.primed, c.rolled - g_hostCounts.rolled,
                c.caught - g_hostCounts.caught, g_census.ran - g_hostBefore.ran, g_hostFound, found,
                static_cast<unsigned long long>(attributed), static_cast<unsigned long long>(own),
                c.busy - g_hostCounts.busy);
        UE_LOGI("[DESK-PING-DRILL] host DONE %s", g_hostPass ? "PASS" : "FAIL");
        g_host = HStep::Done;
        return;
    }
    case HStep::Done:
        return;
    }
}

// ---- the client ---------------------------------------------------------------------------------------------------

enum class CStep : uint8_t { Ready, Walk, Enter, Place, Settle, Pinging, Verdict, Done };
CStep    g_client = CStep::Ready;
int      g_leg = 0;  // 0 the miss, 1 the catch
uint64_t g_stepMs = 0;
uint64_t g_readyMs = 0;
uint64_t g_tryMs = 0;  // the last try of a step that retries its reads
bool     g_pass = true;
bool     g_started = false;
unsigned g_vk = 0;
std::vector<ue_wrap::FVector> g_ring;  // the standpoints about button_coords a route reaches, shortest first
size_t   g_stand = 0;
bool     g_ringBuilt = false;
Triangle g_tri;
Triangle g_missTri;
Census   g_before;
PS::Counts g_counts;
int32_t  g_found = 0;
std::shared_ptr<coop::director::BackgroundWalk> g_walk;

void Go(CStep s) {
    g_client = s;
    g_stepMs = ::GetTickCount64();
}

void Abandon(const char* why) {
    UE_LOGW("[DESK-PING-DRILL] ABANDONED: %s", why);
    g_client = CStep::Done;
}

const char* LegName() { return g_leg == 0 ? "miss" : "catch"; }

void ClientTick(coop::net::Session* s) {
    const uint64_t now = ::GetTickCount64();
    void* desk = CD::EnsureResolved() ? CD::Instance() : nullptr;
    void* player = coop::players::Registry::Get().Local();
    switch (g_client) {
    case CStep::Ready: {
        if (!coop::net_pump::HasAnnouncedWorldReady()) return;
        if (!g_readyMs) g_readyMs = now;
        const bool settled = coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
        if (!settled || !CensusLive() || !desk || !player) {
            if (g_censusDead)
                Abandon("the census watches did not go live");
            else if (now - g_readyMs > kStartBoundMs)
                Abandon("the join, the census, the desk or the player never came");
            return;
        }
        UE_LOGI("[DESK-PING-DRILL] client: the census is live");
        Go(CStep::Walk);
        return;
    }
    case CStep::Walk: {
        void* button = desk ? DPR::Member(desk, L"button_coords") : nullptr;
        if (!button) { Abandon("the desk's button_coords does not read"); return; }
        if (!g_walk) {
            if (!g_ringBuilt) {
                g_ring = coop::director::ReachableStandpoints(player, E::GetComponentLocation(button), kStandRingCm,
                                                              kStandpoints, kStandReachCm);
                g_ringBuilt = true;
                UE_LOGI("[DESK-PING-DRILL] client: a route reaches %zu of the %d standpoints about button_coords",
                        g_ring.size(), kStandpoints);
            }
            if (g_stand >= g_ring.size()) { Abandon("no standpoint about button_coords could be walked to"); return; }
            g_walk = coop::director::StartBackgroundWalk(g_ring[g_stand], kStandReachCm, kWalkDeadlineS);
            return;
        }
        const int w = g_walk->state.load();
        if (w == 0) return;
        g_walk.reset();
        if (w == 2) {
            UE_LOGI("[DESK-PING-DRILL] client: standpoint %zu of %zu could not be walked to", g_stand + 1,
                    g_ring.size());
            ++g_stand;
            return;
        }
        if (!DPR::Press(desk, player, button)) { Abandon("the press on button_coords did not dispatch"); return; }
        Go(CStep::Enter);
        return;
    }
    case CStep::Enter: {
        if (!coop::device_occupancy::LocalHolds(L"desk")) {
            if (now - g_stepMs > kEnterBoundMs) Abandon("the press never put this player on the coordinates screen");
            return;
        }
        std::wstring key;
        if (!ue_wrap::profile::KeybindDisplayName(L"coord_ping", key)) {
            Abandon("coord_ping's key does not read");
            return;
        }
        g_vk = GW::VirtualKeyOf(key);
        if (!g_vk) {
            UE_LOGW("[DESK-PING-DRILL] client: coord_ping is bound to '%ls', which this drill cannot press",
                    key.c_str());
            Abandon("coord_ping's key has no virtual key here");
            return;
        }
        UE_LOGI("[DESK-PING-DRILL] client is on the coordinates screen; coord_ping is '%ls'", key.c_str());
        Go(CStep::Place);
        return;
    }
    case CStep::Place: {
        if (now - g_tryMs < kRetryMs) return;
        g_tryMs = now;
        float cx = 0.f, cy = 0.f;
        if (!PickCentre(g_leg == 1, g_missTri.cx, g_missTri.cy, cx, cy)) {
            if (now - g_stepMs > kPlaceBoundMs) Abandon("no centre clears every sky signal");
            return;
        }
        g_tri = TriangleAt(cx, cy);
        if (g_leg == 0) g_missTri = g_tri;
        if (!Commit(g_tri)) { Abandon("the triangle did not commit"); return; }
        UE_LOGI("[DESK-PING-DRILL] client committed the %s's triangle about (%.0f,%.0f)", LegName(), cx, cy);
        Go(CStep::Settle);
        return;
    }
    case CStep::Settle: {
        if (now - g_tryMs < kRetryMs) return;
        g_tryMs = now;
        bool canPing = false;
        const bool ready = CP::ReadCanPing(canPing) && canPing && (g_leg == 0 || SkyHasAt(g_tri.cx, g_tri.cy));
        if (!ready) {
            if (now - g_stepMs > kPlaceBoundMs)
                Abandon(g_leg == 0 ? "the panel's markers never reached the triangle"
                                   : "the host's fixture never reached this sky");
            return;
        }
        g_before = g_census;
        g_counts = PS::CountsNow();
        ue_wrap::profile::ReadSignalsFound(g_found);
        if (!GW::PostKeyPress(g_vk)) { Abandon("the key could not be posted to the game window"); return; }
        g_started = false;
        Go(CStep::Pinging);
        return;
    }
    case CStep::Pinging: {
        bool pinging = false;
        DG::ReadPinging(desk, pinging);
        if (!g_started) {
            if (pinging) { g_started = true; return; }
            if (now - g_stepMs > kPingStartBoundMs) Abandon("the key did not start a ping");
            return;
        }
        if (g_leg == 1) {
            // The busy leg goes out behind this ping's own verdict, so it finds the host's desk holding that verdict,
            // queued or rolling, whatever the host's timing.
            if (PS::CountsNow().refused == g_counts.refused) {
                if (now - g_stepMs > kPingEndBoundMs) Abandon("the ping never reached its verdict");
                return;
            }
            CP::DishAim aim;
            CP::ReadDishAim(aim);
            coop::net::DeskPingVerdictPayload p{};
            p.op = coop::net::desk_ping::kOpIntent;
            p.seq = 0xB0B0u;
            p.viewX = aim.viewX;
            p.viewY = aim.viewY;
            p.aim.c0X = aim.c0X;
            p.aim.c0Y = aim.c0Y;
            p.aim.c1X = aim.c1X;
            p.aim.c1Y = aim.c1Y;
            p.aim.c2X = aim.c2X;
            p.aim.c2Y = aim.c2Y;
            s->SendReliableToSlot(0, coop::net::ReliableKind::DeskPingVerdict, &p, static_cast<int>(sizeof(p)));
            UE_LOGI("[DESK-PING-DRILL] client sent a second verdict behind its catch's");
            Go(CStep::Verdict);
            return;
        }
        // The miss waits for this run's end: the catch's key starts a ping only on an idle machine.
        if (pinging) {
            if (now - g_stepMs > kPingEndBoundMs) Abandon("the ping never ended");
            return;
        }
        Go(CStep::Verdict);
        return;
    }
    case CStep::Verdict: {
        const PS::Counts c = PS::CountsNow();
        if (g_leg == 0) {
            if (g_census.failed == g_before.failed && now - g_stepMs <= kVerdictBoundMs) return;
            const bool ok = g_census.entered == g_before.entered + 1 && g_census.ran == g_before.ran &&
                            c.refused == g_counts.refused + 1 && c.outputsHeld > g_counts.outputsHeld &&
                            g_census.failed == g_before.failed + 1;
            UE_LOGI("[DESK-PING-DRILL] client miss %s: its gatherSignal entered %u and ran %u, refused %u, held %u; "
                    "the verdict's failure lines in its log %u", ok ? "PASS" : "FAIL",
                    g_census.entered - g_before.entered, g_census.ran - g_before.ran, c.refused - g_counts.refused,
                    c.outputsHeld - g_counts.outputsHeld, g_census.failed - g_before.failed);
            g_pass = g_pass && ok;
            g_leg = 1;
            Go(CStep::Place);
            return;
        }
        // The find, the busy refusal and the catch itself arrive on their own lanes, each in its time; the desk is read
        // once the two counts are in.
        const bool counted = c.finds > g_counts.finds && c.answered > g_counts.answered;
        CD::CoordSignal sig;
        const bool holds = counted && CD::ReadCoordSignal(sig) && sig.objectName == kFixtureName;
        if (!holds && now - g_stepMs <= kVerdictBoundMs) return;
        if (!counted) CD::ReadCoordSignal(sig);
        int32_t found = 0;
        const bool ok = g_census.entered == g_before.entered + 1 && g_census.ran == g_before.ran &&
                        c.refused == g_counts.refused + 1 && c.finds == g_counts.finds + 1 &&
                        ue_wrap::profile::ReadSignalsFound(found) && found == g_found + 1 && holds;
        UE_LOGI("[DESK-PING-DRILL] client catch %s: its gatherSignal entered %u and ran %u, refused %u; finds %u, its "
                "own count %d -> %d; its desk holds '%ls'", ok ? "PASS" : "FAIL", g_census.entered - g_before.entered,
                g_census.ran - g_before.ran, c.refused - g_counts.refused, c.finds - g_counts.finds, g_found, found,
                sig.objectName.empty() ? L"nothing" : sig.objectName.c_str());
        const bool busyOk = c.answered == g_counts.answered + 1 && g_census.busy == g_before.busy + 1;
        UE_LOGI("[DESK-PING-DRILL] client busy %s: refusals shown %u, busy lines %u", busyOk ? "PASS" : "FAIL",
                c.answered - g_counts.answered, g_census.busy - g_before.busy);
        g_pass = g_pass && ok && busyOk;
        UE_LOGI("[DESK-PING-DRILL] client DONE %s", g_pass ? "PASS" : "FAIL");
        g_client = CStep::Done;
        return;
    }
    case CStep::Done:
        return;
    }
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (!On() || !session || !session->running()) return;
    if (session->role() == coop::net::Role::Host) {
        HostTick(session);
        return;
    }
    if (session->connected()) ClientTick(session);
}

void OnDisconnect() {
    g_host = HStep::WaitClient;
    g_hostMs = 0;
    g_rolledMs = 0;
    g_hostPass = true;
    g_client = CStep::Ready;
    g_leg = 0;
    g_readyMs = 0;
    g_tryMs = 0;
    g_pass = true;
    g_walk.reset();
    g_ring.clear();
    g_stand = 0;
    g_ringBuilt = false;
}

}  // namespace coop::dev::desk_ping_drill
