// coop/world/salt_rocket_ignite.cpp -- see coop/world/salt_rocket_ignite.h: the gate rows, the client half and the
// lifecycle. The host half is salt_rocket_ignite_host.cpp.

#include "coop/world/salt_rocket_ignite.h"

#include "salt_rocket_ignite_detail.h"  // co-located private header (src tree, not include/)

#include "coop/element/registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/hand_item.h"
#include "coop/world/spawn_authority.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace coop::salt_rocket_ignite {
namespace {

namespace EL = coop::element;
namespace R  = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;
namespace sg = ue_wrap::script_gate;

UE_FACT_FN(kRowAttempt, "salt_rocket", L"prop_saltrocket_C", L"attemptIgnite", Declared);
// prop_C's ignite, on any class: the lighter and the torch call it on whatever they hit, so the callback decides by
// the object's class.
UE_FACT_FN(kRowIgnite, "salt_rocket", nullptr, L"ignite", AnyClass);
UE_FACT_FN(kRowLighter, "salt_rocket", L"prop_lighter_C", L"playerHandUse_LMB", Declared);
UE_FACT_FN(kRowTorch, "salt_rocket", L"prop_torch_C", L"playerHitWith", Declared);

constexpr int kTagAttempt = 0x53524154;  // 'SRAT'
constexpr int kTagIgnite  = 0x53524947;  // 'SRIG'
constexpr int kTagLighter = 0x53524C54;  // 'SRLT'
constexpr int kTagTorch   = 0x53525448;  // 'SRTH'

// The tools whose verb gives a refusal the player's scope.
enum Tool : uint8_t { kLighter = 0, kTorch = 1, kNoTool = 2 };
constexpr const wchar_t* ToolClass(uint8_t tool) { return tool == kLighter ? L"prop_lighter_C" : L"prop_torch_C"; }

// A light waits in the client's queue for the hand announce of its tool, and for the rocket to be the host's: eight
// clicks, each at most two intents, twice the eight the host serves a sender in its first second.
constexpr int kPendingCap = 16;
// How long a light waits for the host's spawn of a rocket this client has just placed. The wait has no edge of its
// own: the drop lane refuses a place silently (coop/props/prop_drop_intent.cpp), so the bound is a protocol deadline,
// the shape of EX-2's kRocketLifeMs, and 2000 ms is EX-2's flat window for the hand lane's round trip. MTA answers a
// failed request at once (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:3038-3044), but our drop lane
// refuses a place silently, so the wait is bounded instead; the orphan is tracked off-arc in
// TRACKER_OCTOBER_3D_2026.
constexpr uint64_t kPlaceAnswerMs = 2000;
constexpr uint64_t kStatsEveryMs = 60000;

std::atomic<coop::net::Session*> g_session{nullptr};
Counters g_counters{};
Counters g_statsSeen{};
uint64_t g_statsAtMs = 0;

// The running tool of each kind, as its watch last saw it: raw pointers each PRE rewrites, used for identity alone.
void* g_toolFn[2]  = {};
void* g_toolObj[2] = {};

struct Pending {
    ue_wrap::CachedObjRef rocket;
    uint8_t               verb = 0;
    uint8_t               tool = 0;
    uint32_t              lookSeq = 0;
    uint64_t              atMs = 0;
};
Pending g_pending[kPendingCap];
int g_pendHead = 0;
int g_pendCount = 0;
bool g_fullSaid = false;

bool g_notRunSaid = false;

bool g_registered = false;
int  g_refused = 0;       // watches the gate refused at registration: they have no table entry to settle on
bool g_settled = false;

// The salt rocket class and the listing it was found under (ObjectIndex ClassListingId): a class unloaded and loaded
// again answers a new listing, so a stale pointer is looked up again and never read through.
void*    g_rocketClass = nullptr;
uint64_t g_rocketListing = 0;

// The tool the local player's own verb is running on: the body is active on this thread and its object is the
// engine's hold (the hand actor), not the announce latch.
uint8_t RunningTool() {
    for (uint8_t k = kLighter; k <= kTorch; ++k)
        if (g_toolFn[k] && g_toolObj[k] && sg::IsBodyActive(g_toolFn[k]) &&
            coop::hand_item::IsLocalHandViewActor(g_toolObj[k]))
            return k;
    return kNoTool;
}

void PushPending(void* rocket, uint8_t verb, uint8_t tool) {
    if (g_pendCount >= kPendingCap) {
        ++g_counters.pendingFull;
        if (!g_fullSaid) {
            g_fullSaid = true;
            UE_LOGW("[SALT] %d lights wait to be sent -- the next one is dropped", kPendingCap);
        }
        return;
    }
    Pending& e = g_pending[(g_pendHead + g_pendCount) % kPendingCap];
    e.rocket.Set(rocket);
    e.verb = verb;
    e.tool = tool;
    e.lookSeq = coop::hand_item::OwnHandLookSeq();
    e.atMs = coop::net::NowMs();
    ++g_pendCount;
}

void PopPending() {
    g_pending[g_pendHead].rocket.Reset();
    g_pendHead = (g_pendHead + 1) % kPendingCap;
    --g_pendCount;
}

void ClearPending() {
    while (g_pendCount > 0) PopPending();
}

sg::Verdict OnToolPre(const sg::Call& c) {
    const int k = c.tag == kTagLighter ? kLighter : kTorch;
    g_toolFn[k] = c.function;
    g_toolObj[k] = c.object;
    return sg::Verdict::Run;
}

// On an active client session the rocket's two light verbs do not run: the host performs the light. A refusal the
// local player's own lighter or torch verb caused is queued for the host; any other is a world fire on this client.
sg::Verdict OnRocketPre(const sg::Call& c) {
    if (!coop::spawn_authority::IsActiveClientSession()) return sg::Verdict::Run;
    const bool ignite = c.tag == kTagIgnite;
    if (ignite && !detail::IsRocket(c.object)) return sg::Verdict::Run;
    const uint8_t tool = RunningTool();
    // The lighter calls both verbs, the torch only ignite.
    const bool player = tool == kLighter || (tool == kTorch && ignite);
    if (player) {
        PushPending(c.object, ignite ? uint8_t{1} : uint8_t{0}, tool);
        ++g_counters.playerRefused;
    } else {
        ++g_counters.worldRefused;
    }
    return sg::Verdict::Cancel;
}

struct WatchDef {
    const ue_wrap::facts::Function* row;
    int tag;
    sg::PreFn pre;
};
constexpr WatchDef kWatches[] = {
    { &kRowAttempt, kTagAttempt, &OnRocketPre },
    { &kRowIgnite,  kTagIgnite,  &OnRocketPre },
    { &kRowLighter, kTagLighter, &OnToolPre },
    { &kRowTorch,   kTagTorch,   &OnToolPre },
};
constexpr int kWatchCount = static_cast<int>(sizeof(kWatches) / sizeof(kWatches[0]));

void SettleWatches() {
    if (g_settled || !g_registered) return;
    sg::ResolvePendingNames();
    int live = 0, settled = 0;
    for (const WatchDef& w : kWatches) {
        if (sg::NameWatchLive(*w.row, w.tag)) ++live;
        if (sg::NameWatchSettled(*w.row, w.tag)) ++settled;
    }
    // A refused watch is settled dead: it has no table entry, so it is neither live nor settled there.
    if (live == kWatchCount) {
        g_settled = true;
        UE_LOGI("salt_rocket_ignite: the light gates are live (attemptIgnite, ignite, the lighter's and the torch's "
                "verbs)");
    } else if (settled + g_refused == kWatchCount) {
        g_settled = true;
        UE_LOGE("salt_rocket_ignite: %d of %d light gates are dead and %d were refused -- a client's light on a "
                "rocket stays its own", kWatchCount - live - g_refused, kWatchCount, g_refused);
    }
}

// The head first: a light goes once the hand announce of its tool has, and once the rocket is the host's; the first
// head that must wait holds everything behind it, so a click's attempt and ignite stay in order. The look-seq wait and
// the send retry are each bounded by an edge: the hand lane's next look, and the send's own acceptance.
void TickClient() {
    coop::net::Session* s = detail::SessionPtr();
    if (!s) return;
    while (g_pendCount > 0) {
        const Pending& h = g_pending[g_pendHead];
        void* rocket = h.rocket.Get();
        if (!rocket) {
            ++g_counters.pendingDead;
            PopPending();
            continue;
        }
        // The tool's announce sends it. A look at the hand since the click that left the tool unannounced means it
        // never will be: dropped. No look yet -- the hand lane may not have started -- the light waits. The tool's
        // announce and the use travel separately, the shape EX-2 inherited (explosion_request_client.cpp). Source
        // sends them in one command (reference/source-sdk-2013/src/game/client/in_main.cpp:1176); VOTV's hand lane
        // is a separate stream, so the client waits.
        if (!coop::hand_item::OwnAnnounced(ToolClass(h.tool))) {
            if (coop::hand_item::OwnHandLookSeq() == h.lookSeq) return;
            ++g_counters.notAnnounced;
            PopPending();
            continue;
        }
        // A rocket this client has just placed waits for the host's spawn of it, so the light never overtakes the drop
        // intent on the Default lane.
        const EL::ElementId eid = EL::Registry::Get().EidForActor(rocket);
        if (!EL::Registry::IsAllowedHostAllocatedEid(eid)) {
            if (coop::net::NowMs() - h.atMs < kPlaceAnswerMs) return;
            ++g_counters.unspawned;
            PopPending();
            continue;
        }
        coop::net::PropIgniteIntentPayload p{};
        const std::wstring key = ue_wrap::prop::GetInteractableKeyString(rocket);
        if (!key.empty() && key != L"None") coop::net::WireKeyFromString(key, p.key);
        p.elementId = static_cast<uint32_t>(eid);
        p.verb = h.verb;
        if (!s->SendReliableToSlot(0, coop::net::ReliableKind::PropIgniteIntent, &p, sizeof(p))) return;
        ++g_counters.sent;
        PopPending();
    }
}

bool Moved(const Counters& c) {
    const Counters zero{};
    return std::memcmp(&c, &zero, sizeof(c)) != 0;
}

void SayCounters(const char* when) {
    char refused[256];
    size_t n = 0;
    refused[0] = '\0';
    for (int r = 0; r < static_cast<int>(Refusal::Count); ++r) {
        if (g_counters.refused[r] == 0 || n >= sizeof(refused)) continue;
        const int w = std::snprintf(refused + n, sizeof(refused) - n, " %s=%u", RefusalName(static_cast<Refusal>(r)),
                                    g_counters.refused[r]);
        if (w > 0) n += static_cast<size_t>(w);
    }
    const Counters& c = g_counters;
    UE_LOGI("[SALT] ignite lane %s: client playerRefused=%u worldRefused=%u pendingFull=%u pendingDead=%u "
            "notAnnounced=%u unspawned=%u sent=%u; host waited=%u performed=%u/%u refused%s", when, c.playerRefused,
            c.worldRefused, c.pendingFull, c.pendingDead, c.notAnnounced, c.unspawned, c.sent, c.waited,
            c.performed[0], c.performed[1], refused);
}

}  // namespace

