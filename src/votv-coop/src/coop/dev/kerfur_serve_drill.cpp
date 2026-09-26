// coop/dev/kerfur_serve_drill.cpp -- see coop/dev/kerfur_serve_drill.h.

#include "coop/dev/kerfur_serve_drill.h"

#include "coop/config/config.h"
#include "coop/dev/director/director.h"
#include "coop/dev/spawn_npc.h"
#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/join_membership_sweep.h"
#include "coop/props/prop_snapshot.h"

#include "ue_wrap/actors/kerfur.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

namespace coop::dev::kerfur_serve_drill {
namespace {

namespace E  = ue_wrap::engine;
namespace EL = coop::element;
namespace K  = ue_wrap::kerfur;
namespace OI = ue_wrap::object_index;
namespace P  = ue_wrap::profile;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

constexpr const wchar_t* kDisc = L"prop_floppyDisc_R_C";  // a proper disc: the insert refuses the white one
constexpr const wchar_t* kVerb = L"actionName";
constexpr int      kTagVerb        = 0x4B535201;  // 'KSR' 1
constexpr float    kAwayMinCm      = 1000.f;      // the client's pile, 10 to 30 m out
constexpr float    kAwayMaxCm      = 3000.f;
constexpr float    kApartCm        = 1000.f;      // and 10 m from the host, flat
constexpr float    kNearHostCm     = 1000.f;      // the host's Omega spawns 2.5 m in front of it
constexpr float    kFollowCm       = 200.f;       // settled by a player: the 100 uu MoveTo acceptance, the lag
constexpr float    kDiscAsideCm    = 100.f;
constexpr uint64_t kSettleMs       = 5000;        // one passing by leaves in under 2 s
constexpr uint64_t kFollowWindowMs = 60000;       // from the client's follow
constexpr uint64_t kVerbWindowMs   = 240000;      // a client verb reaching the host: its waits, walk and leg A
constexpr uint64_t kMirrorWindowMs = 60000;       // the Omega's mirror, once the load tail quiesced
constexpr uint64_t kDiscWindowMs   = 60000;       // the host's disc named in its hand, from the mirror's arrival
constexpr uint64_t kSampleMs       = 100;
constexpr uint64_t kReportMs       = 5000;
constexpr uint8_t  kStateFollow    = 0;           // enum_kerfurCommand
constexpr int      kWalkDeadlineS  = 60;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_saidArm = false;

bool IsEnabled_() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::kerfur_serve_drill);
    return s;
}

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// A slot's body here: the local player, or a puppet once it has taken a pose.
bool BodyAt(int slot, ue_wrap::FVector& at) {
    auto& reg = coop::players::Registry::Get();
    if (slot == reg.LocalPeerId()) {
        void* me = reg.Local();
        return me && E::TryGetActorLocation(me, at);
    }
    coop::RemotePlayer* rp = reg.Puppet(static_cast<uint8_t>(slot));
    return rp && rp->valid() && rp->HasPose() && E::TryGetActorLocation(rp->GetActor(), at);
}

// An FString parameter's storage: the TArray<wchar_t> {Data, Num, Max}, Num counting the terminator.
struct FStringView {
    const wchar_t* data;
    int32_t num;
    int32_t max;
};

void* HoldingActor(void* player) {
    E::MainPlayerGrabState gs{};
    return (player && E::ReadMainPlayerGrabState(player, gs)) ? gs.holdingActor : nullptr;
}

// ---- host ----

enum class HostStep { WaitJoin, WaitReports, WaitIdle, Kill, Done };
HostStep g_host = HostStep::WaitJoin;
int g_joined = -1;
bool g_watched = false;
void* g_omega = nullptr;
int32_t g_omegaIdx = -1;
void* g_hand = nullptr;
int32_t g_handIdx = -1;
bool g_legB = false;
uint64_t g_hostStepMs = 0;

