// coop/dev/kerfur_serve_drill_disc.cpp -- see coop/dev/kerfur_serve_drill_disc.h.

#include "coop/dev/kerfur_serve_drill_disc.h"

#include "coop/config/config.h"
#include "coop/dev/director/director.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/kerfur_serve_drill_internal.h"
#include "coop/dev/spawn_npc.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/props/join_membership_sweep.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/prop_snapshot.h"
#include "coop/session/rig_verdict.h"
#include "coop/session/shutdown.h"
#include "harness/autotest/meet.h"

#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/actors/kerfur.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace coop::dev::kerfur_serve_drill::disc {
namespace {

namespace E  = ue_wrap::engine;
namespace K  = ue_wrap::kerfur;
namespace OI = ue_wrap::object_index;
namespace P  = ue_wrap::profile;
namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace DC = coop::dev::drill_chat;
using coop::rig_verdict::Outcome;
namespace sg = ue_wrap::script_gate;
using detail::kDisc;

UE_FACT_FN(kRowActionName, "dev.kerfur_serve_drill_disc", L"kerfurOmega_C", L"actionName", Declared);
constexpr int      kTagVerb        = 0x4B535244;  // 'KSRD'
constexpr const char* kVerdictTag = "kerfur-serve-disc";  // leg D's name on its `rig: VERDICT` line, its own latch
constexpr int32_t  kGetReportsIdx  = 5;           // kerfurOmega::getActionOptions: [Turn_off, Follow, Idle, Patrol,
                                                  // Fix_servers, Get_reports, ...] for a kerfur not on a task
constexpr float    kNearHostCm     = 1000.f;      // the host's Omega spawns 2.5 m in front of it
constexpr float    kReachCm        = 150.f;       // the client's walk up to the Omega's mirror
constexpr float    kDiscAsideCm    = 100.f;
constexpr int      kWalkDeadlineS  = 60;
constexpr uint64_t kQuietWindowMs  = 240000;      // the client's load tail
constexpr uint64_t kMirrorWindowMs = 60000;       // the Omega's mirror, once the load tail quiesced
constexpr uint64_t kHandWindowMs   = 30000;       // the client's disc announced to the host
constexpr uint64_t kSnapWindowMs   = 60000;       // the host's snapshot, said after the client's line
constexpr uint64_t kStowWindowMs   = 10000;       // the stow of a destroyed disc announced
constexpr uint64_t kOmegaWindowMs  = 30000;       // the host's Omega found beside it, once spawned
constexpr uint64_t kBeforeWindowMs = 240000;      // the host waits for the client's line: its walk, pickup and aim
constexpr uint64_t kVerbWindowMs   = 30000;       // the host waits for the verb, once the client's after line is in
constexpr uint64_t kLaneWindowMs   = 5000;        // the client's stow applied on the host's hand lane

// The chat lines of the two peers; the rest after a prefix is `|`-separated.
const std::string kBeforeLine = "kerfur-serve client disc: ";        // class|key|item|floppies
const std::string kSnapLine   = "kerfur-serve host snapshot taken";
const std::string kAfterLine  = "kerfur-serve client disc after: ";  // gone|held|discs|localbody|state

constexpr const wchar_t* kDiscClasses[] = {L"prop_floppyDisc_R_C",  L"prop_floppyDisc_G_C", L"prop_floppyDisc_Y_C",
                                           L"prop_floppyDisc_Bl_C", L"prop_floppyDisc_Wh_C", L"prop_floppyDisc_B_C",
                                           L"prop_floppyDisc_O_C"};

std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<bool>  g_clientStarted{false};
std::atomic<void*> g_clientMirror{nullptr};   // the Omega mirror the client presses on, for the verb's watch
std::atomic<bool>  g_localBodyRan{false};     // the client's own copy ran get_reports
bool g_watched = false;
bool g_recording = false;  // the host's chat-history recording has started

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return s;
}

// The `|`-separated fields of `rest`, empties kept.
std::vector<std::string> Fields(const std::string& rest) {
    std::vector<std::string> out;
    size_t from = 0;
    for (;;) {
        const size_t bar = rest.find('|', from);
        out.push_back(rest.substr(from, bar == std::string::npos ? std::string::npos : bar - from));
        if (bar == std::string::npos) return out;
        from = bar + 1;
    }
}

