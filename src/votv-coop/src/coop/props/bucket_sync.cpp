// coop/props/bucket_sync.cpp -- see coop/props/bucket_sync.h.

#include "coop/props/bucket_sync.h"

#include "coop/config/config.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace coop::bucket_sync {
namespace {

namespace N  = coop::net;
namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
namespace sg = ue_wrap::script_gate;
using ue_wrap::FVector;

UE_FACT_FN(kRowTick, "bucket_sync", nullptr, L"ReceiveTick", AnyClass);
UE_FACT_FN(kRowHandUse, "bucket_sync", nullptr, L"playerHandUse_LMB", AnyClass);
constexpr int kTag = 0x424B5453;   // 'BKTS'

constexpr uint64_t kMinGapMs = 100;      // one bucket's rows at most ten a second (a tipped bucket pours every tick)
constexpr uint64_t kParkMs = 10000;      // a row for a bucket not bound here yet waits this long
constexpr float    kLevelEps = 0.5f;     // a change of water worth a row
constexpr float    kAlcEps = 0.05f;      // of brewing
constexpr float    kDryEps = 30.f;       // of the drying clock, seconds
constexpr float    kReachCm = 600.f;     // a client's ask names a bucket and an item this near its player

std::atomic<N::Session*> g_session{nullptr};

uint64_t Now() { return ::GetTickCount64(); }

// The four classes, by name, per world: a Blueprint class is a new object in every world. The world's generation is
// read once a frame (RefreshForWorld, from Tick), not in the watch's callbacks, which run for every Blueprint tick.
struct Classes {
    void* mop = nullptr;
    void* concrete = nullptr;
    void* bag = nullptr;
    void* fixer = nullptr;
    uint32_t gen = 0;
};
Classes g_cls;
const Classes& Cls() { return g_cls; }

void ClearOffsets();
// A class not loaded yet is asked again each frame until it is: the concrete bucket's loads with its first one.
void RefreshForWorld() {
    const uint32_t gen = ue_wrap::world_identity::Generation();
    const bool fresh = gen != g_cls.gen;
    if (fresh) {
        g_cls = Classes{};
        g_cls.gen = gen;
        ClearOffsets();
    }
    if (!g_cls.mop) g_cls.mop = OI::ClassByName(L"prop_bucket_C");
    if (!g_cls.concrete) g_cls.concrete = OI::ClassByName(L"prop_concreteBucket_C");
    if (!g_cls.bag) g_cls.bag = OI::ClassByName(L"prop_cementBag_C");
    if (!g_cls.fixer) g_cls.fixer = OI::ClassByName(L"prop_wallfixer_C");
}

// What a class is to this lane: a mop bucket, a concrete bucket, a cement bag, or none, by its parent chain, so a
// subclass with no body of its own counts as its parent (prop_bucket_mop_C, the base's mop bucket, is a prop_bucket_C
// with nothing of its own: Ghezra/Multivoid issue 42, where a host's mop bucket matched nothing and never synced). One
// walk per class, kept for the world.
std::unordered_map<void*, int> g_types;
void* g_lastCls = nullptr;   // the tick's watch asks about one class after another: the last answer, kept
int g_lastType = -1;
int TypeOfClass(void* cls) {
    if (!cls) return -1;
    const Classes& k0 = Cls();
    if (cls == k0.mop) return N::kBucketMop;
    if (cls == k0.concrete) return N::kBucketConcrete;
    if (cls == k0.bag) return N::kBucketBag;
    // VOTVCOOP_BUCKET_EXACT=1: the exact classes alone, as before issue 42 -- the bucket drill's red arm. Resolved once.
    static const bool exact = coop::config::ReadEnv("VOTVCOOP_BUCKET_EXACT") == "1";
    if (exact) return -1;
    if (cls == g_lastCls) return g_lastType;
    auto it = g_types.find(cls);
    if (it != g_types.end()) {
        g_lastCls = cls;
        g_lastType = it->second;
        return it->second;
    }
    const Classes& k = Cls();
    int type = -1;
    for (void* c = cls; c && type < 0; c = R::SuperStructOf(c)) {
        if (c == k.mop) type = N::kBucketMop;
        else if (c == k.concrete) type = N::kBucketConcrete;
        else if (c == k.bag) type = N::kBucketBag;
    }
    // A base not loaded yet decides nothing: asked again once it is.
    if (type >= 0 || (k.mop && k.concrete && k.bag)) {
        g_types.emplace(cls, type);
        g_lastCls = cls;
        g_lastType = type;
    }
    return type;
}

int TypeOf(void* actor) { return actor ? TypeOfClass(R::ClassOf(actor)) : -1; }

// A field's offset on a class, looked up once (the reflection walk is linear) and kept for the world: the key is the
// class and the name's literal, and the table goes with the world's generation, as the classes do.
struct OffKey {
    void* cls;
    const wchar_t* name;
    bool operator==(const OffKey& o) const { return cls == o.cls && name == o.name; }
};
struct OffHash {
    size_t operator()(const OffKey& k) const {
        return std::hash<void*>()(k.cls) ^ (std::hash<const void*>()(k.name) * 31u);
    }
};
std::unordered_map<OffKey, int32_t, OffHash> g_offs;
void ClearOffsets() {
    g_offs.clear();
    g_types.clear();
    g_lastCls = nullptr;
}
int32_t Off(void* obj, const wchar_t* name) {
    void* const cls = obj ? R::ClassOf(obj) : nullptr;
    if (!cls) return -1;
    auto it = g_offs.find(OffKey{cls, name});
    if (it == g_offs.end()) it = g_offs.emplace(OffKey{cls, name}, R::FindPropertyOffset(cls, name)).first;
    return it->second;
}
template <class T>
T Get(void* obj, const wchar_t* name, T dflt) {
    const int32_t off = Off(obj, name);
    return off >= 0 ? *reinterpret_cast<const T*>(static_cast<const char*>(obj) + off) : dflt;
}
template <class T>
void Put(void* obj, const wchar_t* name, T v) {
    const int32_t off = Off(obj, name);
    if (off >= 0) *reinterpret_cast<T*>(static_cast<char*>(obj) + off) = v;
}

bool CallNoArgs(void* obj, const wchar_t* fn) {
    void* const f = obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), fn) : nullptr;
    ue_wrap::ParamFrame frame(f);
    return f && frame.valid() && ue_wrap::Call(obj, frame);
}

