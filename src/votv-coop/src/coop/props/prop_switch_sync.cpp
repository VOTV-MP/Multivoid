// coop/props/prop_switch_sync.cpp -- see coop/props/prop_switch_sync.h.
//
// 0.9.0n. prop_deskLamp_C, prop_floorlamp_C, prop_lightIntWall1_C (and prop_lightIntWall2_C, which inherits it) and
// prop_cookingPad_C flip `active` in their actionOptionIndex and playerHandUse_RMB (the wall light also in wirePass)
// and run `upd`; the cord's plug and unplug run `upd` too. The hot plate's switch is its action 8, and its action 5
// steps its heat level `power` (an int, 0..4) and runs `upd`. prop_spotlightHead_C (and its tripod subclass) flips
// `active` in toggleLight and runs `upd`; prop_spotlight_C's head is a child actor, its `lighhead`, which names the
// spotlight back as its `tripod`. prop_beacon_C sets `isActive` from `open`'s `condition`. prop_smokeDetec_C sets
// `isActive` from `alarm`'s `activate`: its once-a-second timer runs `check`, which sounds it when `checkFire` finds a
// fire in its sphere, and its action 5 silences it. Every one of those bodies ends in the watched `upd`, `open` or
// `alarm`, so the switch is read where it lands, whatever pressed it.

#include "coop/props/prop_switch_sync.h"

#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace coop::prop_switch_sync {
namespace {

namespace N  = coop::net;
namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
namespace sg = ue_wrap::script_gate;
namespace EL = coop::element;

UE_FACT_FN(kRowDesk, "prop_switch_sync", L"prop_deskLamp_C", L"upd", Declared);
UE_FACT_FN(kRowFloor, "prop_switch_sync", L"prop_floorlamp_C", L"upd", Declared);
UE_FACT_FN(kRowWall, "prop_switch_sync", L"prop_lightIntWall1_C", L"upd", Declared);
UE_FACT_FN(kRowSpot, "prop_switch_sync", L"prop_spotlightHead_C", L"upd", Declared);
UE_FACT_FN(kRowPad, "prop_switch_sync", L"prop_cookingPad_C", L"upd", Declared);
UE_FACT_FN(kRowBeacon, "prop_switch_sync", L"prop_beacon_C", L"Open", Declared);
UE_FACT_FN(kRowSmoke, "prop_switch_sync", L"prop_smokeDetec_C", L"alarm", Declared);
UE_FACT_FN(kRowSmokeCheck, "prop_switch_sync", L"prop_smokeDetec_C", L"check", Declared);
constexpr int kTagSmokeCheck = 0x50535780;

struct Kind {
    const ue_wrap::facts::Function* row;
    const wchar_t* cls;
    const wchar_t* field;  // the switch
    const wchar_t* setFn;  // set through this function's bool `setParam`; null: the field written, then `upd` run
    const wchar_t* setParam;
    bool level;            // carries the hot plate's heat level, its int `power`
    bool offOnly;          // a client may ask it off, never on (the smoke alarm: the host's fires sound it)
};
const Kind kKinds[] = {
    {&kRowDesk, L"prop_deskLamp_C", L"active", nullptr, nullptr, false, false},
    {&kRowFloor, L"prop_floorlamp_C", L"active", nullptr, nullptr, false, false},
    {&kRowWall, L"prop_lightIntWall1_C", L"active", nullptr, nullptr, false, false},
    {&kRowSpot, L"prop_spotlightHead_C", L"active", nullptr, nullptr, false, false},
    {&kRowPad, L"prop_cookingPad_C", L"active", nullptr, nullptr, true, false},
    {&kRowBeacon, L"prop_beacon_C", L"isActive", L"Open", L"condition", false, false},
    {&kRowSmoke, L"prop_smokeDetec_C", L"isActive", L"alarm", L"activate", false, true},
};
constexpr size_t kKindCount = sizeof(kKinds) / sizeof(kKinds[0]);
constexpr int kTagBase = 0x50535700;     // 'PSW' + the kind's index
constexpr size_t kSpotKind = 3;
constexpr float kReachCm = 600.f;        // the sender's body to the prop
constexpr uint64_t kWindowMs = 500;      // a sender's asks are counted per half second,
constexpr int kWindowAsks = 8;           // and past this many in one the rest are answered with the host's state
constexpr int kMaxLevel = 4;             // the hot plate steps its level modulo 5
constexpr size_t kMaxHeld = 64;
constexpr size_t kMaxSeen = 1024;

N::Session* g_session = nullptr;
bool g_watched = false;
bool g_settled = false;
void* g_applying = nullptr;  // CLIENT: the prop a host's state is being applied to; its own `upd` asks nothing
uint64_t g_windowAt[N::kMaxPeers] = {};
int g_windowAsks[N::kMaxPeers] = {};

bool IsHost() { return g_session && g_session->running() && g_session->role() == N::Role::Host; }
bool IsClient() { return g_session && g_session->running() && g_session->role() == N::Role::Client; }

// Each kind's class, asked again when the class set moves.
void* g_bases[kKindCount] = {};
void* g_tripodBase = nullptr;  // prop_spotlight_C, whose head is a child actor
void RefreshBases() {
    static uint64_t s_set = ~0ull, s_unlist = ~0ull;
    const uint64_t set = OI::ClassSetVersion(), unlist = OI::ClassUnlistVersion();
    if (set == s_set && unlist == s_unlist) return;
    s_set = set;
    s_unlist = unlist;
    for (size_t i = 0; i < kKindCount; ++i) g_bases[i] = OI::ClassByName(kKinds[i].cls);
    g_tripodBase = OI::ClassByName(L"prop_spotlight_C");
}

// The kind of a live prop, or -1 for one that is no switched prop.
int KindOf(void* obj) {
    if (!obj || !R::IsLive(obj)) return -1;
    RefreshBases();
    void* const cls = R::ClassOf(obj);
    for (size_t i = 0; i < kKindCount; ++i)
        if (g_bases[i] && R::IsDescendantOfAny(cls, &g_bases[i], 1)) return static_cast<int>(i);
    return -1;
}

// The switch, and the level where the kind carries one.
struct State {
    bool on = false;
    uint8_t level = 0;
    bool operator==(const State& o) const { return on == o.on && level == o.level; }
};

int32_t* LevelOf(void* obj, int kind) {
    if (!kKinds[kind].level) return nullptr;
    static R::InstanceOffset s_level(L"power");
    const int32_t off = s_level.Of(obj);
    return off >= 0 ? reinterpret_cast<int32_t*>(static_cast<char*>(obj) + off) : nullptr;
}

bool ReadState(void* obj, int kind, State& out) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || kind < 0 || !R::FindBoolProperty(R::ClassOf(obj), kKinds[kind].field, off, mask) || off < 0) return false;
    out.on = (*(static_cast<uint8_t*>(obj) + off) & mask) != 0;
    const int32_t* const lv = LevelOf(obj, kind);
    out.level = lv && *lv >= 0 && *lv <= kMaxLevel ? static_cast<uint8_t>(*lv) : 0;
    return true;
}

