// coop/dev/nametag_drill.cpp -- see coop/dev/nametag_drill.h.

#include "coop/dev/nametag_drill.h"

#include "coffee_drill_detail.h"  // co-located private header: SpawnProp (a prop_C named by a row)

#include "coop/comms/chat_log.h"
#include "coop/comms/chat_sync.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/director/director.h"  // StartBackgroundWalk: to the cup, staged by the director
#include "coop/dev/drill_hand.h"         // TakeIntoHandNow: the nametag into the hand
#include "coop/net/session.h"
#include "coop/player/hand_item.h"       // OwnAnnounced: the hand lane has the nametag
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_nametag.h"
#include "coop/save/join_window_baseline.h"  // HasCapture: the instant the joiner's save was cut
#include "coop/session/host_world_settle.h"  // Settled: the host's world has finished starting up
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady
#include "coop/session/rig_verdict.h"

#include "ue_wrap/actors/nametag.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <string>

namespace coop::dev::nametag_drill {
namespace {

namespace E  = ue_wrap::engine;
namespace NT = ue_wrap::nametag;
namespace PR = ue_wrap::prop;
namespace R  = ue_wrap::reflection;
namespace CD = coop::dev::coffee_drill::detail;
namespace RV = coop::rig_verdict;

// The one verdict tag, said by the HOST alone, so a host-side FAIL cannot be lost behind a client's PASS. The client
// says nothing to the rig: it tells the host in the lobby chat `done ok|fail|invalid <why>`, and the host says PASS when
// its own half held and the client's is ok, FAIL on its own contradiction or the client's fail, INVALID on the client's
// invalid. A bound below only ever says INVALID: a verdict is an event (a request answered, the host's tag come, the
// join over) or the point after which nothing changes (the game's own use is synchronous, so its effect is read in the
// tick that clicked).
constexpr const char* kVerdict = "nametag";
constexpr const char* kHead    = "[NAMETAG-DRILL] ";

constexpr const wchar_t* kTagText = L"mvDrillTag";
constexpr float    kAheadCm       = 120.f;
constexpr float    kNearCm        = 3000.f;   // CLIENT: the cup this near its player
constexpr float    kBesideHostCm  = 600.f;    // CLIENT, window mode: the cup this near the host's puppet
constexpr float    kWalkReachCm   = 110.f;
constexpr int      kWalkDeadlineS = 60;
constexpr uint64_t kEquipBoundMs  = 10000;
constexpr uint64_t kFindBoundMs   = 60000;
constexpr uint64_t kRestBoundMs   = 30000;
constexpr uint64_t kSeenBoundMs   = 15000;
constexpr uint64_t kWatchBoundMs  = 180000;
constexpr uint64_t kReadyBoundMs  = 120000;   // HOST: the client's ready or found to the host's next step
constexpr uint64_t kDoneBoundMs   = 120000;   // HOST: its half held to the client's report
constexpr uint64_t kWindowBoundMs = 600000;   // HOST, window mode: the click held to the joiner's save being cut
constexpr uint64_t kCheckEveryMs  = 250;
constexpr float    kRestCm        = 0.5f;     // a copy that moved less than this since the last check is at rest

// run: the client tags the host's cup. host: the host player tags a cup the client holds a copy of. window: the host
// tags a cup before the client joins, between the client's save being cut and its world being ready; the client's join
// ends with its copy tagged.
enum class Mode : uint8_t { Off, Run, Host, Window };
Mode TheMode() {
    static const Mode m = [] {
        const std::string v = coop::config::ResolveString(::coop::config_registry::rows::nametag_drill);
        return v == "run" ? Mode::Run : v == "host" ? Mode::Host : v == "window" ? Mode::Window : Mode::Off;
    }();
    return m;
}
bool WindowMode() { return TheMode() == Mode::Window; }
bool HostTags() { return TheMode() == Mode::Host || WindowMode(); }

enum class Step : uint8_t { Arm, Find, Rest, Wait, Walk, Equip, Hold, Use, Seen, Watch, Await, Done };
Step     g_step = Step::Arm;
uint64_t g_stepMs = 0;
uint64_t g_nextCheckMs = 0;
int      g_session = 1;
ue_wrap::CachedObjRef g_cup, g_item;
uint64_t g_applied0 = 0, g_pub0 = 0;                          // the lane's counters at the click
uint64_t g_asked0 = 0, g_refused0 = 0, g_spent0 = 0;          // CLIENT: the lane's counters at the click
std::shared_ptr<coop::director::BackgroundWalk> g_walk;
std::set<void*> g_oldCups;                                    // CLIENT: the cups that stood before the host's was spawned
ue_wrap::FVector g_lastCupAt{};                               // CLIENT: the copy's place at the last rest check
bool     g_lastCupKnown = false;
coop::net::Session* g_s = nullptr;                            // this peer's session, set by Tick
bool     g_isHost = false;                                    // this peer's role, set by Tick
bool     g_hostOk = false;                                    // HOST: its own half held
bool     g_clientOk = false;                                  // HOST: the client said `done ok`
bool     g_clientDone = false;                                // HOST: the client's report was read
std::string g_clientWhy;                                      // HOST: what the client said held

const char* Role(bool host) { return host ? "host" : "client"; }
void Next(Step s) { g_step = s; g_stepMs = ::GetTickCount64(); }
bool Expired(uint64_t bound) { return ::GetTickCount64() - g_stepMs > bound; }
void Cleanup() {
    if (void* c = g_cup.Get(); c && g_isHost) E::DestroyActor(c);
}

// The chat line the client's reports start with, and the text a peer says after it.
std::string ClientHead(const char* what) { return std::string(kHead) + "c1 " + what; }

// HOST: the text after `head` in the first line slot 1 said in the lobby record.
bool ClientSaid(const std::string& head, std::string* rest) {
    bool found = false;
    coop::chat_log::ForEach([&](const coop::chat_log::Row& r) {
        if (found || r.slot != 1 || r.text.compare(0, head.size(), head) != 0) return;
        found = true;
        if (rest) *rest = r.text.substr(head.size());
    });
    return found;
}

// HOST: the one verdict.
void HostEnds(RV::Outcome o, const char* why) {
    if (o == RV::Outcome::Fail) UE_LOGW("%sFAIL in session %d: %s", kHead, g_session, why);
    else if (o == RV::Outcome::Invalid) UE_LOGW("%sABANDONED in session %d: %s", kHead, g_session, why);
    RV::Say(kVerdict, o, "%s", why);
    Cleanup();
    g_step = Step::Done;
}

// CLIENT: what this peer found, told to the host; it says nothing to the rig.
void Report(const char* word, const char* why) {
    const std::string line = ClientHead("done ") + word + " " + std::string(why).substr(0, 110);
    UE_LOGI("%sclient %s in session %d: %s", kHead, word, g_session, why);
    coop::chat_sync::QueueSend(line);
    Cleanup();
    g_step = Step::Done;
}

void Fail(const char* what) {
    if (g_isHost) HostEnds(RV::Outcome::Fail, what);
    else Report("fail", what);
}
void Abandon(const char* why) {   // the drill could not measure: a walk, a setup or a bound that did not come to pass
    if (g_isHost) HostEnds(RV::Outcome::Invalid, why);
    else Report("invalid", why);
}
void Pass(const char* what) { Report("ok", what); }   // CLIENT only: the host's PASS is its own

// HOST: its own half held; the verdict is PASS once the client's is ok too.
void HostHalfOk(const char* what) {
    UE_LOGI("%shost DONE in session %d: %s", kHead, g_session, what);
    g_hostOk = true;
    Next(Step::Await);
}

// HOST: read the client's report once; PASS when both halves are ok.
void PollClient() {
    if (g_step == Step::Done) return;
    if (!g_clientDone) {
        std::string rest;
        if (!ClientSaid(ClientHead("done "), &rest)) return;
        g_clientDone = true;
        const size_t sp = rest.find(' ');
        const std::string word = rest.substr(0, sp);
        g_clientWhy = sp == std::string::npos ? std::string() : rest.substr(sp + 1);
        if (word == "ok") g_clientOk = true;
        else HostEnds(word == "invalid" ? RV::Outcome::Invalid : RV::Outcome::Fail,
                      ("the client: " + g_clientWhy).c_str());
    }
    if (g_step != Step::Done && g_hostOk && g_clientOk)
        HostEnds(RV::Outcome::Pass, ("the host's half held; the client: " + g_clientWhy).c_str());
}

bool CupTagged(void* cup) {
    std::wstring t;
    return cup && NT::ReadPropNametag(cup, t) && t == kTagText;
}

void* HeldNametag(void* player) {
    E::MainPlayerGrabState gs{};
    void* held = E::ReadMainPlayerGrabState(player, gs) ? gs.holdingActor : nullptr;
    return NT::IsNametagItem(held) ? held : nullptr;
}

// The drill's tag written on the item in hand, as the game's own entry would leave it.
bool WriteItemTag(void* item) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(item), L"tag");
    if (off < 0) return false;
    const R::FName n = ue_wrap::fname_utils::StringToFName(kTagText);
    std::memcpy(static_cast<uint8_t*>(item) + off, &n, sizeof(n));
    std::wstring back;
    return NT::ReadItemTag(item, back) && back == kTagText;
}

