// coop/props/miner_sync.cpp -- see coop/props/miner_sync.h.
//
// prop_miner_C's graph, from its bytecode (0.9.0n). playerUsedOn, on a blocking hit: `upgrade` with the player's
// holding_name, and on success the 2D sound warpbox_tile and K2_DestroyActor on the player's holding_actor. `upgrade`:
// scrap_metal adds a slot (16 at most), miner_gpu a GPU while a slot is free (16 at most), miner_fan a cooler (6 at
// most), each then `upd`, and a hint when it is refused. actionOptionIndex: action 4 claims, when powered and the points
// are above 0: a hint naming them, lib_C::addPoints, and accumulatedPoints to 0. ReceiveTick: overheat cools by
// delta / (14..16), and while powered `i_calc` steps and `calc` mines. getData and loadData carry the four counts and
// overheat in the rig's save record.

#include "coop/props/miner_sync.h"

#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/requester_feedback/requester_feedback.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include <chrono>
#include <cmath>
#include <cwchar>
#include <string>
#include <vector>

namespace coop::miner_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
namespace sg = ue_wrap::script_gate;
namespace EL = coop::element;
namespace N  = coop::net;

UE_FACT_FN(kRowTick, "miner_sync", L"prop_miner_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowUse, "miner_sync", L"prop_miner_C", L"playerUsedOn", Declared);
UE_FACT_FN(kRowAction, "miner_sync", L"prop_miner_C", L"actionOptionIndex", Declared);
constexpr int kTagTick = 0x4D4E5201, kTagUse = 0x4D4E5202, kTagAction = 0x4D4E5203;  // 'MNR' 1..3

constexpr uint8_t  kActionClaim = 4;       // EInteractionActions: the rig's claim
constexpr float    kReachCm = 600.f;       // a client's ask, from its eye to the rig's root
constexpr uint64_t kAskEveryMs = 250;      // a slot's asks, one per this
constexpr uint64_t kPointsEveryMs = 1000;  // the points and the heat alone, at most once per this
constexpr uint64_t kStateLifeMs = 10000;   // a state for a copy not yet made waits this long

const wchar_t* const kItems[] = {L"scrap_metal", L"miner_gpu", L"miner_fan"};  // by kMinerItem*

coop::net::Session* g_session = nullptr;
bool g_watched = false;
bool g_settled = false;
bool g_applying = false;  // this lane's own calls into the rig: the gate lets them run

bool IsHost() { return g_session && g_session->running() && g_session->role() == N::Role::Host; }
bool IsClient() { return g_session && g_session->running() && g_session->role() == N::Role::Client; }

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

int32_t* IntAt(void* rig, const wchar_t* name) {
    const int32_t off = rig ? R::FindPropertyOffset(R::ClassOf(rig), name) : -1;
    return off >= 0 ? reinterpret_cast<int32_t*>(static_cast<char*>(rig) + off) : nullptr;
}

float* FloatAt(void* rig, const wchar_t* name) {
    const int32_t off = rig ? R::FindPropertyOffset(R::ClassOf(rig), name) : -1;
    return off >= 0 ? reinterpret_cast<float*>(static_cast<char*>(rig) + off) : nullptr;
}

struct Counts {
    int32_t gpus = 0, slots = 0, fans = 0, points = 0;
    float overheat = 0.f;
};

bool Read(void* rig, Counts& c) {
    const int32_t* g = IntAt(rig, L"upg_gpu");
    const int32_t* s = IntAt(rig, L"upg_slots");
    const int32_t* f = IntAt(rig, L"upg_fans");
    const int32_t* p = IntAt(rig, L"accumulatedPoints");
    const float* h = FloatAt(rig, L"overheat");
    if (!g || !s || !f || !p || !h) return false;
    c = Counts{*g, *s, *f, *p, *h};
    return true;
}

uint32_t EidOf(void* rig) {
    const EL::ElementId eid = EL::Registry::Get().EidForActor(rig);
    return eid == EL::kInvalidId ? 0u : static_cast<uint32_t>(eid);
}

bool IsLiveRig(void* obj) { return obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__"); }

template <class F>
void ForEachRig(F&& fn) {
    void* const cls = OI::ClassByName(L"prop_miner_C");
    if (!cls) return;
    std::vector<void*> found;
    OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
        if (IsLiveRig(obj)) static_cast<std::vector<void*>*>(ctx)->push_back(obj);
    }, &found);
    for (void* r : found) fn(r);
}