void* ObjectAt(void* obj, R::InstanceOffset& field) {
    const int32_t off = obj ? field.Of(obj) : -1;
    void* const v = off >= 0 ? *reinterpret_cast<void**>(static_cast<char*>(obj) + off) : nullptr;
    return v && R::IsLive(v) ? v : nullptr;
}

uint32_t EidOf(void* obj) {
    const EL::ElementId eid = EL::Registry::Get().EidForActor(obj);
    return eid == EL::kInvalidId ? 0u : static_cast<uint32_t>(eid);
}

// A prop's name on the wire: its own element id, or a spotlight head's tripod's with `head` set. False for none yet.
bool Address(void* obj, int kind, N::PropSwitchPayload& p) {
    p.eid = EidOf(obj);
    p.head = 0;
    if (p.eid) return true;
    if (kind != static_cast<int>(kSpotKind)) return false;
    static R::InstanceOffset s_tripod(L"tripod");
    void* const tripod = ObjectAt(obj, s_tripod);
    p.eid = tripod ? EidOf(tripod) : 0u;
    p.head = 1;
    return p.eid != 0;
}

void* Resolve(const N::PropSwitchPayload& p) {
    void* const a = EL::LiveActorOfType(static_cast<EL::ElementId>(p.eid), EL::ElementType::Prop);
    if (!a || !p.head) return a;
    static R::InstanceOffset s_head(L"lighhead");
    return ObjectAt(a, s_head);
}

