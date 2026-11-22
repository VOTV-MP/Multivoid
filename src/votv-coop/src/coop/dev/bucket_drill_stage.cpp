// coop/dev/bucket_drill_stage.cpp -- the bucket drill's stage. The host spawns buckets on a water volume and lets the game's own tick
// fill them, carries each with its own hand the moment its own fill is full and sets it down by the game's own place (the player's precision-placement mode:
// updatePrecPlace; then in one call the ghost moved to each candidate spot before the puppet, in the table's order, updatePrecPlace
// again so `cantPlacePrec` reads that place, and precisionPlacemet_drop at the first free one, which spawns the item at the
// placement ghost's transform), and spawns the items behind the puppet, clear of every candidate. No teleport, and no write of a
// bucket's state: the water is the game's.
// The host says the elements' ids once they stand; the client binds its copies to them. The host's own hold, use and drop are
// small step machines (HostUseStep, HostDropStep) the arms' preparations use.

#include "bucket_drill_detail.h"

#include "coop/dev/drill_ground.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"

#include "ue_wrap/actors/bucket.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace coop::dev::bucket_drill::detail {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace B  = ue_wrap::bucket;
namespace OI = ue_wrap::object_index;
namespace DG = coop::dev::drill_ground;
namespace HB = coop::hand_item;

