// coop/dev/swinger_drill.cpp -- see coop/dev/swinger_drill.h.

#include "coop/dev/swinger_drill.h"

#include "coop/comms/chat_feed.h"
#include "coop/comms/chat_sync.h"
#include "coop/config/config.h"
#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/director/director.h"
#include "coop/dev/director/routes.h"
#include "coop/dev/drill_chat_pace.h"
#include "coop/dev/swinger_drill_join.h"
#include "coop/dev/swinger_drill_pose.h"
#include "coop/element/portable_identity.h"
#include "coop/dev/toggle_drill.h"
#include "coop/interactables/interactable_sync.h"
#include "coop/interactables/swinger_join_pose.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/swinger.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/engine/engine_physics.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace coop::dev::swinger_drill {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace PR = ue_wrap::prop;
namespace SW = ue_wrap::swinger;
namespace J  = coop::dev::swinger_drill_join;
namespace PS = coop::dev::swinger_drill_pose;
using Clock = std::chrono::steady_clock;
// The tolerances and the distance are shared with the joiner arm (swinger_drill_join.h).
using J::Dist; using J::kRestDeg; using J::kRestCm; using J::kSettleDeg; using J::kSettleCm;
using J::kSampleEvery; using J::kClientBound;

enum class Arm { Off, Measure, Observe, Rejoin };
enum class Step { Pick, WalkTo, Grab, Drag, Release, Tail, Echo, Done, Invalid };

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::swinger_drill);
        return v == "measure" ? Arm::Measure : v == "observe" ? Arm::Observe : v == "rejoin" ? Arm::Rejoin : Arm::Off;
    }();
    return a;
}

bool Rejoin() { return ArmOf() == Arm::Rejoin; }   // measure for life 1, then the joiner arm (swinger_drill_join.h)

// The acting peer: the first client of the measure and rejoin arms. The host, the observe arm and any later client only watch.
bool Acts() {
    return (ArmOf() == Arm::Measure || Rejoin()) && !coop::roster::LocalIsHost() &&
           coop::players::Registry::Get().LocalPeerId() <= 1;
}
const char* Role() {
    if (coop::roster::LocalIsHost()) return "host";
    return ArmOf() == Arm::Observe || coop::players::Registry::Get().LocalPeerId() > 1 ? "observer" : "client";
}

constexpr int   kPickRoutes     = 6;      // the nearest lids whose route is asked (each is up to 16 path queries)
constexpr float kRouteEndReachCm = 160.f; // a route that ends further from the lid reaches no lid
constexpr float kReachCm        = 130.f;  // the director's stop distance from the lid, level
constexpr int   kWalkDeadlineS  = 300;    // a long first walk from a New Game start (the fireext drill's)
// The feed (see the drag below): it starts at kFeedStart per axis and doubles every kFeedEvery* ticks, kFeedDoublings
// times, to the cap; once the client's own door has turned kSwingDeg it holds for kHoldAfterSwingTicks more.
constexpr float kFeedStart      = 3.f;
constexpr int   kFeedDoublings  = 11;     // 3 * 2^11 = 6144
constexpr int   kFeedEveryFirst = 30;
constexpr int   kFeedEveryLater = 10;
constexpr int   kHoldAfterSwingTicks = 60;
constexpr auto  kScanEvery      = std::chrono::milliseconds(100);
constexpr auto  kTail           = std::chrono::seconds(5);   // how long a lid that stopped moving is still sampled
constexpr float kMovedDeg       = 0.5f;   // a mesh rotation that changed by more than this has moved
constexpr const char* kTag      = "[swing-drill] ";   // the chat lines: grab, settled, verdict
// The verdict. The client's door must turn kSwingDeg or the run is INVALID; a peer passes when its own door turned
// kFollowDeg during the drag and rests within kSettleDeg of where the client's door rested.
constexpr float kSwingDeg       = 15.f;
constexpr float kFollowDeg      = 10.f;
constexpr auto  kJudgeBound     = std::chrono::seconds(60);   // a watcher that hears nothing new this long judges as it stands

// ---- The lids: every peer watches every non-lockable lid its world holds ------------------------------

struct Reading {
    ue_wrap::FVector loc{};
    ue_wrap::FRotator actorRot{}, meshRel{}, meshWorld{};
    int opened = -1, sim = -1;             // -1: unread
    PS::ParentPose parent;                 // the prop the lid hangs on, as it stands at this read
};

struct Lid {
    ue_wrap::CachedObjRef ref;
    std::wstring laneKey, propKey, ident;   // the lane's key, the game's Key, the portable identity's readable form
    bool perProcessKey = false;            // a child of a keyed prop (`k:<parent>/<part>`): its game Key differs
    ue_wrap::FRotator scanned{};           // the mesh's world rotation at the last scan that saw it move
    ue_wrap::FRotator restRot{};           // its rotation while the lid was not moving: where a move starts from
    ue_wrap::FRotator armRot{};            // its rotation when the drill armed: where the door began, never updated
    float maxDev = 0.f;                    // the largest angle from restRot since the lid last started moving
    bool tracked = false;
    Clock::time_point lastMoveAt{}, nextSample{};
};