// The state set on this copy through the prop's own body: `open(condition)`, or the fields written and `upd` run.
bool Apply(void* obj, int kind, const State& st) {
    void* const cls = R::ClassOf(obj);
    if (kKinds[kind].setFn) {
        ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(cls, kKinds[kind].setFn));
        if (!f.valid()) return false;
        f.Set<bool>(kKinds[kind].setParam, st.on);
        return ue_wrap::Call(obj, f);
    }
    int32_t off = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(cls, kKinds[kind].field, off, mask) || off < 0) return false;
    uint8_t& b = *(static_cast<uint8_t*>(obj) + off);
    b = st.on ? static_cast<uint8_t>(b | mask) : static_cast<uint8_t>(b & ~mask);
    if (int32_t* const lv = LevelOf(obj, kind)) *lv = st.level;
    void* const upd = R::FindDispatchFunctionCached(cls, L"upd");
    return upd && R::CallFunction(obj, upd, nullptr);
}

// The state last sent (host) or last known (client), per prop.
struct Seen {
    ue_wrap::CachedObjRef ref;
    State st;
};
std::vector<Seen> g_seen;

Seen* FindSeen(void* obj) {
    for (Seen& s : g_seen)
        if (s.ref.Is(obj)) return &s;
    return nullptr;
}

// Records `st` for `obj`; false when it was recorded so already.
bool Note(void* obj, const State& st, bool* wasKnown = nullptr) {
    Seen* s = FindSeen(obj);
    if (wasKnown) *wasKnown = s != nullptr;
    if (s) {
        if (s->st == st) return false;
        s->st = st;
        return true;
    }
    for (size_t i = 0; i < g_seen.size();)
        if (!g_seen[i].ref.Alive()) g_seen.erase(g_seen.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    if (g_seen.size() >= kMaxSeen) return true;
    g_seen.emplace_back();
    g_seen.back().ref.Set(obj);
    g_seen.back().st = st;
    return true;
}

bool SendState(void* obj, int kind, const State& st, int slot) {
    N::PropSwitchPayload p{};
    if (!g_session || !g_session->connected() || !Address(obj, kind, p)) return false;
    p.on = st.on ? 1 : 0;
    p.level = st.level;
    const bool ok = slot >= 0 ? g_session->SendReliableToSlot(static_cast<uint8_t>(slot), N::ReliableKind::PropSwitchState,
                                                              &p, sizeof(p))
                              : g_session->SendReliable(N::ReliableKind::PropSwitchState, &p, sizeof(p));
    UE_LOGI("prop_switch: HOST %ls eid %u%hs is %hs, level %u -- sent to %hs (ok=%d)", kKinds[kind].cls, p.eid,
            p.head ? " (its head)" : "", st.on ? "on" : "off", p.level, slot >= 0 ? "one slot" : "every client",
            ok ? 1 : 0);
    return ok;
}

// The watched body ended: the switch has landed.
void OnSwitchPost(const sg::Call& c) {
    const int kind = c.tag - kTagBase;
    if (kind < 0 || kind >= static_cast<int>(kKindCount) || !c.object || c.object == g_applying) return;
    State st;
    if (!(IsHost() || IsClient()) || !ReadState(c.object, kind, st)) return;
    bool known = false;
    if (!Note(c.object, st, &known)) return;
    if (IsHost()) {
        SendState(c.object, kind, st, -1);
        return;
    }
    if (!known) return;  // first seen on this client: its baseline, no press
    N::PropSwitchPayload p{};
    if (!Address(c.object, kind, p)) return;  // a copy not bound yet: its spawn's own state, no press
    p.on = st.on ? 1 : 0;
    p.level = st.level;
    const bool ok = g_session->SendReliable(N::ReliableKind::PropSwitchIntent, &p, sizeof(p));
    UE_LOGI("prop_switch: CLIENT switched %ls eid %u%hs %hs, level %u -- asked of the host (ok=%d)", kKinds[kind].cls,
            p.eid, p.head ? " (its head)" : "", st.on ? "on" : "off", p.level, ok ? 1 : 0);
}

// CLIENT: the host's state on this copy.
bool TakeState(const N::PropSwitchPayload& p) {
    void* const obj = Resolve(p);
    const int kind = KindOf(obj);
    State cur;
    if (kind < 0 || !ReadState(obj, kind, cur)) return false;
    State want;
    want.on = p.on != 0;
    want.level = kKinds[kind].level ? p.level : 0;
    Note(obj, want);
    if (cur == want) return true;
    g_applying = obj;
    const bool ran = Apply(obj, kind, want);
    g_applying = nullptr;
    UE_LOGI("prop_switch: CLIENT %ls eid %u%hs is %hs, level %u, as the host's (ran=%d)", kKinds[kind].cls, p.eid,
            p.head ? " (its head)" : "", want.on ? "on" : "off", want.level, ran ? 1 : 0);
    return true;
}

std::vector<N::PropSwitchPayload> g_held;
bool g_heldFullSaid = false;

template <class F>
void ForEachSwitched(F&& fn) {
    RefreshBases();
    struct Walk { std::vector<void*> classes; } walk;
    OI::ForEachClass([](void* ctx, void* cls, void*) {
        Walk& w = *static_cast<Walk*>(ctx);
        for (void* b : g_bases)
            if (b && R::IsDescendantOfAny(cls, &b, 1)) {
                w.classes.push_back(cls);
                return;
            }
    }, &walk);
    std::vector<void*> found;
    for (void* cls : walk.classes)
        OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
            if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__"))
                static_cast<std::vector<void*>*>(ctx)->push_back(obj);
        }, &found);
    for (void* o : found) fn(o);
}