void* RigOf(uint32_t eid) {
    void* const rig = EL::LiveActorOfType(static_cast<EL::ElementId>(eid), EL::ElementType::Prop);
    return rig && R::ClassNameOf(rig) == L"prop_miner_C" ? rig : nullptr;
}

// The player's held item: its name (holding_name) and actor (holding_actor), as the rig's use reads them.
bool Held(void* player, std::wstring& name, void*& actor) {
    if (!player) return false;
    const int32_t nOff = R::FindPropertyOffset(R::ClassOf(player), L"holding_name");
    const int32_t aOff = R::FindPropertyOffset(R::ClassOf(player), L"holding_actor");
    if (nOff < 0 || aOff < 0) return false;
    name = R::ToString(*reinterpret_cast<const R::FName*>(static_cast<const char*>(player) + nOff));
    actor = *reinterpret_cast<void* const*>(static_cast<const char*>(player) + aOff);
    return true;
}

int ItemOf(const std::wstring& name) {
    for (int i = 0; i < 3; ++i)
        if (_wcsicmp(name.c_str(), kItems[i]) == 0) return i;  // an FName keeps its first spelling (scrap_Metal)
    return -1;
}

// ---- the host ----------------------------------------------------------------------------------------------------
struct Known {
    ue_wrap::CachedObjRef ref;
    Counts sent;
    bool said = false;
    uint64_t pointsAt = 0;
};
std::vector<Known> g_known;
struct Ask { int slot; N::MinerPayload p; };
std::vector<Ask> g_asks;
uint64_t g_lastAskMs[N::kMaxPeers] = {};
uint64_t g_nextScanMs = 0;

void SendState(void* rig, const Counts& c, int slot) {
    if (!g_session) return;
    N::MinerPayload p{};
    p.eid = EidOf(rig);
    if (!p.eid) return;
    p.op = N::kMinerOpState;
    p.gpus = c.gpus;
    p.slots = c.slots;
    p.fans = c.fans;
    p.points = c.points;
    p.overheat = c.overheat;
    if (slot >= 0) g_session->SendReliableToSlot(static_cast<uint8_t>(slot), N::ReliableKind::MinerState, &p, sizeof(p));
    else g_session->SendReliable(N::ReliableKind::MinerState, &p, sizeof(p));
}

void Answer(int slot, uint32_t eid, uint8_t item, bool granted) {
    N::MinerPayload p{};
    p.eid = eid;
    p.op = N::kMinerOpAnswer;
    p.item = item;
    p.granted = granted ? 1 : 0;
    g_session->SendReliableToSlot(static_cast<uint8_t>(slot), N::ReliableKind::MinerState, &p, sizeof(p));
}

bool Upgrade(void* rig, uint8_t item) {
    ue_wrap::ParamFrame f(R::FindFunction(R::ClassOf(rig), L"upgrade"));
    if (!f.valid()) return false;
    f.Set<R::FName>(L"upgrade", ue_wrap::fname_utils::StringToFName(kItems[item]));
    return ue_wrap::Call(rig, f) && f.Get<bool>(L"success");
}

bool Claim(void* rig, void* player) {
    ue_wrap::ParamFrame f(R::FindFunction(R::ClassOf(rig), L"actionOptionIndex"));
    if (!f.valid()) return false;
    f.Set<void*>(L"player", player);
    f.Set<uint8_t>(L"action", kActionClaim);
    return ue_wrap::Call(rig, f);
}