std::vector<Lid> g_lids;
bool g_armed = false;
Clock::time_point g_nextScan{};

Reading Read(void* d) {
    Reading r;
    E::TryGetActorLocation(d, r.loc);
    E::TryGetActorRotation(d, r.actorRot);
    if (void* mesh = PR::GetStaticMesh(d)) {
        r.meshWorld = E::GetComponentWorldRotation(mesh);
        r.sim = E::IsComponentSimulatingPhysics(mesh) ? 1 : 0;
        SW::TryReadMeshRelativeRotation(d, r.meshRel);
    }
    bool open = false;
    if (SW::TryReadOpen(d, open)) r.opened = open ? 1 : 0;
    r.parent = PS::ReadParent(d);
    return r;
}

void Say(const char* what, const Lid& l, const Reading& r) {
    UE_LOGI("[SWING-DRILL] %s %s key='%ls': actor rot (%.2f, %.2f, %.2f) loc (%.1f, %.1f, %.1f) | mesh rel rot "
            "(%.2f, %.2f, %.2f) world rot (%.2f, %.2f, %.2f) | opened %d simulating %d | %s",
            Role(), what, l.laneKey.c_str(), r.actorRot.Pitch, r.actorRot.Yaw, r.actorRot.Roll, r.loc.X, r.loc.Y,
            r.loc.Z, r.meshRel.Pitch, r.meshRel.Yaw, r.meshRel.Roll, r.meshWorld.Pitch, r.meshWorld.Yaw,
            r.meshWorld.Roll, r.opened, r.sim, PS::Describe(r.parent).c_str());
}

// One walk of the object array, when the drill arms, never again.
void ArmWatch() {
    g_lids.clear();
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (!o || !R::IsLive(o) || !coop::dev::toggle_drill::IsSteadyLid(o)) continue;
        if (R::NameStartsWith(R::NameOf(o), L"Default__") || !PR::GetStaticMesh(o)) continue;   // a lid has a mesh
        Lid l;
        l.ref.Set(o);
        l.laneKey = coop::interactable_sync::ContainerKey(o);
        if (l.laneKey.empty()) continue;   // the lane does not index it: no peer could name it
        l.propKey = PR::GetInteractableKeyString(o);
        l.ident = coop::element::PortableIdentity(o);
        l.perProcessKey = l.ident.rfind(L"k:", 0) == 0 && l.ident.find(L'/') != std::wstring::npos;
        const Reading r = Read(o);
        l.scanned = l.restRot = l.armRot = r.meshWorld;
        g_lids.push_back(std::move(l));
        Say("lid", g_lids.back(), r);
    }
    if (g_lids.empty() && coop::roster::LocalIsHost()) return;   // not latched: the host retries (J::HostArmDue)
    g_armed = true;
    g_nextScan = Clock::now();
    UE_LOGI("[SWING-DRILL] %s armed: %zu non-lockable lid(s) watched", Role(), g_lids.size());
}

// Held-prop receive counters since the last line (the host and the observer).
struct Rx {
    uint32_t door = 0, other = 0, unresolved = 0;
    int slot = -1;
    std::wstring key;
    uint32_t eid = 0;
    void* actor = nullptr;
    void* mesh = nullptr;
    ue_wrap::FVector loc{};
    ue_wrap::FRotator rot{};
};
Rx g_rx;

void PrintRx() {
    if (g_rx.door == 0 && g_rx.other == 0 && g_rx.unresolved == 0) return;
    UE_LOGI("[SWING-DRILL] %s propPose rx: to a swinger %u, to another prop %u, unresolved %u | last slot %d key='%ls' "
            "eid=%u -> the ACTOR %p (SetActorLocation + SetActorRotation on its root, mesh %p, simulating %d) target "
            "loc (%.1f, %.1f, %.1f) rot (%.2f, %.2f, %.2f)",
            Role(), g_rx.door, g_rx.other, g_rx.unresolved, g_rx.slot, g_rx.key.c_str(), g_rx.eid, g_rx.actor,
            g_rx.mesh, g_rx.mesh && R::IsLive(g_rx.mesh) && E::IsComponentSimulatingPhysics(g_rx.mesh) ? 1 : 0,
            g_rx.loc.X, g_rx.loc.Y, g_rx.loc.Z, g_rx.rot.Pitch, g_rx.rot.Yaw, g_rx.rot.Roll);
    g_rx.door = g_rx.other = g_rx.unresolved = 0;
}

// ---- The client's chat lines ---------------------------------------------------------------------------
//
// The client marks its phases in the lobby chat, the one reliable channel every peer already reads: a line when it
// grabs the door (the lane's key for it and the game's own Key), and one when its door has settled after the
// release, with the rotation it rests at. A watching peer answers with a verdict line. The poses are unreliable and
// carry no phase, so a watching peer judges on these lines and never on a gap in the stream. The host admits a
// client's chat at a fixed rate, so a peer's lines go out one per kTalkEveryMs.