// The discs standing on this peer, of the seven classes: the exact-class walk sees one class at a time. A disc
// in a hand counts (the hand item is an actor), one in an inventory does not (no reader exists for it).
struct Discs {
    int total = 0;
    std::string per;  // "R=1 G=0 ..." of the classes that stand
};
Discs CountDiscs() {
    Discs d;
    for (const wchar_t* name : kDiscClasses) {
        void* cls = OI::ClassByName(name);
        if (!cls) continue;
        int n = 0;
        OI::ForEachInstance(cls, [](void* p, void* obj, int32_t index) {
            if (!obj || (R::SlotFlags(index) & (R::slot_flags::Dying | R::slot_flags::NotYetReadable))) return;
            if (R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
            ++*static_cast<int*>(p);
        }, &n);
        d.total += n;
        if (n) d.per += Narrow(std::wstring(name).substr(16)) + "=" + std::to_string(n) + " ";
    }
    if (d.per.empty()) d.per = "none";
    return d;
}

// ---- host ----

enum class HostStep { WaitJoin, WaitOmega, WaitBefore, WaitAfter, Done };
HostStep g_host = HostStep::WaitJoin;
int g_joined = -1;
void* g_omega = nullptr;
int32_t g_omegaIdx = -1;
uint64_t g_hostStepMs = 0;
// The client's before line.
std::string g_clientCls, g_clientKey, g_clientDiscs;
// The host's side before the press.
struct Side {
    bool hasRead = false, has = false;
    bool laneHolds = false;
    std::string laneItem;
    bool keyHeld = false;
    int discs = 0;
    std::string per;
};
Side g_before;
bool g_verbRan = false, g_verbHas = false, g_verbHasRead = false;
uint8_t g_verbState = 0xFF;
std::string g_afterRest;
bool g_afterSeen = false;
uint64_t g_afterMs = 0;

// The verb, as it returns. On the host the client's relayed get_reports: whether the Omega took a disc, read inside
// the call's return. On the client the verb running on its own copy, which only the radial dispatch can bring about.
void OnVerbPost(const sg::Call& c) {
    std::wstring name;
    if (!detail::VerbNameOf(c, name) || name != L"get_reports") return;
    if (c.object && c.object == g_clientMirror.load(std::memory_order_acquire))
        g_localBodyRan.store(true, std::memory_order_release);
    if (g_host == HostStep::WaitAfter && c.object == g_omega) {
        g_verbRan = true;
        g_verbHasRead = K::ReadHasFloppy(g_omega, g_verbHas);
        bool spooky = false;
        uint8_t face = 0;
        K::ReadKerfurState(g_omega, g_verbState, spooky, face);
    }
}

// The nearest Omega to the host's own body within the spawn's reach, not a mirror (the host owns it).
void* FindOwnOmega() {
    void* cls = OI::ClassByName(P::name::NpcClass_KerfurOmega);
    void* me = coop::players::Registry::Get().Local();
    ue_wrap::FVector host{};
    if (!cls || !me || !E::TryGetActorLocation(me, host)) return nullptr;
    struct Ctx { ue_wrap::FVector host; void* best; float bestCm; } ctx{host, nullptr, kNearHostCm};
    OI::ForEachInstance(cls, [](void* p, void* obj, int32_t index) {
        auto* c = static_cast<Ctx*>(p);
        if (!obj || (R::SlotFlags(index) & (R::slot_flags::Dying | R::slot_flags::NotYetReadable))) return;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
        ue_wrap::FVector at{};
        if (!E::TryGetActorLocation(obj, at)) return;
        const float dx = at.X - c->host.X, dy = at.Y - c->host.Y, dz = at.Z - c->host.Z;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d <= c->bestCm) { c->best = obj; c->bestCm = d; }
    }, &ctx);
    return ctx.best;
}

Side ReadSide() {
    Side s;
    s.hasRead = K::ReadHasFloppy(g_omega, s.has);
    const uint8_t slot = static_cast<uint8_t>(g_joined);
    s.laneHolds = coop::hand_item::HeldClassIs(slot, kDisc);
    s.laneItem = Narrow(coop::hand_item::HeldItem(slot));
    s.keyHeld = !g_clientKey.empty() &&
                coop::prop_key_index::Find(std::wstring(g_clientKey.begin(), g_clientKey.end())) != nullptr;
    const Discs d = CountDiscs();
    s.discs = d.total;
    s.per = d.per;
    return s;
}