// CLIENT: the cups that stood before the host's was spawned, so the one it spawns is told apart.
void SnapshotCups() {
    g_oldCups.clear();
    void* cls = ue_wrap::object_index::ClassByName(L"prop_C");
    if (!cls) return;
    ue_wrap::object_index::ForEachInstance(cls, [](void*, void* obj, int32_t) {
        if (PR::GetPropNameString(obj) == L"cup") g_oldCups.insert(obj);
    }, nullptr);
}

// The nearest keyed cup to `from` within `withinCm`; `skipOld` leaves out the ones that stood at the snapshot.
struct Near { ue_wrap::FVector from; void* best; float bestCm; bool skipOld; };
void* NearestCup(const ue_wrap::FVector& from, float withinCm, bool skipOld) {
    Near ctx{from, nullptr, withinCm, skipOld};
    void* cls = ue_wrap::object_index::ClassByName(L"prop_C");
    if (!cls) return nullptr;
    ue_wrap::object_index::ForEachInstance(cls, [](void* c, void* obj, int32_t) {
        auto* n = static_cast<Near*>(c);
        ue_wrap::FVector at{};
        if (R::NameStartsWith(R::NameOf(obj), L"Default__") || PR::GetPropNameString(obj) != L"cup" ||
            (n->skipOld && g_oldCups.count(obj)) || !E::TryGetActorLocation(obj, at))
            return;
        const std::wstring key = PR::GetInteractableKeyString(obj);
        if (key.empty() || key == L"None") return;
        const float cm = std::sqrt((at.X - n->from.X) * (at.X - n->from.X) + (at.Y - n->from.Y) * (at.Y - n->from.Y) +
                                   (at.Z - n->from.Z) * (at.Z - n->from.Z));
        if (cm < n->bestCm) { n->best = obj; n->bestCm = cm; }
    }, &ctx);
    return ctx.best;
}

