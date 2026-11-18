// coop/props/bucket_sync.cpp -- see coop/props/bucket_sync.h.

#include "coop/props/bucket_sync.h"

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

constexpr uint64_t kPollMs = 250;        // the host reads its buckets four times a second
constexpr uint64_t kDiscoverMs = 1000;   // and looks for new ones once a second
constexpr uint64_t kParkMs = 10000;      // a row for a bucket not bound here yet waits this long
constexpr float    kLevelEps = 0.5f;     // a change of water worth a row
constexpr float    kAlcEps = 0.05f;      // of brewing
constexpr float    kDryEps = 30.f;       // of the drying clock, seconds
constexpr float    kReachCm = 600.f;     // a client's ask names a bucket and an item this near its player

std::atomic<N::Session*> g_session{nullptr};

uint64_t Now() { return ::GetTickCount64(); }

// The four classes, by name, per world: a Blueprint class is a new object in every world.
struct Classes {
    void* mop = nullptr;
    void* concrete = nullptr;
    void* bag = nullptr;
    void* fixer = nullptr;
    uint32_t gen = 0;
};
Classes g_cls;
const Classes& Cls() {
    const uint32_t gen = ue_wrap::world_identity::Generation();
    if (gen != g_cls.gen || !g_cls.mop) {
        g_cls.gen = gen;
        g_cls.mop = OI::ClassByName(L"prop_bucket_C");
        g_cls.concrete = OI::ClassByName(L"prop_concreteBucket_C");
        g_cls.bag = OI::ClassByName(L"prop_cementBag_C");
        g_cls.fixer = OI::ClassByName(L"prop_wallfixer_C");
    }
    return g_cls;
}

int TypeOf(void* actor) {
    if (!actor) return -1;
    void* const c = R::ClassOf(actor);
    const Classes& k = Cls();
    if (c && c == k.mop) return N::kBucketMop;
    if (c && c == k.concrete) return N::kBucketConcrete;
    if (c && c == k.bag) return N::kBucketBag;
    return -1;
}

template <class T>
T Get(void* obj, const wchar_t* name, T dflt) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    return off >= 0 ? *reinterpret_cast<const T*>(static_cast<const char*>(obj) + off) : dflt;
}
template <class T>
void Put(void* obj, const wchar_t* name, T v) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
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

// ---------------------------------------------------------------- HOST

struct Watched {
    void* actor = nullptr;
    N::BucketRow sent{};
    bool everSent = false;
};
std::unordered_map<uint32_t, Watched> g_watched;
uint64_t g_pollAt = 0, g_discoverAt = 0;
uint64_t g_rowsSent = 0, g_intentsRun = 0, g_intentsRefused = 0;

void Discover() {
    std::unordered_map<uint32_t, Watched> next;
    const Classes& k = Cls();
    for (void* cls : {k.mop, k.concrete, k.bag}) {
        if (!cls) continue;
        OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
            auto* out = static_cast<std::unordered_map<uint32_t, Watched>*>(ctx);
            if (!obj || !R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
            const uint32_t eid = EidOf(obj);
            if (!eid) return;
            Watched w;
            w.actor = obj;
            auto it = g_watched.find(eid);
            if (it != g_watched.end() && it->second.actor == obj) w = it->second;
            (*out)[eid] = w;
        }, &next);
    }
    g_watched.swap(next);
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

void HostTick(N::Session& s) {
    const uint64_t now = Now();
    if (now >= g_discoverAt) {
        g_discoverAt = now + kDiscoverMs;
        Discover();
    }
    if (now < g_pollAt) return;
    g_pollAt = now + kPollMs;
    if (!s.AnyWorldReadyPeer()) return;
    std::vector<N::BucketRow> rows;
    for (auto& [eid, w] : g_watched) {
        if (!R::IsLive(w.actor)) continue;
        const N::BucketRow r = Read(w.actor, eid);
        if (r.type == 0xFF) continue;
        if (w.everSent && !Changed(w.sent, r)) continue;
        if (!w.everSent && r.type == N::kBucketBag) {   // a bag is told when its units move, not when first seen
            w.sent = r;
            w.everSent = true;
            continue;
        }
        w.sent = r;
        w.everSent = true;
        rows.push_back(r);
        if (r.type == N::kBucketBag)
            UE_LOGI("bucket_sync: cement bag eid=%u now %d units -- told", eid, static_cast<int>(r.count));
    }
    if (!rows.empty()) SendRows(s, rows, -1);
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

void SendRows(N::Session& s, const std::vector<N::BucketRow>& rows, int slot);

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
    if (!c.object || !IsClient() || TypeOf(c.object) != N::kBucketMop) return;
    g_post[c.object] = Read(c.object, EidOf(c.object));
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
        const bool b = sg::WatchName(kRowHandUse, kTag, &OnHandPre, nullptr);
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
    SettleWatches();
    if (s->role() == N::Role::Host) HostTick(*s);
    else ClientTick(*s);
}

void OnPeerWorldReady(int slot) {
    N::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != N::Role::Host) return;
    Discover();
    std::vector<N::BucketRow> rows;
    for (auto& [eid, w] : g_watched) {
        if (!R::IsLive(w.actor)) continue;
        const N::BucketRow r = Read(w.actor, eid);
        if (r.type != 0xFF) rows.push_back(r);
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
    g_watched.clear();
    g_post.clear();
    g_hostRow.clear();
    g_parked.clear();
    g_asks.clear();
    g_rowsSent = g_intentsRun = g_intentsRefused = g_applied = g_asked = 0;
    g_pollAt = g_discoverAt = 0;
}

}  // namespace coop::bucket_sync