void LogSide(const char* when, const Side& s) {
    UE_LOGI("[KERFUR-SERVE] host: %s -- Omega hasFloppy=%s, the client's hand as my lane holds it: class %s item '%s', "
            "an actor holding its disc's key here=%d, discs standing here=%d (%s)", when,
            !s.hasRead ? "unread" : (s.has ? "1" : "0"), s.laneHolds ? "a red disc" : "none", s.laneItem.c_str(),
            s.keyHeld ? 1 : 0, s.discs, s.per.c_str());
}

void Snapshot(const std::string& rest) {
    const auto f = Fields(rest);
    g_clientCls = f.size() > 0 ? f[0] : "";
    g_clientKey = f.size() > 1 ? f[1] : "";
    g_clientDiscs = f.size() > 3 ? f[3] : "?";
    g_before = ReadSide();
    LogSide("before the press", g_before);
}

void HostFinish() {
    const Side after = ReadSide();
    LogSide("after the press", after);
    const auto f = Fields(g_afterRest);
    const std::string clientAfter = f.size() > 0 ? f[0] : "?";
    UE_LOGI("[KERFUR-SERVE] host: the verb ran here=%d; at it the Omega hasFloppy=%s in state %u; the client says its "
            "copy ran the verb=%s, its Omega in state %s", g_verbRan ? 1 : 0,
            !g_verbHasRead ? "unread" : (g_verbHas ? "1" : "0"), static_cast<unsigned>(g_verbState),
            f.size() > 2 ? f[2].c_str() : "?", f.size() > 3 ? f[3].c_str() : "?");
    const std::string insert = !g_verbRan ? "none (the verb never ran here)" : (g_verbHas ? "1" : "0");
    UE_LOGI("[KERFUR-SERVE] leg D: client disc before=%s after=%s host insert=%s host still holds the client's disc "
            "key=%d", g_clientCls.c_str(), clientAfter.c_str(), insert.c_str(), after.keyHeld ? 1 : 0);
    UE_LOGI("[KERFUR-SERVE] leg D detail: the host's key for the client's disc before=%d after=%d; its hand lane held "
            "the disc before=%d after=%d (item '%s' -> '%s'); discs standing on the host %d -> %d; on the client %s -> "
            "%s", g_before.keyHeld ? 1 : 0, after.keyHeld ? 1 : 0, g_before.laneHolds ? 1 : 0, after.laneHolds ? 1 : 0,
            g_before.laneItem.c_str(), after.laneItem.c_str(), g_before.discs, after.discs, g_clientDiscs.c_str(),
            f.size() > 1 ? f[1].c_str() : "?");
    g_host = HostStep::Done;
    if (!g_verbRan) {  // the relayed command never reached the Omega: the question was not asked of the host
        coop::rig_verdict::Say(kVerdictTag, Outcome::Invalid,
                               "host: the client's relayed get_reports never ran on my Omega");
        return;
    }
    coop::rig_verdict::Say(kVerdictTag, Outcome::Pass, "leg D measured: client disc %s, host insert=%s, key kept=%d",
                           clientAfter.c_str(), insert.c_str(), after.keyHeld ? 1 : 0);
}

void TickHost(coop::net::Session* s, uint64_t ms) {
    if (g_host == HostStep::Done) return;
    if (g_host == HostStep::WaitJoin) {
        for (int i = 1; i < static_cast<int>(coop::players::kMaxPeers); ++i)
            if (s->IsSlotWorldReady(i) && coop::prop_snapshot::IsBracketClosed(i)) {
                g_joined = i;
                coop::dev::spawn_npc::SpawnKerfurOmega();
                UE_LOGI("[KERFUR-SERVE] host: slot %d's join is over -- spawned an Omega in front of me, staying here", i);
                g_hostStepMs = ms;
                g_host = HostStep::WaitOmega;
                return;
            }
        return;
    }
    if (g_host == HostStep::WaitOmega) {
        if ((g_omega = FindOwnOmega())) {
            g_omegaIdx = R::InternalIndexOf(g_omega);
            UE_LOGI("[KERFUR-SERVE] host: my Omega is %p", g_omega);
            g_hostStepMs = ms;
            g_host = HostStep::WaitBefore;
        } else if (ms - g_hostStepMs >= kOmegaWindowMs) {
            detail::HostInvalid("my Omega never stood beside me", kVerdictTag);
            g_host = HostStep::Done;
        }
        return;
    }
    if (!R::IsLiveByIndex(g_omega, g_omegaIdx)) {
        detail::HostInvalid("my Omega is gone", kVerdictTag);
        g_host = HostStep::Done;
        return;
    }
    if (g_host == HostStep::WaitBefore) {
        std::string rest;
        if (DC::SeenRestOf(kBeforeLine, &rest)) {
            Snapshot(rest);
            DC::Say(kSnapLine);
            g_hostStepMs = ms;
            g_host = HostStep::WaitAfter;
        } else if (ms - g_hostStepMs >= kBeforeWindowMs) {
            detail::HostInvalid("the client's disc line never came", kVerdictTag);
            g_host = HostStep::Done;
        }
        return;
    }
    // WaitAfter: the client's line, then the verb here (it travelled the same lane first, but not the same tick),
    // then its stow on my hand lane.
    if (!g_afterSeen) {
        if (DC::SeenRestOf(kAfterLine, &g_afterRest)) {
            g_afterSeen = true;
            g_afterMs = ms;
        } else if (ms - g_hostStepMs >= kBeforeWindowMs) {
            detail::HostInvalid("the client's after line never came", kVerdictTag);
            g_host = HostStep::Done;
        }
        return;
    }
    const bool laneBusy = coop::hand_item::HandStateOwed(static_cast<uint8_t>(g_joined));
    if ((!g_verbRan && ms - g_afterMs < kVerbWindowMs) || (laneBusy && ms - g_afterMs < kLaneWindowMs)) return;
    HostFinish();
}