// The walk to the cup, before the nametag is taken: a walk with an item in hand lets it go. The director stages a far
// walk at a known-good anchor; nobody is teleported by the drill.
void StartWalk(bool host) {
    ue_wrap::FVector at{};
    void* cup = g_cup.Get();
    if (!cup || !E::TryGetActorLocation(cup, at)) { Abandon("the cup died"); return; }
    g_walk = coop::director::StartBackgroundWalk(at, kWalkReachCm, kWalkDeadlineS);
    UE_LOGI("%s%s: walking to the cup", kHead, Role(host));
    Next(Step::Walk);
}

// What the host's own click did, read in the tick that clicked: the game's body is synchronous. True when decided.
// The cup carries the tag, the item is spent (the body destroys it as it ends) and the lane told the clients once.
bool JudgeHostUse() {
    if (!CupTagged(g_cup.Get())) {
        Abandon("the host player's own use did not tag its cup: the drill's click did not take");
        return true;
    }
    if (g_item.Alive()) return false;
    if (coop::prop_nametag::HostPublishedCount() != g_pub0 + 1) {
        Fail("the host player's tag is on its cup but the lane did not tell the clients");
        return true;
    }
    HostHalfOk("its cup carries the tag, the nametag is spent and the lane told the clients once");
    return true;
}

// What a client's click did, from the lane's counters. True when decided. A click that asked the host nothing is the
// lane failing at its seat (the game's own use ran on this copy: nametag_client_runs); then the host's refusal is a
// fail, and its tag for this request is the last event: the copy tagged and the item spent, ok.
bool JudgeClientUse() {
    namespace PN = coop::prop_nametag;
    if (PN::AskedCount() == g_asked0) {
        char why[150];
        std::snprintf(why, sizeof(why), "the click asked the host nothing: this copy is %s and the nametag %s here",
                      CupTagged(g_cup.Get()) ? "tagged" : "untagged", g_item.Alive() ? "stands" : "is spent");
        Fail(why);
        return true;
    }
    if (PN::RefusedAnswerCount() != g_refused0) {
        Fail("the host refused a tag the client made within reach of its cup with a nametag in hand");
        return true;
    }
    if (PN::SpentCount() == g_spent0) return false;
    const bool tagged = CupTagged(g_cup.Get());
    const bool spent  = !g_item.Alive();
    if (!tagged || !spent) {
        char why[150];
        std::snprintf(why, sizeof(why), "the host's tag for this click came but the copy is %s and the nametag is %s",
                      tagged ? "tagged" : "untagged", spent ? "spent" : "not spent");
        Fail(why);
        return true;
    }
    Pass("the host's tag for this click came: the copy carries it and the nametag is spent");
    return true;
}

