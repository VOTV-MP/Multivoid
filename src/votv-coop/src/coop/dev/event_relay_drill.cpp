// coop/dev/event_relay_drill.cpp -- see coop/dev/event_relay_drill.h.
//
// The steps, each ending on a line the peer prints:
//   1. HOST, once slot 1 is world-ready and its bracket closed: reads its event count (h0), spawns an npc_krampus_C
//      1500 cm above itself, parks it (no movement, no gravity) and clears its four BeginPlay timers, so it
//      neither stabs, teleports nor despawns; it prints the krampus's eid and the count then.
//   2. CLIENT (each life), once its join is over: finds the krampus mirror, clears the same four timers on it, and
//      once its target and applied counts are both 1 reads its copies in the event senders, its count and
//      lib_C::getEvent; the read step passes when the copies are 1 and getEvent is true. The bound is 60 s from the
//      join.
//   3. HOST: counts the rising edges of slot 1's world-ready (the second is the rejoin's) and the chat rows that
//      hold the read line; at the second row it destroys the krampus; the done step passes once its count is back
//      to h0. The bound is 60 s from the second edge.
//   4. CLIENT, after its read PASS: once its entry for the krampus is gone, the done step passes when its count
//      fell by one. The bound is 60 s from its read PASS.

#include "coop/dev/event_relay_drill.h"

#include "coop/comms/chat_feed.h"
#include "coop/comms/chat_sync.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/spawn_npc.h"
#include "coop/element/mirror_managers.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_snapshot.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/world/creature_event_relay.h"

