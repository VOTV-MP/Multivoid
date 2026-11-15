// coop/world/erie_sync.cpp -- see coop/world/erie_sync.h.
//
// prop_eriePlush_C's graph, cooked (0.10.0; capture `### prop_erieplush_c`): the hit handler is the quest (morbing and eating end it;
// `supereater` eats any prop; with no `food` a paper sets haspaper, a pencil haspencil, each with `upd` and `write`; with a `food` a prop
// named for it is eaten and `fullness` rises by 0.01, the odds of `fuck`). `write` runs when both are in (4 s on: the notes and the
// food). `fuck` mints eriePlushLive_C and grants `liveerie`. `anger` (fire, a microwave) mints npc_angryErieFlesh_C.

#include "coop/world/erie_sync.h"

#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/remote_prop.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/profile.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::erie_sync {
namespace {

namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
using Clock = std::chrono::steady_clock;
using Payload = coop::net::AmbientFixtureStatePayload;

UE_FACT_FN(kRowHit, "erie_sync", L"prop_eriePlush_C",
           L"BndEvt__prop_eriePlush_StaticMesh_K2Node_ComponentBoundEvent_0_ComponentHitSignature__DelegateSignature", Declared);
UE_FACT_FN(kRowAchieve, "erie_sync", L"lib_C", L"progressAchievement", Declared);
constexpr int kTagHit = 0x45524948;   // 'ERIH'
constexpr int kTagAch = 0x45524143;   // 'ERAC'

constexpr const wchar_t* kHitFn =
    L"BndEvt__prop_eriePlush_StaticMesh_K2Node_ComponentBoundEvent_0_ComponentHitSignature__DelegateSignature";
constexpr const wchar_t* kPlushClasses[] = {L"prop_eriePlush_C", L"prop_eriePlush_superEater_C"};
constexpr long long kPollMs = 1000;
constexpr long long kSameHitMs = 1500;     // a client forwards one prop's hits on one plush at most this often
constexpr float kReachCm = 600.f;          // the host's copy of the prop must stand this near its plush
constexpr uint8_t kFlagPaper = 1, kFlagPencil = 2;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watchAsked = false;
bool g_drillCalls = false;   // [dev] see SetDrillCallsCount
Clock::time_point g_lastPoll{};
std::atomic<uint64_t> g_refusedHits{0}, g_forwardedHits{0};

coop::net::Session* Client() {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->running() && s->connected() && s->role() == coop::net::Role::Client) ? s : nullptr;
}

bool IsHost() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Host;
}

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

bool BoolOf(void* obj, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    return obj && R::FindBoolProperty(R::ClassOf(obj), name, off, mask) && off >= 0 && (*(static_cast<const uint8_t*>(obj) + off) & mask) != 0;
}

void SetBoolOf(void* obj, const wchar_t* name, bool v) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask) || off < 0) return;
    uint8_t* p = static_cast<uint8_t*>(obj) + off;
    *p = v ? static_cast<uint8_t>(*p | mask) : static_cast<uint8_t>(*p & ~mask);
}

std::wstring FNameOf(void* obj, const wchar_t* name) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off < 0) return L"?";
    return R::ToString(*reinterpret_cast<const R::FName*>(static_cast<const uint8_t*>(obj) + off));
}

std::wstring KeyOf(void* plush) {
    const std::wstring k = ue_wrap::prop::GetInteractableKeyString(plush);
    return (k.empty() || k == L"None") ? std::wstring() : k;
}

uint32_t Hash(const std::wstring& k) {
    uint32_t h = 2166136261u;
    for (wchar_t c : k) { h ^= static_cast<uint32_t>(c); h *= 16777619u; }
    return h ? h : 1u;
}

uint8_t FlagsOf(void* plush) {
    return static_cast<uint8_t>((BoolOf(plush, L"haspaper") ? kFlagPaper : 0) | (BoolOf(plush, L"haspencil") ? kFlagPencil : 0));
}

bool Call0(void* obj, const wchar_t* fn) {
    void* f = obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), fn) : nullptr;
    return f && R::CallFunction(obj, f, nullptr);
}