namespace {

constexpr float kMopSpawnApartCm = 130.f;   // a mop bucket's body is wide: a pair spawned 70 cm apart sent one through the water and the world
float g_waterBottom = 0.f;   // the Z of the bottom of the volume the buckets were spawned over

Spec g_spec;
int g_ph = 0, g_placing = 0, g_cur = -1;   // the stage's phase, the buckets carried out so far, the one being carried (-1 none)
bool g_placed[kMaxPlains] = {};
ue_wrap::CachedObjRef g_b[kMaxPlains], g_i[kMaxItems];
std::wstring g_bKey[kMaxPlains], g_iKey[kMaxItems];
int g_calm[kMaxPlains + kMaxItems] = {};
float g_lastHeight[kMaxPlains] = {};   // each bucket's water at the stage's previous read (-1 none yet, set by StageReset)
PlaceJob g_job;

void* ClassOfProp(const wchar_t* name) {
    const std::wstring cls = std::wstring(name) + L"_C";
    if (void* const c = OI::ClassByName(cls.c_str())) return c;
    const std::wstring path = L"/Game/objects/" + std::wstring(name) + L"." + cls;
    return ue_wrap::asset_load::LoadObjectByPath(path.c_str());
}

void* Resolve(ue_wrap::CachedObjRef& ref, const std::wstring& key) {
    if (void* const a = ref.Get()) return a;
    if (void* const a = ByKey(key)) {
        ref.Set(a);
        return a;
    }
    return nullptr;
}

bool CompSet(void* comp, const wchar_t* fn, const wchar_t* param, const void* value, size_t size) {
    void* const f = comp ? R::FindDispatchFunctionCached(R::ClassOf(comp), fn) : nullptr;
    ue_wrap::ParamFrame frame(f);
    return f && frame.valid() && frame.SetRaw(param, value, static_cast<int32_t>(size)) && frame.Set<bool>(L"bSweep", false) &&
           frame.Set<bool>(L"bTeleport", true) && ue_wrap::Call(comp, frame);
}

void* PtrOf(void* object, const wchar_t* name) {
    const int32_t off = object ? R::FindPropertyOffset(R::ClassOf(object), name) : -1;
    void* p = nullptr;
    if (off >= 0) std::memcpy(&p, static_cast<const char*>(object) + off, sizeof(p));
    return p;
}

bool BoolOf(void* o, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    return o && R::FindBoolProperty(R::ClassOf(o), name, off, mask) && (static_cast<const uint8_t*>(o)[off] & mask) != 0;
}

// The nearest live water volume that is not frozen to `who`: its centre, the Z of its top and of its bottom. False when the world has none.
bool NearestWater(void* who, ue_wrap::FVector& centre, float& topZ, float& bottomZ) {
    ue_wrap::FVector me{};
    void* const cls = OI::ClassByName(L"waterVolume_C");
    if (!who || !cls || !E::TryGetActorLocation(who, me)) return false;
    struct Ctx {
        bool found = false;
        float bestD = 0.f;
        ue_wrap::FVector me, centre;
        float top = 0.f, bottom = 0.f;
    } ctx;
    ctx.me = me;
    OI::ForEachInstance(cls, [](void* c, void* obj, int32_t) {
        auto* x = static_cast<Ctx*>(c);
        if (!obj || !R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__") || BoolOf(obj, L"frozen")) return;
        ue_wrap::FVector origin{}, extent{};
        if (!E::GetActorBounds(obj, false, origin, extent)) return;
        const float d = DG::Dist(origin, x->me);
        if (x->found && d >= x->bestD) return;
        x->found = true;
        x->bestD = d;
        x->centre = origin;
        x->top = origin.Z + extent.Z;
        x->bottom = origin.Z - extent.Z;
    }, &ctx);
    centre = ctx.centre;
    topZ = ctx.top;
    bottomZ = ctx.bottom;
    return ctx.found;
}

// Each staged bucket's z and water, for a verdict that names where a bucket stood.
std::string BucketReadings() {
    std::string out;
    for (int i = 0; i < g_spec.plains; ++i) {
        void* const b = g_b[i].Get();
        ue_wrap::FVector at{};
        B::Plain p;
        char buf[128];
        if (b && E::TryGetActorLocation(b, at) && B::ReadPlain(b, p))
            std::snprintf(buf, sizeof(buf), "%sbucket %d z=%.0f height=%.1f of %.1f", i ? "; " : "", i, static_cast<double>(at.Z),
                          static_cast<double>(p.height), static_cast<double>(p.maxHeight));
        else
            std::snprintf(buf, sizeof(buf), "%sbucket %d unreadable", i ? "; " : "", i);
        out += buf;
    }
    return out;
}

// The game's own place test: updatePrecPlace(true) writes `cantPlacePrec` from the ghost's overlap when the ghost's place changed.
bool UpdatePrecPlace(void* player) {
    void* const fn = player ? R::FindDispatchFunctionCached(R::ClassOf(player), L"updatePrecPlace") : nullptr;
    ue_wrap::ParamFrame f(fn);
    return fn && f.valid() && f.Set<bool>(L"isPrecisionPlacement", true) && ue_wrap::Call(player, f);
}

// The classes of the actors the ghost's overlap box reports (the query the game's own test runs), for a blocked place's verdict.
std::wstring OverlapNames(void* overlap) {
    void* const fn = overlap ? R::FindDispatchFunctionCached(R::ClassOf(overlap), L"GetOverlappingActors") : nullptr;
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid() || !ue_wrap::Call(overlap, f)) return L"(the overlap could not be queried)";
    struct Array { void** data; int32_t num, max; } a{};
    if (!f.GetRaw(L"OverlappingActors", &a, sizeof(a))) return L"(the overlap list could not be read)";
    std::wstring out;
    for (int32_t i = 0; i < a.num && i < 4; ++i)
        out += (out.empty() ? L"" : L", ") + (a.data[i] ? R::ClassNameOf(a.data[i]) : std::wstring(L"(null)"));
    if (a.num > 4) out += L", ...";
    if (a.data) R::EngineFree(a.data);   // the engine allocated the out array
    return out.empty() ? L"(none listed at this read)" : out;
}

const wchar_t* PlainClass(int i) { return g_spec.mop && i == 0 ? L"prop_bucket_mop" : L"prop_bucket"; }

// The slot's actor, bucket slots first.
void* SlotActor(int slot) { return slot < kMaxPlains ? StageBucket(slot) : StageItem(slot - kMaxPlains); }

void* Anchor() { return g_spec.nearHost ? Local() : Puppet(); }

// One bucket's carry by the game's place; see PlaceHeld and OfferSpots. The places offered are the table's, clear of the buckets already
// set down and with a floor under them; a client that must act on the bucket walks to it (bucket_drill_walk.cpp).
// The places are worked out where PlaceHeld reads them (the ghost's moves, sub 3) and where the count is said (sub 0). 0 working, 1 placed,
// -1 stopped.
int PlaceStep(int i) {
    void* const anchor = Anchor();
    if (!anchor) {
        Late("the host's player or the client's puppet is gone");
        return 0;
    }
    Offer offer;
    if (g_job.sub == 0 || g_job.sub == 3) {
        ue_wrap::FVector placed[kMaxPlains];   // where each bucket already set down stands
        int nPlaced = 0;
        for (int j = 0; j < g_spec.plains; ++j) {
            void* const other = g_placed[j] ? g_b[j].Get() : nullptr;
            if (other && E::TryGetActorLocation(other, placed[nPlaced])) ++nPlaced;
        }
        OfferIn in;
        in.anchor = anchor;
        in.placed = placed;
        in.nPlaced = nPlaced;
        if (!OfferSpots(in, offer)) {
            Late("the host's player or the client's puppet is gone");
            return 0;
        }
        if (offer.broken) {
            Invalid(offer.broken);
            g_job = PlaceJob{};
            return -1;
        }
        if (offer.n == 0) {
            char why[512];
            std::snprintf(why, sizeof(why),
                          "no place for bucket %d is offered: of %d candidates, %d lie within %.0f cm of a bucket already set down and %d have no "
                          "floor under the place",
                          i, kSpotCount, offer.crowded, static_cast<double>(kClearOfBucketCm), offer.noFloor);
            Invalid(why);
            g_job = PlaceJob{};
            return -1;
        }
    }
    if (g_job.sub == 0) {
        g_job.key = g_bKey[i];
        Fact("%d of %d candidate places are offered for bucket %d (%d within %.0f cm of a bucket set down, %d with no floor under the place)",
             offer.n, kSpotCount, i, offer.crowded, static_cast<double>(kClearOfBucketCm), offer.noFloor);
    }
    const int r = PlaceHeld(g_job, StageBucket(i), offer.spots, offer.n);
    if (r > 0) {
        g_b[i].Set(g_job.placed);
        g_job = PlaceJob{};
    } else if (r < 0) {
        g_job = PlaceJob{};
    }
    return r;
}

// What stands within kClearOfBucketCm (level) of `at` among the staged buckets and items and the two players, nearest first, for a
// verdict that names what a spawn overlapped.
std::string NeighbourOf(void* self, const ue_wrap::FVector& at) {
    struct Near { std::string name; float cm; };
    std::vector<Near> found;
    const auto consider = [&](void* a, const std::string& name) {
        ue_wrap::FVector p{};
        if (!a || a == self || !E::TryGetActorLocation(a, p)) return;
        const float cm = std::hypot(p.X - at.X, p.Y - at.Y);
        if (cm < kClearOfBucketCm) found.push_back({name, cm});
    };
    for (int j = 0; j < g_spec.plains; ++j)
        if (g_placed[j]) consider(g_b[j].Get(), "bucket " + std::to_string(j));
    for (int j = 0; j < g_spec.nItems; ++j) consider(g_i[j].Get(), "item " + std::to_string(j));
    consider(Local(), "the host's player");
    consider(Puppet(), "the client's puppet");
    std::sort(found.begin(), found.end(), [](const Near& a, const Near& b) { return a.cm < b.cm; });
    std::string out;
    char buf[64];
    for (const Near& n : found) {
        std::snprintf(buf, sizeof(buf), " (%.0f cm)", static_cast<double>(n.cm));
        out += (out.empty() ? "" : ", ") + n.name + buf;
    }
    return out.empty() ? "no staged prop or player within the clear distance" : out;
}

}  // namespace