void HostInvalid(const char* why) {
    g_host = HostStep::Done;
    UE_LOGW("[KERFUR-SERVE] INVALID (host) -- %s", why);
}

// The host's own pickup of a red disc it spawns beside itself; its hand item is what the insert must leave.
bool TakeDisc() {
    void* me = coop::players::Registry::Get().Local();
    void* cls = R::FindClass(kDisc);
    ue_wrap::FVector at{};
    if (!me || !cls || !E::TryGetActorLocation(me, at)) {
        HostInvalid("the host or the red disc's class did not read");
        return false;
    }
    void* disc = E::SpawnActor(cls, ue_wrap::FVector{at.X + kDiscAsideCm, at.Y, at.Z});
    bool collected = false;
    if (!disc || !E::CallMainPlayerHoldObject(me, disc, collected) || !collected) {
        HostInvalid("the host's pickup of a red disc did not take");
        return false;
    }
    g_hand = HoldingActor(me);
    if (!g_hand || R::ClassNameOf(g_hand) != kDisc) {
        UE_LOGW("[KERFUR-SERVE] host: the hand holds %ls", g_hand ? R::ClassNameOf(g_hand).c_str() : L"nothing");
        HostInvalid("the host's hand did not come to hold the red disc");
        return false;
    }
    g_handIdx = R::InternalIndexOf(g_hand);
    return true;
}

// Leg B, as the client's get_reports returns: the insert inside it read somebody's hand. It refuses an
// empty one by going back to the follow state, so that state shows it ran rather than waited on a montage.
void JudgeReports() {
    bool has = true;
    const bool hasRead = K::ReadHasFloppy(g_omega, has);
    void* hand = HoldingActor(coop::players::Registry::Get().Local());
    const bool kept = R::IsLiveByIndex(g_hand, g_handIdx) && hand == g_hand;
    uint8_t state = 0xFF, face = 0;
    bool spooky = false;
    K::ReadKerfurState(g_omega, state, spooky, face);
    g_legB = kept && hasRead && !has && state == kStateFollow;
    UE_LOGI("[KERFUR-SERVE] host: leg B %s -- as the client's get_reports returned my red disc is %s, the Omega "
            "%s, in state %u", g_legB ? "PASS" : "FAIL", kept ? "still in my hand" : "GONE FROM MY HAND",
            !hasRead ? "did not read" : (has ? "HOLDS A DISC" : "holds none"), static_cast<unsigned>(state));
    g_hostStepMs = ::GetTickCount64();
    g_host = HostStep::WaitIdle;
}

// Leg C, the tick after the client's idle ran, outside any verb: the kill verb's event sets the follow state
// and calls move() before it sets `kill`, and whom that move() went for is the Omega's `targetActor` as the
// verb returns. The move() reaches its follow branch only with no montage, no server and no remote control,
// and an idle Omega has none of the three.
void RunKill() {
    void* me = coop::players::Registry::Get().Local();
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(g_joined));
    void* client = (rp && rp->valid()) ? rp->GetActor() : nullptr;
    if (!R::IsLiveByIndex(g_omega, g_omegaIdx) || !me) { HostInvalid("the Omega or my body is gone"); return; }
    K::RunActionName(g_omega, me, L"kill");
    const bool killed = K::ReadKill(g_omega);
    void* target = K::ReadTargetActor(g_omega);
    const bool legC = killed && target == me;
    UE_LOGI("[KERFUR-SERVE] host: leg C %s -- as the kill verb returned the Omega is %s and went for %ls", legC ? "PASS"
            : "FAIL", killed ? "murderous" : "NOT MURDEROUS", target == me ? L"me" : target == client ? L"THE CLIENT"
            : target ? L"ANOTHER ACTOR" : L"NOBODY");
    g_host = HostStep::Done;
    UE_LOGI("[KERFUR-SERVE] host DONE -- leg B (the disc insert) %s, leg C (murder mode) %s; leg A is the "
            "client's line", g_legB ? "PASS" : "FAIL", legC ? "PASS" : "FAIL");
}