void JudgeAsks() {
    std::vector<Ask> asks;
    asks.swap(g_asks);
    for (const Ask& a : asks) {
        void* const rig = RigOf(a.p.eid);
        coop::RemotePlayer& rp = coop::puppet_drive::Puppet(static_cast<uint8_t>(a.slot));
        ue_wrap::FVector eye{}, at{};
        if (!rig || !rp.valid() || !rp.TryGetEyePosition(eye) || !E::TryGetActorLocation(rig, at)) {
            UE_LOGW("miner_sync: HOST slot %d asked of eid %u, which is no rig here -- refused", a.slot, a.p.eid);
            if (a.p.op == N::kMinerOpAdd) Answer(a.slot, a.p.eid, a.p.item, false);
            continue;
        }
        const float dx = eye.X - at.X, dy = eye.Y - at.Y, dz = eye.Z - at.Z;
        const bool reach = dx * dx + dy * dy + dz * dz <= kReachCm * kReachCm;
        const coop::requester_feedback::Scope scope(static_cast<uint8_t>(a.slot), 0);  // the rig's hints are the asker's
        g_applying = true;
        if (a.p.op == N::kMinerOpAdd) {
            const bool granted = reach && Upgrade(rig, a.p.item);
            Answer(a.slot, a.p.eid, a.p.item, granted);
            UE_LOGI("miner_sync: HOST slot %d's %ls on rig eid %u %hs", a.slot, kItems[a.p.item], a.p.eid,
                    granted ? "taken" : reach ? "refused by the rig" : "out of reach -- refused");
        } else {
            const bool ran = reach && Claim(rig, rp.actor());
            UE_LOGI("miner_sync: HOST slot %d claimed rig eid %u's points (%hs)", a.slot, a.p.eid,
                    ran ? "ran" : "out of reach -- refused");
        }
        g_applying = false;
        g_nextScanMs = 0;  // the change goes out at once
    }
}

Known& KnownOf(void* rig) {
    for (Known& k : g_known)
        if (k.ref.Is(rig)) return k;
    g_known.emplace_back();
    g_known.back().ref.Set(rig);
    return g_known.back();
}

void TickHost() {
    JudgeAsks();
    const uint64_t now = NowMs();
    if (now < g_nextScanMs) return;
    g_nextScanMs = now + 100;
    ForEachRig([now](void* rig) {
        Counts c;
        if (!Read(rig, c)) return;
        Known& k = KnownOf(rig);
        const bool counts = !k.said || c.gpus != k.sent.gpus || c.slots != k.sent.slots || c.fans != k.sent.fans;
        const bool claimed = c.points < k.sent.points;
        const bool drift = c.points != k.sent.points || std::fabs(c.overheat - k.sent.overheat) > 0.01f;
        if (!counts && !claimed && !(drift && now - k.pointsAt >= kPointsEveryMs)) return;
        SendState(rig, c, -1);
        if (counts) UE_LOGI("miner_sync: HOST rig eid %u holds %d GPUs in %d slots, %d coolers -- sent", EidOf(rig),
                            c.gpus, c.slots, c.fans);
        k.sent = c;
        k.said = true;
        k.pointsAt = now;
    });
}

// ---- the client --------------------------------------------------------------------------------------------------
struct Pending { N::MinerPayload p; uint64_t since; };
std::vector<Pending> g_states;