// Hold, enter the place mode, then in one call set the ghost at each of `spots` in turn, read the game's place test there and drop at
// the first free one; then the new actor under the key stands at rest. `item` is read at the first call.
int PlaceHeld(PlaceJob& j, void* item, const ue_wrap::FVector* spots, int nSpots) {
    void* const me = Local();
    if (!me) {
        Late("the host's player is gone");
        return 0;
    }
    switch (j.sub) {
    case 0:
        if (!item) {
            Invalid("a prop to carry is gone");
            return -1;
        }
        j.old = item;
        if (j.key.empty()) j.key = KeyOf(item);
        if (!Hold(me, item)) {
            Invalid("Hold Object did not run");
            return -1;
        }
        j.sub = 1;
        Go();
        return 0;
    case 1:
        if (!HandReady()) {
            Late("the hand never held the prop");
            return 0;
        }
        j.sub = 2;
        Go();
        return 0;
    case 2:
        if (!UpdatePrecPlace(me) || !PtrOf(me, L"precMesh")) {
            Invalid("the game's place mode could not be entered");
            return -1;
        }
        j.sub = 3;
        Go();
        return 0;
    case 3: {   // one call from the first move to the drop: the game's own tick re-aims the ghost at the player's view every frame
        void* const ghost = PtrOf(me, L"precMesh");
        if (!ghost) {
            Invalid("the placement ghost could not be read");
            return -1;
        }
        if (!CompSet(ghost, L"K2_SetWorldRotation", L"NewRotation", &j.rot, sizeof(j.rot))) {
            Invalid("the placement ghost could not be turned");
            return -1;
        }
        int blocked = 0, chosen = -1;
        std::string seen;   // each blocked place's overlap classes, for the verdict when none is free
        for (int k = 0; k < nSpots && chosen < 0; ++k) {
            const ue_wrap::FVector& at = spots[k];
            if (!CompSet(ghost, L"K2_SetWorldLocation", L"NewLocation", &at, sizeof(at))) {
                Invalid("the placement ghost could not be moved");
                return -1;
            }
            if (!UpdatePrecPlace(me)) {   // the game's own place test at the moved ghost
                Invalid("the game's place test could not be run at the ghost's place");
                return -1;
            }
            if (!BoolOf(me, L"cantPlacePrec")) {
                chosen = k;
                break;
            }
            const std::wstring overlap = OverlapNames(PtrOf(me, L"precMesh_overlap"));
            char line[320];
            std::snprintf(line, sizeof(line), "place %d of %d at (%.0f, %.0f, %.0f) is blocked (cantPlacePrec); its overlap holds: %ls", k + 1, nSpots,
                          static_cast<double>(at.X), static_cast<double>(at.Y), static_cast<double>(at.Z), overlap.c_str());
            Fact("%s", line);
            std::snprintf(line, sizeof(line), "%s%d: %ls", seen.empty() ? "" : "; ", k + 1, overlap.c_str());
            seen += line;
            ++blocked;
        }
        if (chosen < 0) {   // a blocked place is said at once: its drop would only show a hint and keep the hand
            if (seen.size() > 300) seen.resize(300);
            char why[512];
            std::snprintf(why, sizeof(why), "none of the %d candidate places is free for the game's place (cantPlacePrec); overlaps by place: %s", nSpots,
                          seen.empty() ? "(no candidate was offered)" : seen.c_str());
            Invalid(why);
            return -1;
        }
        Fact("the place chosen is candidate %d of %d at (%.0f, %.0f, %.0f), %d blocked before it", chosen + 1, nSpots,
             static_cast<double>(spots[chosen].X), static_cast<double>(spots[chosen].Y), static_cast<double>(spots[chosen].Z), blocked);
        j.at = spots[chosen];
        void* const fn = R::FindDispatchFunctionCached(R::ClassOf(me), L"precisionPlacemet_drop");
        ue_wrap::ParamFrame f(fn);
        const bool placed = fn && f.valid() && ue_wrap::Call(me, f);
        Fact("precisionPlacemet_drop ran=%d", placed ? 1 : 0);
        if (!placed) {
            Invalid("the game's place verb could not be called");
            return -1;
        }
        j.sub = 4;
        Go();
        return 0;
    }
    default: {
        void* const b = ByKey(j.key);
        if (!b || b == j.old || !HandEmpty()) {
            j.calm = 0;
            Late("the placed prop never stood under its key with the hand empty");
            return 0;
        }
        ue_wrap::FVector now{};
        if (E::TryGetActorLocation(b, now) && std::hypot(now.X - j.at.X, now.Y - j.at.Y) > kShovedCm) {
            char why[400];
            std::snprintf(why, sizeof(why),
                          "the placed prop was pushed %.0f cm from its place at (%.0f, %.0f) as it spawned; beside the place: %s",
                          static_cast<double>(std::hypot(now.X - j.at.X, now.Y - j.at.Y)), static_cast<double>(j.at.X),
                          static_cast<double>(j.at.Y), NeighbourOf(b, j.at).c_str());
            Invalid(why);
            return -1;
        }
        if (!Calm(b, j.calm)) {
            Late("the placed prop never stood under its key calm with the hand empty");
            return 0;
        }
        j.placed = b;
        return 1;
    }
    }
}