#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/engine/save_capture.h"
#include "ue_wrap/world/active_events.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace coop::dev::event_relay_drill {
namespace {

namespace R  = ue_wrap::reflection;
namespace AE = ue_wrap::active_events;
namespace EL = coop::element;

constexpr const wchar_t* kKrampus = L"npc_krampus_C";
constexpr const char* kKrampusTypeName = "npc_krampus_C";
constexpr float kSpawnAboveCm = 1500.f;
constexpr long long kBoundMs = 60000;
constexpr std::string_view kReadLine = "evr: read";

std::atomic<coop::net::Session*> g_session{nullptr};

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

const char* RoleName(bool host) { return host ? "host" : "client"; }

// One verdict line: the tag, PASS or FAIL, the step, the role, then the readings.
void Verdict(bool pass, const char* step, bool host, const char* fmt, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (pass) UE_LOGI("[EVR] PASS %s role=%s %s", step, RoleName(host), buf);
    else      UE_LOGW("[EVR] FAIL %s role=%s %s", step, RoleName(host), buf);
}

int ReadCount() {
    int32_t n = -1;
    AE::ReadCount(n);
    return n;
}

int CopiesOf(void* actor) {
    AE::Senders arr{};
    if (!actor || !AE::EnsureResolved() || !AE::ReadSenders(arr) || arr.num < 0 || arr.num > 4096 || !arr.data) return 0;
    int c = 0;
    for (int32_t i = 0; i < arr.num; ++i)
        if (arr.data[i] == actor) ++c;
    return c;
}

// The krampus's four BeginPlay timers (npc_krampus.cpp:389-404), cleared as kerfur::NeutralizeAiTimers clears a
// kerfur's: K2_ClearTimer(Object, FunctionName) on KismetSystemLibrary's default object, the name an FString whose
// count and capacity include the terminator.
void ClearKrampusTimers(void* actor) {
    static void* s_ksl = nullptr;
    static void* s_clear = nullptr;
    if (!s_ksl) s_ksl = R::FindClassDefaultObject(L"KismetSystemLibrary");
    if (s_ksl && !s_clear) {
        if (void* kc = R::FindClass(L"KismetSystemLibrary")) s_clear = R::FindFunction(kc, L"K2_ClearTimer");
    }
    if (!s_ksl || !s_clear) {
        UE_LOGW("[EVR] K2_ClearTimer unresolved -- the krampus's timers are NOT cleared");
        return;
    }
    static const wchar_t* kTimers[4] = {L"timerButter", L"playerOverlap", L"tryToDespawn", L"safeTp"};
    for (const wchar_t* fn : kTimers) {
        ue_wrap::ParamFrame f(s_clear);
        if (!f.valid()) continue;
        f.Set<void*>(L"Object", actor);
        const int32_t num = static_cast<int32_t>(std::wcslen(fn)) + 1;
        struct { const wchar_t* data; int32_t n; int32_t m; } fs{fn, num, num};
        if (!f.SetRaw(L"FunctionName", &fs, sizeof(fs))) continue;
        ue_wrap::Call(s_ksl, f);
    }
}

// ---- the host -----------------------------------------------------------------------------------
bool g_hostSpawned = false;     // step 1 ran (or failed): once per process
bool g_hostFinished = false;
ue_wrap::CachedObjRef g_krampus;
EL::ElementId g_krampusEid = EL::kInvalidId;
int  g_h0 = 0;
bool g_lastReady1 = false;
int  g_readyEdges = 0;
long long g_edge2Ms = 0;
bool g_destroyed = false;
int  g_rowsSeen = 0;

void HostFinish() { g_hostFinished = true; }

void HostSpawn() {
    g_hostSpawned = true;
    g_h0 = ReadCount();
    void* local = coop::players::Registry::Get().Local();
    ue_wrap::FVector at{};
    if (!local || !ue_wrap::engine::TryGetActorLocation(local, at)) {
        Verdict(false, "spawn", true, "the host pawn's location could not be read");
        HostFinish();
        return;
    }
    at.Z += kSpawnAboveCm;
    void* krampus = coop::dev::spawn_npc::SpawnAt(kKrampus, at);
    if (!krampus) {
        Verdict(false, "spawn", true, "SpawnAt returned null");
        HostFinish();
        return;
    }
    ue_wrap::puppet::DisableCharacterTicks(krampus);
    ClearKrampusTimers(krampus);
    g_krampus.Set(krampus);
    g_krampusEid = EL::Registry::Get().EidForActor(krampus);
    if (g_krampusEid == EL::kInvalidId) {
        Verdict(false, "spawn", true, "the krampus was not enrolled as an Npc element");
        HostFinish();
        return;
    }
    UE_LOGI("[EVR] host spawned eid=%u n=%d", g_krampusEid, ReadCount());
}

// The chat rows whose text holds the read line, both tiers, counted into g_rowsSeen.
void CountReadRows() {
    g_rowsSeen = 0;
    static const std::function<void(const coop::chat_feed::RowView&)> fn = [](const coop::chat_feed::RowView& r) {
        if (r.line.find(kReadLine) != std::string_view::npos) ++g_rowsSeen;
    };
    coop::chat_feed::ForEachRow(fn);
}

void HostTick(coop::net::Session* s) {
    if (g_hostFinished) return;
    const bool ready1 = s->IsSlotWorldReady(1);
    if (ready1 && !g_lastReady1) {
        ++g_readyEdges;
        if (g_readyEdges == 2) g_edge2Ms = NowMs();
    }
    g_lastReady1 = ready1;
    if (!g_hostSpawned) {
        if (ready1 && coop::prop_snapshot::IsBracketClosed(1)) HostSpawn();
        return;
    }
    if (g_readyEdges < 2) return;
    if (NowMs() - g_edge2Ms >= kBoundMs) {
        Verdict(false, "done", true, "expired destroyed=%d rows=%d n=%d h0=%d", g_destroyed ? 1 : 0, g_rowsSeen,
                ReadCount(), g_h0);
        HostFinish();
        return;
    }
    if (!g_destroyed) {
        CountReadRows();
        if (g_rowsSeen < 2) return;
        void* krampus = g_krampus.Get();
        if (!krampus) {
            Verdict(false, "destroy", true, "the krampus is already gone");
            HostFinish();
            return;
        }
        ue_wrap::engine::DestroyActor(krampus);
        g_destroyed = true;
        return;
    }
    if (ReadCount() == g_h0) {
        Verdict(true, "done", true, "n=%d h0=%d", ReadCount(), g_h0);
        HostFinish();
    }
}

// ---- the client ---------------------------------------------------------------------------------
enum class Phase { WaitJoin, Mirror, End, Done };
Phase g_phase = Phase::WaitJoin;
long long g_phaseStartMs = 0;
EL::ElementId g_mirrorEid = EL::kInvalidId;
ue_wrap::CachedObjRef g_mirror;
int g_n1 = 0;
std::vector<EL::Npc*> g_scratch;

void ClientFinish() { g_phase = Phase::Done; }

// The krampus mirror, once, by a bounded scan of the NPC mirrors while the step waits.
void FindMirror() {
    EL::NpcMirrors().Snapshot(g_scratch);
    for (EL::Npc* n : g_scratch) {
        if (!n || n->GetTypeName() != kKrampusTypeName) continue;
        void* actor = n->LiveActor();
        if (!actor) continue;
        g_mirror.Set(actor);
        g_mirrorEid = n->GetId();
        ClearKrampusTimers(actor);
        return;
    }
}

// The fixed reading of the read step, a PASS or a FAIL.
void ReadVerdict(bool pass) {
    void* actor = g_mirror.Get();
    const int c = CopiesOf(actor);
    const int target = g_mirrorEid == EL::kInvalidId ? -1 : coop::creature_event_relay::TargetFor(g_mirrorEid);
    const int applied = g_mirrorEid == EL::kInvalidId ? -1 : coop::creature_event_relay::AppliedFor(g_mirrorEid);
    g_n1 = ReadCount();
    const bool ev = ue_wrap::save_capture::GameEventState() == ue_wrap::save_capture::EventState::On;
    const bool ok = pass && c == 1 && ev;
    Verdict(ok, "read", false, "mirror=%d c=%d target=%d applied=%d n=%d getEvent=%d", actor ? 1 : 0, c, target,
            applied, g_n1, ev ? 1 : 0);
    if (ok) {
        coop::chat_sync::QueueSend(std::string(kReadLine));
        g_phase = Phase::End;
        g_phaseStartMs = NowMs();
    } else {
        ClientFinish();
    }
}

void ClientTick() {
    switch (g_phase) {
    case Phase::WaitJoin:
        if (!coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
            return;
        g_phase = Phase::Mirror;
        g_phaseStartMs = NowMs();
        return;
    case Phase::Mirror: {
        if (NowMs() - g_phaseStartMs >= kBoundMs) {
            ReadVerdict(false);
            return;
        }
        if (g_mirrorEid == EL::kInvalidId) FindMirror();
        if (g_mirrorEid == EL::kInvalidId) return;
        const int target = coop::creature_event_relay::TargetFor(g_mirrorEid);
        if (target == 1 && coop::creature_event_relay::AppliedFor(g_mirrorEid) == 1) ReadVerdict(true);
        return;
    }
    case Phase::End: {
        if (NowMs() - g_phaseStartMs >= kBoundMs) {
            Verdict(false, "done", false, "expired n=%d n1=%d", ReadCount(), g_n1);
            ClientFinish();
            return;
        }
        if (coop::creature_event_relay::TargetFor(g_mirrorEid) != -1) return;
        const int n2 = ReadCount();
        Verdict(n2 == g_n1 - 1, "done", false, "n1=%d n2=%d", g_n1, n2);
        ClientFinish();
        return;
    }
    case Phase::Done:
        return;
    }
}

}  // namespace

bool IsEnabled() {
    static const bool on =
        coop::config::ResolveEnum(::coop::config_registry::rows::event_relay_drill) == "on";
    return on;
}

void Install(coop::net::Session* session) {
    if (!IsEnabled()) return;
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    if (!IsEnabled()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    if (s->role() == coop::net::Role::Host) HostTick(s);
    else ClientTick();
}

void OnDisconnect() {
    g_phase = Phase::WaitJoin;
    g_mirrorEid = EL::kInvalidId;
    g_mirror.Reset();
    g_n1 = 0;
}

}  // namespace coop::dev::event_relay_drill