// The client's get_reports and its closing idle, seen as each returns on the host: the idle is the client's
// word that leg A is judged, sent through the one channel every build runs, the verb.
void OnVerbPost(const sg::Call& c) {
    if (g_host != HostStep::WaitReports && g_host != HostStep::WaitIdle) return;
    static void*   sFn = nullptr;
    static int32_t sNameOff = -1;
    if (c.function != sFn) {
        sFn = c.function;
        sNameOff = R::FindParamOffset(c.function, L"name");
    }
    if (sNameOff < 0 || !c.locals) return;
    const auto* fs = reinterpret_cast<const FStringView*>(c.locals + sNameOff);
    if (!fs->data || fs->num <= 1) return;
    const std::wstring name(fs->data, static_cast<size_t>(fs->num - 1));
    if (g_host == HostStep::WaitReports && name == L"get_reports") {
        g_omega = c.object;
        g_omegaIdx = R::InternalIndexOf(c.object);
        JudgeReports();
    } else if (g_host == HostStep::WaitIdle && name == L"idle" && c.object == g_omega) {
        g_host = HostStep::Kill;
    }
}

void TickHost(coop::net::Session* s, uint64_t ms) {
    if (!g_watched)
        g_watched = sg::WatchClassName(P::name::NpcClass_KerfurOmega, kVerb, kTagVerb, nullptr, &OnVerbPost);
    if (g_host == HostStep::Done) return;
    if (g_host == HostStep::WaitJoin) {
        for (int i = 1; i < static_cast<int>(coop::players::kMaxPeers); ++i)
            if (s->IsSlotWorldReady(i) && coop::prop_snapshot::IsBracketClosed(i)) {
                g_joined = i;
                coop::dev::spawn_npc::SpawnKerfurOmega();
                if (!TakeDisc()) return;
                UE_LOGI("[KERFUR-SERVE] host: slot %d's join is over -- spawned an Omega in front of me, holding a red "
                        "disc (%p), staying here", i, g_hand);
                g_hostStepMs = ms;
                g_host = HostStep::WaitReports;
                return;
            }
        return;
    }
    if (g_host == HostStep::WaitReports || g_host == HostStep::WaitIdle) {
        if (ms - g_hostStepMs >= kVerbWindowMs)
            HostInvalid(g_host == HostStep::WaitReports ? "the client's get_reports never ran here"
                                                        : "the client's idle never ran here");
        return;
    }
    RunKill();
}

// ---- client ----

enum class ClientStep { WaitQuiet, WaitMirror, WaitHostDisc, WalkAway, JudgeFollow, Done };
ClientStep g_client = ClientStep::WaitQuiet;
void* g_mirror = nullptr;
int32_t g_mirrorIdx = -1;
std::shared_ptr<coop::director::BackgroundWalk> g_walk;
uint64_t g_stepMs = 0, g_sampleMs = 0, g_reportMs = 0, g_byMeSince = 0, g_byHostSince = 0;

void ClientInvalid(const char* why) {
    g_client = ClientStep::Done;
    UE_LOGW("[KERFUR-SERVE] INVALID (client) -- %s", why);
}

// The Omega mirror nearest the host's body, within the spawn's reach of it.
void* FindMirrorByHost() {
    void* cls = OI::ClassByName(P::name::NpcClass_KerfurOmega);
    ue_wrap::FVector host{};
    if (!cls || !BodyAt(0, host)) return nullptr;
    struct Ctx { ue_wrap::FVector host; void* best; float bestCm; } ctx{host, nullptr, kNearHostCm};
    OI::ForEachInstance(cls, [](void* p, void* obj, int32_t index) {
        auto* c = static_cast<Ctx*>(p);
        if (!obj || (R::SlotFlags(index) & (R::slot_flags::Dying | R::slot_flags::NotYetReadable))) return;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
        auto& reg = EL::Registry::Get();
        EL::Element* el = reg.Get(reg.EidForActor(obj));
        ue_wrap::FVector at{};
        if (!el || !el->IsMirror() || !E::TryGetActorLocation(obj, at)) return;
        const float d = Dist(at, c->host);
        if (d <= c->bestCm) { c->best = obj; c->bestCm = d; }
    }, &ctx);
    return ctx.best;
}