// The tagger's steps, either peer: walk to the cup, the nametag into the hand, the item's own left click. False when the
// step is not one of these.
bool TagTick(void* player, bool host) {
    switch (g_step) {
    case Step::Walk: {
        const int w = g_walk ? g_walk->state.load() : 2;
        if (w == 0) return true;
        g_walk.reset();
        if (w != 1) { Abandon("the walk to the cup did not arrive"); return true; }
        Next(Step::Equip);
        return true;
    }
    case Step::Equip: {
        void* held = HeldNametag(player);
        if (!held) {
            if (Expired(kEquipBoundMs)) { Abandon("no nametag came into the hand"); return true; }
            static uint64_t lastMs = 0;
            if (::GetTickCount64() - lastMs > 1000) {
                lastMs = ::GetTickCount64();
                coop::dev::drill_hand::TakeIntoHandNow(L"nametag", L"prop_nametag_C");
            }
            return true;
        }
        if (!g_item.Get()) {
            if (!WriteItemTag(held)) { Abandon("the nametag's tag did not take the drill's text"); return true; }
            g_item.Set(held);
        }
        // A client's click is judged against the hand its owner announced: the click waits for that announce, so the
        // host has the hand state ahead of the request on the one ordered lane. The product does not wait: a click
        // before its announce is refused with a reason and the item stays. The drill measures the judge, not that
        // residual.
        if (!host && !coop::hand_item::OwnAnnounced(L"prop_nametag_C")) {
            if (Expired(kEquipBoundMs)) Abandon("the hand lane never announced the nametag in this client's hand");
            return true;
        }
        UE_LOGI("%s%s: a nametag reading '%ls' in hand", kHead, Role(host), kTagText);
        Next(WindowMode() ? Step::Hold : Step::Use);
        return true;
    }
    case Step::Hold: {   // window mode: the click waits for the joiner's save to be cut, and goes before its world is ready
        if (g_s && g_s->IsSlotWorldReady(1)) {
            Abandon("the client's world was ready before the host clicked: the window closed");
        } else if (coop::join_window_baseline::HasCapture(1)) {
            UE_LOGI("%shost: the client's save is cut and its world is not ready -- clicking", kHead);
            Next(Step::Use);
        } else if (Expired(kWindowBoundMs)) {
            Abandon("no save of the client's was cut in 10 min after the host's nametag was in hand");
        }
        return true;
    }
    case Step::Use: {
        void* item = g_item.Get();
        if (!item) { Abandon("the nametag left the hand before the click"); return true; }
        // A held item's own use breaks the interaction trace the item reads, so the hit it acts on is written here,
        // on the cup's mesh, as a player's look onto the cup leaves it (ue_wrap/engine: WriteMainPlayerHitResult).
        void* cup = g_cup.Get();
        ue_wrap::FVector at{};
        if (!cup || !E::TryGetActorLocation(cup, at) ||
            !E::WriteMainPlayerHitResult(player, cup, PR::GetStaticMesh(cup), at)) {
            Abandon("the player's hit could not be put on the cup");
            return true;
        }
        namespace PN = coop::prop_nametag;
        g_applied0 = PN::AppliedForClients();
        g_pub0     = PN::HostPublishedCount();
        g_asked0   = PN::AskedCount();
        g_refused0 = PN::RefusedAnswerCount();
        g_spent0   = PN::SpentCount();
        void* fn = R::FindDispatchFunctionCached(R::ClassOf(item), L"playerHandUse_LMB");
        ue_wrap::ParamFrame f(fn);
        if (!fn || !f.valid() || !f.Set<void*>(L"player", player) || !ue_wrap::Call(item, f)) {
            Abandon("the nametag's left click did not dispatch");
            return true;
        }
        UE_LOGI("%s%s clicked the cup with the nametag", kHead, Role(host));
        Next(Step::Seen);
        if (host) JudgeHostUse();   // the game's body is synchronous: its effect is already here
        else JudgeClientUse();
        return true;
    }
    case Step::Seen:
        if (host ? JudgeHostUse() : JudgeClientUse()) return true;
        if (Expired(kSeenBoundMs))
            Abandon(host ? "15 s after the click the host's nametag still stands"
                         : "15 s after the click neither the host's tag nor its refusal had come");
        return true;
    default:
        return false;
    }
}