void* SpawnAt(const wchar_t* name, float x, float y, float z) {
    void* const c = ClassOfProp(name);
    return c ? E::SpawnActor(c, {x, y, z}) : nullptr;
}

void* SpawnBefore(const wchar_t* name, void* who, float fwdCm, float rightCm) {
    ue_wrap::FVector at{};
    if (!who || !E::TryGetActorLocation(who, at)) return nullptr;
    const ue_wrap::FVector f = E::GetActorForwardVector(who);
    return SpawnAt(name, at.X + f.X * fwdCm - f.Y * rightCm, at.Y + f.Y * fwdCm + f.X * rightCm, at.Z);
}

void StageBegin(const Spec& spec) {
    StageReset();
    g_spec = spec;
}

void StageReset() {
    g_spec = Spec{};
    g_ph = g_placing = 0;
    g_cur = -1;
    for (bool& p : g_placed) p = false;
    for (auto& r : g_b) r.Reset();
    for (auto& r : g_i) r.Reset();
    for (auto& k : g_bKey) k.clear();
    for (auto& k : g_iKey) k.clear();
    for (int& c : g_calm) c = 0;
    for (float& h : g_lastHeight) h = -1.f;
    g_job = PlaceJob{};
}

void StageSetPlains(int n) {
    for (int i = n; i < kMaxPlains; ++i) {
        g_b[i].Reset();
        g_bKey[i].clear();
    }
    g_spec.plains = n;
}