uint32_t EidOf(void* actor) {
    const coop::element::ElementId e = coop::element::Registry::Get().EidForActor(actor);
    return (e == coop::element::kInvalidId) ? 0u : static_cast<uint32_t>(e);
}

void* ActorOf(uint32_t eid) {
    coop::element::Element* el = coop::element::Registry::Get().Get(static_cast<coop::element::ElementId>(eid));
    void* actor = el ? el->GetActor() : nullptr;
    if (!actor || !R::IsLiveByIndex(actor, el->GetInternalIdx())) return nullptr;
    return actor;
}

// `actor`'s row as it stands; type -1 when it is none of the three.
N::BucketRow Read(void* actor, uint32_t eid) {
    N::BucketRow r{};
    r.eid = eid;
    const int t = TypeOf(actor);
    r.type = static_cast<uint8_t>(t < 0 ? 0xFF : t);
    if (t == N::kBucketMop) {
        r.level = Get<float>(actor, L"Height", 0.f);
        r.count = static_cast<int16_t>(Get<int32_t>(actor, L"soap", 0));
        r.progress = Get<float>(actor, L"alcoholP", 0.f);
        r.flags = static_cast<uint8_t>((Get<uint8_t>(actor, L"alcohol", 0) ? N::kBucketAlcohol : 0) |
                                       (Get<uint8_t>(actor, L"alcReady", 0) ? N::kBucketAlcReady : 0));
    } else if (t == N::kBucketConcrete) {
        r.count = static_cast<int16_t>(Get<int32_t>(actor, L"units", 0));
        r.progress = Get<float>(actor, L"dryTimer", 0.f);
        r.flags = Get<uint8_t>(actor, L"dry", 0) ? N::kBucketDry : 0;
    } else if (t == N::kBucketBag) {
        r.count = static_cast<int16_t>(Get<int32_t>(actor, L"units", 0));
    }
    return r;
}