// This world's live plushes (the plain one and the super eater), each with a key.
void LivePlushes(std::vector<void*>& out) {
    out.clear();
    void* world = ue_wrap::world_identity::CurrentWorld();
    if (!world) return;
    for (const wchar_t* name : kPlushClasses) {
        void* cls = ue_wrap::object_index::ClassByName(name);
        if (!cls) continue;
        std::vector<void*> found;
        ue_wrap::object_index::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
            if (obj) static_cast<std::vector<void*>*>(ctx)->push_back(obj);
        }, &found);
        for (void* obj : found)
            if (R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__") && ue_wrap::world_identity::WorldOf(obj) == world &&
                !KeyOf(obj).empty())
                out.push_back(obj);
    }
}

bool IsPlush(void* obj) {
    if (!obj || !R::IsLive(obj)) return false;
    void* base = ue_wrap::object_index::ClassByName(kPlushClasses[0]);
    return base && R::IsDescendantOfAny(R::ClassOf(obj), &base, 1);
}

bool IsPropOf(void* obj, const wchar_t* cls) {
    void* base = ue_wrap::object_index::ClassByName(cls);
    return base && obj && R::IsDescendantOfAny(R::ClassOf(obj), &base, 1);
}

// What a hit on the plush can mean to the quest: a paper, a pencil, a food (named for the wish), and any prop for the super eater.
bool IsQuestProp(void* plush, void* other) {
    if (!IsPropOf(other, L"prop_C")) return false;
    if (BoolOf(plush, L"supereater")) return true;
    return IsPropOf(other, L"prop_notebook_paper_C") || IsPropOf(other, L"prop_pencil_C") || IsPropOf(other, L"prop_food_C");
}

void* MeshOf(void* prop) {
    const int32_t off = prop ? R::FindPropertyOffset(R::ClassOf(prop), L"staticmesh") : -1;
    return off < 0 ? nullptr : *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(prop) + off);
}

// ---- the hit ----

std::unordered_map<uint64_t, long long> g_lastForward;   // (the plush's slot, the prop's eid) -> when it was sent

void Forward(coop::net::Session* s, void* plush, void* other) {
    const std::wstring key = KeyOf(plush);
    if (key.empty()) {
        UE_LOGW("erie_sync: CLIENT's plush %p has no key -- its hit is not asked of the host", plush);
        return;
    }
    coop::element::ElementId eid = coop::prop_element_tracker::GetPropElementIdForActor(other);
    if (eid == coop::element::kInvalidId) eid = coop::remote_prop::ResolveMirrorEidByActor(other);
    if (eid == coop::element::kInvalidId) {
        UE_LOGI("erie_sync: CLIENT's plush was hit by %p, a prop of no element id here -- not asked of the host", other);
        return;
    }
    const uint64_t slot = (static_cast<uint64_t>(static_cast<uint32_t>(R::InternalIndexOf(plush))) << 32) | static_cast<uint32_t>(eid);
    const long long now = NowMs();
    auto it = g_lastForward.find(slot);
    if (it != g_lastForward.end() && now - it->second < kSameHitMs) return;
    if (g_lastForward.size() > 256) g_lastForward.clear();
    g_lastForward[slot] = now;
    coop::net::PropIgniteIntentPayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.elementId = static_cast<uint32_t>(eid);
    p.verb = kVerbErieHit;
    const bool sent = s->SendReliable(coop::net::ReliableKind::PropIgniteIntent, &p, sizeof(p));
    ++g_forwardedHits;
    UE_LOGI("erie_sync: CLIENT's plush '%ls' was hit by %ls (eid %u) -- refused here, the host is asked: %d", key.c_str(),
            R::ClassNameOf(other).c_str(), p.elementId, sent ? 1 : 0);
}

// CLIENT: the copy runs no quest. Whatever the game's handler would do (a paper and a pencil eaten, a food eaten, the morph) is the
// host's, and the host's copies of the props cross.
sg::Verdict OnHitPre(const sg::Call& c) {
    auto* s = Client();
    if (!s || (c.fromOurCode && !g_drillCalls)) return sg::Verdict::Run;
    static R::ParamOffset s_other{L"OtherActor"};
    void* other = nullptr;
    const int32_t off = s_other.Of(c.function);
    if (off >= 0 && c.locals) std::memcpy(&other, c.locals + off, sizeof(other));
    const uint64_t n = ++g_refusedHits;
    if ((n & (n - 1)) == 0)
        UE_LOGI("erie_sync: CLIENT's plush %p hit handler refused (call #%llu)", c.object, static_cast<unsigned long long>(n));
    if (other && R::IsLive(other) && IsQuestProp(c.object, other)) Forward(s, c.object, other);
    return sg::Verdict::Cancel;
}

