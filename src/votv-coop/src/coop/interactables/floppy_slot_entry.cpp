// coop/interactables/floppy_slot_entry.cpp -- see coop/interactables/floppy_slot_entry.h.

#include "coop/interactables/floppy_slot_entry.h"

#include "coop/net/session.h"

#include "ue_wrap/actors/floppy_disc.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/devices/floppy_slot.h"

#include <atomic>
#include <chrono>
#include <cstdint>

namespace coop::floppy_slot_entry {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace FD = ue_wrap::floppy_disc;
namespace FS = ue_wrap::floppy_slot;
namespace P  = ue_wrap::profile;

// One window for every device, because the mark is on the DISC and a disc does not know which
// slot it came out of.
//
// Its FLOOR is the overlap a disc's own materialisation raises: the hitbox reports it on the spawn
// frame and the device defers the insert by one tick, so a frame or two. Its CEILING is the pause
// for which the device that ejected keeps its own hitbox off -- re-enabling a collider re-reports
// every body already inside it, so at the end of that pause the game deliberately re-takes a disc
// still sitting in the slot. The two devices do not agree on the pause: the signal server waits a
// second, the laptop half of one.
//
// 750 ms sits under the server's and OVER the laptop's, and the difference is deliberate rather
// than overlooked. Under the server's, the peer that ejected keeps single-player behaviour exactly.
// Over the laptop's, that peer's re-take is delayed by a quarter second and nothing else changes:
// it still happens, the slot lane still carries it, and the peers still converge -- which is the
// same outcome the server's case has, since the peer that did NOT eject re-takes in neither.
// Splitting the constant per device would buy that quarter second at the price of a disc having to
// remember its origin.
//
// Lapsing costs nothing: the disc becomes an ordinary disc and the native rule resumes, with no
// retry, no resend and nobody waiting.
constexpr uint64_t kTransitMs = 750;

// A disc materialising is a spawn, and a spawn burst is a join. Sized so a join's worth of discs
// keeps its marks; the eviction counter says when it was not, rather than going quietly blind.
constexpr size_t kMaxTransit = 32;

struct Transit {
    ue_wrap::CachedObjRef disc;
    uint64_t              bornMs = 0;
    unsigned              logged = 0;   // refusals printed for this entry, capped
};

Transit g_transit[kMaxTransit];

std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<bool> g_installed{false};
std::atomic<bool> g_spawnSeam{false};
std::atomic<bool> g_finishSeam{false};

// Counters, read at the session summary. A refusal is the lane doing its job; an eviction is the
// lane admitting it lost a mark it should have kept.
std::atomic<unsigned long long> g_seen{0};
std::atomic<unsigned long long> g_refused{0};
std::atomic<unsigned long long> g_marked{0};
std::atomic<unsigned long long> g_markedLate{0};   // marks the finish seam added the deferred one missed
std::atomic<unsigned long long> g_evicted{0};

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// The counters have to land WHILE a session runs. A rig peer is killed rather than disconnected,
// so a summary that only prints at teardown prints nothing at all, and a lane that refuses nothing
// then looks the same as a lane nobody asked. Every 15 s, and only once the numbers have moved.
uint64_t g_nextReportMs = 0;
unsigned long long g_lastReported = ~0ull;

void ReportCounters(const char* when) {
    UE_LOGI("floppy_slot_entry: %s -- overlaps seen=%llu, inserts refused as in-transit=%llu, "
            "discs marked=%llu (of them %llu seen only by the finish backstop), marks evicted "
            "before their window closed=%llu. Seen=0 means the entry is not dispatched where this "
            "interceptor sits; refused=0 with seen>0 means no disc was in transit when one came.",
            when, g_seen.load(std::memory_order_relaxed),
            g_refused.load(std::memory_order_relaxed), g_marked.load(std::memory_order_relaxed),
            g_markedLate.load(std::memory_order_relaxed),
            g_evicted.load(std::memory_order_relaxed));
}

void ReportPeriodic() {
    const uint64_t now = NowMs();
    if (now < g_nextReportMs) return;
    g_nextReportMs = now + 15000;
    const unsigned long long sum = g_seen.load(std::memory_order_relaxed) +
                                   g_refused.load(std::memory_order_relaxed) +
                                   g_marked.load(std::memory_order_relaxed);
    if (sum == g_lastReported) return;   // nothing moved; the last line still describes the lane
    g_lastReported = sum;
    ReportCounters("COUNTERS");
}

// Take the slot whose window closed longest ago; if every slot is still inside its window, take
// the oldest anyway and say so, because a silent overwrite would read exactly like the bug.
Transit& SlotFor(uint64_t now) {
    Transit* best = &g_transit[0];
    for (auto& t : g_transit) {
        if (!t.disc.Raw() || now - t.bornMs >= kTransitMs) return t;
        if (t.bornMs < best->bornMs) best = &t;
    }
    g_evicted.fetch_add(1, std::memory_order_relaxed);
    return *best;
}

// The mark, set where the disc appears. Every birth reaches it -- the game's own eject spawn and
// our spawn of a disc another peer ejected alike -- because the seams below are the spawn calls
// themselves, and neither birth is a player putting the disc anywhere.
//
// Re-marking would push the window out, so an actor that already holds a live mark keeps it and
// the caller learns nothing was added.
bool Mark(void* disc) {
    const uint64_t now = NowMs();
    for (auto& t : g_transit)
        if (t.disc.Raw() == disc && now - t.bornMs < kTransitMs) return false;
    Transit& t = SlotFor(now);
    t.disc.Set(disc);
    t.bornMs = now;
    t.logged = 0;
    g_marked.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// Is this actor a disc still inside its transit window? Pointer identity through the slot-
// validated ref, so a recycled object array slot cannot inherit a mark.
Transit* FindTransit(void* actor, uint64_t now) {
    for (auto& t : g_transit) {
        if (t.disc.Raw() != actor) continue;
        if (now - t.bornMs >= kTransitMs) return nullptr;
        if (!t.disc.Alive()) return nullptr;
        return &t;
    }
    return nullptr;
}

// ---- the seams ----------------------------------------------------------------------------

// The spawn seam, and it has to be the DEFERRED half. A spawn's components register inside
// FinishSpawningActor, and registering a collider reports its overlaps there and then -- so a disc
// materialising inside a box raises the box's entry BEFORE FinishSpawningActor has returned, and a
// mark set on the way out is set after the box has already claimed it. The deferred call hands the
// actor over before any of that: constructed, unregistered, still at the origin.
//
// Fires for EVERY deferred spawn in the game, so it does one class test and nothing else on the
// miss. Neither the world context nor the issuing Blueprint is read: the rule is about the disc,
// not about who spawned it, so a caller list would be a second thing to keep current.
void OnBeginDeferredSpawn(void* /*context*/, void* /*sourceObject*/, void* spawnedResult) {
    if (!spawnedResult) return;                       // a spawn the world refused
    if (!FD::IsDiscClass(R::ClassOf(spawnedResult))) return;
    Mark(spawnedResult);
}

// The finish half is kept as a BACKSTOP for a disc that reaches the world without the deferred
// call -- a plain SpawnActor has no deferred half at all. It marks nothing the deferred seam
// already covered, and its own counter says how often it was the only one that saw a birth.
void OnFinishSpawn(void* /*context*/, void* /*sourceObject*/, void* spawnedResult) {
    if (!spawnedResult) return;
    if (!FD::IsDiscClass(R::ClassOf(spawnedResult))) return;
    if (Mark(spawnedResult)) g_markedLate.fetch_add(1, std::memory_order_relaxed);
}

// The overlap entry, one interceptor for every device kind and both roles.
//
// On the thread rule: an interceptor CAN fire off the game thread, and this one reads a table the
// spawn seams write, so it would not be safe if it did. It does not -- a component reports its
// overlaps from UpdateOverlaps and both spawn seams are actor spawns, all three game-thread only,
// so the table has one writer and one reader on one thread. Nothing else needs re-validating: it
// calls no engine function and touches no actor memory.
bool OnSlotOverlapPre(void* self, void* params) {
    if (!params) return false;
    // The delegate signature is (UPrimitiveComponent* Overlapped, AActor* OtherActor, ...): the
    // entering actor is the second pointer, as the coin's collect interceptor reads it.
    void* other = *reinterpret_cast<void**>(static_cast<uint8_t*>(params) + sizeof(void*));
    if (!other) return false;
    g_seen.fetch_add(1, std::memory_order_relaxed);

    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return false;          // solo: the native rule is the whole rule

    Transit* t = FindTransit(other, NowMs());
    if (!t) return false;                             // an ordinary disc, or not a disc at all

    g_refused.fetch_add(1, std::memory_order_relaxed);
    if (t->logged < 3) {
        ++t->logged;
        UE_LOGI("floppy_slot_entry: REFUSED device=%p overlap by disc=%p -- the disc materialised "
                "%llu ms ago and is still in transit out of a slot. This peer did not eject it and "
                "nobody put it here; the native entry would swallow it and relay the destroy, "
                "killing it on every peer.",
                self, other, static_cast<unsigned long long>(NowMs() - t->bornMs));
    }
    return true;                                      // replace the dispatch: no insert, no destroy
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);

    // The spawn seams first: without them every mark is missing and the interceptors below would
    // register and refuse nothing.
    if (!g_spawnSeam.load(std::memory_order_acquire)) {
        void* gsCls = R::FindClass(P::name::GameplayStaticsClass);
        void* deferred = gsCls ? R::FindFunction(gsCls, P::name::BeginDeferredSpawnFn) : nullptr;
        void* finish   = gsCls ? R::FindFunction(gsCls, P::name::FinishSpawningActorFn) : nullptr;
        if (finish && !g_finishSeam.load(std::memory_order_acquire) &&
            ue_wrap::ufunction_hook::InstallPostHook(finish, &OnFinishSpawn))
            g_finishSeam.store(true, std::memory_order_release);
        if (deferred && ue_wrap::ufunction_hook::InstallPostHook(deferred, &OnBeginDeferredSpawn)) {
            g_spawnSeam.store(true, std::memory_order_release);
            UE_LOGI("floppy_slot_entry: spawn seams installed on %ls (backstop %ls=%d) -- a disc "
                    "that materialises is marked in transit for %llu ms",
                    P::name::BeginDeferredSpawnFn, P::name::FinishSpawningActorFn,
                    g_finishSeam.load(std::memory_order_acquire) ? 1 : 0,
                    static_cast<unsigned long long>(kTransitMs));
        }
        return;   // one step a tick; the interceptors go on once the marks exist
    }

    if (g_installed.load(std::memory_order_acquire)) { ReportPeriodic(); return; }

    size_t registered = 0, named = 0;
    for (uint8_t k = 0; k < FS::kDeviceKindCount; ++k) {
        void* fns[FS::kMaxSlotOverlapEntries] = {};
        const size_t n = FS::SlotOverlapEntries(static_cast<FS::DeviceKind>(k), fns,
                                                FS::kMaxSlotOverlapEntries);
        if (n == 0) return;   // a kind whose class has not loaded: retry next tick, all or nothing
        named += n;
        for (size_t i = 0; i < n; ++i)
            if (GT::RegisterInterceptor(fns[i], &OnSlotOverlapPre)) ++registered;
    }
    if (registered != named) {
        UE_LOGE("floppy_slot_entry: %zu of %zu overlap entries took an interceptor (table full?) "
                "-- the unregistered ones still swallow a disc in transit and relay its destroy",
                registered, named);
        return;   // retry: a full table can free up, and a partial guard is the bug on some devices
    }
    g_installed.store(true, std::memory_order_release);
    UE_LOGI("floppy_slot_entry: installed -- %zu overlap entries intercepted across %u device "
            "kinds", registered, static_cast<unsigned>(FS::kDeviceKindCount));
}

void OnDisconnect() {
    ReportCounters("SESSION SUMMARY");
    g_seen.store(0, std::memory_order_relaxed);
    g_refused.store(0, std::memory_order_relaxed);
    g_marked.store(0, std::memory_order_relaxed);
    g_markedLate.store(0, std::memory_order_relaxed);
    g_evicted.store(0, std::memory_order_relaxed);
    g_nextReportMs = 0;
    g_lastReported = ~0ull;
    // The marks are actors of the world that is going away. The interceptors and the spawn seam
    // stay: both are UFunction registrations, not world state, and re-registering the same pair is
    // a no-op.
    for (auto& t : g_transit) { t.disc.Reset(); t.bornMs = 0; t.logged = 0; }
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::floppy_slot_entry