// ---- client ----

// The walk, the aim and the press run on one worker thread: the director's WalkToHost blocks while it walks. Each
// engine read goes to the game thread by RunAndWait; a wait is on a state the game thread reads, with a bound.
bool Alive() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->connected() && !coop::shutdown::IsShuttingDown();
}

bool WaitGame(const std::function<bool()>& pred, uint64_t boundMs) {
    const uint64_t t0 = ::GetTickCount64();
    for (;;) {
        bool ok = false;
        bool alive = true;
        if (GT::RunAndWait([&](std::atomic<int>& done) {
                alive = Alive();
                ok = alive && pred();
                done.store(1);
            }) != 1)
            return false;
        if (ok) return true;
        if (!alive || ::GetTickCount64() - t0 >= boundMs) return false;
        ::Sleep(100);
    }
}

bool Run(const std::function<bool()>& fn) {
    bool ok = false;
    if (GT::RunAndWait([&](std::atomic<int>& done) { ok = fn(); done.store(1); }) != 1) return false;
    return ok;
}

// A step that could not run: the drill's own client invalid path, unless the session already ended.
void Invalid(const char* why) {
    Run([&] {
        if (Alive()) detail::ClientInvalid(why, kVerdictTag);
        return true;
    });
}

// The camera turned at the mirror through a fan of headings, each held until the player's interaction trace and its
// look-at name the mirror and the look took it (what useSelectedAction is sent to).
bool AimedAt(void* mirror) {
    void* me = coop::players::Registry::Get().Local();
    return me && E::ReadMainPlayerHitActor(me) == mirror && E::ReadMainPlayerLookAtActor(me) == mirror &&
           E::MainPlayerHasSelectedAction(me);
}

bool AimFan(void* mirror, int32_t mirrorIdx) {
    static constexpr float kYaw[] = {0.f, -5.f, 5.f, -10.f, 10.f, -16.f, 16.f};
    static constexpr float kPitch[] = {0.f, 8.f, -8.f, 16.f, -16.f};
    for (float dp : kPitch) {
        for (float dy : kYaw) {
            if (!Run([=] {
                    void* p = coop::players::Registry::Get().Local();
                    void* c = p ? E::GetController(p) : nullptr;
                    ue_wrap::FVector at{};
                    if (!c || !R::IsLiveByIndex(mirror, mirrorIdx) || !E::TryGetActorLocation(mirror, at)) return false;
                    ue_wrap::FRotator r = coop::director::LookAt(E::GetCameraLocation(), at);
                    r.Pitch += dp;
                    r.Yaw += dy;
                    E::SetControlRotation(c, r);
                    return true;
                }))
                return false;
            if (WaitGame([=] { return AimedAt(mirror); }, 300)) return true;
            if (!Alive()) return false;
        }
    }
    return false;
}

struct Before {
    std::string cls, key, item;
    int discs = 0;
    std::string per;
    uint8_t state = 0xFF;
};