// Whether `now` differs from `was` by a row's worth.
bool Changed(const N::BucketRow& was, const N::BucketRow& now) {
    if (was.type != now.type || was.flags != now.flags || was.count != now.count) return true;
    if (now.type == N::kBucketMop)
        return std::fabs(was.level - now.level) > kLevelEps || std::fabs(was.progress - now.progress) > kAlcEps;
    if (now.type == N::kBucketConcrete) return std::fabs(was.progress - now.progress) > kDryEps;
    return false;
}

// The water of a mop bucket moved by a verb: its level or its soap.
bool WaterMoved(const N::BucketRow& was, const N::BucketRow& now) {
    return std::fabs(was.level - now.level) > 0.01f || was.count != now.count;
}

void Apply(void* actor, const N::BucketRow& r) {
    if (r.type == N::kBucketMop) {
        Put<float>(actor, L"Height", r.level);
        Put<int32_t>(actor, L"soap", r.count);
        Put<float>(actor, L"alcoholP", r.progress);
        Put<uint8_t>(actor, L"alcohol", (r.flags & N::kBucketAlcohol) ? 1 : 0);
        Put<uint8_t>(actor, L"alcReady", (r.flags & N::kBucketAlcReady) ? 1 : 0);
        CallNoArgs(actor, L"upd");
    } else if (r.type == N::kBucketConcrete) {
        Put<int32_t>(actor, L"units", r.count);
        Put<float>(actor, L"dryTimer", r.progress);
        Put<uint8_t>(actor, L"dry", (r.flags & N::kBucketDry) ? 1 : 0);
        CallNoArgs(actor, L"updStage");   // the mesh of the stage, and updDry
    } else if (r.type == N::kBucketBag) {
        Put<int32_t>(actor, L"units", r.count);
        UE_LOGI("bucket_sync: cement bag eid=%u now %d units, as the host's", r.eid, static_cast<int>(r.count));
    }
}

bool IsClient() {
    N::Session* s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->role() != N::Role::Host;
}

bool IsHost() {
    N::Session* s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->role() == N::Role::Host;
}

// ---------------------------------------------------------------- HOST

// What the host last told of each bucket and bag it has seen, by actor. A bucket's own tick marks it when its state
// moved past a row's worth since; a verb the host runs (its player's cement bag, a client's ask) marks what it changed.
// The mod's tick sends the marked ones and does nothing when none is marked: no read on a timer, no walk for new
// buckets (a new one is seen at its first tick, a bag at its first mix).
struct Known {
    uint32_t eid = 0;
    N::BucketRow sent{};
    bool everSent = false;
    bool dirty = false;
    uint64_t sentAt = 0;
};
std::unordered_map<void*, Known> g_known;
std::vector<void*> g_dirty;
uint64_t g_rowsSent = 0, g_intentsRun = 0, g_intentsRefused = 0;
constexpr size_t kPruneAbove = 256;   // past this many known actors the dead ones go

void MarkDirty(void* actor) {
    if (!actor || TypeOf(actor) < 0) return;
    Known& k = g_known[actor];
    if (k.dirty) return;
    k.dirty = true;
    g_dirty.push_back(actor);
}