enum class Phase { Grab, Settled, Verdict };
struct ChatEvent {
    unsigned long long key;
    Phase what;
    std::string mv, gameKey;   // a Grab line's two keys; a Verdict line's role and result
    ue_wrap::FRotator rot{};   // a Settled line's rotation and place: where the client's door came to rest
    ue_wrap::FVector at{};
    PS::ParentPose parent;     // a Settled line's parent pose: where the prop the client's door hangs on stood
};

std::vector<std::string> g_outbox;
size_t g_outNext = 0;
uint64_t g_nextSayMs = 0;
unsigned long long g_chatMark = 0;   // the feed's newest row when the drill armed: older rows are another run's
bool g_hostVerdict = false;   // the client has seen the host's verdict line

uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
}

void SayLine(const std::string& rest) { g_outbox.push_back(std::string(kTag) + rest); }

void PumpOutbox() {
    const uint64_t now = NowMs();
    if (g_outNext >= g_outbox.size() || now < g_nextSayMs) return;
    coop::chat_sync::QueueSend(g_outbox[g_outNext++]);
    g_nextSayMs = now + coop::dev::kTalkEveryMs;
    if (g_outNext == g_outbox.size()) { g_outbox.clear(); g_outNext = 0; }
}

void MarkChat() {
    coop::chat_feed::ForEachRow([](const coop::chat_feed::RowView& r) { g_chatMark = (std::max)(g_chatMark, r.key); });
}

// The drill's lines newer than the mark, oldest first.
std::vector<ChatEvent> NewChat() {
    std::vector<ChatEvent> out;
    unsigned long long top = g_chatMark;
    coop::chat_feed::ForEachRow([&](const coop::chat_feed::RowView& r) {
        const size_t at = r.line.find(kTag);
        if (r.key <= g_chatMark || at == std::string_view::npos || (Rejoin() && r.retained)) return;   // a past life's row
        top = (std::max)(top, r.key);
        const std::string rest(r.line.substr(at + std::strlen(kTag)));
        ChatEvent e{r.key, Phase::Verdict, {}, {}, {}};
        // "grab <lane key> <game Key>", "settled ..." (PS::SettledLine), "verdict <role> <PASS|FAIL|INVALID>". The
        // host's word to a world-ready client (go, rest, void) is read by J::PollWord, not here.
        const std::vector<std::string> word = J::SplitWords(rest);
        if (word.size() == 3 && word[0] == "grab") {
            if (word[1].empty() || word[2].empty()) {   // an empty key would match the first lid that has none
                UE_LOGW("[SWING-DRILL] %s refuses a grab line with an empty key", Role());
                return;
            }
            e.what = Phase::Grab;
            e.mv = word[1];
            e.gameKey = word[2];
        } else if (PS::ParseSettled(word, &e.rot, &e.at, &e.parent)) {
            e.what = Phase::Settled;
        } else if (word.size() == 3 && word[0] == "verdict") {
            e.mv = word[1];
            e.gameKey = word[2];
        } else {
            return;
        }
        out.push_back(std::move(e));
    });
    g_chatMark = top;
    std::sort(out.begin(), out.end(), [](const ChatEvent& a, const ChatEvent& b) { return a.key < b.key; });
    return out;
}

// ---- The verdict of a watching peer -------------------------------------------------------------------
//
// What the client's door did is read from the poses this peer takes for it (the held-item stream carries the
// client's own door transform every tick): the poses before the client's `settled` line are its drag. That line
// carries the rotation the client's door came to rest at. This peer judges once its own copy has come to rest too:
// simulating again (the stream's release unparked it) and the same on two scans in a row. The two orientations are
// compared as the angle between their quaternions, never component by component: near pitch +-90 degrees the
// pitch, yaw and roll of one orientation are not unique.

struct Verdict {
    bool started = false, said = false, settledSeen = false, heard = false, haveRead = false;
    Clock::time_point lastEvent{};
    ue_wrap::FRotator first{};        // the client's door at the first pose
    ue_wrap::FRotator clientRest{};   // where the client's `settled` line says its door came to rest,
    ue_wrap::FVector clientAt{};      // in orientation and in place
    PS::ParentPose clientParent;      // and where its door's parent prop stood then
    ue_wrap::FRotator lastRead{};     // this peer's door at the last settle read
    ue_wrap::FVector lastAt{};
    Clock::time_point nextRead{};
    float clientDev = 0.f;            // the largest angle of the client's door from `first`
    int lid = -1;                     // this peer's copy of the door, once a pose resolved to it
    uint32_t resolved = 0, unresolved = 0;
};
Verdict g_v;

