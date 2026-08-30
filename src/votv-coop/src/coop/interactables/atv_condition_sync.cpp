// coop/interactables/atv_condition_sync.cpp -- see header for the lane's contract (authority
// split, last-expressed edges, defer guard). This file is the policy; ue_wrap/devices/
// atv_condition owns the layout and the dispatch.

#include "coop/interactables/atv_condition_sync.h"

#include "coop/interactables/atv_sync_internal.h"
#include "coop/net/protocol.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/devices/atv_condition.h"

#include <atomic>
#include <cmath>
#include <cstring>

namespace coop::atv_condition_sync {

namespace AC = ue_wrap::atv_condition;

namespace {

// The wire block is the contiguous tail of AtvStatePayload -- pinned here so the memcpy-based
// idle gate cannot drift from the struct silently.
constexpr size_t kCondOff = offsetof(coop::net::AtvStatePayload, tiresDurability);
constexpr size_t kCondBytes = sizeof(coop::net::AtvStatePayload) - kCondOff;
static_assert(kCondBytes == 64, "v147 condition block must be 64 bytes (and AtvEntry::lastSentCond with it)");

// Change-edge quanta (qf round 3: pure-visual quanta, not correctness bounds). Dirt domain is
// 0..1 (a 1% step is invisible in the shader lerp); health domain 0..100 drives smoke
// freq/color.
constexpr float kDirtEps = 0.01f;
constexpr float kHealthEps = 0.5f;

std::atomic<unsigned long long> g_applied{0}, g_updTires{0}, g_updDirt{0}, g_updSpare{0},
    g_updHealth{0}, g_presenceSkipped{0}, g_deferred{0}, g_invalid{0};

void PayloadToSnapshot(const coop::net::AtvStatePayload& p, AC::Snapshot& s) {
    for (int i = 0; i < 4; ++i) {
        s.dur[i]  = p.tiresDurability[i];
        s.dirt[i] = p.tiresDirt[i];
        s.fixes[i] = p.tiresFixes[i];
        s.types[i] = p.tiresTypes[i];
    }
    s.bodyDirt = p.bodyDirt;
    s.spareDur = p.spareDurability;
    s.spareDirt = p.spareDirt;
    s.fuel = p.fuel;
    s.health = p.health;
    s.mask = p.tiresMask;
    s.hasSpare = p.hasSpare != 0;
    s.spareFixes = p.spareFixes;
}

bool DirtGroupChanged(const AC::Snapshot& in, const AC::Snapshot& expressed) {
    if (std::memcmp(in.fixes, expressed.fixes, sizeof(in.fixes)) != 0) return true;
    for (int i = 0; i < 4; ++i)
        if (std::fabs(in.dirt[i] - expressed.dirt[i]) > kDirtEps) return true;
    return std::fabs(in.bodyDirt - expressed.bodyDirt) > kDirtEps;
}

}  // namespace

void FillPayload(void* actor, coop::net::AtvStatePayload& p) {
    // p arrives memset(0) from ReadPayload -- tiresValid is already 0, which IS the honest
    // "producer carried nothing" value (the v143 birthLen rule). Only a COMPLETE read flips it.
    if (!AC::Resolved() && !AC::Resolve()) {
        static bool warned = false;
        if (!warned) { warned = true;
            UE_LOGW("atv_condition: fill with unresolved layout -- payloads ship tiresValid=0 "
                    "this session (receivers keep their local state)"); }
        return;
    }
    AC::Snapshot s;
    if (!AC::Read(actor, s)) {
        static bool warned = false;
        if (!warned) { warned = true;
            UE_LOGW("atv_condition: actor read failed at fill (array head unreadable) -- "
                    "shipping tiresValid=0 for it"); }
        return;
    }
    for (int i = 0; i < 4; ++i) {
        p.tiresDurability[i] = s.dur[i];
        p.tiresDirt[i]  = s.dirt[i];
        p.tiresFixes[i] = s.fixes[i];
        p.tiresTypes[i] = s.types[i];
    }
    p.bodyDirt = s.bodyDirt;
    p.spareDurability = s.spareDur;
    p.spareDirt = s.spareDirt;
    p.fuel = s.fuel;
    p.health = s.health;
    p.tiresMask = s.mask;
    p.hasSpare = s.hasSpare ? 1 : 0;
    p.spareFixes = s.spareFixes;
    p.tiresValid = 1;
}

void ApplyPayload(coop::atv_sync::AtvEntry& e, const coop::net::AtvStatePayload& p,
                  uint8_t senderSlot) {
    using coop::atv_sync::AtvEntry;
    if (!p.tiresValid) {
        g_invalid.fetch_add(1, std::memory_order_relaxed);
        return;  // the producer could not read; keeping local state IS the contract
    }
    if (!AC::Resolved() && !AC::Resolve()) return;  // fill already warned once
    void* actor = e.actor;
    if (!actor) return;

    // Seed the last-expressed baseline FROM THE ACTOR (qf round 4): first apply for this
    // entry, or the entry repointed to a new actor after an index rebuild. A save-loaded
    // joiner whose world already matches the host thus fires NO verb on its first packet.
    if (!e.condExpressedSeeded || e.condExpressedActor != actor) {
        AC::Snapshot seeded;
        if (!AC::Read(actor, seeded)) {
            // Cannot establish a baseline -> writing accumulators is still safe (values,
            // no verbs), but no verb may fire against an unknown rig state. Retry next apply.
            // Audit MINOR-6: say so once -- a permanently unreadable receiver actor must not
            // look healthy in the counters.
            static bool warned = false;
            if (!warned) { warned = true;
                UE_LOGW("atv_condition: receiver actor unreadable at seed -- values-only "
                        "applies until it reads (no verbs will fire)"); }
            AC::Snapshot in;
            PayloadToSnapshot(p, in);
            AC::WriteAccumulators(actor, in);
            g_applied.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        e.condExpressed = seeded;
        e.condExpressedSeeded = true;
        e.condExpressedActor = actor;
    }

    AC::Snapshot in;
    PayloadToSnapshot(p, in);

    // MAJOR-1 (audit): the 13 floats reach engine fields and the HOST SAVE raw, and a NaN
    // would ALSO disable this lane's own edges forever (fabs(NaN-x)>eps is false) -- values
    // poison while no verb re-derives. Non-finite is the SYMMETRIC garbage filter (v141
    // precedent): refuse the BLOCK like tiresValid=0, keep the packet's pose half.
    {
        const float* fs[] = { &in.dur[0], &in.dur[1], &in.dur[2], &in.dur[3],
                              &in.dirt[0], &in.dirt[1], &in.dirt[2], &in.dirt[3],
                              &in.bodyDirt, &in.spareDur, &in.spareDirt, &in.fuel, &in.health };
        for (const float* f : fs) {
            if (!std::isfinite(*f)) {
                g_invalid.fetch_add(1, std::memory_order_relaxed);
                static bool warned = false;
                if (!warned) { warned = true;
                    UE_LOGW("atv_condition: non-finite float in a condition block from slot %u "
                            "-- block refused (counted as invalid)", static_cast<unsigned>(senderSlot)); }
                return;
            }
        }
    }
    // CLIENT-SCOPED domain clamps (audit MINOR-1, the A54 family). Per the standing rule the
    // HOST may cheat and we relay it -- so a bound on host-authored values would be a bug;
    // only client-authored values are clamped to the game's own domains before they can
    // reach the host's persisted fields.
    if (senderSlot != 0) {
        auto c01  = [](float& v) { if (v < 0.f) v = 0.f; else if (v > 1.f)   v = 1.f; };
        auto c100 = [](float& v) { if (v < 0.f) v = 0.f; else if (v > 100.f) v = 100.f; };
        for (int i = 0; i < 4; ++i) { c100(in.dur[i]); c01(in.dirt[i]); }
        c01(in.bodyDirt); c100(in.spareDur); c01(in.spareDirt); c100(in.fuel); c100(in.health);
    }

    const AC::Snapshot& ex = e.condExpressed;
    const bool applyPresence = senderSlot == 0;

    // Change groups vs the EXPRESSED baseline, over the WRITTEN set only (qf round 3: presence
    // terms leave the groups entirely when presence is not applied, else updTires would refire
    // at 20 Hz through the whole deliberately-divergent client-eject window).
    const bool presenceDiffers = in.mask != ex.mask || in.hasSpare != ex.hasSpare;
    const bool typesDiffer = std::memcmp(in.types, ex.types, sizeof(in.types)) != 0;
    const bool groupTires = applyPresence && (in.mask != ex.mask || typesDiffer);
    const bool groupDirt = DirtGroupChanged(in, ex);
    const bool groupSpare = (applyPresence && in.hasSpare != ex.hasSpare) ||
                            in.spareFixes != ex.spareFixes ||
                            std::fabs(in.spareDirt - ex.spareDirt) > kDirtEps;
    const bool groupHealth = std::fabs(in.health - ex.health) > kHealthEps;

    if (!applyPresence && presenceDiffers) {
        // The KNOWN-BROKEN direction, counted and visible (CRUTCHES row; (b2) asserts it):
        // a non-host author's presence claim is not consumed -- its paired wheel-prop birth
        // cannot travel, so consuming the mask would persist an item loss on the host.
        const auto n = g_presenceSkipped.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1 || n % 100 == 0)
            UE_LOGW("atv_condition: presence from slot %u DIFFERS (mask %02X vs expressed %02X) "
                    "-- skipped by the authority rule, x%llu (KNOWN divergence until the "
                    "act-as-host tire-eject intent lane; docs/CRUTCHES.md)",
                    static_cast<unsigned>(senderSlot), p.tiresMask, ex.mask, n);
    }

    // Fields first, always: values are cheap truth; verbs are the gated part.
    AC::WriteAccumulators(actor, in);
    if (applyPresence && presenceDiffers)
        AC::WritePresence(actor, in.mask, in.hasSpare);
    g_applied.fetch_add(1, std::memory_order_relaxed);

    if (!(groupTires || groupDirt || groupSpare || groupHealth)) return;

    // The defer guard (qf round 2): skipTireUpdate is measured writer-less and default-FALSE,
    // but if some future external writer sets it, the reducers no-op silently -- so defer
    // rather than dispatch into a no-op, and retry on the next apply (the expressed baseline
    // has not advanced, so the same edges recompute).
    bool skip = true;
    if (!AC::ReadSkipTireUpdate(actor, skip)) skip = true;
    if (skip) {
        const auto n = g_deferred.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1)
            UE_LOGW("atv_condition: skipTireUpdate reads TRUE at apply -- verbs deferred "
                    "(measured writer-less; an external writer exists if you see this)");
        return;  // the expressed baseline did not advance, so the same edges recompute next apply
    }