bool ApplyState(const N::MinerPayload& p) {
    void* const rig = RigOf(p.eid);
    Counts c;
    if (!rig || !Read(rig, c)) return false;
    *IntAt(rig, L"accumulatedPoints") = p.points;
    *FloatAt(rig, L"overheat") = p.overheat;
    if (c.gpus == p.gpus && c.slots == p.slots && c.fans == p.fans) return true;
    *IntAt(rig, L"upg_gpu") = p.gpus;
    *IntAt(rig, L"upg_slots") = p.slots;
    *IntAt(rig, L"upg_fans") = p.fans;
    void* const upd = R::FindFunction(R::ClassOf(rig), L"upd");
    g_applying = true;
    const bool ran = upd && R::CallFunction(rig, upd, nullptr);  // its GPUs, coolers and slots shown
    g_applying = false;
    UE_LOGI("miner_sync: CLIENT rig eid %u holds %d GPUs in %d slots, %d coolers, as the host's (upd ran=%d)", p.eid,
            p.gpus, p.slots, p.fans, ran ? 1 : 0);
    return true;
}

// A granted use: the item goes from this player's hand, as the rig's own use would have spent it.
void ApplyAnswer(const N::MinerPayload& p) {
    if (!p.granted || p.item > N::kMinerItemFan) {
        UE_LOGI("miner_sync: CLIENT the host's rig refused this player's %ls", p.item <= N::kMinerItemFan ? kItems[p.item] : L"?");
        return;
    }
    std::wstring name;
    void* held = nullptr;
    if (!Held(coop::players::Registry::Get().Local(), name, held) || _wcsicmp(name.c_str(), kItems[p.item]) != 0 || !held || !R::IsLive(held)) {
        UE_LOGW("miner_sync: CLIENT the host took this player's %ls, which has left the hand -- nothing spent here",
                kItems[p.item]);
        return;
    }
    const bool gone = ue_wrap::component_calls::CallParamlessNamed(held, L"K2_DestroyActor");
    UE_LOGI("miner_sync: CLIENT the host's rig took this player's %ls -- spent from the hand (ok=%d)", kItems[p.item],
            gone ? 1 : 0);
}