void Judge(bool timedOut) {
    g_v.said = true;
    PrintRx();
    const Lid* l = g_v.lid >= 0 ? &g_lids[static_cast<size_t>(g_v.lid)] : nullptr;
    void* door = l ? l->ref.Get() : nullptr;
    void* mesh = door ? PR::GetStaticMesh(door) : nullptr;
    const ue_wrap::FRotator mine = mesh ? E::GetComponentWorldRotation(mesh) : ue_wrap::FRotator{};
    ue_wrap::FVector mineAt{};
    if (door) E::TryGetActorLocation(door, mineAt);
    const float fromArmDeg = mesh ? E::RotatorAngleDeg(mine, l->armRot) : -1.f;   // how far the door moved overall
    const char* result = "INVALID";
    if (g_v.clientDev < kSwingDeg) {
        UE_LOGW("[SWING-DRILL] %s INVALID the client's door turned only %.1f deg on the stream (need %.0f)", Role(),
                g_v.clientDev, kSwingDeg);
    } else {
        const float hostDev = l ? l->maxDev : 0.f;
        const bool haveRest = mesh && g_v.settledSeen && !timedOut;
        const float settle = haveRest ? E::RotatorAngleDeg(mine, g_v.clientRest) : 180.f;
        const float apart = haveRest ? Dist(mineAt, g_v.clientAt) : -1.f;
        const PS::ParentPose mineParent = PS::ReadParent(door);
        std::string parentWhy;
        const bool parentOk = PS::ParentsAgree(mineParent, g_v.clientParent, &parentWhy);
        uint32_t parentRows = 0;
        std::string syncWhy = "judged on the host only";
        const bool synced = !coop::roster::LocalIsHost() || PS::ParentSynced(door, &parentRows, &syncWhy);
        result = haveRest && hostDev >= kFollowDeg && settle <= kSettleDeg && apart <= kSettleCm && parentOk && synced
                     ? "PASS" : "FAIL";
        UE_LOGI("[SWING-DRILL] %s %s: follow -- this peer's door turned %.1f deg of the client's %.1f (need >= %.0f) | "
                "settle -- this peer's door at (%.2f, %.2f, %.2f) place (%.1f, %.1f, %.1f), the client's at (%.2f, %.2f, "
                "%.2f) place (%.1f, %.1f, %.1f): %.2f deg between the two orientations (need <= %.0f), %.1f cm apart "
                "(need <= %.0f)%s | poses resolved %u, unresolved %u | rests %.1f deg from where it began | %s | %s | "
                "parent test %s (%s, need <= %.0f cm and %.0f deg) | parent synced: %s (%s), parent rows %u", Role(),
                result, hostDev, g_v.clientDev, kFollowDeg, mine.Pitch, mine.Yaw, mine.Roll, mineAt.X, mineAt.Y,
                mineAt.Z, g_v.clientRest.Pitch, g_v.clientRest.Yaw, g_v.clientRest.Roll, g_v.clientAt.X,
                g_v.clientAt.Y, g_v.clientAt.Z, settle, kSettleDeg, apart, kSettleCm,
                timedOut ? " (no settled line, or this peer's door never came to rest)" : "", g_v.resolved,
                g_v.unresolved, fromArmDeg, PS::Describe(mineParent).c_str(),
                PS::VerdictParent(mineParent, g_v.clientParent).c_str(), parentOk ? "ok" : "FAIL", parentWhy.c_str(),
                kSettleCm, kSettleDeg, synced ? "ok" : "FAIL", syncWhy.c_str(), parentRows);
    }
    J::NoteVerdict(l ? l->laneKey : std::wstring(), result, fromArmDeg);
    SayLine(std::string("verdict ") + Role() + " " + result);
    UE_LOGI("[SWING-DRILL] %s DONE", Role());
}

// A PropPose from the client arrived: `lid` is this peer's copy of the door when it resolved to one, else -1.
void PoseArrived(const ue_wrap::FRotator& rot, int lid) {
    if (Acts() || g_v.said) return;
    if (lid >= 0) { g_v.lid = lid; ++g_v.resolved; } else { ++g_v.unresolved; }
    g_v.lastEvent = Clock::now();
    g_v.heard = true;
    if (!g_v.started) {
        g_v.started = true;
        g_v.first = rot;
    } else if (!g_v.settledSeen) {
        g_v.clientDev = (std::max)(g_v.clientDev, E::RotatorAngleDeg(rot, g_v.first));
    }
}

// The client's key and this peer's, for the same lid: the proof that a lid whose game Key differs between
// processes is the one dragged.
void CheckKey(const ChatEvent& e) {
    for (const Lid& l : g_lids) {
        if (l.laneKey != std::wstring(e.mv.begin(), e.mv.end())) continue;
        const std::wstring theirs(e.gameKey.begin(), e.gameKey.end());
        UE_LOGI("[SWING-DRILL] %s key check: lane key '%s' -- this peer's game Key '%ls', the client's '%ls': %s", Role(),
                e.mv.c_str(), l.propKey.c_str(), theirs.c_str(), l.propKey == theirs ? "MATCH" : "DIFFER");
        return;
    }
    UE_LOGW("[SWING-DRILL] %s key check: no lid here carries the lane key '%s'", Role(), e.mv.c_str());
}

void OnChat(const ChatEvent& e) {
    if (e.what == Phase::Verdict) {   // the client is done once the host has judged
        if (Acts() && e.mv == "host") g_hostVerdict = true;
        return;
    }
    if (Acts() || g_v.said) return;
    g_v.lastEvent = Clock::now();
    g_v.heard = true;
    if (e.what == Phase::Grab) {
        CheckKey(e);
    } else {
        g_v.settledSeen = true;
        g_v.clientRest = e.rot;
        g_v.clientAt = e.at;
        g_v.clientParent = e.parent;
        if (g_v.lid < 0) Judge(false);   // no pose reached a door here: nothing to wait for
    }
}

