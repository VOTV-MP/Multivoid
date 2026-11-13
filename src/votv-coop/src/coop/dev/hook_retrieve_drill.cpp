// coop/dev/hook_retrieve_drill.cpp -- see coop/dev/hook_retrieve_drill.h.

#include "coop/dev/hook_retrieve_drill.h"

#include "coffee_drill_detail.h"  // co-located private header: SpawnProp (a prop_C named by a row)

#include "coop/comms/chat_feed.h"
#include "coop/comms/chat_log.h"
#include "coop/comms/chat_sync.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/director/aim_fan.h"
#include "coop/dev/director/director.h"  // StartBackgroundWalk: to the hook, wherever its cups came to rest
#include "coop/items/hook_anchor.h"
#include "coop/items/hook_retrieve.h"
#include "coop/net/session.h"
#include "coop/save/join_window_baseline.h"  // HasCapture: the instant the joiner's save was cut
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/session/host_world_settle.h"  // Settled: the host's world has finished starting up
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady
#include "coop/session/rig_verdict.h"

#include "ue_wrap/actors/hook.h"
#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

namespace coop::dev::hook_retrieve_drill {
namespace {

namespace E  = ue_wrap::engine;
namespace H  = ue_wrap::hook;
namespace PR = ue_wrap::prop;
namespace R  = ue_wrap::reflection;
namespace CD = coop::dev::coffee_drill::detail;
namespace RV = coop::rig_verdict;

// The one verdict tag, said by the HOST alone, so a host-side FAIL cannot be lost behind a client's PASS (the act
// drill's shape). The client says nothing to the rig: it tells the host in the lobby chat `c1 done ok|fail|invalid
// <why>`, and the host says PASS when its own half held and the client's is ok, FAIL on its own contradiction or the
// client's fail, INVALID on the client's invalid. A bound below only ever says INVALID: a verdict is an event (a request
// answered, a copy gone, an item come) or the point after which nothing changes (the game's pickup body is
// synchronous, so the press's own effect is read in the tick that pressed).
constexpr const char* kVerdict = "hook-retrieve";
constexpr const char* kHead    = "[HOOK-RETRIEVE-DRILL] ";

constexpr float    kAheadCm       = 120.f;   // before the player: inside the interaction reach
constexpr float    kApartCm       = 45.f;    // each cup this far to one side of the line ahead
constexpr float    kHeadUpCm      = 35.f;    // the head above its cup, so the trace meets the hook before the cup
constexpr uint16_t kSeq           = 0xFF01;  // the drill's hook in the host's own owner space, clear of a player's
constexpr uint64_t kTakeBoundMs   = 120000;  // the tie (or the client's ready edge) to the hook gone
constexpr uint64_t kDoneBoundMs   = 120000;  // HOST: its half held to the client's report
constexpr uint64_t kRestBoundMs   = 30000;   // HOST: the cups' spawn to both at rest
constexpr uint64_t kWindowBoundMs = 600000;  // HOST, window mode: the aim taken to the joiner's save being cut
constexpr uint64_t kFindBoundMs   = 60000;   // CLIENT: world-ready to the anchored copy
constexpr uint64_t kReadyBoundMs  = 3000;    // the aim to the player's look settling on the hook
constexpr uint64_t kSeenBoundMs   = 15000;   // the press to the hook gone and the item in the inventory
constexpr uint64_t kCheckEveryMs  = 250;
constexpr float    kWalkReachCm   = 120.f;
constexpr int      kWalkDeadlineS = 60;
constexpr int      kReaimMax      = 5;

// run: the client takes the hook, its copy an anchored mirror. join: the hook is tied before the client joins, so its
// copy is the one its save loaded. host / joinhost: the same two copies, and the host player takes the hook. window: the
// hook is tied before the client joins and the host player takes it between the client's save being cut and its world
// being ready; the client's copy, which its save loaded, must be gone when its join is over.
enum class Mode : uint8_t { Off, Run, Join, Host, JoinHost, Window };
Mode TheMode() {
    static const Mode m = [] {
        const std::string v = coop::config::ResolveString(::coop::config_registry::rows::hook_retrieve_drill);
        if (v == "run") return Mode::Run;
        if (v == "join") return Mode::Join;
        if (v == "host") return Mode::Host;
        if (v == "joinhost") return Mode::JoinHost;
        if (v == "window") return Mode::Window;
        return Mode::Off;
    }();
    return m;
}
bool WindowMode() { return TheMode() == Mode::Window; }
bool JoinMode() { return TheMode() == Mode::Join || TheMode() == Mode::JoinHost || WindowMode(); }
bool HostTakes() { return TheMode() == Mode::Host || TheMode() == Mode::JoinHost || WindowMode(); }

enum class Step : uint8_t { Arm, Settle, Tie, Wait, Find, Walk, Aim, Ready, Hold, Press, Seen, Watch, Await, Done };
Step     g_step = Step::Arm;
uint64_t g_stepMs = 0;
uint64_t g_nextCheckMs = 0;
int      g_session = 1;
ue_wrap::CachedObjRef g_cupA, g_cupB, g_hook;
std::wstring g_key;
uint64_t g_granted0 = 0;
uint64_t g_asked0 = 0, g_taken0 = 0, g_refusedAns0 = 0;   // CLIENT: the lane's counters at the press
int      g_items0 = 0;
coop::director::AimFan g_fan;
int      g_reaims = 0;
std::shared_ptr<coop::director::BackgroundWalk> g_walk;
ue_wrap::FVector g_restA{}, g_restB{};   // HOST: the cups' places at the last rest check
bool     g_restKnown = false;
coop::net::Session* g_s = nullptr;       // this peer's session, set by Tick

const char* Role(bool host) { return host ? "host" : "client"; }
void Next(Step s) { g_step = s; g_stepMs = ::GetTickCount64(); }
bool Expired(uint64_t bound) { return ::GetTickCount64() - g_stepMs > bound; }
void Cleanup() {
    for (auto* r : {&g_cupA, &g_cupB, &g_hook})
        if (void* a = r->Get()) E::DestroyActor(a);
}
bool g_isHost = false;            // this peer's role, set by Tick
bool g_puppetSaid = false;        // HOST: `host puppet` was said
bool g_hostOk = false;            // HOST: its own half held
bool g_clientOk = false;          // HOST: the client said `done ok`
bool g_clientDone = false;        // HOST: the client's report was read
std::string g_clientWhy;          // HOST: what the client said held

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

// The lines the host says: `host hook <key>` once its hook is tied (the one hook every step of this drill is about) and
// `host puppet` once its puppet of slot 1 has a pose, the fact the host's judge reads (RemotePlayer valid and HasPose,
// coop/element/intent_authority.cpp AuthorizeSegment): a press before it is refused no-body.
std::string HostHead(const char* what) { return std::string(kHead) + "host " + what; }

// CLIENT: the text after `head` in the first row of this client's chat feed that carries it. A row the host said before
// this client joined is a retained one, and counts.
bool HostSaid(const std::string& head, std::string* rest) {
    bool found = false;
    coop::chat_feed::ForEachRow([&](const coop::chat_feed::RowView& r) {
        const size_t at = found ? std::string_view::npos : r.line.find(head);
        if (at == std::string_view::npos) return;
        found = true;
        if (rest) *rest = std::string(r.line.substr(at + head.size()));
    });
    return found;
}

// CLIENT: the key of the hook the host tied, from its line; false until it has come.
bool HostHookKey(std::wstring* key) {
    std::string rest;
    if (!HostSaid(HostHead("hook "), &rest)) return false;
    key->assign(rest.begin(), rest.end());
    while (!key->empty() && key->back() == L' ') key->pop_back();
    return !key->empty();
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

// The hook items in this player's inventory, by their record's class.
int HookItems() {
    ue_wrap::inventory::LivePersonalStore store;
    if (!ue_wrap::inventory::ReadLivePersonalStore(store)) return -1;
    int n = 0;
    for (const auto& r : store.records)
        if (r.className == L"prop_hook_C") ++n;
    return n;
}

// HOST: both cups are at rest: no body moving (linear under 5 cm/s, angular under 20 deg/s) and neither one has moved
// since the previous check. A cup that does not read is not at rest.
bool CupsAtRest() {
    constexpr float kLinCms = 5.f, kAngDegS = 20.f, kMovedCm = 0.5f;
    auto still = [](const ue_wrap::FVector& v, float lim) { return v.X * v.X + v.Y * v.Y + v.Z * v.Z < lim * lim; };
    void* a = g_cupA.Get();
    void* b = g_cupB.Get();
    ue_wrap::FVector linA{}, angA{}, linB{}, angB{}, atA{}, atB{};
    if (!a || !b || !E::GetActorRootPhysicsVelocity(a, linA, angA) || !E::GetActorRootPhysicsVelocity(b, linB, angB) ||
        !E::TryGetActorLocation(a, atA) || !E::TryGetActorLocation(b, atB))
        return false;
    const bool steady = g_restKnown && still({atA.X - g_restA.X, atA.Y - g_restA.Y, atA.Z - g_restA.Z}, kMovedCm) &&
                        still({atB.X - g_restB.X, atB.Y - g_restB.Y, atB.Z - g_restB.Z}, kMovedCm);
    g_restA = atA;
    g_restB = atB;
    g_restKnown = true;
    return steady && still(linA, kLinCms) && still(angA, kAngDegS) && still(linB, kLinCms) && still(angB, kAngDegS);
}

void StartWalk(void* hook) {
    ue_wrap::FVector a{}, b{};
    if (!hook || !H::EndLocations(hook, a, b)) { Abandon("the hook's ends do not read"); return; }
    g_walk = coop::director::StartBackgroundWalk(a, kWalkReachCm, kWalkDeadlineS);
    Next(Step::Walk);
}

void Reaim() {
    if (++g_reaims > kReaimMax) { Abandon("the trace kept leaving the hook after the aim took it"); return; }
    g_fan = coop::director::AimFan();
    Next(Step::Aim);
}

// What the press did, read from the lane's counters and the world. True when this peer's part is over.
// HOST (its own pickup, the game's): the hook is gone with one item in the inventory and no grant counted, its half;
// the client reports its copy gone. CLIENT: a press that asked the host nothing is the lane failing at its seat (the
// game's pickup ran on the copy, or the press was dropped) -- the clientruns red ends here; then the host's refusal
// is a fail, and its grant, which runs the pickup here, is the last event: the copy gone and one more item, ok.
bool JudgePress(bool host) {
    if (host) {
        if (g_hook.Alive()) return false;
        const int items = HookItems();
        if (coop::hook_retrieve::GrantedCount() != g_granted0) {
            Fail("the host player's own pickup was counted as a client's grant");
        } else if (items != g_items0 + 1) {
            char why[120];
            std::snprintf(why, sizeof(why), "the hook went with the host's pickup but its inventory holds %d hook item(s), "
                          "was %d", items, g_items0);
            Fail(why);
        } else {
            char what[140];
            std::snprintf(what, sizeof(what), "the hook went with the host player's pickup and its item reached the "
                          "inventory (%d -> %d), no grant", g_items0, items);
            HostHalfOk(what);
        }
        return true;
    }
    if (coop::hook_retrieve::AskedCount() == g_asked0) {
        char why[200];
        std::snprintf(why, sizeof(why), "the press asked the host nothing: the hook %s here and the inventory holds %d "
                      "hook item(s), was %d", g_hook.Alive() ? "still stands" : "went", HookItems(), g_items0);
        Fail(why);
        return true;
    }
    if (coop::hook_retrieve::RefusedAnswerCount() != g_refusedAns0) {
        Fail("the host refused a pickup the client made within reach of its hook");
        return true;
    }
    if (coop::hook_retrieve::TakenCount() == g_taken0) return false;
    const int items = HookItems();
    if (g_hook.Alive() || items != g_items0 + 1) {
        char why[140];
        std::snprintf(why, sizeof(why), "the host's grant ran here but the copy %s and the inventory holds %d hook "
                      "item(s), was %d", g_hook.Alive() ? "still stands" : "went", items, g_items0);
        Fail(why);
        return true;
    }
    char what[120];
    std::snprintf(what, sizeof(what), "the host granted the pickup once, the copy went and the item reached the "
                  "inventory (%d -> %d)", g_items0, items);
    Pass(what);
    return true;
}

// The taker's steps, either peer: walk to the head, turn the trace onto the hook, wait for the look to offer it (the
// pop-up), press E. False when the step is not one of these.
bool TakeTick(void* player, bool host) {
    switch (g_step) {
    case Step::Walk: {
        const int w = g_walk ? g_walk->state.load() : 2;
        if (w == 0) return true;
        g_walk.reset();
        if (w != 1) { Abandon("the walk to the hook did not arrive"); return true; }
        g_fan = coop::director::AimFan();
        Next(Step::Aim);
        return true;
    }
    case Step::Aim: {
        void* hook = g_hook.Get();
        ue_wrap::FVector a{}, b{};
        if (!hook || !H::EndLocations(hook, a, b)) { Abandon("the hook died or its ends do not read"); return true; }
        const bool aimed = E::ReadMainPlayerHitActor(player) == hook;
        const auto st = g_fan.Tick(player, a, aimed);
        if (st == coop::director::AimFan::State::Failed) {
            Fail("no pose of the fan put the player's trace on the hook -- no pop-up");
            return true;
        }
        if (st == coop::director::AimFan::State::Aimed) Next(Step::Ready);
        return true;
    }
    case Step::Ready: {
        void* hook = g_hook.Get();
        if (!hook) { Abandon("the hook died"); return true; }
        if (E::ReadMainPlayerHitActor(player) != hook) { Reaim(); return true; }
        if (E::MainPlayerHasSelectedAction(player)) {
            UE_LOGI("[HOOK-RETRIEVE-DRILL] %s: the look took the hook -- the pop-up offers its pickup", Role(host));
            Next(WindowMode() ? Step::Hold : Step::Press);
            return true;
        }
        if (Expired(kReadyBoundMs)) Abandon("the trace took the hook but the player's look had not offered it in 3 s");
        return true;
    }
    case Step::Hold: {   // window mode: the press waits for the joiner's save to be cut, and goes before its world is ready
        void* hook = g_hook.Get();
        if (!hook) { Abandon("the hook died"); return true; }
        if (E::ReadMainPlayerHitActor(player) != hook) { Reaim(); return true; }
        if (g_s && g_s->IsSlotWorldReady(1)) {
            Abandon("the client's world was ready before the host pressed: the window closed");
        } else if (coop::join_window_baseline::HasCapture(1)) {
            UE_LOGI("[HOOK-RETRIEVE-DRILL] host: the client's save is cut and its world is not ready -- pressing");
            Next(Step::Press);
        } else if (Expired(kWindowBoundMs)) {
            Abandon("no save of the client's was cut in 10 min after the host's aim took the hook");
        }
        return true;
    }
    case Step::Press: {
        void* hook = g_hook.Get();
        if (!hook) { Abandon("the hook died"); return true; }
        if (E::ReadMainPlayerHitActor(player) != hook) { Reaim(); return true; }
        g_items0 = HookItems();
        g_granted0 = coop::hook_retrieve::GrantedCount();
        g_asked0 = coop::hook_retrieve::AskedCount();
        g_taken0 = coop::hook_retrieve::TakenCount();
        g_refusedAns0 = coop::hook_retrieve::RefusedAnswerCount();
        if (!E::CallMainPlayerUseSelectedAction(player)) { Abandon("useSelectedAction did not dispatch"); return true; }
        UE_LOGI("[HOOK-RETRIEVE-DRILL] %s pressed the hook (%d hook item(s) carried)", Role(host), g_items0);
        Next(Step::Seen);
        // The press's own effect is already here: the game's pickup body is synchronous, so a host press that leaves its
        // hook standing is decided now. The client's grant is the host's answer, which is asynchronous: it waits.
        if (!JudgePress(host) && host)
            Fail("the host's press left its hook standing: the game's own pickup did not remove it");
        return true;
    }
    case Step::Seen:   // CLIENT: the host's grant or refusal, or the bound says INVALID
        if (JudgePress(false)) return true;
        if (Expired(kSeenBoundMs)) Abandon("15 s after the press neither the host's grant nor its refusal had come");
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
    if (!g_puppetSaid) {   // the client presses only after this: its request needs the puppet the host judges it by
        coop::RemotePlayer& rp = coop::puppet_drive::Puppet(1);
        if (rp.valid() && rp.HasPose()) {
            g_puppetSaid = true;
            coop::chat_sync::QueueSend(HostHead("puppet"));
        }
    }
    void* player = coop::players::Registry::Get().Local();
    if (!player) return;
    if (HostTakes() && TakeTick(player, true)) return;
    switch (g_step) {
    case Step::Arm: {
        if (!H::EnsureResolved()) return;
        // Before whom the cups stand: the client's puppet, or in the join modes the host itself, before anyone joined.
        void* before = nullptr;
        if (JoinMode()) {
            if (!coop::host_world_settle::Settled()) return;   // the world's own start-up is over (rig: READY hosting)
            before = player;
        } else {
            if (!s->IsSlotWorldReady(1)) return;
            before = coop::puppet_drive::Puppet(1).GetActor();
        }
        ue_wrap::FVector at{};
        if (!before || !R::IsLive(before) || !E::TryGetActorLocation(before, at)) return;
        const ue_wrap::FVector f = E::GetActorForwardVector(before);
        const ue_wrap::FVector right{-f.Y, f.X, 0.f};
        const ue_wrap::FVector mid{at.X + f.X * kAheadCm, at.Y + f.Y * kAheadCm, at.Z + 20.f};
        g_cupA.Set(CD::SpawnProp(L"prop_C", {mid.X - right.X * kApartCm, mid.Y - right.Y * kApartCm, mid.Z}, L"cup"));
        g_cupB.Set(CD::SpawnProp(L"prop_C", {mid.X + right.X * kApartCm, mid.Y + right.Y * kApartCm, mid.Z}, L"cup"));
        if (!g_cupA.Get() || !g_cupB.Get()) { Abandon("the cups did not spawn"); return; }
        UE_LOGI("[HOOK-RETRIEVE-DRILL] host: two cups spawned before %s", JoinMode() ? "the host" : "the client's puppet");
        Next(Step::Settle);
        return;
    }
    case Step::Settle:   // the cups fall to the floor first: they are at rest when neither moved since the last check
        if (CupsAtRest()) Next(Step::Tie);
        else if (Expired(kRestBoundMs)) Abandon("the cups did not come to rest in 30 s");
        return;
    case Step::Tie: {
        void* a = g_cupA.Get();
        void* b = g_cupB.Get();
        void* mesh = a ? PR::GetStaticMesh(a) : nullptr;
        ue_wrap::FVector at{};
        if (!a || !b || !mesh || !E::TryGetActorLocation(a, at)) { Abandon("a cup died or does not read"); return; }
        void* hook = H::SpawnCanonical(H::Kind::Hook, at, ue_wrap::FRotator{});
        if (!hook) { Abandon("the hook did not spawn"); return; }
        g_hook.Set(hook);
        if (!H::AttachHead(hook, a, mesh, {at.X, at.Y, at.Z + kHeadUpCm}, {0.f, 0.f, 1.f}, b, /*checkLen=*/false,
                           /*unfreezeFrozen=*/false) ||
            !H::WriteAttachedFlags(hook, true, true)) {
            Abandon("attach_a did not dispatch on the hook");
            return;
        }
        const auto r = coop::hook_anchor::SendCommit(kSeq, H::Kind::Hook, hook);
        if (r != coop::hook_anchor::CommitResult::AdoptedLocally) { Abandon("the hook lane did not adopt the hook"); return; }
        g_key = PR::GetActorSaveKeyString(hook);
        g_granted0 = coop::hook_retrieve::GrantedCount();
        UE_LOGI("[HOOK-RETRIEVE-DRILL] host: hook '%ls' tied between the cups and adopted -- the %s's to take",
                g_key.c_str(), HostTakes() ? "host" : "client");
        coop::chat_sync::QueueSend(HostHead("hook ") + std::string(g_key.begin(), g_key.end()));   // the clients' one hook
        Next(HostTakes() ? Step::Wait : Step::Watch);
        return;
    }
    case Step::Wait:   // HOST takes: once the client says it holds its copy, the host walks to the hook
        if (!WindowMode() && !ClientSaid(ClientHead("found"), nullptr)) {   // window: the client has no copy yet
            if (Expired(kTakeBoundMs)) Abandon("the client never said it held its copy of the hook");
            return;
        }
        UE_LOGI("[HOOK-RETRIEVE-DRILL] host: the client holds its copy -- walking to its hook to take it");
        StartWalk(g_hook.Get());
        return;
    case Step::Watch: {   // the CLIENT takes: the host's hook goes by one granted pickup
        const uint64_t granted = coop::hook_retrieve::GrantedCount() - g_granted0;
        if (g_hook.Alive()) {
            if (Expired(kTakeBoundMs)) Abandon("the hook still stands on the host 120 s after the tie");
            return;
        }
        if (granted != 1) {
            char why[120];
            std::snprintf(why, sizeof(why), "the hook went on the host with %llu granted pickups, not one",
                          static_cast<unsigned long long>(granted));
            Fail(why);
            return;
        }
        HostHalfOk("the hook went with one granted pickup of the client's");
        return;
    }
    case Step::Await:   // its half held: the client's report decides, or the bound says INVALID
        if (Expired(kDoneBoundMs)) Abandon("the client's report never came 120 s after the host's half held");
        return;
    default:
        return;
    }
}

// A mirror of the lane carries skipSave; a hook the save loaded does not.
bool SkipSave(void* hook) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(hook), L"skipSave", off, mask) || off < 0) return false;
    return (static_cast<const uint8_t*>(hook)[off] & mask) != 0;
}

// CLIENT, window mode, at the join's end: the host took its hook after this client's save was cut and before its world
// was ready, so the Gone of that moment never reached it. The connect replay's word has been applied by now (it lands
// ahead of the snapshot whose completion ends the join), so the copy the save loaded is gone, and a Gone took it.
void JudgeWindow(const std::wstring& key) {
    void* copy = coop::hook_retrieve::FindHook(key);   // the hook the host named, and no other
    const uint64_t gone = coop::hook_retrieve::GoneCopyCount();
    char what[160];
    if (!copy && gone > 0) {
        std::snprintf(what, sizeof(what), "the host's word at the join took this client's copy of the hook the host had "
                      "taken since the save was cut (%llu copy gone)", static_cast<unsigned long long>(gone));
        Pass(what);
    } else if (copy) {
        std::snprintf(what, sizeof(what), "a hook the host took after this client's save was cut still stands here after "
                      "the join (%llu copies taken by the host's word)", static_cast<unsigned long long>(gone));
        Fail(what);
    } else {
        Abandon("this client's save held no copy of the hook and none came: the take was before its cut");
    }
}

void ClientTick() {
    if (g_step == Step::Done) return;
    void* player = coop::players::Registry::Get().Local();
    if (!player) return;
    if (!HostTakes() && TakeTick(player, false)) return;
    switch (g_step) {
    case Step::Arm:
        if (!coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
            return;
        Next(Step::Find);
        return;
    case Step::Find: {
        std::wstring key;
        if (!HostHookKey(&key)) {   // the hook is the one the host names, and no other
            if (Expired(kFindBoundMs)) Abandon("the host never named its hook in 60 s");
            return;
        }
        if (WindowMode()) { JudgeWindow(key); return; }
        if (!HostSaid(HostHead("puppet"), nullptr)) {   // the host judges the press by its puppet of this client
            if (Expired(kFindBoundMs)) Abandon("the host never said its puppet of this client had a pose in 60 s");
            return;
        }
        void* hook = coop::hook_retrieve::FindHook(key);
        g_items0 = HookItems();
        if (!hook || g_items0 < 0) {
            if (Expired(kFindBoundMs)) Abandon("this client held no copy of the host's hook within 60 s");
            return;
        }
        g_hook.Set(hook);
        g_key = key;
        UE_LOGI("[HOOK-RETRIEVE-DRILL] client: the anchored copy of hook '%ls' is here (%s); %d hook item(s) carried",
                g_key.c_str(), SkipSave(hook) ? "a mirror" : "loaded by the save", g_items0);
        if (HostTakes()) {
            coop::chat_sync::QueueSend(ClientHead("found"));   // the host waits on it before it walks to its hook
            Next(Step::Watch);
            return;
        }
        StartWalk(hook);   // the director stages a far walk at a known-good anchor; nobody is teleported by the drill
        return;
    }
    case Step::Watch: {   // the HOST takes: this client's copy goes, and no item reaches this inventory
        if (g_hook.Alive()) {
            if (Expired(kTakeBoundMs)) Abandon("this client's copy still stands 120 s on -- the host's pickup never reached it");
            return;
        }
        const int items = HookItems();
        if (items != g_items0) {
            char why[120];
            std::snprintf(why, sizeof(why), "the copy went, but this inventory's hook items moved %d -> %d", g_items0,
                          items);
            Fail(why);
            return;
        }
        Pass("this copy of the hook went with the host's pickup and no item reached this inventory");
        return;
    }
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
    g_restKnown = false;
    g_cupA.Reset();
    g_cupB.Reset();
    g_hook.Reset();
    g_key.clear();
    g_granted0 = g_asked0 = g_taken0 = g_refusedAns0 = 0;
    g_items0 = 0;
    g_reaims = 0;
    g_hostOk = g_clientOk = g_clientDone = g_puppetSaid = false;
    g_clientWhy.clear();
    if (g_walk) coop::director::CancelBackgroundWalk(*g_walk);
    g_walk.reset();
}

}  // namespace coop::dev::hook_retrieve_drill