void* StageBucket(int i) { return i >= 0 && i < kMaxPlains ? Resolve(g_b[i], g_bKey[i]) : nullptr; }
void* StageItem(int i) { return i >= 0 && i < kMaxItems ? Resolve(g_i[i], g_iKey[i]) : nullptr; }
std::wstring StageBucketKey(int i) { return g_bKey[i]; }
std::wstring StageItemKey(int i) { return g_iKey[i]; }

bool StageStep(coop::net::Session& s) {
    switch (g_ph) {
    case 0: {   // the places the stage needs: the client's puppet joined, or the host's own player
        void* const anchor = Anchor();
        if (!g_spec.nearHost && !HostMayStart(s)) return false;
        if (!anchor || !Local()) {
            Late("the client's puppet or the host's player never stood");
            return false;
        }
        ue_wrap::FVector water{};
        float top = 0.f;
        if (!NearestWater(anchor, water, top, g_waterBottom)) {
            Invalid("the world has no water volume that is not frozen: no bucket can be filled by the game's tick");
            return false;
        }
        for (int i = 0; i < g_spec.nItems; ++i) {   // an item beside a candidate place would block the game's place there
            const float side = (static_cast<float>(i) - 1.f) * g_spec.itemSpreadCm;
            if (!ItemClearOfSpots(g_spec.itemFwdCm, side)) {
                char why[160];
                std::snprintf(why, sizeof(why), "the stage's spec stands an item within %.0f cm of a place a bucket may be set down",
                              static_cast<double>(kClearOfBucketCm));
                Invalid(why);
                return false;
            }
        }
        const float spawnApart = g_spec.mop ? kMopSpawnApartCm : kClearOfBucketCm;   // two plain buckets touch at 83-92 cm
        for (int i = 0; i < g_spec.plains; ++i) {
            void* const b = SpawnAt(PlainClass(i), water.X + spawnApart * static_cast<float>(i), water.Y, top + 30.f);
            if (!b) {
                Invalid("a bucket class did not resolve or the world refused a spawn");
                return false;
            }
            g_b[i].Set(b);
        }
        for (int i = 0; i < g_spec.nItems; ++i) {
            void* const it = SpawnBefore(g_spec.items[i], anchor, g_spec.itemFwdCm, (static_cast<float>(i) - 1.f) * g_spec.itemSpreadCm);
            if (!it) {
                Invalid("an item class did not resolve or the world refused a spawn");
                return false;
            }
            g_i[i].Set(it);
        }
        Fact("a water volume's top is at z=%.0f; %d buckets are spawned over it for the game's tick to fill, %d items before the anchor",
             top, g_spec.plains, g_spec.nItems);
        g_ph = 1;
        Go();
        return false;
    }
    case 1: {   // a bucket is carried out of the water the moment its own fill is full: nothing stays in the volume to sink
        for (int i = 0; i < g_spec.plains; ++i) {   // every bucket still on the water first: one that fell is said at once
            if (g_placed[i] || i == g_cur) continue;
            void* const b = g_b[i].Get();
            ue_wrap::FVector at{};
            if (!b) {
                Invalid("a bucket on the water died before it was carried out");
                return false;
            }
            if (E::TryGetActorLocation(b, at) && at.Z < g_waterBottom) {
                const std::string r = BucketReadings();
                Fact("a bucket fell below the water volume's bottom (z=%.0f): %s", static_cast<double>(g_waterBottom), r.c_str());
                Invalid("a bucket on the water fell through it (below the volume's bottom) before the stage carried it out");
                return false;
            }
        }
        if (g_cur < 0) {   // the next bucket to carry: the first whose fill has stopped rising above half and that has a key and an element id
            for (int i = 0; i < g_spec.plains && g_cur < 0; ++i) {
                void* const b = g_placed[i] ? nullptr : g_b[i].Get();
                B::Plain p;
                if (!b) continue;
                if (g_bKey[i].empty()) g_bKey[i] = KeyOf(b);
                if (!B::ReadPlain(b, p)) continue;
                // A mop bucket comes to rest above the volume's top and part-fills, so "full" is not the event: the fill has stopped
                // when two reads of the tick agree, and the bar is the BP's own checkAlc one, half the bucket's maximum.
                const bool stopped = p.height == g_lastHeight[i];
                g_lastHeight[i] = p.height;
                if (stopped && !g_bKey[i].empty() && EidOf(b) != 0 && p.height > p.maxHeight / 2.f) {
                    g_cur = i;
                    Fact("bucket %d's fill stopped rising at %.2f of %.2f cm", i, static_cast<double>(p.height), static_cast<double>(p.maxHeight));
                }
            }
            if (g_cur < 0) {
                if (Late("the game's tick never filled a bucket on the water past half and stopped, or a bucket never got a key and an element id"))
                    Fact("at the bound: %s", BucketReadings().c_str());
                return false;
            }
        }
        const int r = PlaceStep(g_cur);
        if (r < 0) return false;
        if (r > 0) {
            Fact("bucket %d stands at rest after the game's place", g_cur);
            g_placed[g_cur] = true;
            g_cur = -1;
            ++g_placing;
            if (g_placing >= g_spec.plains) g_ph = 2;
            Go();
        }
        return false;
    }
    case 2: {   // every item has a key and an element id and stands at rest
        for (int i = 0; i < g_spec.nItems; ++i) {
            void* const it = g_i[i].Get();
            if (!it) {
                Invalid("an item died before the stage was set");
                return false;
            }
            if (g_iKey[i].empty()) g_iKey[i] = KeyOf(it);
            if (g_iKey[i].empty() || EidOf(it) == 0 || !Calm(it, g_calm[kMaxPlains + i])) {
                Late("an item never got a key and an element id and came to rest");
                return false;
            }
        }
        g_ph = 3;
        Go();
        return true;
    }
    default:
        return true;
    }
}