void StartWalkAway() {
    void* me = coop::players::Registry::Get().Local();
    ue_wrap::FVector host{};
    coop::director::DirectorGoal goal;
    if (!me || !BodyAt(0, host) ||
        !coop::director::PickReachablePile(me, kAwayMinCm, kAwayMaxCm, goal, &host, kApartCm)) {
        ClientInvalid("no nav-reachable pile 10 to 30 m away and 10 m from the host");
        return;
    }
    UE_LOGI("[KERFUR-SERVE] client: Omega mirror %p by the host -- walking to a pile at (%.0f,%.0f,%.0f), %.0f cm "
            "from the host", g_mirror, goal.targetPos.X, goal.targetPos.Y, goal.targetPos.Z,
            Dist(goal.targetPos, host));
    g_walk = coop::director::StartBackgroundWalk(goal.targetPos, goal.reachCm, kWalkDeadlineS);
    g_client = ClientStep::WalkAway;
}

// Leg A is judged: the idle tells the host, which runs leg C.
void LegAOver() {
    K::RunActionName(g_mirror, coop::players::Registry::Get().Local(), L"idle");
    UE_LOGI("[KERFUR-SERVE] client: commanded idle -- leg A is judged");
    g_client = ClientStep::Done;
}

// Leg A, judged here from this client's own follow, which is the same moment on every build: where its mirror
// settles on the host's pose stream, and the state the host's stream gives it there.
void JudgeFollow(uint64_t ms) {
    ue_wrap::FVector k{}, m{}, h{};
    const int me = coop::players::Registry::Get().LocalPeerId();
    if (!E::TryGetActorLocation(g_mirror, k) || !BodyAt(me, m) || !BodyAt(0, h)) {
        ClientInvalid("a place did not read");
        return;
    }
    const float toMe = Dist(k, m), toHost = Dist(k, h);
    const bool byMe = toMe <= kFollowCm && toHost > 2.f * kFollowCm;
    const bool byHost = toHost <= kFollowCm && toMe > 2.f * kFollowCm;
    g_byMeSince = byMe ? (g_byMeSince ? g_byMeSince : ms) : 0;
    g_byHostSince = byHost ? (g_byHostSince ? g_byHostSince : ms) : 0;
    const double after = static_cast<double>(ms - g_stepMs) / 1000.0;
    if (g_byMeSince && ms - g_byMeSince >= kSettleMs) {
        uint8_t state = 0xFF, face = 0;
        bool spooky = false;
        K::ReadKerfurState(g_mirror, state, spooky, face);
        const bool pass = state == kStateFollow;
        UE_LOGI("[KERFUR-SERVE] client: leg A %s -- the Omega settled %.0f cm from me, %.1f s after my follow (%.0f cm "
                "from the host), in state %u%s", pass ? "PASS" : "FAIL", toMe, after, toHost,
                static_cast<unsigned>(state), pass ? "" : ", not follow");
        LegAOver();
        return;
    }
    if (g_byHostSince && ms - g_byHostSince >= kSettleMs) {
        UE_LOGI("[KERFUR-SERVE] client: leg A FAIL -- the Omega I commanded settled %.0f cm from the host, %.1f s "
                "after my follow (%.0f cm from me)", toHost, after, toMe);
        LegAOver();
        return;
    }
    if (ms - g_stepMs >= kFollowWindowMs) {
        UE_LOGW("[KERFUR-SERVE] client: leg A UNDECIDED -- %llu s after my follow the Omega is %.0f cm from me, "
                "%.0f cm from the host", static_cast<unsigned long long>(kFollowWindowMs / 1000), toMe, toHost);
        LegAOver();
        return;
    }
    if (ms - g_reportMs >= kReportMs) {
        g_reportMs = ms;
        UE_LOGI("[KERFUR-SERVE] client: the Omega is %.0f cm from me, %.0f cm from the host", toMe, toHost);
    }
}