void SendRows(N::Session& s, const std::vector<N::BucketRow>& rows, int slot) {
    for (size_t i = 0; i < rows.size(); i += N::kBucketRowsMax) {
        N::BucketStatePayload p{};
        p.count = static_cast<uint8_t>(std::min<size_t>(N::kBucketRowsMax, rows.size() - i));
        for (int j = 0; j < p.count; ++j) p.rows[j] = rows[i + j];
        if (slot < 0) s.SendReliable(N::ReliableKind::BucketState, &p, sizeof(p));
        else s.SendReliableToSlot(slot, N::ReliableKind::BucketState, &p, sizeof(p));
        g_rowsSent += p.count;
    }
}

// HOST, after a bucket's tick: marked when its state moved past a row's worth since what was told.
void HostAfterTick(void* actor) {
    Known& k = g_known[actor];
    if (k.dirty) return;
    if (k.everSent && !Changed(k.sent, Read(actor, k.eid))) return;
    k.dirty = true;
    g_dirty.push_back(actor);
}

void HostTick(N::Session& s) {
    if (g_dirty.empty()) return;
    const bool anyone = s.AnyWorldReadyPeer();
    const uint64_t now = Now();
    std::vector<N::BucketRow> rows;
    std::vector<void*> keep;
    for (void* actor : g_dirty) {
        auto it = g_known.find(actor);
        if (it == g_known.end()) continue;
        Known& k = it->second;
        if (!R::IsLive(actor) || TypeOf(actor) < 0) {
            g_known.erase(it);
            continue;
        }
        if (!anyone) {   // no one to tell; a joiner is sent every bucket at its world-ready
            k.dirty = false;
            continue;
        }
        if (!k.eid || ActorOf(k.eid) != actor) {   // a new actor, or a new one where a dead one was
            const uint32_t eid = EidOf(actor);
            if (eid != k.eid) k.everSent = false;
            k.eid = eid;
        }
        if (!k.eid || (k.everSent && now - k.sentAt < kMinGapMs)) {   // not bound yet, or told too lately: later
            keep.push_back(actor);
            continue;
        }
        k.dirty = false;
        const N::BucketRow r = Read(actor, k.eid);
        if (k.everSent && !Changed(k.sent, r)) continue;
        k.sent = r;
        k.everSent = true;
        k.sentAt = now;
        rows.push_back(r);
        if (r.type == N::kBucketBag)
            UE_LOGI("bucket_sync: cement bag eid=%u now %d units -- told", k.eid, static_cast<int>(r.count));
    }
    g_dirty.swap(keep);
    if (!rows.empty()) SendRows(s, rows, -1);
    if (g_known.size() > kPruneAbove)
        for (auto it = g_known.begin(); it != g_known.end();)
            it = (!it->second.dirty && !R::IsLive(it->first)) ? g_known.erase(it) : std::next(it);
}

// Whether slot `slot`'s player stands within reach of `actor`; a player out of it is said with its distance.
bool InReach(int slot, void* actor) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
    FVector p{}, a{};
    if (!rp || !rp->valid() || !rp->TryGetLocation(p) || !E::TryGetActorLocation(actor, a)) {
        UE_LOGI("bucket_sync: slot %d's player or the bucket has no place on the host", slot);
        return false;
    }
    const float x = p.X - a.X, y = p.Y - a.Y, z = p.Z - a.Z;
    const float d2 = x * x + y * y + z * z;
    if (d2 > kReachCm * kReachCm)
        UE_LOGI("bucket_sync: slot %d's player at (%.0f,%.0f,%.0f) is %.0f cm from the bucket at (%.0f,%.0f,%.0f)", slot, p.X,
                p.Y, p.Z, std::sqrt(d2), a.X, a.Y, a.Z);
    return d2 <= kReachCm * kReachCm;
}