    // Verb order: spare first (chains updDirt), tires second (also chains updDirt), bare
    // updDirt only when neither heavy verb ran. Expression advances per group WITH its verb.
    bool dirtExpressed = false;
    if (groupSpare) {
        if (AC::CallVerb(actor, AC::Verb::UpdSpareTire)) {
            g_updSpare.fetch_add(1, std::memory_order_relaxed);
            // MAJOR-3 (audit): hasSpare is PRESENCE -- advancing its expression on a
            // non-host packet (groupSpare can fire via fixes/dirt alone) records a value
            // that was never WRITTEN, and the host's later legitimate edge then compares
            // equal and never lands. Advance presence expression only when presence applied.
            if (applyPresence) e.condExpressed.hasSpare = in.hasSpare;
            e.condExpressed.spareFixes = in.spareFixes;
            e.condExpressed.spareDirt = in.spareDirt;
            e.condExpressed.spareDur = in.spareDur;
            dirtExpressed = true;  // updSpareTire ends in updDirt (measured)
        }
    }
    if (groupTires) {
        // Capture the FROM value before advancing: `ex` references e.condExpressed, so
        // logging after the advance printed "0B -> 0B" (caught in run B-solo, 19:26:59).
        const uint8_t maskWas = ex.mask;
        if (AC::CallVerb(actor, AC::Verb::UpdTires)) {
            g_updTires.fetch_add(1, std::memory_order_relaxed);
            e.condExpressed.mask = in.mask;
            std::memcpy(e.condExpressed.types, in.types, sizeof(in.types));
            dirtExpressed = true;  // updTires contains updDirt (measured)
            UE_LOGI("atv_condition: updTires dispatched (mask %02X -> %02X) -- presence edge "
                    "from the host-authored stream", maskWas, in.mask);
        }
    }
    if (groupDirt && !dirtExpressed) {
        if (AC::CallVerb(actor, AC::Verb::UpdDirt)) {
            g_updDirt.fetch_add(1, std::memory_order_relaxed);
            dirtExpressed = true;
        }
    }
    if (dirtExpressed) {
        std::memcpy(e.condExpressed.fixes, in.fixes, sizeof(in.fixes));
        std::memcpy(e.condExpressed.dirt, in.dirt, sizeof(in.dirt));
        e.condExpressed.bodyDirt = in.bodyDirt;
    }
    if (groupHealth) {
        if (AC::CallVerb(actor, AC::Verb::UpdHealth)) {
            g_updHealth.fetch_add(1, std::memory_order_relaxed);
            e.condExpressed.health = in.health;
        }
    }
    // fuel and the spare/tire durabilities have no verb: no expression to advance (values
    // were written above; nothing derives from them at apply time).
}

bool CondChangedSinceLastSend(const coop::atv_sync::AtvEntry& e,
                              const coop::net::AtvStatePayload& p) {
    if (!e.haveLastSentCond) return true;
    return std::memcmp(e.lastSentCond,
                       reinterpret_cast<const uint8_t*>(&p) + kCondOff, kCondBytes) != 0;
}

void NoteSent(coop::atv_sync::AtvEntry& e, const coop::net::AtvStatePayload& p) {
    std::memcpy(e.lastSentCond, reinterpret_cast<const uint8_t*>(&p) + kCondOff, kCondBytes);
    e.haveLastSentCond = true;
}

Counters ReadCounters() {
    Counters c;
    c.applied = g_applied.load();
    c.updTiresCalled = g_updTires.load();
    c.updDirtCalled = g_updDirt.load();
    c.updSpareCalled = g_updSpare.load();
    c.updHealthCalled = g_updHealth.load();
    c.presenceSkippedDiffering = g_presenceSkipped.load();
    c.deferred = g_deferred.load();
    c.invalidBlocks = g_invalid.load();
    return c;
}

}  // namespace coop::atv_condition_sync