// CLIENT: the smoke detector's once-a-second check for fire is the host's, whose fires are the shared ones.
sg::Verdict OnSmokeCheckPre(const sg::Call&) { return IsClient() ? sg::Verdict::Cancel : sg::Verdict::Run; }

}  // namespace

void Install(N::Session* session) {
    g_session = session;
    if (g_watched) return;
    g_watched = true;
    if (!sg::WatchName(kRowSmokeCheck, kTagSmokeCheck, &OnSmokeCheckPre, nullptr))
        UE_LOGE("prop_switch: the watch on the smoke detector's check was refused -- a client sounds its own alarm");
    for (size_t i = 0; i < kKindCount; ++i)
        if (!sg::WatchName(*kKinds[i].row, kTagBase + static_cast<int>(i), nullptr, &OnSwitchPost))
            UE_LOGE("prop_switch: the watch on %ls was refused -- its switch is each machine's own", kKinds[i].cls);
}

void Tick() {
    if (!g_watched || g_settled) return;
    sg::ResolvePendingNames();
    for (size_t i = 0; i < kKindCount; ++i)
        if (!sg::NameWatchSettled(*kKinds[i].row, kTagBase + static_cast<int>(i))) return;
    if (!sg::NameWatchSettled(kRowSmokeCheck, kTagSmokeCheck)) return;
    g_settled = true;
    if (!sg::NameWatchLive(kRowSmokeCheck, kTagSmokeCheck))
        UE_LOGE("prop_switch: the watch on the smoke detector's check is dead -- a client sounds its own alarm");
    int live = 0;
    for (size_t i = 0; i < kKindCount; ++i) {
        if (sg::NameWatchLive(*kKinds[i].row, kTagBase + static_cast<int>(i))) ++live;
        else UE_LOGE("prop_switch: the watch on %ls is dead -- its switch is each machine's own", kKinds[i].cls);
    }
    UE_LOGI("prop_switch: %d of %zu switch watches are live (lamps, wall lights, spotlight, hot plate, beacon, smoke "
            "alarm)", live, kKindCount);
}

void SendJoinSnapshotForSlot(int slot) {
    if (!IsHost() || slot <= 0) return;
    int sent = 0;
    ForEachSwitched([slot, &sent](void* obj) {
        const int kind = KindOf(obj);
        State st;
        if (ReadState(obj, kind, st) && SendState(obj, kind, st, slot)) ++sent;
    });
    UE_LOGI("prop_switch: HOST sent %d switches to joining slot %d", sent, slot);
}