bool StageSay() {
    std::string line = "stage " + Ints({g_spec.plains, g_spec.nItems});
    for (int slot = 0; slot < kMaxPlains + kMaxItems; ++slot) {
        const bool used = slot < kMaxPlains ? slot < g_spec.plains : slot - kMaxPlains < g_spec.nItems;
        if (!used) continue;
        void* const a = SlotActor(slot);
        const int eid = a ? EidOf(a) : 0;
        if (!eid || !Calm(a, g_calm[slot])) {
            Late("a staged prop never had an element id and came to rest");
            return false;
        }
        line += " " + std::to_string(eid);
    }
    Say(line);
    return true;
}

// ---- the host's own hold, use and drop ----------------------------------------------------------------------------------------

int HostUseStep(HostUse& j) {
    void* const me = Local();
    void* const item = j.item.Get();
    void* const target = j.target.Get();
    switch (j.ph) {
    case 0:
        if (!me || !item || !target) {
            Invalid("the host's item or its target is gone");
            return -1;
        }
        if (!Hold(me, item)) {
            Invalid("Hold Object did not run on the host");
            return -1;
        }
        j.ph = 1;
        Go();
        return 0;
    case 1: {
        if (!HandReady()) {
            Late("the host's hand never held the item");
            return 0;
        }
        if (!target) {
            Invalid("the host's target is gone");
            return -1;
        }
        void* const hand = HB::LocalHandActor();
        void* const hitBefore = j.soap ? nullptr : E::ReadMainPlayerHitActor(me);
        B::Plain tp;
        const bool plain = B::ReadPlain(target, tp);
        const bool ran = j.soap ? UseOn(me, target) : HandUse(me, hand, target, j.rmb);
        if (!j.soap) {   // HandUse writes the hit on the target, as the client's click does; the readings say what the click saw
            void* const hitAfter = E::ReadMainPlayerHitActor(me);
            Fact("the host's use: the target reads %.2f of %.2f cm, canConcrete=%d; the player's own trace named %ls, the written hit names "
                 "%ls (the target is %ls)",
                 plain ? static_cast<double>(tp.height) : -1.0, plain ? static_cast<double>(tp.maxHeight) : -1.0, plain && tp.canConcrete ? 1 : 0,
                 hitBefore ? R::ClassNameOf(hitBefore).c_str() : L"(none)", hitAfter ? R::ClassNameOf(hitAfter).c_str() : L"(none)",
                 R::ClassNameOf(target).c_str());
        }
        Fact("the host's own use ran=%d", ran ? 1 : 0);
        if (!ran) {
            Invalid("the host's own use of its held item did not run");
            return -1;
        }
        j.ph = 2;
        return 1;
    }
    default:
        return 1;
    }
}