// A refused ask leaves the sender's copy as its verb left it: the host's row of the bucket, to the sender alone, puts
// it back.
void Refuse(int slot, const char* why, uint32_t eid) {
    ++g_intentsRefused;
    N::Session* s = g_session.load(std::memory_order_acquire);
    void* actor = ActorOf(eid);
    if (s && actor && TypeOf(actor) >= 0) SendRows(*s, {Read(actor, eid)}, slot);
    UE_LOGI("bucket_sync: slot %d's ask on bucket eid=%u refused: %s (#%llu)", slot, eid, why,
            static_cast<unsigned long long>(g_intentsRefused));
}

// The cement bag's own mix (prop_cementBag_C playerHandUse_LMB), on the host's copies: the bucket becomes a concrete
// bucket of round(Height / MaxHeight * 20) units, taken from the bag, which goes when empty.
void Mix(int slot, void* bucket, void* bag, uint32_t eid) {
    if (!Get<uint8_t>(bucket, L"canConcrete", 0)) return Refuse(slot, "the bucket takes no concrete", eid);
    const float maxH = Get<float>(bucket, L"MaxHeight", 0.f);
    const int need = maxH > 0.f ? static_cast<int>(std::lround(Get<float>(bucket, L"Height", 0.f) / maxH * 20.f)) : 0;
    const int have = Get<int32_t>(bag, L"units", 0);
    if (need <= 0) return Refuse(slot, "not enough water", eid);
    if (have < need) return Refuse(slot, "not enough cement", eid);
    void* lib = R::FindClassDefaultObject(L"lib_C");
    void* fn = lib ? R::FindDispatchFunctionCached(R::ClassOf(lib), L"replaceProp") : nullptr;
    ue_wrap::ParamFrame f(fn);
    const R::FName name = ue_wrap::fname_utils::StringToFName(L"concreteBucket");
    void* none = nullptr;
    if (!fn || !f.valid() || !f.Set<void*>(L"prop", bucket) || !f.SetRaw(L"new", &name, sizeof(name)) ||
        !f.Set<bool>(L"delete", false) || !f.Set<void*>(L"playerHolding", none) ||
        !f.Set<void*>(L"__WorldContext", bag) || !ue_wrap::Call(lib, f))
        return Refuse(slot, "lib.replaceProp did not run", eid);
    void* made = f.Get<void*>(L"Return");
    if (!made || TypeOf(made) != N::kBucketConcrete) return Refuse(slot, "replaceProp made no concrete bucket", eid);
    Put<int32_t>(bag, L"units", have - need);
    MarkDirty(bag);   // the concrete bucket is told at its first tick
    Put<int32_t>(made, L"units", need);
    CallNoArgs(made, L"updStage");
    E::DestroyActor(bucket);
    if (have - need <= 0) E::DestroyActor(bag);
    ++g_intentsRun;
    UE_LOGI("bucket_sync: slot %d mixed cement into bucket eid=%u: a concrete bucket of %d units, the bag left %d", slot,
            eid, need, have - need);
}

// ---------------------------------------------------------------- CLIENT

std::unordered_map<void*, N::BucketRow> g_post;        // a mop bucket's row after its last tick or a host row
std::unordered_map<uint32_t, N::BucketRow> g_hostRow;  // the host's last row of each bucket
struct Parked {
    N::BucketRow row;
    uint64_t until;
};
std::unordered_map<uint32_t, Parked> g_parked;
std::vector<N::BucketIntentPayload> g_asks;
uint64_t g_applied = 0, g_asked = 0;

void Ask(uint8_t op, uint32_t itemEid, const N::BucketRow& bucket) {
    N::BucketIntentPayload p{};
    p.op = op;
    p.itemEid = itemEid;
    p.bucket = bucket;
    g_asks.push_back(p);
}

void* PlayerOf(const sg::Call& c) {
    const int32_t off = c.locals ? R::FindParamOffset(c.function, L"player") : -1;
    void* p = nullptr;
    if (off >= 0) std::memcpy(&p, c.locals + off, sizeof(p));
    return p;
}

