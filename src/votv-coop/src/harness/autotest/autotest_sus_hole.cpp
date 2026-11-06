// harness/autotest/autotest_sus_hole.cpp -- TEST-ONLY: a client's sus hole is rolled by its own game and made by the host.
//
// The client drives its own sus hole spawner's body, which is not refused, until one hole is stamped. Its hole goes to the host
// as an intent and the host makes the hole at that place, 100 to 500 m from the client. The hole must reach the client at that
// place, once (the client's own is gone), and stay there when the host's copy ends, as a hole ends by its own viewer. An intent
// the drill sends from beside its own player is refused. The host reads the lane's counters on both peers and prints
// `sus-hole: VERDICT`. Gated by VOTVCOOP_RUN_SUS_HOLE; the folder is VOTVCOOP_CAN_DIR. Red arms: VOTVCOOP_SUS_HOLE_RED.

#include "harness/autotest.h"
#include "harness/autotest/peer_signal.h"
#include "harness/session_runtime.h"

#include "coop/config/config.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/net_pump.h"
#include "coop/world/ambient_fixture_sync.h"
#include "coop/world/sus_hole_intent.h"
#include "ue_wrap/actors/ambient_creature.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace harness::autotest {
namespace {

namespace AC = ue_wrap::ambient_creature;
namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;
namespace PS = harness::autotest::peer_signal;
namespace SI = coop::sus_hole_intent;
using PS::Await;
using PS::AwaitText;
using PS::Mark;

int g_fails = 0;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

// A readiness wait on a state the game thread reads. A judged wait (`abort` false) ends in the caller's check.
template <class Pred>
bool WaitGT(const char* what, int seconds, Pred pred, bool abort = true) {
    return PS::WaitFor(what, seconds, [&] {
        bool ok = false;
        OnGameThread([&] { ok = pred(); });
        return ok;
    }, abort);
}

void Check(bool ok, const char* what) {
    UE_LOGI("sus-hole: %hs %hs", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}

// A peer's fails count as it left it in its done signal; text that is no count is a fail.
int ParseFails(const std::string& text) {
    char* end = nullptr;
    const long n = std::strtol(text.c_str(), &end, 10);
    return end != text.c_str() && n >= 0 ? static_cast<int>(n) : 1;
}

float Flat(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y;
    return std::sqrt(dx * dx + dy * dy);
}

std::string Place(const ue_wrap::FVector& v) {
    char b[96];
    std::snprintf(b, sizeof(b), "%.1f %.1f %.1f", v.X, v.Y, v.Z);
    return b;
}

bool ParsePlace(const std::string& text, ue_wrap::FVector& out) {
    std::istringstream in(text);
    return static_cast<bool>(in >> out.X >> out.Y >> out.Z);
}

// The holes standing within 150 cm of `at`.
size_t HolesNear(const ue_wrap::FVector& at) {
    std::vector<void*> holes;
    AC::ListHoles(holes);
    size_t n = 0;
    for (void* h : holes) {
        ue_wrap::FVector p{};
        if (E::TryGetActorLocation(h, p) && Flat(p, at) < 150.f && std::fabs(p.Z - at.Z) < 200.f) ++n;
    }
    return n;
}

// The host's own pawn.
void* Me() { return coop::players::Registry::Get().Local(); }

uint64_t Resolved(const SI::Counters& c) {
    uint64_t n = c.accepted;
    for (const uint64_t r : c.refused) n += r;
    return n;
}

// ---- the host ----

void HostBody() {
    if (!Await("client_ready", 480)) return;
    std::unordered_set<void*> before;
    OnGameThread([&] {
        std::vector<void*> holes;
        AC::ListHoles(holes);
        before.insert(holes.begin(), holes.end());
    });
    Mark("host_ready");

    // The client's own roll: its intent, and the hole the host makes of it.
    std::string sentText;
    if (!AwaitText("sent", sentText, 240)) return;
    ue_wrap::FVector sentAt{};
    const bool sent = ParsePlace(sentText, sentAt);
    bool made = false;
    if (sent) WaitGT("the host made the client's hole", 60, [&] { return (made = SI::Read().accepted >= 1); }, false);
    Check(made, "the host made a hole from the client's intent");
    void* hole = nullptr;
    ue_wrap::FVector at{};
    if (made) {
        OnGameThread([&] {
            std::vector<void*> holes;
            AC::ListHoles(holes);
            for (void* h : holes)
                if (!before.count(h)) hole = h;
            if (hole) E::TryGetActorLocation(hole, at);
        });
        Check(hole != nullptr, "the hole stands here");
    }
    if (hole) {
        ue_wrap::FVector client{};
        OnGameThread([&] {
            coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(1);
            if (rp && rp->valid()) E::TryGetActorLocation(rp->GetActor(), client);
        });
        const float off = Flat(at, client);
        UE_LOGI("sus-hole: hole at (%.0f,%.0f,%.0f), %.0f m from the client; the client's roll stood at (%.0f,%.0f,%.0f)", at.X, at.Y, at.Z,
                off / 100.f, sentAt.X, sentAt.Y, sentAt.Z);
        // The spawner's ring is 100 to 500 m; the host's own bound adds the pose pad on the outer edge.
        Check(off >= 9000.f && off <= 51000.f, "it stands 100 to 500 m from the client");
        Check(Flat(at, sentAt) < 10.f && std::fabs(at.Z - sentAt.Z) <= 200.f, "it stands at the place the client's own roll stood");
    }
    Mark("hole", hole ? Place(at) : "none");
    if (!Await("hole_seen", 120)) return;

    // The host's copy ends by the host's view; a hole's end does not cross. A second hole the host makes after it is the fence:
    // the reliable lane keeps order, so by the time it reaches the client an end sent before it would have.
    ue_wrap::FVector fenceAt{};
    if (hole) {
        OnGameThread([&] {
            if (R::IsLive(hole)) AC::Destroy(hole);
            void* pawn = Me();
            ue_wrap::FVector me{};
            if (pawn && E::TryGetActorLocation(pawn, me)) {
                fenceAt = {me.X + 1000.f, me.Y, me.Z};
                AC::SpawnFixture(false, fenceAt, {});
            }
        });
    }
    Mark("host_ended", hole ? Place(fenceAt) : "none");

    // An intent from beside its own player is refused: the count of intents judged must rise by one, and none be made.
    uint64_t resolved0 = 0, accepted0 = 0;
    OnGameThread([&] {
        resolved0 = Resolved(SI::Read());
        accepted0 = SI::Read().accepted;
    });
    Mark("go_bad");
    if (!Await("bad_sent", 120)) return;
    WaitGT("the near intent was judged", 60, [&] { return Resolved(SI::Read()) > resolved0; }, false);
    OnGameThread([&] {
        const SI::Counters c = SI::Read();
        UE_LOGI("sus-hole: judged=%llu made=%llu refused near=%llu far=%llu", static_cast<unsigned long long>(Resolved(c)),
                static_cast<unsigned long long>(c.accepted), static_cast<unsigned long long>(c.refused[SI::kTooNear]),
                static_cast<unsigned long long>(c.refused[SI::kTooFar]));
        Check(c.refused[SI::kTooNear] == 1 && c.accepted == accepted0, "an intent from beside its own player is refused, and no hole is made");
    });
}

void RunHost() {
    HostBody();
    std::string clientText;
    const int clientFails = AwaitText("client_done", clientText, 60) ? ParseFails(clientText) : 1;   // no word is no pass
    if (g_fails == 0 && clientFails == 0) UE_LOGI("sus-hole: VERDICT PASS host=%d client=%d", g_fails, clientFails);
    else UE_LOGW("sus-hole: VERDICT FAIL host=%d client=%d", g_fails, clientFails);
    UE_LOGI("sus-hole: DONE on host fails=%d", g_fails);
}

// ---- the client ----

bool ClientBody() {
    if (!WaitGT("this client's world", 240, [] { return Me() && coop::net_pump::HasAnnouncedWorldReady(); })) return false;
    Mark("client_ready");
    if (!Await("host_ready", 240)) return false;

    // Its own spawner's body, as the game runs it every 300 to 600 s: the roll is a random point and a grass trace, so the body
    // runs until the hook has stamped a hole.
    uint64_t stamped0 = 0, copies0 = 0;   // copies0: the hole copies before the host's answer can have come
    OnGameThread([&] { stamped0 = SI::Read().stamped; });
    bool stamped = false;
    for (int i = 0; i < 4000 && !stamped; ++i) {
        OnGameThread([&] {
            void* spawner = nullptr;
            if (void* c = OI::ClassByName(L"ticker_susHoleSpawner_C"))
                OI::ForEachInstance(c, [](void* ctx, void* obj, int32_t) {
                    if (R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) *static_cast<void**>(ctx) = obj;
                }, &spawner);
            void* fn = spawner ? R::FindDispatchFunctionCached(R::ClassOf(spawner), L"ReceiveTick") : nullptr;
            ue_wrap::ParamFrame f(fn);
            if (fn && f.valid()) {
                f.Set<float>(L"DeltaSeconds", 0.016f);
                ue_wrap::Call(spawner, f);
            }
            stamped = SI::Read().stamped > stamped0;
            if (stamped) copies0 = coop::ambient_fixture_sync::Read().holeCopies;
        });
    }
    Check(stamped, "this player's own sus hole spawner rolled a hole");

    // Its intent goes to the host; the lane destroys the stamped hole once the world is announced.
    WaitGT("this player's intent went out", 30, [] { return SI::Read().sent >= 1; }, false);
    ue_wrap::FVector sentAt{};
    bool sent = false;
    OnGameThread([&] {
        const SI::Counters c = SI::Read();
        sent = c.sent >= 1;
        sentAt = {c.sentX, c.sentY, c.sentZ};
    });
    Check(sent, "this player's roll went to the host as an intent");
    Mark("sent", sent ? Place(sentAt) : "none");

    // The host's hole: at that place, once.
    std::string body;
    if (!AwaitText("hole", body, 240)) return false;
    const bool hostMade = body != "none";
    if (hostMade) {
        WaitGT("the host's hole reached this player", 30, [&] { return coop::ambient_fixture_sync::Read().holeCopies > copies0; }, false);
        OnGameThread([&] {
            Check(coop::ambient_fixture_sync::Read().holeCopies > copies0, "the host's hole reached this player");
            Check(HolesNear(sentAt) == 1, "one hole stands at the place, the host's: this player's own is gone");
        });
    }
    Mark("hole_seen");

    // The host's copy ends and its fence hole follows: this player's hole stays.
    if (!AwaitText("host_ended", body, 240)) return false;
    if (hostMade) {
        WaitGT("the fence hole reached this player", 30, [&] { return coop::ambient_fixture_sync::Read().holeCopies > copies0 + 1; }, false);
        OnGameThread([&] {
            Check(coop::ambient_fixture_sync::Read().holeCopies > copies0 + 1, "the host's next hole reached this player");
            Check(HolesNear(sentAt) >= 1, "the host's copy ending leaves this player's hole");
        });
    }

    // An intent from beside this player, which the host must refuse.
    if (!Await("go_bad", 240)) return false;
    OnGameThread([&] {
        ue_wrap::FVector me{};
        if (!Me() || !E::TryGetActorLocation(Me(), me)) return;
        coop::net::SusHoleIntentPayload p{me.X + 2000.f, me.Y, me.Z};
        harness::session_runtime::Session().SendReliable(coop::net::ReliableKind::SusHoleIntent, &p, sizeof(p));
    });
    Mark("bad_sent");
    return true;
}

void RunClient() {
    const bool ran = ClientBody();
    Mark("client_done", std::to_string(ran ? g_fails : g_fails + 1));   // a client that stopped short is no pass
    UE_LOGI("sus-hole: DONE on client fails=%d", g_fails);
}

}  // namespace

DWORD WINAPI SusHoleThread(LPVOID /*arg*/) {
    if (!PS::Begin("sus-hole")) return 0;
    if (IsClientRole()) RunClient();
    else RunHost();
    return 0;
}

}  // namespace harness::autotest