namespace detail {

coop::net::Session* SessionPtr() { return g_session.load(std::memory_order_acquire); }

Counters& MutCounters() { return g_counters; }

bool& NotRunSaid() { return g_notRunSaid; }

bool IsRocket(void* obj) {
    if (!obj) return false;
    // A class held across ticks stands only while its listing does; a null or stale one is looked up again.
    if (!g_rocketClass || OI::ClassListingId(g_rocketClass) != g_rocketListing) {
        g_rocketClass = OI::ClassByName(L"prop_saltrocket_C");
        g_rocketListing = g_rocketClass ? OI::ClassListingId(g_rocketClass) : 0;
    }
    return g_rocketClass && R::ClassOf(obj) == g_rocketClass;
}

void ForgetRocketClass() {
    g_rocketClass = nullptr;
    g_rocketListing = 0;
}

}  // namespace detail

const char* RefusalName(Refusal r) {
    switch (r) {
        case Refusal::NotWorldReady: return "not_world_ready";
        case Refusal::Malformed:     return "malformed";
        case Refusal::QueueFull:     return "queue_full";
        case Refusal::Gone:          return "gone";
        case Refusal::NotRocket:     return "not_rocket";
        case Refusal::Reach:         return "reach";
        case Refusal::NoTool:        return "no_tool";
        case Refusal::SenderDead:    return "sender_dead";
        case Refusal::Count:         break;
    }
    return "unknown";
}

