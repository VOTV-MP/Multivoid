// coop/props/bucket_sync.cpp -- see coop/props/bucket_sync.h.

#include "coop/props/bucket_sync.h"

#include "bucket_internal.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/death_seam.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/props/bucket_hold_ledger.h"
#include "coop/dev/perf_probe.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_save_data.h"
#include "ue_wrap/actors/bucket.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::bucket_sync {
namespace {

namespace N  = coop::net;
namespace R  = ue_wrap::reflection;
namespace B  = ue_wrap::bucket;
namespace OI = ue_wrap::object_index;
namespace sg = ue_wrap::script_gate;

// The cement bag's own transfer on the host (its player's): both bags it touched are told.
UE_FACT_FN(kRowTransfer, "bucket_sync", L"prop_cementBag_C", L"transferUnit", Declared);
constexpr int kTagTransfer = 0x424B5452;   // 'BKTR'

// Rows go on change only, coalesced: MTA relays an element's data once per change (CGame.cpp:2715-2798, Packet_CustomData),
// and there is no periodic resync, so a quiet bucket stays at its last row.
constexpr uint64_t kMinGapMs = 100;        // one element's rows at most ten a second (a tipped bucket pours every tick)
constexpr size_t   kMaxParked = 512;       // a loud backstop on rows waiting for a copy to bind; each leaves at its bind or its destroy
constexpr float    kAlcEps = 0.01f;        // a brewing climb worth a row
constexpr float    kDryEps = 5.f;          // a drying climb worth a row, seconds
// The bounds a receiver holds a row to: the classes' own maxima (a bucket's concrete 20, a bag's 50; the water against the
// copy's own maxHeight at the apply), and for soap the bound the BP lacks (a soap adds 10), against a forged row only.
constexpr float    kHeightCap = 1000.f;
constexpr int16_t  kSoapCap = 10000;
constexpr int16_t  kConcreteUnitsCap = 20;
constexpr int16_t  kBagUnitsCap = 50;
constexpr float    kDryCap = 1e6f;

std::atomic<N::Session*> g_session{nullptr};

uint64_t Now() { return ::GetTickCount64(); }

uint64_t g_counts[static_cast<size_t>(Count::kCount)] = {};
unsigned long long g_pinCalls = 0, g_pinTicks = 0;   // the pin body's cost while the perf probe is armed

void LogPinCost() {
    UE_LOGI("[BUCKET] pin cost: %llu calls, %.3f ms", g_pinCalls, coop::dev::perf_probe::TicksToMs(g_pinTicks));
}

// The red `exact`: only the plain bucket class itself is told, as the lane took it before the mop bucket was one.
bool Skipped(void* actor) {
    return dev::RedOf() == dev::Red::Exact && B::KindOf(actor) == B::Kind::Plain && R::ClassNameOf(actor) != L"prop_bucket_C";
}

uint8_t TypeOfKind(B::Kind k) {
    switch (k) {
    case B::Kind::Plain:    return N::kBucketPlain;
    case B::Kind::Concrete: return N::kBucketConcrete;
    case B::Kind::Bag:      return N::kBucketBag;
    default:                return 0xFF;
    }
}

// `actor`'s row as it stands, clamped to what a receiver accepts; false when it is none of the three or a member does not read.
bool ReadRow(void* actor, uint32_t eid, N::BucketRow& r) {
    r = N::BucketRow{};
    r.eid = eid;
    switch (B::KindOf(actor)) {
    case B::Kind::Plain: {
        B::Plain p;
        if (!B::ReadPlain(actor, p)) return false;
        r.type = N::kBucketPlain;
        r.level = (std::max)(p.height, 0.f);
        r.count = static_cast<int16_t>(std::clamp<int32_t>(p.soap, 0, kSoapCap));
        r.progress = std::clamp(p.alcoholP, 0.f, 1.f);
        r.flags = static_cast<uint8_t>((p.alcohol ? N::kBucketAlcohol : 0) | (p.alcReady ? N::kBucketAlcReady : 0));
        return true;
    }
    case B::Kind::Concrete: {
        B::Concrete c;
        if (!B::ReadConcrete(actor, c)) return false;
        r.type = N::kBucketConcrete;
        r.count = static_cast<int16_t>(std::clamp<int32_t>(c.units, 0, kConcreteUnitsCap));
        r.progress = std::clamp(c.dryTimer, 0.f, kDryCap);
        r.flags = c.dry ? N::kBucketDry : 0;
        return true;
    }
    case B::Kind::Bag: {
        int32_t units = 0, maxUnits = 0;
        if (!B::ReadBag(actor, units, maxUnits)) return false;
        r.type = N::kBucketBag;
        r.count = static_cast<int16_t>(std::clamp<int32_t>(units, 0, kBagUnitsCap));
        return true;
    }
    default:
        return false;
    }
}

// Whether `now` differs from `was` by a row's worth: bools, ints and the water exact; the brewing climb and the drying clock
// past their steps.
bool Changed(const N::BucketRow& was, const N::BucketRow& now) {
    if (was.type != now.type || was.flags != now.flags || was.count != now.count) return true;
    if (now.type == N::kBucketPlain) return was.level != now.level || std::fabs(was.progress - now.progress) > kAlcEps;
    if (now.type == N::kBucketConcrete) return std::fabs(was.progress - now.progress) > kDryEps;
    return false;
}

// A row a receiver keeps: a known type and every number in its class's range.
bool RowInRange(const N::BucketRow& r) {
    if (!std::isfinite(r.level) || !std::isfinite(r.progress)) return false;
    switch (r.type) {
    case N::kBucketPlain:
        return r.level >= 0.f && r.level <= kHeightCap && r.count >= 0 && r.count <= kSoapCap && r.progress >= 0.f &&
               r.progress <= 1.f;
    case N::kBucketConcrete:
        return r.count >= 0 && r.count <= kConcreteUnitsCap && r.progress >= 0.f && r.progress <= kDryCap;
    case N::kBucketBag:
        return r.count >= 0 && r.count <= kBagUnitsCap;
    default:
        return false;
    }
}

bool KindMatches(void* actor, uint8_t type) { return TypeOfKind(B::KindOf(actor)) == type; }

void* ActorOf(uint32_t eid) { return coop::element::LivePropActor(static_cast<coop::element::ElementId>(eid)); }

// ---------------------------------------------------------------- HOST

// What the host last told of each element it has seen, by element id: the row sent, its seq, and whether one is owed. A
// bucket's own tick marks it when its state moved past a row's worth since; a verb the host applied marks it outright. The
// tick sends the marked ones and does nothing while none is marked.
struct Known {
    ue_wrap::CachedObjRef actor;
    N::BucketRow sent{};
    uint32_t seq = 0;
    bool everSent = false;
    bool dirty = false;
    uint64_t sentAt = 0;
};
std::unordered_map<uint32_t, Known> g_known;
std::vector<uint32_t> g_dirty, g_keep;
std::vector<N::BucketRow> g_rows;
std::unordered_map<uint32_t, uint32_t> g_commitSeq;
std::unordered_map<uint32_t, uint32_t> g_joinSeq;   // the seq the last join send numbered each element's row with
std::unordered_set<uint32_t> g_warnedRefused;
std::unordered_set<std::wstring> g_warnedRefusedKeys;
bool g_peersReady = false;   // a world-ready peer was there at the tick's start

// The host's row of a copy that had no element id when a refusal owed it to a slot. The mint of an id is announced from any thread
// (prop_element_tracker), so the callback only says one happened; the next HostTick sends the rows whose copies have an id now.
struct Owed {
    int slot;
    ue_wrap::CachedObjRef actor;
};
std::vector<Owed> g_owed;
std::atomic<bool> g_minted{false};
void FlushOwed();

// Whether the author's record of this copy's drop is still in flight: its numbers are the class default's until it lands, and a
// row of them would put every peer's copy there, the author's included.
bool RecordAwaited(void* actor) {
    const std::wstring key = ue_wrap::prop::GetKeyString(actor);
    return !key.empty() && coop::prop_save_data::StillAwaited(key);
}

Known& KnownOf(uint32_t eid, void* actor) {
    Known& k = g_known[eid];
    if (!k.actor.Is(actor)) {
        k = Known{};
        k.actor.Set(actor);
    }
    return k;
}

// One batch of rows to every peer (slot < 0) or to one slot. True when the transport took it; a refusal is said once per element.
bool SendBatch(N::Session& s, const N::BucketRow* rows, size_t n, int slot) {
    N::BucketStatePayload p{};
    p.count = static_cast<uint8_t>(n);
    for (size_t i = 0; i < n; ++i) p.rows[i] = rows[i];
    const bool ok = slot < 0 ? s.SendReliable(N::ReliableKind::BucketState, &p, sizeof(p))
                             : s.SendReliableToSlot(slot, N::ReliableKind::BucketState, &p, sizeof(p));
    if (!ok) {
        for (size_t i = 0; i < n; ++i)
            if (g_warnedRefused.insert(rows[i].eid).second)
                UE_LOGW("bucket_sync: a row of element %u was refused by the transport", rows[i].eid);
    }
    return ok;
}

void HostTick(N::Session& s) {
    const uint64_t now = Now();
    g_peersReady = s.AnyWorldReadyPeer();
    if (!g_owed.empty() && g_minted.exchange(false, std::memory_order_relaxed)) FlushOwed();
    if (g_dirty.empty()) return;
    g_keep.clear();
    g_rows.clear();
    for (const uint32_t eid : g_dirty) {
        const auto it = g_known.find(eid);
        if (it == g_known.end()) continue;
        Known& k = it->second;
        void* const actor = k.actor.Get();
        if (!actor) {
            g_known.erase(it);
            continue;
        }
        if (!g_peersReady) {   // no one to tell; a joiner is sent every element at its world-ready
            k.dirty = false;
            continue;
        }
        if (k.everSent && now - k.sentAt < kMinGapMs) {   // told too lately: later, with the latest values
            g_keep.push_back(eid);
            continue;
        }
        N::BucketRow r{};
        if (!ReadRow(actor, eid, r) || Skipped(actor)) {
            k.dirty = false;
            continue;
        }
        r.seq = ++k.seq;
        k.sent = r;
        k.everSent = true;
        k.sentAt = now;
        k.dirty = false;
        g_rows.push_back(r);
    }
    g_dirty.swap(g_keep);
    for (size_t i = 0; i < g_rows.size(); i += N::kBucketRowsMax) {
        const size_t n = (std::min<size_t>)(N::kBucketRowsMax, g_rows.size() - i);
        if (SendBatch(s, g_rows.data() + i, n, -1)) {
            Note(Count::RowsSent);
            continue;
        }
        for (size_t j = 0; j < n; ++j) {   // the transport was full: the same elements are owed again next tick
            const auto it = g_known.find(g_rows[i + j].eid);
            if (it == g_known.end() || it->second.dirty) continue;
            it->second.dirty = true;
            g_dirty.push_back(it->first);
        }
    }
}

// The row of `actor` now, to one slot, numbered as the element's current row; an element no row was numbered for takes its first
// number here, and is not marked sent, since its first row still goes to every peer.
bool SendRowNow(int slot, void* actor) {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    const uint32_t eid = detail::EidOf(actor);
    N::BucketRow r{};
    if (!s || !eid || !ReadRow(actor, eid, r)) return false;
    Known& k = KnownOf(eid, actor);
    if (k.seq == 0) ++k.seq;
    r.seq = k.seq;
    return SendBatch(*s, &r, 1, slot);
}

void OnExpressed(void* /*actor*/, coop::element::ElementId /*eid*/) { g_minted.store(true, std::memory_order_relaxed); }

// An owed row ends with its copy: a dead actor, or the one of the element that ended.
void PruneOwed(uint32_t endedEid) {
    for (size_t i = 0; i < g_owed.size();) {
        void* const a = g_owed[i].actor.Get();
        if (!a || (endedEid && detail::EidOf(a) == endedEid)) g_owed.erase(g_owed.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
}

void FlushOwed() {
    for (size_t i = 0; i < g_owed.size();) {
        void* const a = g_owed[i].actor.Get();
        if (a && !detail::EidOf(a)) {
            ++i;
            continue;
        }
        if (a && !SendRowNow(g_owed[i].slot, a) && detail::FirstRowRefusal(ue_wrap::prop::GetKeyString(a)))
            UE_LOGW("bucket_sync: the host's row owed to slot %d was not sent at the copy's mint (the row did not read, or the transport "
                    "refused it)", g_owed[i].slot);
        g_owed.erase(g_owed.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

// ---------------------------------------------------------------- CLIENT

struct Parked {
    N::BucketRow row;
    uint64_t at;
};
std::unordered_map<uint32_t, Parked> g_parked;
std::unordered_map<uint32_t, N::BucketRow> g_hostRow;     // the last row applied to each element's copy
std::unordered_map<uint32_t, uint32_t> g_appliedSeq;
std::unordered_map<uint32_t, uint32_t> g_firstSeq;        // the seq of the first row applied to each element's copy
bool g_parkFullSaid = false, g_rangeSaid = false;

// Why OnStateWire refuses a message; each cause is said once per sender.
enum class WireCause : size_t { Direction, Length, BadCount, Pad, Row, kCount };
bool g_wireSaid[N::kMaxPeers][static_cast<size_t>(WireCause::kCount)] = {};

// Puts the row on the copy: its members written through ue_wrap::bucket, then the game's own repaint of what they show. False
// when the copy's own maxima say the row is out of range.
bool Apply(void* actor, const N::BucketRow& r) {
    switch (r.type) {
    case N::kBucketPlain: {
        B::Plain p;
        if (!B::ReadPlain(actor, p) || r.level > p.maxHeight + 0.01f) return false;
        B::WritePlain(actor, r.level, r.count, r.progress, (r.flags & N::kBucketAlcohol) != 0,
                      (r.flags & N::kBucketAlcReady) != 0);
        B::CallUpd(actor);
        return true;
    }
    case N::kBucketConcrete:
        B::WriteConcrete(actor, r.count, r.progress, (r.flags & N::kBucketDry) != 0);
        B::CallUpdStage(actor);
        B::CallUpdDry(actor);
        return true;
    case N::kBucketBag: {
        int32_t units = 0, maxUnits = 0;
        if (!B::ReadBag(actor, units, maxUnits) || r.count > maxUnits) return false;
        B::WriteBagUnits(actor, r.count);
        return true;
    }
    default:
        return false;
    }
}

void ApplyRow(void* actor, const N::BucketRow& r) {
    if (!Apply(actor, r)) {
        Note(Count::RowsRefused);
        if (!g_rangeSaid) {
            g_rangeSaid = true;
            UE_LOGW("bucket_sync: a row of element %u is past its copy's own range -- not applied (said once)", r.eid);
        }
        return;
    }
    g_parked.erase(r.eid);   // a row applied now is newer than one that waited for the copy's bind
    g_hostRow[r.eid] = r;
    g_appliedSeq[r.eid] = r.seq;
    g_firstSeq.try_emplace(r.eid, r.seq);
    Note(Count::RowsApplied);
}

void Park(const N::BucketRow& r) {
    if (g_parked.size() >= kMaxParked && !g_parked.count(r.eid)) {   // the oldest waits no longer
        auto oldest = g_parked.begin();
        for (auto it = g_parked.begin(); it != g_parked.end(); ++it)
            if (it->second.at < oldest->second.at) oldest = it;
        g_parked.erase(oldest);
        if (!g_parkFullSaid) {
            g_parkFullSaid = true;
            UE_LOGW("bucket_sync: %zu rows wait for a copy to bind -- the oldest was dropped (said once)", kMaxParked);
        }
    }
    g_parked[r.eid] = Parked{r, Now()};
}

// Everything either role keeps of one element ends with it.
void EndElement(uint32_t eid) {
    g_known.erase(eid);
    g_commitSeq.erase(eid);
    g_joinSeq.erase(eid);
    g_warnedRefused.erase(eid);
    g_parked.erase(eid);
    g_hostRow.erase(eid);
    g_appliedSeq.erase(eid);
    g_firstSeq.erase(eid);
    PruneOwed(eid);
}

// A prop element's end of play (a bound copy's destroy or stream-out): its state ends with it.
void OnPropEnd(const coop::element::death_seam::Death& death) { EndElement(static_cast<uint32_t>(death.eid)); }

bool g_deathSubscribed = false, g_deathWarned = false;
int g_deathRetry = 0;

// The engine's own announcement of an element's end, so no state is swept on a clock. A refused subscription is said once and
// retried on every 120th call.
void SubscribeDeaths() {
    if (g_deathSubscribed) return;
    if (g_deathRetry > 0) {
        --g_deathRetry;
        return;
    }
    if (coop::element::death_seam::Install() &&
        coop::element::death_seam::Subscribe(coop::element::ElementType::Prop, &OnPropEnd)) {
        g_deathSubscribed = true;
        return;
    }
    if (!g_deathWarned) {
        g_deathWarned = true;
        UE_LOGW("bucket_sync: the death seam took no subscription for props -- retried");
    }
    g_deathRetry = 120;
}

// A level change announces no element's end, so the state of the old world's elements goes with the world.
void FollowWorld() {
    static uint32_t sWorld = 0;
    const uint32_t gen = ue_wrap::world_identity::Generation();
    if (gen == sWorld) return;
    sWorld = gen;
    g_known.clear();
    g_dirty.clear();
    g_keep.clear();
    g_commitSeq.clear();
    g_joinSeq.clear();
    g_parked.clear();
    g_hostRow.clear();
    g_appliedSeq.clear();
    g_firstSeq.clear();
    g_warnedRefusedKeys.clear();
    g_owed.clear();
}

bool g_watchRegistered = false, g_watchSettled = false;

// The host's own cement bag ran its transfer: the bags it touched are told.
void OnTransferPost(const sg::Call& c) {
    static R::ParamOffset sFrom{L"from"}, sTo{L"to"};
    if (!detail::IsHost() || !c.object || !c.locals) return;
    for (R::ParamOffset* p : {&sFrom, &sTo}) {
        const int32_t off = p->Of(c.function);
        void* bag = nullptr;
        if (off >= 0) std::memcpy(&bag, c.locals + off, sizeof(bag));
        if (B::KindOf(bag) == B::Kind::Bag) MarkDirty(bag);
    }
}

void SettleWatches() {
    if (g_watchSettled) return;
    if (!g_watchRegistered) {
        g_watchRegistered = true;
        if (!sg::WatchName(kRowTransfer, kTagTransfer, nullptr, &OnTransferPost)) {
            g_watchSettled = true;
            UE_LOGE("bucket_sync: the gate took no watch on %ls -- the host's own bag transfer is not told", kRowTransfer.Name());
            return;
        }
    }
    sg::ResolvePendingNames();
    if (!sg::NameWatchSettled(kRowTransfer, kTagTransfer)) return;
    g_watchSettled = true;
    if (sg::NameWatchLive(kRowTransfer, kTagTransfer)) UE_LOGI("bucket_sync: the watch on %ls is live", kRowTransfer.Name());
    else UE_LOGE("bucket_sync: the watch on %ls settled dead -- the host's own bag transfer is not told", kRowTransfer.Name());
}

}  // namespace

namespace detail {

bool IsHost() {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->role() == N::Role::Host;
}

bool IsClient() {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->role() != N::Role::Host;
}

uint32_t EidOf(void* actor) {
    const coop::element::ElementId e = coop::element::Registry::Get().EidForActor(actor);
    return e == coop::element::kInvalidId ? 0u : static_cast<uint32_t>(e);
}

void NoteHostTick(void* actor) {
    if (!g_peersReady) return;   // no one to tell: a joiner is sent every element at its world-ready
    const uint32_t eid = EidOf(actor);
    if (!eid) return;   // a held copy or one not bound yet: its next tick tells it
    Known& k = KnownOf(eid, actor);
    if (k.dirty) return;
    if (!k.everSent && RecordAwaited(actor)) return;   // its first row goes out once the author's record has landed
    N::BucketRow now{};
    if (!ReadRow(actor, eid, now)) return;
    if (k.everSent && !Changed(k.sent, now)) return;
    k.dirty = true;
    g_dirty.push_back(eid);
}

bool FirstRowRefusal(const std::wstring& key) { return g_warnedRefusedKeys.insert(key).second; }

void OweRow(int slot, void* actor) {
    PruneOwed(0);
    g_owed.push_back(Owed{slot, {}});
    g_owed.back().actor.Set(actor);
}

void NotePinCost(unsigned long long ticks) {
    ++g_pinCalls;
    g_pinTicks += ticks;
}

const N::BucketRow* HostRow(uint32_t eid) {
    const auto it = g_hostRow.find(eid);
    return it == g_hostRow.end() ? nullptr : &it->second;
}

}  // namespace detail

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    detail::RegisterRecordGate();
    coop::bucket_hold::Install();
    // The host's mint of an element id wakes the rows owed to a slot whose refused record named a copy with none.
    static bool sFullSaid = false;
    if (coop::prop_element_tracker::SubscribeHostPropExpressed(&OnExpressed) || sFullSaid) return;
    sFullSaid = true;
    UE_LOGW("bucket_sync: the host's prop-birth table is full -- a refused record's row is not sent at its copy's mint");
}

void Tick() {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    SettleWatches();
    detail::SettlePinWatches();
    SubscribeDeaths();
    FollowWorld();
    if (s->role() == N::Role::Host) HostTick(*s);
}

uint32_t MarkDirty(void* actor) {
    if (!detail::IsHost() || B::KindOf(actor) == B::Kind::None) return 0;
    const uint32_t eid = detail::EidOf(actor);
    if (!eid) return 0;
    Known& k = KnownOf(eid, actor);
    if (!k.dirty) {
        k.dirty = true;
        g_dirty.push_back(eid);
    }
    g_commitSeq[eid] = k.seq + 1;
    return k.seq + 1;
}

bool SendRowTo(int slot, void* actor) {
    if (!detail::IsHost() || B::KindOf(actor) == B::Kind::None) return false;
    return SendRowNow(slot, actor);
}

void OnPeerWorldReady(int slot) {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != N::Role::Host) return;
    std::vector<N::BucketRow> rows;
    {
        // The instance index lists each class by itself: every loaded class of the three kinds, subclasses included.
        std::vector<void*> classes;
        OI::ForEachClass([](void* ctx, void* cls, void*) {
            const B::Kind k = B::KindOfClass(cls);
            if (k == B::Kind::Plain || k == B::Kind::Concrete || k == B::Kind::Bag)
                static_cast<std::vector<void*>*>(ctx)->push_back(cls);
        }, &classes);
        for (void* cls : classes) {
            OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
                if (!obj || !R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__") || Skipped(obj)) return;
                const uint32_t eid = detail::EidOf(obj);
                N::BucketRow r{};
                if (!eid || !ReadRow(obj, eid, r)) return;
                Known& k = KnownOf(eid, obj);
                if (!k.everSent && RecordAwaited(obj)) return;   // the author's record is in flight: its row follows the record
                // A join row is a row like any other, numbered by its element's own count: the joiner tells it from the rows
                // that follow by its seq, and no row of an element carries 0. The red `snapshot` takes the number and sends nothing.
                r.seq = ++k.seq;
                g_joinSeq[eid] = r.seq;
                if (dev::RedOf() == dev::Red::Snapshot) return;
                if (!k.everSent) {   // the first row any peer is told of it: the compare for the next tick starts here
                    k.sent = r;
                    k.everSent = true;
                }
                static_cast<std::vector<N::BucketRow>*>(ctx)->push_back(r);
            }, &rows);
        }
    }
    size_t sentRows = 0, refusedRows = 0;
    for (size_t i = 0; i < rows.size(); i += N::kBucketRowsMax) {
        const size_t n = (std::min<size_t>)(N::kBucketRowsMax, rows.size() - i);
        if (SendBatch(*s, rows.data() + i, n, slot)) {
            Note(Count::JoinRows);
            sentRows += n;
        } else {
            refusedRows += n;
        }
    }
    if (refusedRows)   // the transport refuses only for a connection that is going; the reliable backlog takes a full stream
        UE_LOGW("bucket_sync: slot %d's join send was refused by the transport for %zu of %zu rows", slot, refusedRows,
                rows.size());
    UE_LOGI("bucket_sync: slot %d's world is up -- %zu rows sent to it%s", slot, sentRows,
            dev::RedOf() == dev::Red::Snapshot ? " (bucket_red=snapshot: the join send is off)" : "");
}

void OnBound(uint32_t eid, void* actor) {
    if (g_parked.empty() || !detail::IsClient()) return;
    const auto it = g_parked.find(eid);
    if (it == g_parked.end() || !KindMatches(actor, it->second.row.type)) return;
    const N::BucketRow row = it->second.row;
    g_parked.erase(it);
    const auto applied = g_appliedSeq.find(eid);
    if (applied != g_appliedSeq.end() && row.seq <= applied->second) return;   // a row past it has been applied since
    ApplyRow(actor, row);
}

void OnElementEnded(uint32_t eid) { EndElement(eid); }

void OnStateWire(const uint8_t* bytes, size_t len, int senderSlot) {
    const char* refusal = nullptr;
    WireCause cause = WireCause::Direction;
    N::BucketStatePayload p{};
    if (!detail::IsClient()) {
        refusal = "the message is the host's, sent to a peer that is not a client";
    } else if (len != sizeof(p)) {
        refusal = "the length is not a BucketState's";
        cause = WireCause::Length;
    } else {
        std::memcpy(&p, bytes, sizeof(p));
        if (p.count == 0 || p.count > N::kBucketRowsMax) {
            refusal = "the row count is past 1..10";
            cause = WireCause::BadCount;
        } else if (p._pad[0] || p._pad[1] || p._pad[2]) {
            refusal = "the padding is not zero";
            cause = WireCause::Pad;
        } else {
            for (int i = 0; i < p.count; ++i) {
                const N::BucketRow& r = p.rows[i];
                if (r.type > N::kBucketBag || !std::isfinite(r.level) || !std::isfinite(r.progress)) {
                    refusal = "a row has an unknown type or a non-finite value";
                    cause = WireCause::Row;
                    break;
                }
            }
        }
    }
    if (refusal) {
        Note(Count::WireRefused);
        const int slot = senderSlot >= 0 && senderSlot < N::kMaxPeers ? senderSlot : 0;
        if (!g_wireSaid[slot][static_cast<size_t>(cause)]) {
            g_wireSaid[slot][static_cast<size_t>(cause)] = true;
            UE_LOGW("bucket_sync: a BucketState from slot %d is refused: %s (said once per sender and cause)", senderSlot, refusal);
        }
        return;
    }
    for (int i = 0; i < p.count; ++i) {
        const N::BucketRow& r = p.rows[i];
        if (!RowInRange(r)) {
            Note(Count::RowsRefused);
            if (!g_rangeSaid) {
                g_rangeSaid = true;
                UE_LOGW("bucket_sync: a row of element %u is out of its class's range -- dropped (said once)", r.eid);
            }
            continue;
        }
        void* const actor = ActorOf(r.eid);
        if (actor && KindMatches(actor, r.type)) ApplyRow(actor, r);
        else Park(r);   // the copy is not bound yet: its latest row waits for the bind (OnBound) or the element's end
    }
}

void OnDisconnect() {
    if (CountOf(Count::RowsSent) || CountOf(Count::RowsApplied))
        UE_LOGI("bucket_sync: session end -- rows sent %llu (join %llu), applied %llu, refused %llu",
                static_cast<unsigned long long>(CountOf(Count::RowsSent)),
                static_cast<unsigned long long>(CountOf(Count::JoinRows)),
                static_cast<unsigned long long>(CountOf(Count::RowsApplied)),
                static_cast<unsigned long long>(CountOf(Count::RowsRefused)));
    g_known.clear();
    g_dirty.clear();
    g_keep.clear();
    g_rows.clear();
    g_commitSeq.clear();
    g_joinSeq.clear();
    g_warnedRefused.clear();
    g_parked.clear();
    g_hostRow.clear();
    g_appliedSeq.clear();
    g_firstSeq.clear();
    g_warnedRefusedKeys.clear();
    g_owed.clear();
    if (g_pinCalls) LogPinCost();
    g_pinCalls = g_pinTicks = 0;
    g_parkFullSaid = g_rangeSaid = false;
    g_peersReady = false;
    std::memset(g_wireSaid, 0, sizeof(g_wireSaid));
    for (uint64_t& n : g_counts) n = 0;
}

void Note(Count c) { ++g_counts[static_cast<size_t>(c)]; }
uint64_t CountOf(Count c) { return g_counts[static_cast<size_t>(c)]; }

namespace dev {

Red RedOf() {
    static const Red red = [] {
        const std::string v = coop::config::ResolveEnum(coop::config_registry::rows::bucket_red);
        if (v == "apply") return Red::Apply;
        if (v == "pin") return Red::Pin;
        if (v == "tick") return Red::Tick;
        if (v == "ctick") return Red::Ctick;
        if (v == "gate") return Red::Gate;
        if (v == "snapshot") return Red::Snapshot;
        if (v == "exact") return Red::Exact;
        if (v == "alc") return Red::Alc;
        if (v == "judge") return Red::Judge;
        return Red::Off;
    }();
    return red;
}

void PinCostLine() {
    if (coop::dev::perf_probe::Armed()) LogPinCost();
}

uint32_t CommitSeq(uint32_t eid) {
    const auto it = g_commitSeq.find(eid);
    return it == g_commitSeq.end() ? 0u : it->second;
}

uint32_t AppliedSeq(uint32_t eid) {
    const auto it = g_appliedSeq.find(eid);
    return it == g_appliedSeq.end() ? 0u : it->second;
}

uint32_t JoinSeq(uint32_t eid) {
    const auto it = g_joinSeq.find(eid);
    return it == g_joinSeq.end() ? 0u : it->second;
}

uint32_t FirstAppliedSeq(uint32_t eid) {
    const auto it = g_firstSeq.find(eid);
    return it == g_firstSeq.end() ? 0u : it->second;
}

}  // namespace dev

}  // namespace coop::bucket_sync