// ---- the achievement ----

// HOST: `liveerie` is the morph's own grant (lib.progressAchievement, called by `fuck`'s tail). The call runs, and every client is told.
std::vector<std::wstring> g_seenAchievements;

sg::Verdict OnAchievePre(const sg::Call& c) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return sg::Verdict::Run;
    static R::ParamOffset s_name{L"Achievement"};
    const int32_t off = s_name.Of(c.function);
    if (off < 0 || !c.locals) return sg::Verdict::Run;
    R::FName n;
    std::memcpy(&n, c.locals + off, sizeof(n));
    const std::wstring w = R::ToString(n);
    bool seen = false;
    for (const std::wstring& a : g_seenAchievements) seen = seen || a == w;
    if (!seen && g_seenAchievements.size() < 64) {
        g_seenAchievements.push_back(w);
        UE_LOGI("erie_sync: the host's first grant of the achievement '%ls' (the lane tells clients of 'liveerie' only)", w.c_str());
    }
    if (_wcsicmp(w.c_str(), L"liveerie") != 0) return sg::Verdict::Run;
    coop::net::EventCuePayload p{};
    p.cueId = kCueLiveErie;
    const bool sent = s->connected() && s->SendReliable(coop::net::ReliableKind::EventCue, &p, sizeof(p));
    UE_LOGI("erie_sync: the plush became the live Erie on the host -- %s", sent ? "cue sent" : "no peer to send the cue to");
    return sg::Verdict::Run;
}

// ---- HOST: the plush's flags ----

struct Sent { void* actor; uint8_t flags; };
std::unordered_map<uint32_t, Sent> g_sent;   // by the key's hash
bool g_resendAll = false;

void HostPoll(coop::net::Session* s) {
    if (!s->connected()) return;
    std::vector<void*> plushes;
    LivePlushes(plushes);
    for (void* plush : plushes) {
        const uint32_t id = Hash(KeyOf(plush));
        const uint8_t flags = FlagsOf(plush);
        auto it = g_sent.find(id);
        const bool fresh = it == g_sent.end() || it->second.actor != plush;
        const uint8_t last = fresh ? 0 : it->second.flags;
        if (flags == last && !(g_resendAll && flags != 0)) {
            if (fresh) g_sent[id] = Sent{plush, flags};
            continue;
        }
        ue_wrap::FVector at{};
        E::TryGetActorLocation(plush, at);
        Payload p{};
        p.id = id;
        p.x = at.X; p.y = at.Y; p.z = at.Z;
        p.family = coop::net::ambient_fixture::kEriePlush;
        p.alive = 1;
        p._pad[0] = flags;
        if (!s->SendReliable(coop::net::ReliableKind::AmbientFixtureState, &p, sizeof(p))) continue;
        UE_LOGI("erie_sync: HOST sent plush '%ls': paper %d pencil %d (food '%ls')", KeyOf(plush).c_str(), (flags & kFlagPaper) ? 1 : 0,
                (flags & kFlagPencil) ? 1 : 0, FNameOf(plush, L"food").c_str());
        g_sent[id] = Sent{plush, flags};
    }
    g_resendAll = false;
}

// ---- CLIENT: the copy takes the host's flags ----

struct Want { uint8_t flags; bool done; };
std::unordered_map<uint32_t, Want> g_want;

void ClientPoll() {
    bool owed = false;
    for (const auto& kv : g_want) owed = owed || !kv.second.done;
    if (!owed) return;
    std::vector<void*> plushes;
    LivePlushes(plushes);
    for (void* plush : plushes) {
        auto it = g_want.find(Hash(KeyOf(plush)));
        if (it == g_want.end() || it->second.done) continue;
        SetBoolOf(plush, L"haspaper", (it->second.flags & kFlagPaper) != 0);
        SetBoolOf(plush, L"haspencil", (it->second.flags & kFlagPencil) != 0);
        const bool shown = Call0(plush, L"upd");
        it->second.done = true;
        UE_LOGI("erie_sync: this peer's plush '%ls' follows the host: paper %d pencil %d (upd %d)", KeyOf(plush).c_str(),
                (it->second.flags & kFlagPaper) ? 1 : 0, (it->second.flags & kFlagPencil) ? 1 : 0, shown ? 1 : 0);
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_watchAsked) return;
    g_watchAsked = true;
    const bool a = sg::WatchName(kRowHit, kTagHit, &OnHitPre, nullptr);
    const bool b = sg::WatchName(kRowAchieve, kTagAch, &OnAchievePre, nullptr);
    if (!(a && b))
        UE_LOGE("erie_sync: a watch did not register (hit %d achievement %d) -- a client runs the plush quest on its own copy", a, b);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    const bool host = s->role() == coop::net::Role::Host;
    if (!host && g_want.empty()) return;
    const auto now = Clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastPoll).count() < kPollMs) return;
    g_lastPoll = now;
    if (host) HostPoll(s);
    else ClientPoll();
}