Counters GetCounters() { return g_counters; }

void Install(coop::net::Session* s) {
    g_session.store(s, std::memory_order_release);
    if (g_registered) return;
    g_registered = true;
    for (const WatchDef& w : kWatches)
        if (!sg::WatchName(*w.row, w.tag, w.pre, nullptr)) {
            ++g_refused;
            UE_LOGE("salt_rocket_ignite: the script-body gate refused the watch on %ls::%ls -- that light stays each "
                    "peer's own", w.row->Class() ? w.row->Class() : L"*", w.row->Name());
        }
}

void Tick() {
    SettleWatches();
    if (coop::spawn_authority::IsActiveClientSession()) TickClient();
    coop::net::Session* s = detail::SessionPtr();
    if (s && s->running() && s->role() == coop::net::Role::Host) detail::HostServe(*s);
    const uint64_t now = coop::net::NowMs();
    if (now - g_statsAtMs < kStatsEveryMs) return;
    g_statsAtMs = now;
    if (std::memcmp(&g_counters, &g_statsSeen, sizeof(g_counters)) == 0) return;
    g_statsSeen = g_counters;
    SayCounters("stats");
}

void OnWorldChange() {
    detail::HostReset();
    detail::ForgetRocketClass();
    ClearPending();
    g_toolFn[0] = g_toolFn[1] = nullptr;
    g_toolObj[0] = g_toolObj[1] = nullptr;
}

void OnDisconnect() {
    if (Moved(g_counters)) SayCounters("at disconnect");
    OnWorldChange();
    g_counters = Counters{};
    g_statsSeen = Counters{};
    g_fullSaid = false;
    g_notRunSaid = false;
}

}  // namespace coop::salt_rocket_ignite