// This peer's door at rest after the client's `settled` line: simulating (the release unparked it), and two reads a
// sample apart the same in place and in orientation.
void WatchRest(Clock::time_point now) {
    if (Acts() || !g_v.settledSeen || g_v.said || g_v.lid < 0 || now < g_v.nextRead) return;
    g_v.nextRead = now + kSampleEvery;
    void* door = g_lids[static_cast<size_t>(g_v.lid)].ref.Get();
    void* mesh = door ? PR::GetStaticMesh(door) : nullptr;
    ue_wrap::FVector at{};
    if (!mesh || !E::TryGetActorLocation(door, at) || !E::IsComponentSimulatingPhysics(mesh)) {
        g_v.haveRead = false;
        return;
    }
    const ue_wrap::FRotator w = E::GetComponentWorldRotation(mesh);
    if (g_v.haveRead && E::RotatorAngleDeg(w, g_v.lastRead) <= kRestDeg && Dist(at, g_v.lastAt) <= kRestCm) {
        Judge(false);
        return;
    }
    g_v.lastRead = w;
    g_v.lastAt = at;
    g_v.haveRead = true;
}

// Every lid every scan; a lid that moved is sampled every 250 ms until a tail after its last move.
void TickWatch(Clock::time_point now) {
    if (now >= g_nextScan) {
        g_nextScan = now + kScanEvery;
        for (const ChatEvent& e : NewChat()) OnChat(e);
        WatchRest(now);
        for (Lid& l : g_lids) {
            void* d = l.ref.Get();
            void* mesh = d ? PR::GetStaticMesh(d) : nullptr;
            if (!mesh) continue;
            const ue_wrap::FRotator w = E::GetComponentWorldRotation(mesh);
            if (l.tracked) l.maxDev = (std::max)(l.maxDev, E::RotatorAngleDeg(w, l.restRot));
            else if (E::RotatorAngleDeg(w, l.scanned) <= kMovedDeg) l.restRot = w;
            if (E::RotatorAngleDeg(w, l.scanned) <= kMovedDeg) continue;
            l.scanned = w;
            l.lastMoveAt = now;
            if (!l.tracked) {
                l.tracked = true;
                l.maxDev = E::RotatorAngleDeg(w, l.restRot);
                l.nextSample = now;
            }
        }
    }
    for (Lid& l : g_lids) {
        if (!l.tracked) continue;
        void* d = l.ref.Get();
        if (!d) { l.tracked = false; continue; }
        if (now >= l.nextSample) {
            l.nextSample = now + kSampleEvery;
            Say("sample", l, Read(d));
        }
        if (now - l.lastMoveAt > kTail) {
            l.tracked = false;
            Say("settled", l, Read(d));
        }
    }
    static Clock::time_point nextRx{};
    if (now >= nextRx) {
        nextRx = now + kSampleEvery;
        PrintRx();
    }
    // No settled line, or this peer's door never came to rest: the verdict is read as it stands.
    if (!Acts() && g_v.heard && !g_v.said && now - g_v.lastEvent > kJudgeBound) Judge(true);
}

// ---- The acting client ---------------------------------------------------------------------------------

Step g_step = Step::Pick;
int g_dragTicks = 0;
ue_wrap::CachedObjRef g_target;
size_t g_targetLid = 0;
ue_wrap::FVector g_targetAt{};
std::shared_ptr<coop::director::BackgroundWalk> g_walk;
std::unique_ptr<coop::director::AimedGrab> g_grab;
Clock::time_point g_nextRead{}, g_stepSince{};
ue_wrap::FRotator g_lastRead{};   // the door at the last settle read
ue_wrap::FVector g_lastAt{};
bool g_haveRead = false;
bool g_releaseEdge = false;   // the held-item stream has sent the door's release
ue_wrap::FRotator g_grabRot{};   // the client's door at the grab: what its swing is measured from
float g_peakDev = 0.f;           // the largest angle the client's door has turned from g_grabRot

void Invalid(const char* why) {
    UE_LOGW("[SWING-DRILL] client INVALID %s", why);
    g_step = Step::Invalid;
}

void Go(Step s) {
    g_step = s;
    g_stepSince = Clock::now();
}

void EndActor() {
    g_step = Step::Done;
    g_walk.reset();
    g_grab.reset();
}

void LogTarget(const char* what) {
    void* d = g_target.Get();
    if (!d) { UE_LOGI("[SWING-DRILL] client %s -- the door is gone", what); return; }
    Say(what, g_lids[g_targetLid], Read(d));
}

// ---- The drag --------------------------------------------------------------------------------------------
//
// The client feeds the player's input_mouseXY dispatcher while it holds the door. A non-lockable lid reads
// `opened 1` and its spring holds it at the open limit, so a feed that pushes into that limit moves nothing: the
// sign is searched. The first sign ramps slowly from kFeedStart to kFeedCap; each later one ramps faster. The
// door's physics stays on throughout, since the grab's physics handle needs a simulating body. The held-item
// stream then carries the door's motion as it carries any held actor's.