bool OnCue(const coop::net::EventCuePayload& p) {
    if (p.cueId != kCueLiveErie) return false;
    if (!Client()) return true;
    const bool granted = ue_wrap::profile::ProgressAchievement(L"liveerie");
    UE_LOGI("erie_sync: the plush became the live Erie on the host -- this player's achievement: %d", granted ? 1 : 0);
    return true;
}

void OnState(const Payload& p) {
    if (!Client()) return;
    if (p.id == 0 || (p._pad[0] & ~(kFlagPaper | kFlagPencil)) != 0) {
        UE_LOGW("erie_sync: a malformed plush state (id %u, flags 0x%02X) -- dropped", p.id, p._pad[0]);
        return;
    }
    g_want[p.id] = Want{p._pad[0], false};
}

bool TakeIntent(int senderSlot, const uint8_t* bytes, uint16_t len) {
    coop::net::PropIgniteIntentPayload p{};
    if (!bytes || len < sizeof(p)) return false;
    std::memcpy(&p, bytes, sizeof(p));
    if (p.verb != kVerbErieHit) return false;
    if (!IsHost() || p.key.len == 0 || p.key.len > sizeof(p.key.data)) return true;
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    void* plush = coop::prop_key_index::Find(key);
    void* other = coop::element::LivePropActor(static_cast<coop::element::ElementId>(p.elementId));
    if (!IsPlush(plush) || !other || !R::IsLive(other) || !IsQuestProp(plush, other)) {
        UE_LOGW("erie_sync: slot %d's hit of eid %u on '%ls' names no plush or no prop of the quest here -- dropped", senderSlot, p.elementId,
                key.c_str());
        return true;
    }
    ue_wrap::FVector a{}, b{};
    if (!E::TryGetActorLocation(plush, a) || !E::TryGetActorLocation(other, b) ||
        std::sqrt((a.X - b.X) * (a.X - b.X) + (a.Y - b.Y) * (a.Y - b.Y) + (a.Z - b.Z) * (a.Z - b.Z)) > kReachCm) {
        UE_LOGW("erie_sync: slot %d's hit of eid %u on '%ls': the host's copy of the prop is not by the plush -- dropped", senderSlot,
                p.elementId, key.c_str());
        return true;
    }
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(plush), kHitFn);
    ue_wrap::ParamFrame f(fn);
    const uint8_t before = FlagsOf(plush);
    bool ok = fn && f.valid() && f.Set<void*>(L"OtherActor", other);
    if (ok && MeshOf(plush)) f.Set<void*>(L"HitComponent", MeshOf(plush));
    if (ok && MeshOf(other)) f.Set<void*>(L"OtherComp", MeshOf(other));
    const bool ran = ok && ue_wrap::Call(plush, f);
    UE_LOGI("erie_sync: HOST ran slot %d's hit of %ls (eid %u) on plush '%ls': %d; paper %d -> %d, pencil %d -> %d, food '%ls'", senderSlot,
            R::ClassNameOf(other).c_str(), p.elementId, key.c_str(), ran ? 1 : 0, (before & kFlagPaper) ? 1 : 0, (FlagsOf(plush) & kFlagPaper) ? 1 : 0,
            (before & kFlagPencil) ? 1 : 0, (FlagsOf(plush) & kFlagPencil) ? 1 : 0, FNameOf(plush, L"food").c_str());
    return true;
}

void QueueConnectBroadcastForSlot(int /*slot*/) { g_resendAll = true; }

void SetDrillCallsCount(bool on) { g_drillCalls = on; }

uint64_t RefusedHits() { return g_refusedHits.load(); }
uint64_t ForwardedHits() { return g_forwardedHits.load(); }

void OnDisconnect() {
    g_sent.clear();
    g_want.clear();
    g_lastForward.clear();
    g_resendAll = false;
}

}  // namespace coop::erie_sync