DWORD WINAPI ClientThread(LPVOID) {
    void* mirror = nullptr;
    int32_t mirrorIdx = -1;
    Run([] { DC::SeenHas(kSnapLine); return true; });  // starts the chat history's recording
    if (!WaitGame([] { return coop::join_membership_sweep::HasLoadTailQuiesced(); }, kQuietWindowMs)) {
        Invalid("the load tail never quiesced");
        return 0;
    }
    if (!harness::autotest::meet::WalkToHost("[KERFUR-SERVE]", 300.f)) {
        Invalid("the walk to the host did not arrive");
        return 0;
    }
    if (!WaitGame([&] {
            mirror = detail::FindMirrorByHost();
            if (mirror) mirrorIdx = R::InternalIndexOf(mirror);
            return mirror != nullptr;
        }, kMirrorWindowMs)) {
        Invalid("no Omega mirror came by the host");
        return 0;
    }
    g_clientMirror.store(mirror, std::memory_order_release);

    // The client's own red disc in hand, by the game's own pickup, as the host's drill takes its own.
    Before b;
    if (!Run([&] {
            void* me = coop::players::Registry::Get().Local();
            void* cls = R::FindClass(kDisc);
            ue_wrap::FVector at{};
            if (!me || !cls || !E::TryGetActorLocation(me, at)) return false;
            void* disc = E::SpawnActor(cls, ue_wrap::FVector{at.X + kDiscAsideCm, at.Y, at.Z});
            bool collected = false;
            return disc && E::CallMainPlayerHoldObject(me, disc, collected) && collected;
        })) {
        Invalid("the client's pickup of a red disc did not take");
        return 0;
    }
    if (!WaitGame([&] {
            void* hand = detail::HoldingActor(coop::players::Registry::Get().Local());
            return hand && R::ClassNameOf(hand) == kDisc && coop::hand_item::OwnAnnounced(kDisc);
        }, kHandWindowMs)) {
        Invalid("the client's hand never announced the red disc");
        return 0;
    }

    // Within reach of the mirror.
    std::shared_ptr<coop::director::BackgroundWalk> walk;
    Run([&] {
        ue_wrap::FVector me{}, at{};
        void* p = coop::players::Registry::Get().Local();
        if (!p || !E::TryGetActorLocation(p, me) || !E::TryGetActorLocation(mirror, at)) return false;
        const float dx = me.X - at.X, dy = me.Y - at.Y;
        if (std::sqrt(dx * dx + dy * dy) > kReachCm) walk = coop::director::StartBackgroundWalk(at, kReachCm, kWalkDeadlineS);
        return true;
    });
    if (walk && !WaitGame([&] { return walk->state.load() != 0; }, (kWalkDeadlineS + 5) * 1000ull)) {
        Invalid("the walk up to the Omega did not end");
        return 0;
    }
    if (walk && walk->state.load() != 1) {
        Invalid("the walk up to the Omega did not arrive");
        return 0;
    }
    if (!AimFan(mirror, mirrorIdx)) {
        if (Alive()) Invalid("no aim of the fan put the trace and the look-at on the Omega's mirror");
        return 0;
    }

    // What the client holds, before the press, said to the host.
    Run([&] {
        void* me = coop::players::Registry::Get().Local();
        void* hand = detail::HoldingActor(me);
        b.cls = hand ? Narrow(R::ClassNameOf(hand)) : "";
        b.key = hand ? Narrow(ue_wrap::prop::GetKeyString(hand)) : "";
        b.item = Narrow(ue_wrap::inventory::ReadHeldItemName());
        const Discs d = CountDiscs();
        b.discs = d.total;
        b.per = d.per;
        bool spooky = false;
        uint8_t face = 0;
        K::ReadKerfurState(mirror, b.state, spooky, face);
        UE_LOGI("[KERFUR-SERVE] client: before the press -- hand holds %s key '%s' item '%s'; discs standing here=%d (%s); "
                "the Omega mirror %p in state %u, aimed at", b.cls.c_str(), b.key.c_str(), b.item.c_str(), b.discs,
                b.per.c_str(), mirror, static_cast<unsigned>(b.state));
        DC::Say(kBeforeLine + b.cls + "|" + b.key + "|" + b.item + "|" + std::to_string(b.discs));
        return true;
    });
    if (b.cls != Narrow(kDisc)) {
        Invalid("the client's hand did not hold the red disc at the press");
        return 0;
    }
    if (!WaitGame([] { return DC::SeenHas(kSnapLine); }, kSnapWindowMs)) {
        Invalid("the host never said its snapshot");
        return 0;
    }

    // The press, as a player's: the command goes to the host as the menu interceptor relays it, then the radial
    // dispatch itself runs the verb on this copy (an interface call, which the interceptor never sees).
    bool pressOk = false, relayOk = false, indexOk = false, aimed = false;
    bool gone = false;
    uint8_t stateAfter = 0xFF;
    Run([&] {
        void* me = coop::players::Registry::Get().Local();
        aimed = AimedAt(mirror);
        if (!aimed) return true;
        indexOk = E::CallMainPlayerSetActionIndex(me, kGetReportsIdx);
        relayOk = K::RunActionName(mirror, me, L"get_reports");
        pressOk = E::CallMainPlayerUseSelectedAction(me);
        void* hand = detail::HoldingActor(me);
        // A destroyed actor still named by holding_actor is gone: the Blueprint nulls the field after the destroy.
        gone = !hand || !R::IsLiveByIndex(hand, R::InternalIndexOf(hand)) || R::ClassNameOf(hand) != kDisc;
        bool spooky = false;
        uint8_t face = 0;
        K::ReadKerfurState(mirror, stateAfter, spooky, face);
        return true;
    });
    if (!aimed) {
        Invalid("the aim was lost before the press");
        return 0;
    }
    if (!indexOk || !relayOk || !pressOk) {
        Invalid(!indexOk ? "setActionIndex did not run" : !relayOk ? "the relayed command did not run"
                                                                    : "useSelectedAction did not dispatch");
        return 0;
    }
    const bool bodyRan = g_localBodyRan.load(std::memory_order_acquire);
    UE_LOGI("[KERFUR-SERVE] client: pressed get_reports (setActionIndex %d, the relay through the menu interceptor, "
            "useSelectedAction) -- the client's own copy ran the verb=%d, the disc is %s, the Omega mirror in state %u",
            kGetReportsIdx, bodyRan ? 1 : 0, gone ? "GONE from my hand" : "still in my hand",
            static_cast<unsigned>(stateAfter));
    if (gone && !WaitGame([] { return !coop::hand_item::OwnAnnounced(kDisc); }, kStowWindowMs))
        UE_LOGW("[KERFUR-SERVE] client: the stow of the destroyed disc was not announced in %llu s",
                static_cast<unsigned long long>(kStowWindowMs / 1000));
    Run([&] {
        const Discs d = CountDiscs();
        UE_LOGI("[KERFUR-SERVE] client: after the press -- discs standing here=%d (%s)", d.total, d.per.c_str());
        DC::Say(kAfterLine + (gone ? "gone" : "held") + "|" + std::to_string(d.total) + "|" + (bodyRan ? "1" : "0") +
                "|" + std::to_string(stateAfter));
        return true;
    });
    UE_LOGI("[KERFUR-SERVE] leg D (client side): client disc before=%s after=%s, its own copy ran the verb=%d; the "
            "host's line carries the insert and the key", b.cls.c_str(), gone ? "gone" : "held", bodyRan ? 1 : 0);
    return 0;
}

}  // namespace