void HostTick(coop::net::Session* s) {
    if (g_step == Step::Done) return;
    const uint64_t now = ::GetTickCount64();
    if (now < g_nextCheckMs) return;
    g_nextCheckMs = now + kCheckEveryMs;
    PollClient();   // the client's report, whatever step the host is in
    if (g_step == Step::Done) return;
    void* player = coop::players::Registry::Get().Local();
    if (!player) return;
    if (HostTags() && TagTick(player, true)) return;
    switch (g_step) {
    case Step::Arm: {
        // Before whom the cup stands: the one who walks to it. The run arm's client walks, so the cup stands before its
        // puppet; in the host and window arms the host walks, so before the host itself: a cup before the puppet put the
        // puppet between the host and the cup, and the host's walk never left its spot.
        void* before = nullptr;
        if (WindowMode()) {
            if (!coop::host_world_settle::Settled()) return;   // the world's own start-up is over (rig: READY hosting)
            before = player;
        } else {
            if (!s->IsSlotWorldReady(1)) return;
            if (g_stepMs == 0) g_stepMs = now;   // the bound runs from the client's world being ready
            if (!ClientSaid(ClientHead("ready"), nullptr)) {
                if (Expired(kReadyBoundMs)) Abandon("the client never said it had taken its snapshot of the cups");
                return;
            }
            before = HostTags() ? player : coop::puppet_drive::Puppet(1).GetActor();
        }
        ue_wrap::FVector at{};
        if (!before || !R::IsLive(before) || !E::TryGetActorLocation(before, at)) return;
        const ue_wrap::FVector f = E::GetActorForwardVector(before);
        g_cup.Set(CD::SpawnProp(L"prop_C", {at.X + f.X * kAheadCm, at.Y + f.Y * kAheadCm, at.Z + 20.f}, L"cup"));
        if (!g_cup.Get()) { Abandon("the cup did not spawn"); return; }
        g_applied0 = coop::prop_nametag::AppliedForClients();
        UE_LOGI("%shost: a cup '%ls' spawned before %s", kHead, PR::GetInteractableKeyString(g_cup.Get()).c_str(),
                HostTags() ? "the host" : "the client's puppet");
        if (WindowMode()) StartWalk(true);
        else Next(TheMode() == Mode::Host ? Step::Wait : Step::Watch);
        return;
    }
    case Step::Wait:   // HOST tags: once the client says it holds its copy at rest, the host walks to the cup
        if (!ClientSaid(ClientHead("found"), nullptr)) {
            if (Expired(kReadyBoundMs)) Abandon("the client never said it held its copy of the cup");
            return;
        }
        UE_LOGI("%shost: the client holds its copy -- walking to the cup to tag it", kHead);
        StartWalk(true);
        return;
    case Step::Watch: {   // the CLIENT tags: the host's cup takes the tag, applied for a client once
        const uint64_t applied = coop::prop_nametag::AppliedForClients() - g_applied0;
        if (applied > 1) {
            char why[120];
            std::snprintf(why, sizeof(why), "the host applied %llu tags for the client's one click",
                          static_cast<unsigned long long>(applied));
            Fail(why);
            return;
        }
        if (CupTagged(g_cup.Get()) && applied == 1) {
            HostHalfOk("its cup carries the client's tag, applied for the client once");
            return;
        }
        if (Expired(kWatchBoundMs)) Abandon("the client's click never reached the host: its cup is untagged 180 s on");
        return;
    }
    case Step::Await:   // its half held: the client's report decides, or the bound says INVALID
        if (Expired(kDoneBoundMs)) Abandon("the client's report never came 120 s after the host's half held");
        return;
    default:
        return;
    }
}

// CLIENT: the copy is at rest when its place did not move since the last check. A copy that does not read is not at rest.
bool CopyAtRest(void* cup) {
    ue_wrap::FVector at{};
    if (!cup || !E::TryGetActorLocation(cup, at)) return false;
    const float dx = at.X - g_lastCupAt.X, dy = at.Y - g_lastCupAt.Y, dz = at.Z - g_lastCupAt.Z;
    const bool steady = g_lastCupKnown && dx * dx + dy * dy + dz * dz < kRestCm * kRestCm;
    g_lastCupAt = at;
    g_lastCupKnown = true;
    return steady;
}