void TickClient() {
    const uint64_t ms = ::GetTickCount64();
    if (g_client != ClientStep::WaitQuiet && g_client != ClientStep::WaitMirror && g_client != ClientStep::Done &&
        !R::IsLiveByIndex(g_mirror, g_mirrorIdx)) {
        ClientInvalid("the Omega mirror is gone");
        return;
    }
    void* me = coop::players::Registry::Get().Local();
    switch (g_client) {
    case ClientStep::WaitQuiet:
        if (!coop::join_membership_sweep::HasLoadTailQuiesced()) return;
        g_stepMs = ms;
        g_client = ClientStep::WaitMirror;
        return;
    case ClientStep::WaitMirror:
        if ((g_mirror = FindMirrorByHost())) {
            g_mirrorIdx = R::InternalIndexOf(g_mirror);
            g_stepMs = ms;
            g_client = ClientStep::WaitHostDisc;
        } else if (ms - g_stepMs >= kMirrorWindowMs) {
            ClientInvalid("no Omega mirror came by the host");
        }
        return;
    case ClientStep::WalkAway: {
        const int w = g_walk ? g_walk->state.load() : 2;
        if (w == 0) return;
        if (w == 2) { ClientInvalid("the walk away did not arrive"); return; }
        // The radial menu's verb, through the menu interceptor: refused here, sent to the host.
        K::RunActionName(g_mirror, me, L"follow");
        UE_LOGI("[KERFUR-SERVE] client: at my pile -- commanded the Omega to follow");
        g_stepMs = g_sampleMs = g_reportMs = ms;
        g_byMeSince = g_byHostSince = 0;
        g_client = ClientStep::JudgeFollow;
        return;
    }
    case ClientStep::JudgeFollow:
        if (ms - g_sampleMs < kSampleMs) return;
        g_sampleMs = ms;
        JudgeFollow(ms);
        return;
    case ClientStep::WaitHostDisc:
        // The first command of this client to this Omega: its insert must read this client's hand, not the host's.
        if (coop::hand_item::HeldClassIs(0, kDisc)) {
            K::RunActionName(g_mirror, me, L"get_reports");
            UE_LOGI("[KERFUR-SERVE] client: the host holds a red disc -- commanded get_reports first");
            StartWalkAway();
        } else if (ms - g_stepMs >= kDiscWindowMs) {
            ClientInvalid("the host's hand never named a red disc");
        }
        return;
    default: return;
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    if (!IsEnabled_()) return;
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    if (!IsEnabled_()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    if (!g_saidArm) {
        g_saidArm = true;
        UE_LOGI("[KERFUR-SERVE] %s armed", s->role() == coop::net::Role::Host ? "host" : "client");
    }
    if (s->role() == coop::net::Role::Host) TickHost(s, ::GetTickCount64());
    else if (s->connected()) TickClient();
}

void OnDisconnect() {
    g_saidArm = false;
    g_host = HostStep::WaitJoin;
    g_joined = -1;
    g_omega = g_hand = nullptr;
    g_omegaIdx = g_handIdx = -1;
    g_legB = false;
    g_hostStepMs = 0;
    g_client = ClientStep::WaitQuiet;
    g_mirror = nullptr;
    g_mirrorIdx = -1;
    g_walk.reset();
    g_stepMs = g_sampleMs = g_reportMs = g_byMeSince = g_byHostSince = 0;
}

}  // namespace coop::dev::kerfur_serve_drill