sg::Verdict OnTickPre(const sg::Call& c) {
    if (!c.object || !IsClient()) return sg::Verdict::Run;
    const int t = TypeOf(c.object);
    if (t == N::kBucketConcrete) return sg::Verdict::Cancel;   // the spill, the drying, the turn back: the host's
    if (t != N::kBucketMop) return sg::Verdict::Run;
    auto it = g_post.find(c.object);
    if (it == g_post.end()) return sg::Verdict::Run;
    const uint32_t eid = EidOf(c.object);
    const N::BucketRow now = Read(c.object, eid);
    if (!eid || !WaterMoved(it->second, now)) return sg::Verdict::Run;
    // Moved between two ticks: a verb of this peer's. The host's own row is no ask.
    auto h = g_hostRow.find(eid);
    if (h != g_hostRow.end() && !WaterMoved(h->second, now)) return sg::Verdict::Run;
    Ask(N::kBucketIntentWater, 0, now);
    it->second = now;
    return sg::Verdict::Run;
}

void OnTickPost(const sg::Call& c) {
    if (!c.object) return;
    const int t = TypeOf(c.object);
    if (t < 0) return;
    if (IsHost()) return HostAfterTick(c.object);
    if (t == N::kBucketMop && IsClient()) g_post[c.object] = Read(c.object, EidOf(c.object));
}

// HOST: its player's cement bag ran its own body (a mix, or units passed to another bag): the bags it touched are told.
void OnHandPost(const sg::Call& c) {
    if (!c.object || !IsHost() || TypeOf(c.object) != N::kBucketBag) return;
    MarkDirty(c.object);
    void* const player = PlayerOf(c);
    void* const hit = player ? E::ReadMainPlayerHitActor(player) : nullptr;
    if (TypeOf(hit) == N::kBucketBag) MarkDirty(hit);
}

sg::Verdict OnHandPre(const sg::Call& c) {
    if (!c.object || !IsClient()) return sg::Verdict::Run;
    void* const cls = R::ClassOf(c.object);
    const Classes& k = Cls();
    if (!cls || (cls != k.bag && cls != k.fixer)) return sg::Verdict::Run;
    void* const player = PlayerOf(c);
    if (!player || !coop::players::Registry::Get().IsLocal(player)) return sg::Verdict::Run;
    void* const hit = E::ReadMainPlayerHitActor(player);
    const uint32_t itemEid = EidOf(c.object), hitEid = EidOf(hit);
    if (cls == k.bag) {
        if (TypeOf(hit) != N::kBucketMop || !Get<uint8_t>(hit, L"canConcrete", 0)) return sg::Verdict::Run;
        if (!itemEid || !hitEid) return sg::Verdict::Run;
        Ask(N::kBucketIntentConcrete, itemEid, Read(hit, hitEid));
        UE_LOGI("bucket_sync: this client mixed cement (eid=%u) into bucket eid=%u -- asked of the host", itemEid, hitEid);
        return sg::Verdict::Cancel;
    }
    // The trowel on a concrete bucket: its own body runs where the bucket is dry or the trowel still loaded (a hint).
    if (TypeOf(hit) != N::kBucketConcrete || !itemEid || !hitEid) return sg::Verdict::Run;
    if (Get<int32_t>(c.object, L"concrete", 0) > 0 || Get<uint8_t>(hit, L"dry", 0) || Get<int32_t>(hit, L"units", 0) <= 0)
        return sg::Verdict::Run;
    Put<int32_t>(c.object, L"concrete", 5);   // the trowel's own load, as its body gives it
    CallNoArgs(c.object, L"updConcrete");
    Ask(N::kBucketIntentTake, itemEid, Read(hit, hitEid));
    UE_LOGI("bucket_sync: this client's trowel (eid=%u) took from concrete bucket eid=%u -- asked of the host", itemEid,
            hitEid);
    return sg::Verdict::Cancel;
}