struct FeedSign { float x, y; };
constexpr FeedSign kSigns[] = {{-1.f, -1.f}, {1.f, -1.f}, {-1.f, 1.f}, {1.f, 1.f}};
constexpr int kSignCount = static_cast<int>(sizeof(kSigns) / sizeof(kSigns[0]));

int   g_sign = 0, g_signTicks = 0;   // the sign being fed, and how long
bool  g_swung = false;
int   g_swungAtTick = 0;
float g_feedX = 0.f, g_feedY = 0.f;  // the feed now

// Ticks per doubling of the feed on the sign now, and ticks the sign runs: the ramp to the cap, then one more
// doubling's worth at the cap.
int FeedEvery() { return g_sign == 0 ? kFeedEveryFirst : kFeedEveryLater; }
int FeedTicks() { return (kFeedDoublings + 1) * FeedEvery(); }

// The lid, among `ids` (nearest first, at most kPickRoutes of them), whose NavMesh route is shortest; false when
// none is reached.
bool PickAmong(void* player, const ue_wrap::FVector& me, std::vector<std::pair<float, size_t>>& ids, float& best) {
    std::sort(ids.begin(), ids.end());
    if (static_cast<int>(ids.size()) > kPickRoutes) ids.resize(kPickRoutes);
    for (const auto& o : ids) {
        void* d = g_lids[o.second].ref.Get();
        ue_wrap::FVector centre{}, extent{};
        if (!d || (!E::GetActorBounds(d, /*onlyColliding=*/true, centre, extent) && !E::TryGetActorLocation(d, centre)))
            continue;
        float len = 0.f;
        if (!coop::director::RouteInLegs(player, me, centre, kRouteEndReachCm, &len)) continue;
        if (len < best) { best = len; g_target.Set(d); g_targetLid = o.second; g_targetAt = centre; }
    }
    return g_target.Raw() != nullptr;
}

// The lid to drag. A lid whose game Key differs between the two processes (a child of a keyed prop, `k:` in its
// portable identity) goes first: it is the class the portable key exists for, so a green run covers it. Failing
// one a route reaches, the nearest others.
bool PickTarget(void* player, const char*& why) {
    ue_wrap::FVector me{};
    if (!E::TryGetActorLocation(player, me)) { why = "the player's location could not be read"; return false; }
    why = "no non-lockable lid among the nearest few that a route reaches";
    std::vector<std::pair<float, size_t>> perProcess, others;
    bool covered = false;   // a lid the joiner send covers exists (the rejoin arm picks only those)
    for (size_t i = 0; i < g_lids.size(); ++i) {
        if (Rejoin() && !coop::swinger_join_pose::SendsRestPose(g_lids[i].laneKey)) continue;
        covered = true;
        void* d = g_lids[i].ref.Get();
        ue_wrap::FVector at{};
        if (d && E::TryGetActorLocation(d, at)) (g_lids[i].perProcessKey ? perProcess : others).emplace_back(Dist(me, at), i);
    }
    if (!covered && !g_lids.empty())
        why = "no non-lockable lid has a lane key the joiner send covers (an mv_ key of at most 31 characters)";
    float best = 1e30f;
    if (!PickAmong(player, me, perProcess, best) && !PickAmong(player, me, others, best)) return false;
    UE_LOGI("[SWING-DRILL] client target key='%ls' identity '%ls' game Key '%ls'%s at (%.1f, %.1f, %.1f), a %.0f cm route "
            "(%.0f cm straight)", g_lids[g_targetLid].laneKey.c_str(), g_lids[g_targetLid].ident.c_str(),
            g_lids[g_targetLid].propKey.c_str(),
            g_lids[g_targetLid].perProcessKey ? " (a key that differs between processes)" : "", g_targetAt.X,
            g_targetAt.Y, g_targetAt.Z, best, Dist(g_targetAt, me));
    return true;
}