bool Enabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::kerfur_serve_disc);
    return s;
}

void Tick(coop::net::Session* s) {
    g_session.store(s, std::memory_order_release);
    if (!g_watched) g_watched = sg::WatchName(kRowActionName, kTagVerb, nullptr, &OnVerbPost);
    if (s->role() == coop::net::Role::Host) {
        if (!g_recording) {  // the chat history of the client's lines starts with the first read
            g_recording = true;
            DC::SeenHas(kSnapLine);
        }
        TickHost(s, ::GetTickCount64());
        return;
    }
    if (s->connected() && !g_clientStarted.exchange(true)) {
        if (HANDLE h = ::CreateThread(nullptr, 0, &ClientThread, nullptr, 0, nullptr)) ::CloseHandle(h);
    }
}

void OnDisconnect() {
    g_recording = false;
    g_clientStarted.store(false);
    g_clientMirror.store(nullptr);
    g_localBodyRan.store(false);
    g_host = HostStep::WaitJoin;
    g_joined = -1;
    g_omega = nullptr;
    g_omegaIdx = -1;
    g_hostStepMs = g_afterMs = 0;
    g_clientCls.clear();
    g_clientKey.clear();
    g_clientDiscs.clear();
    g_before = Side{};
    g_verbRan = g_verbHas = g_verbHasRead = false;
    g_verbState = 0xFF;
    g_afterRest.clear();
    g_afterSeen = false;
}

}  // namespace coop::dev::kerfur_serve_drill::disc
