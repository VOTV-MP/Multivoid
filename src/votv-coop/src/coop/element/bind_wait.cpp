// coop/element/bind_wait.cpp -- see coop/element/bind_wait.h.

#include "coop/element/bind_wait.h"

#include "coop/config/config.h"           // ResolveInt, the red arm
#include "coop/config/config_registry.h"  // rows::bind_wait_red, bind_wait_mark_unknown
#include "coop/element/element.h"         // the row's actor and its cached slot
#include "coop/element/registry.h"        // Registry: a row awaiting its re-bind
#include "ue_wrap/core/hot_path_guard.h"  // UE_ASSERT_GAME_THREAD
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"      // IsLiveByIndex

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace coop::element::bind_wait {
namespace {

constexpr size_t kMaxHoldsPerEid = 64;
constexpr size_t kMaxHeldBytes   = 1024 * 1024;
constexpr size_t kMaxSlotBytes   = 256 * 1024;   // one client slot's share of it; slot 0 (the host) may use all

struct Held {
    uint16_t kind;
    int slot;
    Replay fn;
    std::vector<uint8_t> bytes;
    bool replayOnGiveUp;   // GiveUpReplayHeld queues it for the next Pump instead of dropping it
};
struct Due {
    uint32_t eid;
    uint32_t gen;   // the eid's generation when it fell due: a give-up since then voids the entry
    std::vector<Held> holds;
};

std::unordered_set<uint32_t> g_deferred;
std::unordered_map<uint32_t, std::vector<Held>> g_holds;   // eid -> holds, in arrival order
std::vector<Due> g_due;                                    // bound, waiting for the next Pump
std::unordered_map<uint32_t, uint32_t> g_gen;              // eid -> generation, bumped by a give-up a due entry can see
std::vector<int> g_leftInPump;                             // slots that left while a Pump was running
bool g_pumping = false;
std::unordered_set<uint32_t> g_overWarned;                 // eids whose bound was already said
std::unordered_set<uint16_t> g_neverBoundKinds;            // kinds whose first never-bound drop was said
size_t g_heldBytes = 0;                                    // bytes in g_holds and g_due, not yet replayed
std::unordered_map<int, size_t> g_slotBytes;               // the same bytes per sender slot, slots other than 0
std::unordered_set<int> g_slotWarned;                      // slots whose share bound was already said
uint32_t g_epoch = 0;                                      // bumped by Clear: a Pump in flight stops

// A Drop made while a Pump runs: (eid, kind) pairs the pass checks before each replay; cleared when it ends.
std::vector<std::pair<uint32_t, uint16_t>> g_droppedInPump;

// The per-kind tallies a drill reads (the header's HeldOf and its siblings).
struct Tally { uint32_t held = 0, coalesced = 0, replayed = 0, dropped = 0, givenUp = 0, neverBound = 0; };
std::unordered_map<uint16_t, Tally> g_kinds;

uint32_t g_deferredN = 0, g_heldN = 0, g_replayedN = 0, g_givenUpN = 0, g_neverBoundN = 0;
uint32_t g_refusedN = 0;                                   // holds refused at a bound
uint32_t g_dropN = 0;                                      // Drop calls that voided a hold: the log cadence
uint32_t g_replayEvents = 0;                               // eids replayed: the log cadence
uint32_t g_emptyGiveUpN = 0;                               // marks that ended with no hold to drop

// The first and every 100th, the log shape of the other receive counters.
bool Loud(uint32_t n) { return n == 1 || n % 100 == 0; }

// The red arm of the drills: Hold always says no, so the caller drops the message as before. Resolved once.
bool Red() {
    static const bool red = coop::config::ResolveInt(coop::config_registry::rows::bind_wait_red) != 0;
    return red;
}

// The drill row: an eid this peer has no registry row for is marked deferred at its first hold, so a seed that
// lands before its spawn is held and replays at the spawn's bind. Resolved once.
bool MarkUnknown() {
    static const bool on = coop::config::ResolveFlag(coop::config_registry::rows::bind_wait_mark_unknown);
    return on;
}

uint32_t TallyOf(uint16_t kind, uint32_t Tally::*field) {
    auto it = g_kinds.find(kind);
    return it == g_kinds.end() ? 0 : it->second.*field;
}

size_t SlotBytes(int slot) {
    auto it = g_slotBytes.find(slot);
    return it == g_slotBytes.end() ? 0 : it->second;
}

// Every byte kept is charged to the pool and, for a slot other than 0, to that slot's share; Release is
// its only undo, on every path that drops a hold.
void Charge(int slot, size_t n) {
    g_heldBytes += n;
    if (slot != 0) g_slotBytes[slot] += n;
}

void Release(int slot, size_t n) {
    g_heldBytes = g_heldBytes >= n ? g_heldBytes - n : 0;
    if (slot == 0) return;
    auto it = g_slotBytes.find(slot);
    if (it == g_slotBytes.end()) return;
    if (it->second > n) it->second -= n;
    else g_slotBytes.erase(it);
}

// Erase the holds of `kind` from one list, releasing their bytes; the count erased.
size_t EraseKind(std::vector<Held>& v, uint16_t kind) {
    const size_t before = v.size();
    v.erase(std::remove_if(v.begin(), v.end(),
                           [&](const Held& h) {
                               if (h.kind != kind) return false;
                               Release(h.slot, h.bytes.size());
                               return true;
                           }),
            v.end());
    return before - v.size();
}

uint32_t GenOf(uint32_t eid) {
    auto it = g_gen.find(eid);
    return it == g_gen.end() ? 0 : it->second;
}

// A row whose actor the engine purged while the row lingers, before the game's re-created native is bound
// onto it (BindUnboundReCreates, the sweep): the bind is coming and RegisterPropMirror frees the mark.
bool AwaitsRebind(uint32_t eid) {
    const coop::element::Element* e = coop::element::Registry::Get().Get(eid);
    if (!e || !e->GetActor()) return false;
    return !ue_wrap::reflection::IsLiveByIndex(e->GetActor(), e->GetInternalIdx());
}

}  // namespace

void MarkDeferred(uint32_t eid, const char* why) {
    UE_ASSERT_GAME_THREAD("bind_wait::MarkDeferred");
    if (!g_deferred.insert(eid).second) return;
    if (Loud(++g_deferredN))
        UE_LOGI("[BIND-WAIT] deferred eid=%u (%s) -- %u marked so far", eid, why, g_deferredN);
}

bool IsDeferred(uint32_t eid) {
    UE_ASSERT_GAME_THREAD("bind_wait::IsDeferred");
    return g_deferred.count(eid) > 0;
}

bool Holds(uint32_t eid, uint16_t kind) {
    UE_ASSERT_GAME_THREAD("bind_wait::Holds");
    if (auto it = g_holds.find(eid); it != g_holds.end())
        for (const Held& h : it->second)
            if (h.kind == kind) return true;
    for (const Due& d : g_due) {
        if (d.eid != eid) continue;
        for (const Held& h : d.holds)
            if (h.kind == kind) return true;
    }
    return false;
}

namespace {
bool HoldImpl(uint32_t eid, uint16_t kind, int senderSlot, Replay fn, const void* bytes, size_t n, bool coalesce,
              bool replayOnGiveUp) {
    if (Red() || !fn) return false;
    if (MarkUnknown() && !coop::element::Registry::Get().Get(eid))
        MarkDeferred(eid, "drill: an eid this peer has no row for");
    if (g_deferred.count(eid) == 0) {
        if (!AwaitsRebind(eid)) return false;
        MarkDeferred(eid, "a row awaiting its re-bind");
    }
    const uint8_t* b = static_cast<const uint8_t*>(bytes);
    std::vector<Held>& v = g_holds[eid];
    // Past a bound the hold is refused but still taken: the caller must not drop it a second way.
    auto refuse = [&]() {
        ++g_refusedN;
        if (g_overWarned.insert(eid).second)
            UE_LOGW("[BIND-WAIT] eid=%u over its bound -- hold refused (%zu hold(s), %zu B held in all)", eid,
                    v.size(), g_heldBytes);
        return true;
    };
    // A client slot may keep kMaxSlotBytes of the pool (MTA bounds a latent transfer per remote, CLatentReceiver
    // and CLatentSendQueue), so one sender cannot starve the rest; said once per slot.
    auto overShare = [&](size_t replaced) {
        if (senderSlot == 0 || SlotBytes(senderSlot) - replaced + n <= kMaxSlotBytes) return false;
        ++g_refusedN;
        if (g_slotWarned.insert(senderSlot).second)
            UE_LOGW("[BIND-WAIT] slot %d is over its share of the hold pool (%zu of %zu B) -- hold refused", senderSlot,
                    SlotBytes(senderSlot), kMaxSlotBytes);
        return true;
    };
    if (coalesce) {
        for (Held& h : v) {
            if (h.kind != kind || h.slot != senderSlot) continue;
            if (g_heldBytes - h.bytes.size() + n > kMaxHeldBytes) return refuse();
            if (overShare(h.bytes.size())) return true;
            Release(h.slot, h.bytes.size());
            Charge(senderSlot, n);
            h.fn = fn;
            h.replayOnGiveUp = h.replayOnGiveUp || replayOnGiveUp;   // a replace never clears the flag
            h.bytes.assign(b, b + n);
            ++g_heldN;
            ++g_kinds[kind].coalesced;
            return true;
        }
    }
    if (v.size() >= kMaxHoldsPerEid || g_heldBytes + n > kMaxHeldBytes) return refuse();
    if (overShare(0)) return true;
    v.push_back(Held{kind, senderSlot, fn, std::vector<uint8_t>(b, b + n), replayOnGiveUp});
    Charge(senderSlot, n);
    ++g_heldN;
    ++g_kinds[kind].held;
    return true;
}
}  // namespace

bool Hold(uint32_t eid, uint16_t kind, int senderSlot, Replay fn, const void* bytes, size_t n, bool coalesce) {
    UE_ASSERT_GAME_THREAD("bind_wait::Hold");
    return HoldImpl(eid, kind, senderSlot, fn, bytes, n, coalesce, /*replayOnGiveUp=*/false);
}

bool HoldReplayOnGiveUp(uint32_t eid, uint16_t kind, int senderSlot, Replay fn, const void* bytes, size_t n) {
    UE_ASSERT_GAME_THREAD("bind_wait::HoldReplayOnGiveUp");
    return HoldImpl(eid, kind, senderSlot, fn, bytes, n, /*coalesce=*/true, /*replayOnGiveUp=*/true);
}

void OnBound(uint32_t eid) {
    UE_ASSERT_GAME_THREAD("bind_wait::OnBound");
    if (g_deferred.erase(eid) == 0) return;
    g_overWarned.erase(eid);
    auto it = g_holds.find(eid);
    if (it == g_holds.end()) return;
    if (!it->second.empty()) g_due.push_back(Due{eid, GenOf(eid), std::move(it->second)});
    g_holds.erase(it);
}

void Pump() {
    UE_ASSERT_GAME_THREAD("bind_wait::Pump");
    if (g_due.empty() || g_pumping) return;
    // Swapped out first: a replay can defer, hold or bind again, and that lands in the next Pump.
    std::vector<Due> due;
    due.swap(g_due);
    const uint32_t epoch = g_epoch;
    g_pumping = true;
    // A give-up or a slot that left while the replays run reaches the swapped-out list through the eid's
    // generation and the recorded slots; both end with the pass.
    struct PassEnd {
        ~PassEnd() {
            g_pumping = false;
            g_leftInPump.clear();
            g_droppedInPump.clear();
            if (g_due.empty()) g_gen.clear();
        }
    } passEnd;
    auto leftInPump = [](int slot) {
        return std::find(g_leftInPump.begin(), g_leftInPump.end(), slot) != g_leftInPump.end();
    };
    // A Clear made by a replay ends the pass: the holds not yet reached are dropped by it, counted per kind.
    auto clearedRest = [&](size_t di, size_t hi) {
        for (; di < due.size(); ++di, hi = 0)
            for (; hi < due[di].holds.size(); ++hi) {
                ++g_givenUpN;
                ++g_kinds[due[di].holds[hi].kind].givenUp;
            }
    };
    for (size_t di = 0; di < due.size(); ++di) {
        Due& d = due[di];
        size_t ran = 0;
        for (size_t hi = 0; hi < d.holds.size(); ++hi) {
            Held& h = d.holds[hi];
            if (g_epoch != epoch) {   // a replay tore the session down
                clearedRest(di, hi);
                return;
            }
            Release(h.slot, h.bytes.size());
            if (GenOf(d.eid) != d.gen || leftInPump(h.slot)) {   // given up, or its sender left, since it fell due
                ++g_givenUpN;
                ++g_kinds[h.kind].givenUp;
                continue;
            }
            if (std::find(g_droppedInPump.begin(), g_droppedInPump.end(), std::make_pair(d.eid, h.kind)) !=
                g_droppedInPump.end()) {   // its module voided it (Drop) since it fell due
                ++g_kinds[h.kind].dropped;
                continue;
            }
            h.fn(h.bytes.data(), h.bytes.size(), h.slot);
            ++g_replayedN;
            ++g_kinds[h.kind].replayed;
            ++ran;
        }
        if (ran > 0 && Loud(++g_replayEvents))
            UE_LOGI("[BIND-WAIT] replayed %zu hold(s) for eid=%u -- %u replayed so far", ran, d.eid,
                    g_replayedN);
    }
}

void Drop(uint32_t eid, uint16_t kind, const char* why) {
    UE_ASSERT_GAME_THREAD("bind_wait::Drop");
    if (g_pumping) g_droppedInPump.emplace_back(eid, kind);   // the pass's swapped-out holds of it skip
    size_t dropped = 0;
    if (auto it = g_holds.find(eid); it != g_holds.end()) {
        dropped += EraseKind(it->second, kind);
        if (it->second.empty()) g_holds.erase(it);
    }
    for (Due& d : g_due)
        if (d.eid == eid) dropped += EraseKind(d.holds, kind);
    if (dropped == 0) return;
    g_kinds[kind].dropped += static_cast<uint32_t>(dropped);
    if (Loud(++g_dropN))
        UE_LOGI("[BIND-WAIT] dropped %zu hold(s) of kind %u for eid=%u (%s) -- %u drop(s) so far", dropped,
                static_cast<unsigned>(kind), eid, why, g_dropN);
}

void GiveUp(uint32_t eid, const char* why) {
    UE_ASSERT_GAME_THREAD("bind_wait::GiveUp");
    const bool wasDeferred = g_deferred.erase(eid) > 0;
    g_overWarned.erase(eid);
    // A due entry for the eid (waiting, or swapped out by a Pump in flight) is voided by the new generation.
    if (g_pumping || !g_due.empty()) ++g_gen[eid];
    size_t dropped = 0;
    if (auto it = g_holds.find(eid); it != g_holds.end()) {
        dropped = it->second.size();
        for (const Held& h : it->second) {
            ++g_kinds[h.kind].givenUp;
            Release(h.slot, h.bytes.size());
        }
        g_holds.erase(it);
    }
    if (dropped == 0) {   // a mark with no hold ends silently, counted
        if (wasDeferred) ++g_emptyGiveUpN;
        return;
    }
    g_givenUpN += static_cast<uint32_t>(dropped);
    UE_LOGI("[BIND-WAIT] gave up eid=%u -- %zu hold(s) dropped (%s)", eid, dropped, why);
}

void GiveUpReplayHeld(uint32_t eid, const char* why) {
    UE_ASSERT_GAME_THREAD("bind_wait::GiveUpReplayHeld");
    const bool wasDeferred = g_deferred.erase(eid) > 0;
    g_overWarned.erase(eid);
    if (g_pumping || !g_due.empty()) ++g_gen[eid];   // as GiveUp: a due entry made before this call is voided
    auto it = g_holds.find(eid);
    if (it == g_holds.end() || it->second.empty()) {
        if (it != g_holds.end()) g_holds.erase(it);
        if (wasDeferred) ++g_emptyGiveUpN;
        return;
    }
    std::vector<Held> held = std::move(it->second);
    g_holds.erase(it);
    // Queued, not replayed here: the caller may be walking a container the replay would change. The next
    // Pump runs them with the eid already unmarked, so a replay that would hold again takes the unbound path.
    Due due{eid, GenOf(eid), {}};
    size_t dropped = 0;
    for (Held& h : held) {
        if (h.replayOnGiveUp) {
            due.holds.push_back(std::move(h));
            continue;
        }
        Release(h.slot, h.bytes.size());
        ++g_kinds[h.kind].givenUp;
        ++dropped;
    }
    const size_t queued = due.holds.size();
    if (queued > 0) g_due.push_back(std::move(due));
    g_givenUpN += static_cast<uint32_t>(dropped);
    UE_LOGI("[BIND-WAIT] gave up eid=%u -- %zu hold(s) queued to replay unbound, %zu dropped (%s)", eid, queued,
            dropped, why);
}

void OnSlotLeft(int slot) {
    UE_ASSERT_GAME_THREAD("bind_wait::OnSlotLeft");
    g_slotWarned.erase(slot);
    if (g_pumping) g_leftInPump.push_back(slot);   // the swapped-out holds of the pass in flight skip it
    size_t dropped = 0;
    auto drop = [&](std::vector<Held>& v) {
        const size_t before = v.size();
        v.erase(std::remove_if(v.begin(), v.end(),
                               [&](const Held& h) {
                                   if (h.slot != slot) return false;
                                   Release(h.slot, h.bytes.size());
                                   ++g_kinds[h.kind].givenUp;
                                   return true;
                               }),
                v.end());
        dropped += before - v.size();
    };
    for (auto& kv : g_holds) drop(kv.second);
    for (Due& d : g_due) drop(d.holds);
    if (dropped == 0) return;
    g_givenUpN += static_cast<uint32_t>(dropped);
    UE_LOGI("[BIND-WAIT] slot %d left -- %zu hold(s) dropped", slot, dropped);
}

void Clear(const char* why) {
    UE_ASSERT_GAME_THREAD("bind_wait::Clear");
    size_t dropped = 0;
    auto count = [&](const std::vector<Held>& v) {
        dropped += v.size();
        for (const Held& h : v) ++g_kinds[h.kind].givenUp;
    };
    for (const auto& kv : g_holds) count(kv.second);
    for (const Due& d : g_due) count(d.holds);
    if (dropped > 0 || !g_deferred.empty())
        UE_LOGI("[BIND-WAIT] cleared (%s) -- %zu eid(s) deferred, %zu hold(s) dropped, %u mark(s) ended empty so far",
                why, g_deferred.size(), dropped, g_emptyGiveUpN);
    g_givenUpN += static_cast<uint32_t>(dropped);
    g_deferred.clear();
    g_holds.clear();
    g_due.clear();
    g_overWarned.clear();
    g_slotWarned.clear();
    g_gen.clear();
    g_heldBytes = 0;
    g_slotBytes.clear();
    ++g_epoch;   // a Pump in flight stops at its next hold: the clear is its generation bump
}

void NoteNeverBound(uint16_t kind, uint32_t eid) {
    UE_ASSERT_GAME_THREAD("bind_wait::NoteNeverBound");
    ++g_neverBoundN;
    ++g_kinds[kind].neverBound;
    if (g_neverBoundKinds.insert(kind).second || Loud(g_neverBoundN))
        UE_LOGW("[BIND-WAIT] kind %u names eid=%u, a prop this peer never bound -- dropped (%u so far)",
                static_cast<unsigned>(kind), eid, g_neverBoundN);
}

bool HasHolds() {
    UE_ASSERT_GAME_THREAD("bind_wait::HasHolds");
    if (!g_due.empty()) return true;
    for (const auto& kv : g_holds)
        if (!kv.second.empty()) return true;
    return false;
}

uint32_t DropsNeverBound() { UE_ASSERT_GAME_THREAD("bind_wait::DropsNeverBound"); return g_neverBoundN; }
uint32_t DeferredCount()   { UE_ASSERT_GAME_THREAD("bind_wait::DeferredCount");   return g_deferredN; }
uint32_t HeldCount()       { UE_ASSERT_GAME_THREAD("bind_wait::HeldCount");       return g_heldN; }
uint32_t ReplayedCount()   { UE_ASSERT_GAME_THREAD("bind_wait::ReplayedCount");   return g_replayedN; }
uint32_t GivenUpCount()    { UE_ASSERT_GAME_THREAD("bind_wait::GivenUpCount");    return g_givenUpN; }
uint32_t RefusedCount()    { UE_ASSERT_GAME_THREAD("bind_wait::RefusedCount");    return g_refusedN; }

uint32_t HeldOf(uint16_t kind)       { UE_ASSERT_GAME_THREAD("bind_wait::HeldOf");       return TallyOf(kind, &Tally::held); }
uint32_t CoalescedOf(uint16_t kind)  { UE_ASSERT_GAME_THREAD("bind_wait::CoalescedOf");  return TallyOf(kind, &Tally::coalesced); }
uint32_t ReplayedOf(uint16_t kind)   { UE_ASSERT_GAME_THREAD("bind_wait::ReplayedOf");   return TallyOf(kind, &Tally::replayed); }
uint32_t DroppedOf(uint16_t kind)    { UE_ASSERT_GAME_THREAD("bind_wait::DroppedOf");    return TallyOf(kind, &Tally::dropped); }
uint32_t GivenUpOf(uint16_t kind)    { UE_ASSERT_GAME_THREAD("bind_wait::GivenUpOf");    return TallyOf(kind, &Tally::givenUp); }
uint32_t NeverBoundOf(uint16_t kind) { UE_ASSERT_GAME_THREAD("bind_wait::NeverBoundOf"); return TallyOf(kind, &Tally::neverBound); }

}  // namespace coop::element::bind_wait