bool HostDropStep(int& ph) {
    if (ph == 0) {
        if (HandEmpty()) return true;
        if (!DropHeld(Local())) {
            Invalid("the host's drop did not run");
            return false;
        }
        ph = 1;
        Go();
        return false;
    }
    if (HandEmpty()) {
        ph = 0;
        return true;
    }
    Late("the host's hand never emptied after its drop");
    return false;
}

// ---- the client's copies ----------------------------------------------------------------------------------------------------

namespace {

int g_cEid[kMaxPlains + kMaxItems] = {};
int g_cCalm[kMaxPlains + kMaxItems] = {};
int g_cPh = 0, g_cPlains = 0, g_cItems = 0;

}  // namespace

bool ClientBindStage(int nPlains, int nItems) {
    if (g_cPh == 0) {
        if (!ClientMayStart()) return false;
        std::string rest;
        if (!Rest("stage ", &rest)) {
            Late("the host's stage line never came");
            return false;
        }
        int v[2 + kMaxPlains + kMaxItems] = {};
        if (!ParseInts(rest, 2 + nPlains + nItems, v) || v[0] != nPlains || v[1] != nItems) {
            Invalid("the host's stage line could not be read");
            return false;
        }
        g_cPlains = nPlains;
        g_cItems = nItems;
        for (int i = 0; i < nPlains; ++i) g_cEid[i] = v[2 + i];
        for (int i = 0; i < nItems; ++i) g_cEid[kMaxPlains + i] = v[2 + nPlains + i];
        g_cPh = 1;
        Go();
        return false;
    }
    if (g_cPh == 1) {
        for (int slot = 0; slot < kMaxPlains + kMaxItems; ++slot) {
            const bool used = slot < kMaxPlains ? slot < g_cPlains : slot - kMaxPlains < g_cItems;
            if (!used) continue;
            void* const a = LiveProp(g_cEid[slot]);
            if (!a || !Calm(a, g_cCalm[slot])) {
                Late("this client's copy of a staged prop never stood here at rest");
                return false;
            }
        }
        g_cPh = 2;
        Go();
    }
    return true;
}

int ClientEid(int slot) { return slot >= 0 && slot < kMaxPlains + kMaxItems ? g_cEid[slot] : 0; }
void* ClientBucket(int i) { return LiveProp(ClientEid(i)); }
void* ClientItem(int i) { return LiveProp(ClientEid(kMaxPlains + i)); }
void ClientRebind(int slot, int eid) {
    if (slot >= 0 && slot < kMaxPlains + kMaxItems) {
        g_cEid[slot] = eid;
        g_cCalm[slot] = 0;
    }
}

void ClientReset() {
    for (int& e : g_cEid) e = 0;
    for (int& c : g_cCalm) c = 0;
    g_cPh = g_cPlains = g_cItems = 0;
}

}  // namespace coop::dev::bucket_drill::detail