void OnIntent(int senderSlot, const N::PropSwitchPayload& p) {
    if (!IsHost() || senderSlot <= 0 || senderSlot >= static_cast<int>(N::kMaxPeers)) return;
    void* const obj = Resolve(p);
    const int kind = KindOf(obj);
    State cur;
    if (kind < 0 || !ReadState(obj, kind, cur)) {
        UE_LOGW("prop_switch: HOST slot %d asked to switch eid %u%hs, which is no switched prop here -- refused", senderSlot,
                p.eid, p.head ? " (its head)" : "");
        return;
    }
    const uint64_t now = ::GetTickCount64();
    if (now - g_windowAt[senderSlot] >= kWindowMs) {
        g_windowAt[senderSlot] = now;
        g_windowAsks[senderSlot] = 0;
    }
    if (++g_windowAsks[senderSlot] > kWindowAsks) {
        UE_LOGW("prop_switch: HOST slot %d's ask of eid %u refused: past %d asks in %llu ms", senderSlot, p.eid,
                kWindowAsks, static_cast<unsigned long long>(kWindowMs));
        SendState(obj, kind, cur, senderSlot);  // the asker's copy takes the host's
        return;
    }
    State want;
    want.on = p.on != 0;
    want.level = kKinds[kind].level && p.level <= kMaxLevel ? p.level : cur.level;
    coop::RemotePlayer* const rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(senderSlot));
    void* const body = rp ? rp->actor() : nullptr;
    ue_wrap::FVector bodyAt{}, at{};
    const bool reach = body && E::TryGetActorLocation(body, bodyAt) && E::TryGetActorLocation(obj, at) &&
                       (bodyAt.X - at.X) * (bodyAt.X - at.X) + (bodyAt.Y - at.Y) * (bodyAt.Y - at.Y) +
                               (bodyAt.Z - at.Z) * (bodyAt.Z - at.Z) <= kReachCm * kReachCm;
    const bool onRefused = kKinds[kind].offOnly && want.on && !cur.on;
    if (!reach || cur == want || onRefused) {
        if (onRefused)
            UE_LOGW("prop_switch: HOST slot %d asked %ls eid %u on, which only the host's own check does -- refused",
                    senderSlot, kKinds[kind].cls, p.eid);
        else if (!reach)
            UE_LOGW("prop_switch: HOST slot %d's switch of %ls eid %u refused: its body is not within %.0f m", senderSlot,
                    kKinds[kind].cls, p.eid, kReachCm / 100.f);
        SendState(obj, kind, cur, senderSlot);  // the asker's copy takes the host's
        return;
    }
    const bool ran = Apply(obj, kind, want);  // the body's end is watched: its state goes to every client
    UE_LOGI("prop_switch: HOST slot %d switched %ls eid %u%hs %hs, level %u (ran=%d)", senderSlot, kKinds[kind].cls,
            p.eid, p.head ? " (its head)" : "", want.on ? "on" : "off", want.level, ran ? 1 : 0);
}

void OnState(const N::PropSwitchPayload& p) {
    if (!IsClient() || TakeState(p)) return;
    for (N::PropSwitchPayload& h : g_held)
        if (h.eid == p.eid && h.head == p.head) {
            h = p;
            return;
        }
    if (g_held.size() >= kMaxHeld) {
        if (!g_heldFullSaid) {
            g_heldFullSaid = true;
            UE_LOGW("prop_switch: CLIENT %zu states are held for copies not bound -- eid %u's is dropped", kMaxHeld, p.eid);
        }
        return;
    }
    g_held.push_back(p);
}

void OnBound(uint32_t eid, void* actor) {
    if (!IsClient() || !actor) return;
    // The copy's state at its bind is its baseline, so its first press is a change: a hot plate or desk lamp runs no
    // `upd` before it is pressed. A tripod's head is the switched prop, not the tripod.
    void* target = actor;
    int kind = KindOf(actor);
    if (kind < 0 && g_tripodBase && R::IsDescendantOfAny(R::ClassOf(actor), &g_tripodBase, 1)) {
        static R::InstanceOffset s_head(L"lighhead");
        target = ObjectAt(actor, s_head);
        kind = KindOf(target);
    }
    State st;
    if (kind >= 0 && !FindSeen(target) && ReadState(target, kind, st)) Note(target, st);
    for (size_t i = 0; i < g_held.size();) {
        if (g_held[i].eid != eid) {
            ++i;
            continue;
        }
        const N::PropSwitchPayload h = g_held[i];
        g_held.erase(g_held.begin() + static_cast<std::ptrdiff_t>(i));
        TakeState(h);
    }
}

void OnClientWorldReady() {
    g_held.clear();
    g_heldFullSaid = false;
    g_seen.clear();
}

void OnDisconnect() {
    OnClientWorldReady();
    for (uint64_t& t : g_windowAt) t = 0;
    for (int& n : g_windowAsks) n = 0;
}

}  // namespace coop::prop_switch_sync