void TickClient() {
    const uint64_t now = NowMs();
    for (size_t i = 0; i < g_states.size();) {
        const bool done = ApplyState(g_states[i].p);
        if (done || now - g_states[i].since > kStateLifeMs) {
            if (!done) UE_LOGW("miner_sync: CLIENT rig eid %u's state found no copy in 10 s -- dropped", g_states[i].p.eid);
            g_states.erase(g_states.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

// ---- the gate ----------------------------------------------------------------------------------------------------
bool Paced() {
    static uint64_t s_lastMs = 0;
    const uint64_t now = NowMs();
    if (now - s_lastMs < kAskEveryMs) return false;
    s_lastMs = now;
    return true;
}

// The tick: a client's copy mines no points of its own and cools by the host's word.
sg::Verdict OnTickPre(const sg::Call&) { return IsClient() && !g_applying ? sg::Verdict::Cancel : sg::Verdict::Run; }

// The use: a client's held scrap metal, GPU or cooler is asked of the host, and nothing else of the body runs here.
sg::Verdict OnUsePre(const sg::Call& c) {
    if (!IsClient() || g_applying || !c.object || !c.function || !c.locals) return sg::Verdict::Run;
    const int32_t off = R::FindParamOffset(c.function, L"player");
    void* const player = off >= 0 ? *reinterpret_cast<void* const*>(c.locals + off) : nullptr;
    std::wstring name;
    void* held = nullptr;
    const int item = Held(player, name, held) ? ItemOf(name) : -1;
    N::MinerPayload p{};
    p.eid = EidOf(c.object);
    if (item < 0 || !p.eid || !Paced()) {
        UE_LOGI("miner_sync: CLIENT a use on rig eid %u with '%ls' in hand (player %hs) -- nothing asked", p.eid,
                name.c_str(), player ? "read" : "missing");
        return sg::Verdict::Cancel;
    }
    p.op = N::kMinerOpAdd;
    p.item = static_cast<uint8_t>(item);
    const bool ok = g_session->SendReliable(N::ReliableKind::MinerIntent, &p, sizeof(p));
    UE_LOGI("miner_sync: CLIENT used %ls on rig eid %u -- asked the host (ok=%d)", kItems[item], p.eid, ok ? 1 : 0);
    return sg::Verdict::Cancel;
}

// The claim: asked of the host, whose rig pays into the shared balance.
sg::Verdict OnActionPre(const sg::Call& c) {
    if (!IsClient() || g_applying || !c.object || !c.function || !c.locals) return sg::Verdict::Run;
    const int32_t off = R::FindParamOffset(c.function, L"action");
    if (off < 0 || c.locals[off] != kActionClaim) return sg::Verdict::Run;
    N::MinerPayload p{};
    p.eid = EidOf(c.object);
    if (!p.eid || !Paced()) return sg::Verdict::Cancel;
    p.op = N::kMinerOpClaim;
    const bool ok = g_session->SendReliable(N::ReliableKind::MinerIntent, &p, sizeof(p));
    UE_LOGI("miner_sync: CLIENT claimed rig eid %u's points -- asked the host (ok=%d)", p.eid, ok ? 1 : 0);
    return sg::Verdict::Cancel;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session = session;
    if (g_watched) return;
    g_watched = true;
    const bool t = sg::WatchName(kRowTick, kTagTick, &OnTickPre, nullptr);
    const bool u = sg::WatchName(kRowUse, kTagUse, &OnUsePre, nullptr);
    const bool a = sg::WatchName(kRowAction, kTagAction, &OnActionPre, nullptr);
    if (!t || !u || !a) UE_LOGE("miner_sync: a watch was refused (tick=%d use=%d action=%d)", t ? 1 : 0, u ? 1 : 0, a ? 1 : 0);
}

void Tick() {
    if (IsHost()) TickHost();
    else if (IsClient()) TickClient();
    if (!g_watched || g_settled) return;
    sg::ResolvePendingNames();
    if (!sg::NameWatchSettled(kRowTick, kTagTick) || !sg::NameWatchSettled(kRowUse, kTagUse) ||
        !sg::NameWatchSettled(kRowAction, kTagAction))
        return;
    g_settled = true;
    const bool t = sg::NameWatchLive(kRowTick, kTagTick), u = sg::NameWatchLive(kRowUse, kTagUse),
               a = sg::NameWatchLive(kRowAction, kTagAction);
    if (t && u && a) UE_LOGI("miner_sync: the watches on prop_miner_C's tick, use and actions are live");
    else UE_LOGE("miner_sync: a watch is dead (tick=%d use=%d action=%d) -- a client's rig runs its own", t ? 1 : 0,
                 u ? 1 : 0, a ? 1 : 0);
}

void SendJoinSnapshotForSlot(int slot) {
    if (!IsHost() || slot < 0) return;
    ForEachRig([slot](void* rig) {
        Counts c;
        if (Read(rig, c)) SendState(rig, c, slot);
    });
}

void OnIntent(int senderSlot, const coop::net::MinerPayload& p) {
    if (!IsHost() || senderSlot <= 0 || senderSlot >= static_cast<int>(N::kMaxPeers) || g_asks.size() >= 64) return;
    if ((p.op != N::kMinerOpAdd && p.op != N::kMinerOpClaim) || (p.op == N::kMinerOpAdd && p.item > N::kMinerItemFan))
        return;
    const uint64_t now = NowMs();
    if (now - g_lastAskMs[senderSlot] < kAskEveryMs) return;
    g_lastAskMs[senderSlot] = now;
    g_asks.push_back({senderSlot, p});
}

void OnState(const coop::net::MinerPayload& p) {
    if (!IsClient()) return;
    if (p.op == N::kMinerOpAnswer) {
        ApplyAnswer(p);
        return;
    }
    if (p.op != N::kMinerOpState || g_states.size() >= 64) return;
    g_states.push_back({p, NowMs()});
}

void OnDisconnect() {
    g_known.clear();
    g_asks.clear();
    g_states.clear();
    for (uint64_t& t : g_lastAskMs) t = 0;
}

}  // namespace coop::miner_sync