// CLIENT, window mode, at the join's end: the host tagged its cup after this client's save was cut and before its world
// was ready, so the publish of that moment never reached it. The connect replay's word lands ahead of the snapshot whose
// completion ends the join, so the copy the save loaded carries the tag by now, or the replay did not name it.
void JudgeWindow() {
    if (CupTagged(g_cup.Get()))
        Pass("the host's tag, put on after this client's save was cut, reached its copy by the join's end");
    else
        Fail("the host's tag, put on after this client's save was cut, is not on this copy at the join's end");
}

void ClientTick() {
    if (g_step == Step::Done) return;
    const uint64_t now = ::GetTickCount64();
    if (now < g_nextCheckMs) return;
    g_nextCheckMs = now + kCheckEveryMs;
    void* player = coop::players::Registry::Get().Local();
    if (!player) return;
    if (!HostTags() && TagTick(player, false)) return;
    switch (g_step) {
    case Step::Arm: {
        if (!coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
            return;
        if (!WindowMode()) {
            SnapshotCups();
            coop::chat_sync::QueueSend(ClientHead("ready"));   // the host spawns its cup after this snapshot
        }
        Next(Step::Find);
        return;
    }
    case Step::Find: {
        ue_wrap::FVector from{};
        void* ref = player;
        if (WindowMode()) {   // the cup stands by the host: wait for its puppet to take the host's place
            coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(0);
            ref = rp ? rp->GetActor() : nullptr;
        }
        if (!ref || !R::IsLive(ref) || !E::TryGetActorLocation(ref, from) ||
            (WindowMode() && std::fabs(from.X) + std::fabs(from.Y) < 1000.f)) {
            if (Expired(kFindBoundMs)) Abandon("the host's puppet never took the host's place within 60 s");
            return;
        }
        void* cup = NearestCup(from, WindowMode() ? kBesideHostCm : kNearCm, !WindowMode());
        if (!cup) {
            if (Expired(kFindBoundMs)) Abandon("no cup stood near within 60 s");
            return;
        }
        g_cup.Set(cup);
        UE_LOGI("%sclient: the cup '%ls' is here", kHead, PR::GetInteractableKeyString(cup).c_str());
        if (WindowMode()) { JudgeWindow(); return; }
        g_lastCupKnown = false;
        Next(Step::Rest);
        return;
    }
    case Step::Rest: {   // the copy came to rest where the host's cup does: the click's reach is judged there
        void* cup = g_cup.Get();
        if (!cup) { Abandon("the cup died"); return; }
        if (!CopyAtRest(cup)) {
            if (Expired(kRestBoundMs)) Abandon("this client's copy of the cup did not come to rest in 30 s");
            return;
        }
        if (TheMode() == Mode::Run) { StartWalk(false); return; }
        coop::chat_sync::QueueSend(ClientHead("found"));   // the host waits on it before it walks to its cup
        g_pub0 = coop::prop_nametag::PublishedCount();
        Next(Step::Watch);
        return;
    }
    case Step::Watch:   // the HOST tags: the host's tag comes, and this client's copy carries it
        if (coop::prop_nametag::PublishedCount() != g_pub0) {
            if (CupTagged(g_cup.Get()))
                Pass("the host's tag came and this client's copy of the cup carries it");
            else
                Fail("the host's tag came but this client's copy of the cup does not carry it");
            return;
        }
        if (Expired(kWatchBoundMs)) Abandon("the host's tag never came within 180 s");
        return;
    default:
        return;
    }
}

}  // namespace

void Tick(coop::net::Session* s) {
    if (TheMode() == Mode::Off || !s) return;
    g_s = s;
    g_isHost = s->role() == coop::net::Role::Host;
    if (g_isHost) HostTick(s);
    else if (s->connected()) ClientTick();
}

void OnDisconnect() {
    if (TheMode() == Mode::Off) return;
    ++g_session;
    g_step = Step::Arm;
    g_stepMs = g_nextCheckMs = 0;
    g_cup.Reset();
    g_item.Reset();
    g_applied0 = g_pub0 = g_asked0 = g_refused0 = g_spent0 = 0;
    g_oldCups.clear();
    g_lastCupKnown = false;
    g_hostOk = g_clientOk = g_clientDone = false;
    g_clientWhy.clear();
    if (g_walk) coop::director::CancelBackgroundWalk(*g_walk);
    g_walk.reset();
}

}  // namespace coop::dev::nametag_drill