void TickActor(Clock::time_point now) {
    if (g_step == Step::Invalid || g_step == Step::Done) return;
    void* player = coop::players::Registry::Get().Local();
    if (!player || !R::IsLive(player) || !E::GetController(player)) return;
    switch (g_step) {
    case Step::Pick: {
        if (Rejoin() && !J::HeardGo()) return;   // the host's word first: a `rest` or a `void` ends this life's drag
        const char* why = "";
        if (!PickTarget(player, why)) { Invalid(why); return; }
        g_walk = coop::director::StartBackgroundWalk(g_targetAt, kReachCm, kWalkDeadlineS, /*carry=*/false);
        Go(Step::WalkTo);
        return;
    }
    case Step::WalkTo: {
        const int st = g_walk ? g_walk->state.load() : 2;
        if (st == 0) return;
        if (st == 2) { Invalid("the walk to the lid did not arrive"); return; }
        LogTarget("before-grab");
        g_grab = std::make_unique<coop::director::AimedGrab>(player, g_target.Get());
        Go(Step::Grab);
        return;
    }
    case Step::Grab: {
        if (!g_target.Get()) { Invalid("the door died before the grab"); return; }
        const coop::director::GrabState gs = g_grab->Tick();
        if (gs == coop::director::GrabState::Working) return;
        UE_LOGI("[SWING-DRILL] client the trace took the door after %d fan pose(s); grab chain: playerGrabbed_pre=%d "
                "useAction=%d playerGrabbed=%d", g_grab->AimPoses(), g_grab->ChainPre() ? 1 : 0,
                g_grab->ChainUse() ? 1 : 0, g_grab->ChainPost() ? 1 : 0);
        if (gs == coop::director::GrabState::Failed) { Invalid(g_grab->Why()); return; }
        Lid& l = g_lids[g_targetLid];
        l.tracked = true;
        l.lastMoveAt = l.nextSample = now;
        g_grabRot = Read(g_target.Get()).meshWorld;
        g_peakDev = 0.f;
        g_sign = g_signTicks = g_swungAtTick = 0;
        g_swung = false;
        LogTarget("grabbed");
        SayLine("grab " + std::string(l.laneKey.begin(), l.laneKey.end()) + " " +
                std::string(l.propKey.begin(), l.propKey.end()));
        g_dragTicks = 0;
        Go(Step::Drag);
        return;
    }
    case Step::Drag: {
        void* d = g_target.Get();
        if (!d || coop::director::Grabbing(player) != d) {
            UE_LOGW("[SWING-DRILL] client the hand lost the door after %d tick(s) of the drag", g_dragTicks);
            Go(Step::Release);
            return;
        }
        // The feed on this sign now: the ramp, or the value the door turned at once it has.
        if (!g_swung) {
            const float mag = (std::min)(kFeedStart * std::exp2(static_cast<float>(g_signTicks / FeedEvery())),
                                         kFeedStart * std::exp2(static_cast<float>(kFeedDoublings)));
            g_feedX = kSigns[g_sign].x * mag;
            g_feedY = kSigns[g_sign].y * mag;
        }
        int delivered = 0;
        if (!E::BroadcastMainPlayerMouseXY(player, g_feedX, g_feedY, &delivered)) {
            Invalid("the player's input_mouseXY dispatcher did not resolve");
            return;
        }
        if (g_signTicks == 0 || g_signTicks % 30 == 0)
            UE_LOGI("[SWING-DRILL] client feed (%+.0f, %+.0f) tick %d: (%.1f, %.1f) ran %d bound entr%s, the door has "
                    "turned %.1f deg", kSigns[g_sign].x, kSigns[g_sign].y, g_signTicks, g_feedX, g_feedY, delivered,
                    delivered == 1 ? "y" : "ies", g_peakDev);
        if (void* mesh = PR::GetStaticMesh(d))
            g_peakDev = (std::max)(g_peakDev, E::RotatorAngleDeg(E::GetComponentWorldRotation(mesh), g_grabRot));
        g_lids[g_targetLid].lastMoveAt = now;
        ++g_dragTicks;
        ++g_signTicks;
        if (!g_swung && g_peakDev >= kSwingDeg) {
            g_swung = true;
            g_swungAtTick = g_dragTicks;
            UE_LOGI("[SWING-DRILL] client SWUNG: sign (%+.0f, %+.0f) moved the door %.1f deg at (%.1f, %.1f), tick %d "
                    "of that sign", kSigns[g_sign].x, kSigns[g_sign].y, g_peakDev, g_feedX, g_feedY, g_signTicks);
        }
        if (g_swung) {
            if (g_dragTicks >= g_swungAtTick + kHoldAfterSwingTicks) Go(Step::Release);
        } else if (g_signTicks >= FeedTicks()) {
            UE_LOGI("[SWING-DRILL] client sign (%+.0f, %+.0f) did not move the door (peak %.1f deg)", kSigns[g_sign].x,
                    kSigns[g_sign].y, g_peakDev);
            if (g_sign + 1 < kSignCount) { ++g_sign; g_signTicks = 0; }
            else Go(Step::Release);
        }
        return;
    }
    case Step::Release: {
        coop::director::CallOnPlayer(player, L"dropGrabObject");
        g_releaseEdge = false;   // the stream's release is the first tick after this that the door is not held
        LogTarget("released");
        g_lids[g_targetLid].lastMoveAt = now;
        if (!g_swung) {
            char why[160];
            snprintf(why, sizeof(why), "no feed sign moved the client's own door: it turned only %.1f deg (need %.0f)",
                     g_peakDev, kSwingDeg);
            Invalid(why);
            return;
        }
        UE_LOGI("[SWING-DRILL] client released: the door turned %.1f deg at its peak", g_peakDev);
        g_haveRead = false;
        Go(Step::Tail);
        return;
    }
    case Step::Tail: {   // at rest: after the stream's release, two reads a sample apart agree in place and orientation
        if (now - g_stepSince > kClientBound) { Invalid("the door never read unchanged within the bound"); return; }
        if (!g_releaseEdge || now < g_nextRead) return;
        g_nextRead = now + kSampleEvery;
        void* d = g_target.Get();
        void* mesh = d ? PR::GetStaticMesh(d) : nullptr;
        ue_wrap::FVector at{};
        if (!mesh || !E::TryGetActorLocation(d, at)) { Invalid("the door has no mesh or place to read"); return; }
        const ue_wrap::FRotator w = E::GetComponentWorldRotation(mesh);
        const float turned = g_haveRead ? E::RotatorAngleDeg(w, g_lastRead) : -1.f;
        const float moved = g_haveRead ? Dist(at, g_lastAt) : -1.f;
        UE_LOGI("[SWING-DRILL] client rest read: %.3f deg and %.2f cm from the last (at rest within %.1f and %.1f)",
                turned, moved, kRestDeg, kRestCm);
        const bool same = g_haveRead && turned <= kRestDeg && moved <= kRestCm;
        g_lastRead = w;
        g_lastAt = at;
        g_haveRead = true;
        if (!same) return;
        LogTarget("settled");
        SayLine(PS::SettledLine(w, at, PS::ReadParent(d)));
        Go(Step::Echo);
        return;
    }
    case Step::Echo:   // the watchers answer the line with their verdicts: the host's ends this run
        if (!g_hostVerdict) {
            if (now - g_stepSince > kClientBound) Invalid("the host's verdict line never came");
            return;
        }
        PrintRx();
        UE_LOGI("[SWING-DRILL] client %s: the door turned %.1f deg at its peak", Rejoin() ? "LIFE DONE" : "DONE", g_peakDev);
        EndActor();
        return;
    case Step::Done:
    case Step::Invalid:
        return;
    }
}

}  // namespace