void ApplyRow(const N::BucketRow& r) {
    void* actor = ActorOf(r.eid);
    if (!actor || TypeOf(actor) != r.type) {
        g_parked[r.eid] = Parked{r, Now() + kParkMs};
        return;
    }
    Apply(actor, r);
    g_hostRow[r.eid] = r;
    if (r.type == N::kBucketMop) g_post[actor] = Read(actor, r.eid);
    ++g_applied;
}

void ClientTick(N::Session& s) {
    if (!g_parked.empty()) {
        const uint64_t now = Now();
        std::vector<N::BucketRow> retry;
        for (auto it = g_parked.begin(); it != g_parked.end();) {
            if (now >= it->second.until) {
                it = g_parked.erase(it);
                continue;
            }
            void* a = ActorOf(it->first);
            if (a && TypeOf(a) == it->second.row.type) {
                retry.push_back(it->second.row);
                it = g_parked.erase(it);
            } else {
                ++it;
            }
        }
        for (const N::BucketRow& r : retry) ApplyRow(r);
    }
    if (g_asks.empty()) return;
    for (const N::BucketIntentPayload& p : g_asks) {
        if (s.SendReliable(N::ReliableKind::BucketIntent, &p, sizeof(p))) ++g_asked;
        else UE_LOGW("bucket_sync: an ask on bucket eid=%u was not sent", p.bucket.eid);
    }
    g_asks.clear();
}

bool g_watchesRegistered = false, g_watchesSettled = false;

void SettleWatches() {
    if (g_watchesSettled) return;
    if (!g_watchesRegistered) {
        g_watchesRegistered = true;
        const bool a = sg::WatchName(kRowTick, kTag, &OnTickPre, &OnTickPost);
        const bool b = sg::WatchName(kRowHandUse, kTag, &OnHandPre, &OnHandPost);
        if (!a || !b) {
            g_watchesSettled = true;
            UE_LOGE("bucket_sync: the gate took no watch on %ls -- a client's buckets run on their own",
                    !a ? kRowTick.Name() : kRowHandUse.Name());
            return;
        }
    }
    sg::ResolvePendingNames();
    if (!sg::NameWatchSettled(kRowTick, kTag) || !sg::NameWatchSettled(kRowHandUse, kTag)) return;
    g_watchesSettled = true;
    const bool live = sg::NameWatchLive(kRowTick, kTag) && sg::NameWatchLive(kRowHandUse, kTag);
    if (live) UE_LOGI("bucket_sync: the watches on %ls and %ls are live", kRowTick.Name(), kRowHandUse.Name());
    else UE_LOGE("bucket_sync: a watch settled dead -- a client's buckets run on their own");
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    N::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    RefreshForWorld();
    SettleWatches();
    if (s->role() == N::Role::Host) HostTick(*s);
    else ClientTick(*s);
}

void OnPeerWorldReady(int slot) {
    N::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != N::Role::Host) return;
    // The one walk of the buckets: a joiner is told every one, whatever it last heard.
    std::vector<N::BucketRow> rows;
    const Classes& k = Cls();
    // The instance index lists each class by itself: every loaded class the lane counts, subclasses included.
    std::vector<void*> classes;
    OI::ForEachClass([](void* ctx, void* cls, void*) {
        if (TypeOfClass(cls) >= 0) static_cast<std::vector<void*>*>(ctx)->push_back(cls);
    }, &classes);
    (void)k;
    for (void* cls : classes) {
        OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
            if (!obj || !R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
            const uint32_t eid = EidOf(obj);
            if (eid) static_cast<std::vector<N::BucketRow>*>(ctx)->push_back(Read(obj, eid));
        }, &rows);
    }
    if (!rows.empty()) SendRows(*s, rows, slot);
    UE_LOGI("bucket_sync: slot %d's world is up -- %zu buckets and bags sent to it", slot, rows.size());
}

