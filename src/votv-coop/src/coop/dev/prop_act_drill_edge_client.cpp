// coop/dev/prop_act_drill_edge_client.cpp -- c1's half of the act drill's phase `edge`: the join-window take before its
// announce, then the steps that time a request against a commit, a reply, a kill and its own expiry. See
// coop/dev/prop_act_drill.h; the table and the host's half are in prop_act_drill_edge.cpp.
//
//   c1 -- holds its own announce from the session's start (EdgeInstall), takes a keyed prop of the loaded save in that
//         window, releases the announce, then runs each step: the commit held, the replay parked, or the second act
//         pressed in the same tick, as the step says.

#include "prop_act_drill_edge.h"

#include "coop/dev/prop_act_drill.h"

#include "coop/dev/announce_hold.h"
#include "coop/dev/director/director.h"
#include "coop/dev/drill_props.h"
#include "coop/dev/drill_chat_pace.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_act_request.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace coop::dev::prop_act_drill::detail {
namespace {

namespace E   = ue_wrap::engine;
namespace R   = ue_wrap::reflection;
namespace EL  = coop::element;
namespace OI  = ue_wrap::object_index;
namespace PAR = coop::prop_act_request;
namespace DP  = coop::dev::drill_props;
using ue_wrap::FVector;

constexpr int kSlot = kClientSlot;
constexpr const char* kSecondTag = " eid2=";    // after the first eid in a pair's ready line
constexpr size_t kMaxPropClasses = 1024;        // the window row's scan: classes collected before their instances are walked

enum class CStage : uint8_t { Window, WindowOver, AwaitReady, Resolve, Walk, AwaitFirst, AwaitLine, AwaitExpiry,
                              AwaitReply, AwaitLate, Say, Done };
// What follows a said line: the next step, c1's wait for the host's line after the commit is held, or nothing.
enum class Then : uint8_t { Advance, AwaitDestroyed, Stop };

CStage g_cs = CStage::Window;
Then g_then = Then::Stop;
uint64_t g_csMs = 0, g_cNextMs = 0, g_nextSayMs = 0;
size_t g_crow = 0;
EL::ElementId g_ceid = EL::kInvalidId, g_ceid2 = EL::kInvalidId;
ue_wrap::CachedObjRef g_mirror, g_mirror2;
int g_invBefore = 0;
std::shared_ptr<coop::director::BackgroundWalk> g_walk;
std::string g_sayLine, g_lineHead;
bool g_sayFailed = false;
bool g_holdOwned = false;   // this client holds its own announce (EdgeInstall)

void CEnter(CStage s) {
    g_cs = s;
    g_csMs = NowMs();
}

bool CExpired(uint64_t bound) { return NowMs() - g_csMs > bound; }

void ReleaseAnnounce() {
    if (!g_holdOwned) return;
    g_holdOwned = false;
    coop::dev::announce_hold::Set(false);
}

// Every hold this phase puts on c1's request machine is let go.
void LetGoAll() {
    PAR::dev::HoldCommits(false);
    PAR::dev::HoldReplayUntilGone(false);
    ReleaseAnnounce();
}

// c1's held commit is sent; the hold ends with it, so what follows is sent at once.
void ReleaseCommits() {
    PAR::dev::ReleaseHeldCommits();
    PAR::dev::HoldCommits(false);
}

void Fail(const EdgeRow& r, const std::string& why) {
    UE_LOGW("%s c%d edge FAIL -- %s: %s", kTag, kSlot, r.act.name, why.c_str());
    LetGoAll();
    g_sayLine = DoneHead(r.act) + "fail " + why.substr(0, 120);
    g_sayFailed = true;
    g_then = Then::Stop;
    CEnter(CStage::Say);
}

void SayThen(const std::string& line, Then then) {
    g_sayLine = line;
    g_sayFailed = false;
    g_then = then;
    CEnter(CStage::Say);
}

void Pass(const EdgeRow& r, const char* what) {
    UE_LOGI("%s c%d edge step %s PASS -- %s", kTag, kSlot, r.act.name, what);
    SayThen(DoneHead(r.act) + "ok", Then::Advance);
}

void AwaitLine(const std::string& head, CStage stage) {
    g_lineHead = head;
    CEnter(stage);
}

// ---- the window row ----------------------------------------------------------------------------------------------

struct PropClasses {
    void* cls[kMaxPropClasses];
    size_t n = 0;
};

struct WindowScan {
    void* found = nullptr;
};

// The first keyed prop of the loaded save that has no row, outside the trash lanes: what a take is cancelled on while
// the announce is held. Null when there is none.
void* FindWindowProp() {
    PropClasses classes;
    OI::ForEachClass(
        [](void* ctx, void* cls, void* anyInstance) {
            auto* c = static_cast<PropClasses*>(ctx);
            if (c->n < kMaxPropClasses && ue_wrap::prop::IsDescendantOfProp(anyInstance)) c->cls[c->n++] = cls;
        },
        &classes);
    WindowScan scan;
    for (size_t i = 0; i < classes.n && !scan.found; ++i) {
        OI::ForEachInstance(
            classes.cls[i],
            [](void* ctx, void* obj, int32_t) {
                auto* sc = static_cast<WindowScan*>(ctx);
                if (sc->found || ue_wrap::prop::IsTrashActor(obj) || ue_wrap::prop::IsTrashBitsPile(obj) || !DP::HasKey(obj) ||
                    EL::Registry::Get().EidForActor(obj) != EL::kInvalidId)
                    return;
                sc->found = obj;
            },
            &scan);
    }
    return scan.found;
}

// The window row's take: the PRE cancels it at the call for the world is not announced, shows the line, sends nothing.
void WindowTake(const EdgeRow& r, void* player) {
    void* const prop = FindWindowProp();
    if (!prop) {
        Fail(r, "window not observed: the loaded save holds no keyed prop without a row");
        return;
    }
    const std::wstring leaf = R::ClassNameOf(prop);
    const int before = InventoryCount(leaf.c_str());
    ue_wrap::CachedObjRef held;
    held.Set(prop);
    if (const char* why = RunVerb(r.act, player, prop)) {
        Fail(r, why);
        return;
    }
    if (!held.Alive() || PAR::dev::PendingId() != 0 || InventoryCount(leaf.c_str()) != before) {
        Fail(r, "the take was not cancelled at the call: the prop is gone, a request is open or the inventory changed");
        return;
    }
    // The first "not ready" of the session is never limited, so a take that was cancelled for it has shown the line.
    if (!PAR::dev::NotReadyShown()) {
        Fail(r, "the take was cancelled but the 'Not ready yet' line did not show");
        return;
    }
    ReleaseAnnounce();
    UE_LOGI("%s c%d edge %s: the take was cancelled in the window, no request, the line shown; the announce is let go",
            kTag, kSlot, r.act.name);
    CEnter(CStage::WindowOver);
}

// ---- the steps ---------------------------------------------------------------------------------------------------

// One take of `mirror`, as a press makes it: cancelled at the call and asked. False after a FAIL.
bool AskTake(const EdgeRow& r, void* player, void* mirror, ue_wrap::CachedObjRef& ref) {
    if (!mirror) {
        Fail(r, "the mirror was gone before the act");
        return false;
    }
    if (const char* why = RunVerb(r.act, player, mirror)) {
        Fail(r, why);
        return false;
    }
    if (!ref.Alive() || PAR::dev::PendingId() == 0) {
        Fail(r, "the take was not cancelled at the call and asked");
        return false;
    }
    return true;
}

// The first take of the step is granted and replayed: its mirror is gone and its item is in the inventory.
bool FirstReplayed(const EdgeRow& r) {
    if (g_mirror.Alive() || InventoryCount(r.act.leaf) != g_invBefore + 1) {
        Fail(r, "the first take's mirror stayed or the inventory did not gain the item");
        return false;
    }
    return true;
}

// The step's act, once the walk has arrived.
void Act(const EdgeRow& r, void* player) {
    switch (r.kind) {
    case Kind::Before:
    case Kind::Late:
    case Kind::Gone:
        PAR::dev::HoldCommits(true);
        break;
    case Kind::Dead:
        PAR::dev::HoldReplayUntilGone(true);
        break;
    default:
        break;
    }
    if (!AskTake(r, player, g_mirror.Get(), g_mirror)) return;
    UE_LOGI("%s c%d edge %s: the call was cancelled and asked (request %u)", kTag, kSlot, r.act.name,
            static_cast<unsigned>(PAR::dev::PendingId()));
    if (r.kind == Kind::Second) {
        // The second act in the tick of the first: cancelled with nothing sent, so the open request stays the first's.
        const uint16_t first = PAR::dev::PendingId();
        if (!g_mirror2.Get()) {
            Fail(r, "the second mirror was gone before the act");
            return;
        }
        if (const char* why = RunVerb(r.act, player, g_mirror2.Get())) {
            Fail(r, why);
            return;
        }
        if (!g_mirror2.Alive() || PAR::dev::PendingId() != first) {
            Fail(r, "the second act was not cancelled with nothing sent");
            return;
        }
    }
    CEnter(r.kind == Kind::Before || r.kind == Kind::Late ? CStage::AwaitFirst : CStage::AwaitReply);
}

// The request is answered and the replay, if any, has run: what c1's copies show.
void ReadReply(const EdgeRow& r) {
    switch (r.kind) {
    case Kind::Before:
        if (g_mirror.Alive() || g_mirror2.Alive() || InventoryCount(r.act.leaf) != g_invBefore + 2) {
            Fail(r, "both takes were not granted and replayed: a mirror stayed or the inventory did not gain two");
            return;
        }
        Pass(r, "the second take waited behind the first's held commit and was granted after it; both are in the inventory");
        return;
    case Kind::Dead:
        PAR::dev::HoldReplayUntilGone(false);
        if (g_mirror.Alive() || InventoryCount(r.act.leaf) != g_invBefore) {
            Fail(r, "the mirror stayed after the host's destroy, or the aborted take changed the inventory");
            return;
        }
        Pass(r, "the grant's replay was parked, the mirror went with the host's destroy, and the take aborted");
        return;
    case Kind::Gone:
        if (!FirstReplayed(r)) return;
        // Its commit is held: the host destroys the target now, then this client's commit finds it gone.
        UE_LOGI("%s c%d edge %s: replayed, the commit is held", kTag, kSlot, r.act.name);
        SayThen(ClientLine(r, "committed-held"), Then::AwaitDestroyed);
        return;
    case Kind::Second:
        if (g_mirror.Alive() || InventoryCount(r.act.leaf) != g_invBefore + 1 || !g_mirror2.Alive() ||
            !EL::LivePropActor(g_ceid2)) {
            Fail(r, "the first take was not replayed or the second prop is not where it was");
            return;
        }
        Pass(r, "the second act was cancelled with nothing sent; only the first was granted and replayed");
        return;
    default:
        return;
    }
}

// The late grant has come in: it was answered with an abort, and nothing of the second take ran.
void ReadLate(const EdgeRow& r) {
    if (PAR::dev::LateGrantsAborted() != 1 || PAR::dev::ExpiredPending() != 0 || PAR::dev::PendingId() != 0) {
        Fail(r, "the late grant was not answered with exactly one abort, or a request is still open");
        return;
    }
    if (!g_mirror2.Alive() || !EL::LivePropActor(g_ceid2) || InventoryCount(r.act.leaf) != g_invBefore + 1) {
        Fail(r, "the second prop is not alive or the inventory gained more than the first take");
        return;
    }
    Pass(r, "the second request expired, its late grant was answered with one abort, and the second prop stands");
}

void BeginWalk(const EdgeRow& r, void* mirror) {
    FVector at{};
    if (!E::TryGetActorLocation(mirror, at)) {
        Fail(r, "the mirror's place could not be read");
        return;
    }
    g_walk = coop::director::StartBackgroundWalk(at, kReachCm, kWalkDeadlineS);
    UE_LOGI("%s c%d edge %s: walking to eid %u at (%.0f, %.0f, %.0f)", kTag, kSlot, r.act.name,
            static_cast<unsigned>(g_ceid), at.X, at.Y, at.Z);
    CEnter(CStage::Walk);
}

void Resolve(const EdgeRow& r) {
    void* const mirror = EL::LivePropActor(g_ceid);
    void* const second = g_ceid2 != EL::kInvalidId ? EL::LivePropActor(g_ceid2) : nullptr;
    if (!mirror || (g_ceid2 != EL::kInvalidId && !second)) {
        if (CExpired(kMirrorBoundMs)) Fail(r, "the host's prop never reached this client");
        return;
    }
    g_invBefore = InventoryCount(r.act.leaf);
    if (g_invBefore < 0) {
        Fail(r, "the inventory could not be read");
        return;
    }
    g_mirror.Set(mirror);
    g_mirror2.Set(second);
    BeginWalk(r, mirror);
}

void Advance() {
    g_mirror.Reset();
    g_mirror2.Reset();
    g_lineHead.clear();
    g_ceid = g_ceid2 = EL::kInvalidId;
    if (++g_crow < kEdgeRowCount) {
        CEnter(CStage::AwaitReady);
        return;
    }
    UE_LOGI("%s c%d edge PASS -- all %u steps held on this client", kTag, kSlot, static_cast<unsigned>(kEdgeRowCount));
    CEnter(CStage::Done);
}

void ClientStep(coop::net::Session* s, void* player, uint64_t now) {
    const EdgeRow& r = kEdgeRows[g_crow < kEdgeRowCount ? g_crow : 0];
    const bool poll = now >= g_cNextMs;
    if (poll) g_cNextMs = now + kPollEveryMs;
    switch (g_cs) {
    case CStage::Window: {
        // The first tick this client's world is up: its slot is known and the engine's object index holds the save.
        const uint8_t self = coop::players::Registry::Get().LocalPeerId();
        if (!s->connected() || self == 0 || !OI::IsSeeded() || OI::Backlog() != 0) return;
        if (self != kSlot) {
            ReleaseAnnounce();
            UE_LOGI("%s c%u edge idle -- this phase runs on c%d only", kTag, static_cast<unsigned>(self), kSlot);
            CEnter(CStage::Done);
            return;
        }
        WindowTake(r, player);
        return;
    }
    case CStage::WindowOver:
        // Once the announce is out and the join is over, the host can hear the window row's done line.
        if (!OwnJoinOver()) return;
        Pass(r, "the take in the window ran no request and showed the line");
        return;
    case CStage::AwaitReady: {
        if (!poll) return;
        std::string rest;
        if (!HostSaid(ReadyHead(r.act), &rest)) {
            if (CExpired(kReadyBoundMs)) Fail(r, "the host never said the step was ready");
            return;
        }
        g_ceid = static_cast<EL::ElementId>(std::strtoul(rest.c_str(), nullptr, 10));
        g_ceid2 = EL::kInvalidId;
        if (TwoProps(r.kind)) {
            const size_t at = rest.find(kSecondTag);
            if (at == std::string::npos) {
                Fail(r, "the host's line named no second prop");
                return;
            }
            g_ceid2 = static_cast<EL::ElementId>(std::strtoul(rest.c_str() + at + std::strlen(kSecondTag), nullptr, 10));
        }
        CEnter(CStage::Resolve);
        return;
    }
    case CStage::Resolve:
        Resolve(r);
        return;
    case CStage::Walk: {
        const int st = g_walk ? g_walk->state.load() : 2;
        if (st == 0) return;
        g_walk.reset();
        if (st == 2) {
            Fail(r, "the walk to the mirror did not arrive");
            return;
        }
        Act(r, player);
        return;
    }
    case CStage::AwaitFirst:
        // The first take is granted and replayed with its commit held; the second is asked at once.
        if (PAR::dev::PendingId() != 0) {
            if (CExpired(kReplyBoundMs)) Fail(r, "the first request was still pending");
            return;
        }
        if (!FirstReplayed(r)) return;
        if (!AskTake(r, player, g_mirror2.Get(), g_mirror2)) return;
        if (r.kind == Kind::Before) AwaitLine(HostLine(r, "waiting"), CStage::AwaitLine);
        else CEnter(CStage::AwaitExpiry);
        return;
    case CStage::AwaitLine:
        if (!poll) return;
        if (!HostSaid(g_lineHead, nullptr)) {
            if (CExpired(kReadyBoundMs)) Fail(r, "the host never said: " + g_lineHead);
            return;
        }
        if (r.kind == Kind::Gone) {
            // The target is dead on the host; the held commit goes, and finds it gone.
            ReleaseCommits();
            Pass(r, "replayed with the commit held; the host destroyed the target; the commit was sent");
            return;
        }
        ReleaseCommits();
        CEnter(CStage::AwaitReply);
        return;
    case CStage::AwaitExpiry:
        // The second request's wait runs out on this client with the host still waiting on the first's reservation.
        if (PAR::dev::ExpiredPending() != 1) {
            if (CExpired(kReadyBoundMs)) Fail(r, "the second request never expired on this client");
            return;
        }
        UE_LOGI("%s c%d edge %s: the second request expired; the held commit goes", kTag, kSlot, r.act.name);
        ReleaseCommits();
        CEnter(CStage::AwaitLate);
        return;
    case CStage::AwaitLate:
        if (PAR::dev::LateGrantsAborted() == 0) {
            if (CExpired(kDoneBoundMs)) Fail(r, "the late grant never came in");
            return;
        }
        ReadLate(r);
        return;
    case CStage::AwaitReply:
        if (PAR::dev::PendingId() != 0) {
            if (CExpired(kReplyBoundMs)) Fail(r, "the request was still pending");
            return;
        }
        ReadReply(r);
        return;
    case CStage::Say:
        // A client's chat passes the host's rate limit only at the drills' spacing; a refused line is lost for good.
        if (now < g_nextSayMs || !OwnJoinOver()) return;
        Say(g_sayLine);
        g_nextSayMs = now + coop::dev::kTalkEveryMs;
        if (g_sayFailed || g_then == Then::Stop) {
            CEnter(CStage::Done);
        } else if (g_then == Then::AwaitDestroyed) {
            AwaitLine(HostLine(r, "destroyed"), CStage::AwaitLine);
        } else {
            Advance();
        }
        return;
    case CStage::Done:
        return;
    }
}

}  // namespace

void EdgeInstall(coop::net::Session* s) {
    if (!s || s->role() != coop::net::Role::Client) return;
    // The announce stays held until the window row's take is made; the row releases it, and a client that is not c1
    // releases it at once.
    g_holdOwned = true;
    coop::dev::announce_hold::Set(true);
    UE_LOGI("%s edge holds this client's announce until the window row's take is made", kTag);
}

void EdgeClientTick(coop::net::Session* s) {
    if (g_cs == CStage::Done) return;
    void* const player = coop::players::Registry::Get().Local();
    if (!player) return;
    ClientStep(s, player, NowMs());
}

void EdgeClientReset() {
    LetGoAll();
    g_cs = CStage::Window;
    g_then = Then::Stop;
    g_csMs = g_cNextMs = g_nextSayMs = 0;
    g_crow = 0;
    g_ceid = g_ceid2 = EL::kInvalidId;
    g_mirror.Reset();
    g_mirror2.Reset();
    g_invBefore = 0;
    g_walk.reset();
    g_sayLine.clear();
    g_lineHead.clear();
    g_sayFailed = false;
}

}  // namespace coop::dev::prop_act_drill::detail