bool IsEnabled() { return ArmOf() != Arm::Off; }

void Tick(coop::net::Session* session) {
    if (!IsEnabled() || !session || !session->connected()) return;
    // The rejoin client reads the host's word from its first connected tick, ahead of the class resolve and the arm
    // gate: a live row fades after a few seconds and the join takes longer. A `void` word or the word bound ends
    // this life.
    if (Rejoin() && Acts() && J::PollWord(kTag, Clock::now()) == J::Word::Void) EndActor();
    if (!SW::EnsureResolved()) return;
    if (!g_armed) {
        // The host's once a peer's world is up and its lids exist (J::HostArmDue); a client's once its join is over.
        const bool host = coop::roster::LocalIsHost();
        if (host ? !J::HostArmDue(*session, Clock::now())
                 : (!coop::net_pump::HasAnnouncedWorldReady() ||
                    coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle))
            return;
        ArmWatch();
        if (!g_armed) return;   // the host found no lid: it asks again on a later tick
        if (!Rejoin()) MarkChat();   // the rejoin arm tells a past life's rows by their tier instead
    }
    const Clock::time_point now = Clock::now();
    PumpOutbox();
    if (Rejoin() && coop::roster::LocalIsHost())   // the host's word to each client that became world-ready
        J::GreetReadySlots(*session, &SayLine);
    TickWatch(now);
    if (Acts() && !J::Active()) TickActor(now);
    J::Tick(now);
}

void OnDisconnect() {
    g_lids.clear();
    g_armed = false;
    g_v = Verdict{};
    g_step = Step::Pick;
    g_dragTicks = 0;
    g_peakDev = 0.f;
    g_sign = g_signTicks = g_swungAtTick = 0;
    g_swung = false;
    g_feedX = g_feedY = 0.f;
    g_outbox.clear();
    g_outNext = 0;
    g_nextSayMs = 0;
    g_chatMark = 0;
    g_hostVerdict = false;
    J::Reset();
    g_haveRead = g_releaseEdge = false;
    g_target.Reset();
    g_walk.reset();
    g_grab.reset();
    g_rx = Rx{};
}

void NoteReleased(void* heldActor) {
    if (IsEnabled() && heldActor && heldActor == g_target.Raw()) g_releaseEdge = true;   // the release has gone out
}

void NotePropPose(int slot, const coop::net::PropPoseSnapshot& pose, void* actor, void* mesh) {
    if (!IsEnabled()) return;
    int lid = -1;
    for (size_t i = 0; i < g_lids.size(); ++i)
        if (g_lids[i].ref.Raw() == actor) lid = static_cast<int>(i);
    if (lid >= 0) ++g_rx.door;
    else ++g_rx.other;
    g_rx.slot = slot;
    g_rx.key.assign(pose.key.data, pose.key.data + pose.key.len);
    g_rx.eid = pose.elementId;
    g_rx.actor = actor;
    g_rx.mesh = mesh;
    g_rx.loc = {pose.x, pose.y, pose.z};
    g_rx.rot = {pose.pitch, pose.yaw, pose.roll};
    if (lid >= 0) PoseArrived(g_rx.rot, lid);   // a pose for another prop is not the door's
}

void NotePropUnresolved(int slot, const coop::net::PropPoseSnapshot& pose) {
    if (!IsEnabled()) return;
    ++g_rx.unresolved;
    g_rx.slot = slot;
    g_rx.key.assign(pose.key.data, pose.key.data + pose.key.len);
    g_rx.eid = pose.elementId;
    g_rx.actor = nullptr;
    PoseArrived(ue_wrap::FRotator{pose.pitch, pose.yaw, pose.roll}, -1);
}

}  // namespace coop::dev::swinger_drill