void OnIntent(int slot, const coop::net::BucketIntentPayload& p) {
    void* bucket = ActorOf(p.bucket.eid);
    if (!bucket) return Refuse(slot, "the host holds no such bucket", p.bucket.eid);
    if (!InReach(slot, bucket)) return Refuse(slot, "out of the sender's reach", p.bucket.eid);
    const int t = TypeOf(bucket);
    switch (p.op) {
    case N::kBucketIntentWater: {
        if (t != N::kBucketMop) return Refuse(slot, "not a mop bucket", p.bucket.eid);
        const float maxH = Get<float>(bucket, L"MaxHeight", 0.f);
        const float level = std::fmax(0.f, std::fmin(p.bucket.level, maxH > 0.f ? maxH : p.bucket.level));
        Put<float>(bucket, L"Height", level);
        Put<int32_t>(bucket, L"soap", std::max<int32_t>(0, p.bucket.count));
        CallNoArgs(bucket, L"upd");
        MarkDirty(bucket);
        ++g_intentsRun;
        UE_LOGI("bucket_sync: slot %d's verb left bucket eid=%u at water %.1f soap %d", slot, p.bucket.eid, level,
                static_cast<int>(p.bucket.count));
        return;
    }
    case N::kBucketIntentConcrete: {
        void* bag = ActorOf(p.itemEid);
        if (t != N::kBucketMop || TypeOf(bag) != N::kBucketBag)
            return Refuse(slot, "not a bucket and a cement bag", p.bucket.eid);
        if (!InReach(slot, bag)) return Refuse(slot, "the bag is out of the sender's reach", p.bucket.eid);
        return Mix(slot, bucket, bag, p.bucket.eid);
    }
    case N::kBucketIntentTake: {
        void* fixer = ActorOf(p.itemEid);
        if (t != N::kBucketConcrete || !fixer || R::ClassOf(fixer) != Cls().fixer)
            return Refuse(slot, "not a concrete bucket and a trowel", p.bucket.eid);
        if (Get<uint8_t>(bucket, L"dry", 0)) return Refuse(slot, "the concrete is dry", p.bucket.eid);
        void* fn = R::FindDispatchFunctionCached(R::ClassOf(bucket), L"takeConcrete");
        ue_wrap::ParamFrame f(fn);
        if (!fn || !f.valid() || !ue_wrap::Call(bucket, f)) return Refuse(slot, "takeConcrete did not run", p.bucket.eid);
        Put<int32_t>(fixer, L"concrete", 5);
        CallNoArgs(fixer, L"updConcrete");
        MarkDirty(bucket);
        ++g_intentsRun;
        UE_LOGI("bucket_sync: slot %d's trowel eid=%u took from concrete bucket eid=%u", slot, p.itemEid, p.bucket.eid);
        return;
    }
    default:
        return;
    }
}

void OnState(const coop::net::BucketStatePayload& p) {
    for (int i = 0; i < p.count; ++i) ApplyRow(p.rows[i]);
}

void OnDisconnect() {
    if (g_rowsSent || g_intentsRun || g_intentsRefused || g_applied || g_asked)
        UE_LOGI("bucket_sync: session end -- host rows sent %llu, asks run %llu refused %llu; client rows applied %llu, "
                "asks sent %llu",
                static_cast<unsigned long long>(g_rowsSent), static_cast<unsigned long long>(g_intentsRun),
                static_cast<unsigned long long>(g_intentsRefused), static_cast<unsigned long long>(g_applied),
                static_cast<unsigned long long>(g_asked));
    g_known.clear();
    g_dirty.clear();
    g_post.clear();
    g_hostRow.clear();
    g_parked.clear();
    g_asks.clear();
    g_rowsSent = g_intentsRun = g_intentsRefused = g_applied = g_asked = 0;
}

}  // namespace coop::bucket_sync
