// coop/props/container_birth.cpp -- see coop/props/container_birth.h.

#include "coop/props/container_birth.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"

#include "coop/element/registry.h"
#include "coop/props/container_slice_wire.h"
#include "coop/props/prop_element_tracker.h"   // SessionIsHost
#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"

#include <chrono>
#include <map>
#include <vector>

namespace coop::props::container_birth {
namespace {

namespace ci = ue_wrap::container_inventory;
namespace cw = coop::props::container_slice_wire;

// The actor is held across ticks, so it is a CachedObjRef: a successor in the same slot at the same
// address reads as dead rather than as the thrown container.
struct Birth {
    ue_wrap::CachedObjRef actor;
    uint8_t  authorSlot = 0;   // host: the author it awaits; client: 0, this peer
    uint64_t deadlineMs = 0;
};
std::vector<Birth> g_authored;          // client
std::set<uint32_t> g_birthEids;         // client: bound births whose slice is owed
std::vector<Birth> g_awaited;           // host
std::set<uint32_t> g_hostBirths;        // host: births with contents, published by the next sweep
std::map<uint32_t, uint64_t> g_owedFirst;   // eid -> deadline of its first publication

// The selftest drives the real functions, so it would otherwise print an eviction, a departure and an
// expiry into the session log -- lines a reader is supposed to treat as defects. Quiet for its
// duration only, as container_park's selftest is.
bool g_quiet = false;

uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool IsHost() { return coop::prop_element_tracker::SessionIsHost(); }

void Push(std::vector<Birth>& births, void* actor, uint8_t authorSlot) {
    size_t mine = 0;
    auto oldest = births.end();
    for (auto it = births.begin(); it != births.end(); ++it) {
        if (it->authorSlot != authorSlot) continue;
        if (oldest == births.end()) oldest = it;   // pushed in order, so the first is the oldest
        ++mine;
    }
    if (mine >= kMaxPerAuthor) {
        if (!g_quiet)
            UE_LOGW("container_birth: slot %u has %zu container transfers in flight -- its oldest is "
                    "given up", static_cast<unsigned>(authorSlot), mine);
        births.erase(oldest);
    }
    Birth b;
    b.actor.Set(actor);
    b.authorSlot = authorSlot;
    b.deadlineMs = NowMs() + kTtlMs;
    births.push_back(std::move(b));
}

uint32_t g_endedGone = 0, g_endedExpired = 0;   // the throw drill's counts; process-long, the drill baselines

}  // namespace

void NoteAuthoredBirth(void* actor) {
    if (IsHost() || !ci::IsContainer(actor)) return;
    // The throw drill's red and gone: the slice is never sent, so the host's copy stays empty and its
    // transfer waits for a slice that does not come.
    static const bool s_skip = coop::config::ResolveFlag(coop::config_registry::rows::container_birth_skip_slice);
    if (s_skip) {
        UE_LOGW("container_birth: DRILL -- CLIENT threw container %p and sends no slice "
                "(dev.container_birth_skip_slice)", actor);
        return;
    }
    void* inv = ci::InventoryOf(actor);
    if (!inv || !ci::IsWorldInventory(inv)) {
        UE_LOGW("container_birth: CLIENT threw container %p, but its inventory %s -- its contents stay on "
                "this peer only", actor, inv ? "is not a world container's" : "does not resolve");
        return;
    }
    Push(g_authored, actor, 0);
    UE_LOGI("container_birth: CLIENT threw container %p -- its contents go to the host once bound", actor);
}

void ExpectBirthSlice(void* actor, uint8_t authorSlot) {
    if (!IsHost() || !ci::IsContainer(actor)) return;
    Push(g_awaited, actor, authorSlot);
}

void NoteHostBirth(void* actor) {
    if (!IsHost() || !ci::IsContainer(actor)) return;
    if (Awaited(actor, 0)) return;   // built from a client's intent: its author holds the contents
    void* inv = ci::InventoryOf(actor);
    if (!inv || !ci::IsWorldInventory(inv)) return;
    const auto eid = coop::element::Registry::Get().EidForActor(actor);
    if (eid == coop::element::kInvalidId) return;
    std::vector<ue_wrap::save_record::SaveRecord> recs;
    const bool readable = ci::ReadContents(inv, recs, cw::kMaxRecords);
    if (readable && recs.empty()) return;   // the mirrors are born empty too
    // Not readable yet is not empty: the publication is owed and retried until it is.
    if (!readable) g_owedFirst[static_cast<uint32_t>(eid)] = NowMs() + kTtlMs;
    g_hostBirths.insert(static_cast<uint32_t>(eid));
}

void OnPeerGone(uint8_t slot) {
    // A transfer is bound to the peer that threw: a later occupant of the slot does not complete it.
    for (size_t i = 0; i < g_awaited.size();) {
        if (g_awaited[i].authorSlot != slot) { ++i; continue; }
        if (!g_quiet) {
            ++g_endedGone;
            UE_LOGW("container_birth: slot %u left before the contents of a container it threw came",
                    static_cast<unsigned>(slot));
        }
        g_awaited.erase(g_awaited.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

uint32_t EndedGone() { return g_endedGone; }
uint32_t EndedExpired() { return g_endedExpired; }

// The host never publishes an awaited container, so nothing a slice could have been based on exists.
bool Awaited(void* actor, uint8_t authorSlot) {
    if (g_awaited.empty() || !actor) return false;
    const uint64_t now = NowMs();
    for (const Birth& b : g_awaited)
        if (b.actor.Is(actor) && (authorSlot == 0 || b.authorSlot == authorSlot) && now < b.deadlineMs)
            return true;
    return false;
}

void Complete(void* actor) {
    for (size_t i = 0; i < g_awaited.size(); ++i) {
        if (!g_awaited[i].actor.Is(actor)) continue;
        g_awaited.erase(g_awaited.begin() + static_cast<std::ptrdiff_t>(i));
        return;
    }
}

void Sweep(uint64_t nowMs, std::set<uint32_t>& dirty) {
    for (size_t i = 0; i < g_authored.size();) {
        const Birth& b = g_authored[i];
        if (!b.actor.Alive() || nowMs >= b.deadlineMs) {
            g_authored.erase(g_authored.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        const auto eid = coop::element::Registry::Get().EidForActor(b.actor.Get());
        if (eid == coop::element::kInvalidId) { ++i; continue; }
        g_birthEids.insert(static_cast<uint32_t>(eid));
        g_owedFirst[static_cast<uint32_t>(eid)] = b.deadlineMs;
        dirty.insert(static_cast<uint32_t>(eid));
        g_authored.erase(g_authored.begin() + static_cast<std::ptrdiff_t>(i));
    }
    for (size_t i = 0; i < g_awaited.size();) {
        const Birth& b = g_awaited[i];
        const bool live = b.actor.Alive();
        if (live && nowMs < b.deadlineMs) { ++i; continue; }
        if (live && !g_quiet) ++g_endedExpired;
        if (live && !g_quiet)
            UE_LOGW("container_birth: the contents of a container slot %u threw never came -- the host's "
                    "copy stays as it built it, and the thrower's own contents are refused from now on and "
                    "lost to it", static_cast<unsigned>(b.authorSlot));
        g_awaited.erase(g_awaited.begin() + static_cast<std::ptrdiff_t>(i));
    }
    if (!g_hostBirths.empty()) {
        dirty.insert(g_hostBirths.begin(), g_hostBirths.end());
        g_hostBirths.clear();
    }
}

bool IsBirthSlice(uint32_t eid) { return g_birthEids.count(eid) != 0; }

Owed FirstOwed(uint32_t eid, uint64_t nowMs) {
    auto it = g_owedFirst.find(eid);
    if (it == g_owedFirst.end()) return Owed::None;
    if (nowMs < it->second) return Owed::Pending;
    g_owedFirst.erase(it);
    g_birthEids.erase(eid);
    return Owed::GivenUp;
}

void FirstSent(uint32_t eid) {
    g_owedFirst.erase(eid);
    g_birthEids.erase(eid);
}

void Forget(uint32_t eid) {
    g_owedFirst.erase(eid);
    g_birthEids.erase(eid);
}

void Reset() {
    g_authored.clear();
    g_birthEids.clear();
    g_awaited.clear();
    g_hostBirths.clear();
    g_owedFirst.clear();
}

bool RunSelftest() {
    Reset();
    g_quiet = true;
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("container_birth selftest FAIL: %s", what);
    };
    auto held = [](uint8_t slot) {
        size_t n = 0;
        for (const Birth& b : g_awaited) n += (b.authorSlot == slot) ? 1 : 0;
        return n;
    };
    // Tagged by its deadline, so which transfer an eviction took is visible. No actor: the selftest
    // runs on the session thread, where no engine object may be touched, and the cap never reads one.
    // A live transfer's deadline and its completion are the thrown-container drill's arms.
    auto push = [](uint8_t slot, uint64_t tag) {
        Push(g_awaited, nullptr, slot);
        g_awaited.back().deadlineMs = tag;
    };

    // An author over its cap gives up its OWN oldest, never another author's.
    push(2, 1);
    for (uint64_t i = 0; i < kMaxPerAuthor; ++i) push(1, 100 + i);
    push(1, 999);
    check(held(1) == kMaxPerAuthor, "an author over its cap holds exactly the cap");
    check(held(2) == 1, "another author's transfer survives the eviction");
    bool oldestGone = true, newestHeld = false;
    for (const Birth& b : g_awaited) {
        if (b.authorSlot == 1 && b.deadlineMs == 100) oldestGone = false;
        if (b.authorSlot == 1 && b.deadlineMs == 999) newestHeld = true;
    }
    check(oldestGone && newestHeld, "the transfer given up is that author's oldest");

    // A departing author's transfers end, and only its; a gone container's ends at the next sweep.
    OnPeerGone(1);
    check(held(1) == 0 && held(2) == 1, "a departing author's transfers end, and only its");
    std::set<uint32_t> dirty;
    Sweep(0, dirty);
    check(g_awaited.empty() && dirty.empty(), "a transfer whose container is gone ends at the sweep");

    // A first publication stays owed until its deadline and is then given up, once, with its birth
    // mark; a sent one and a forgotten one are owed nothing.
    g_owedFirst[7] = 5000;
    g_birthEids.insert(7);
    check(FirstOwed(7, 4999) == Owed::Pending && IsBirthSlice(7), "an owed first publication is pending");
    check(FirstOwed(7, 5000) == Owed::GivenUp, "it is given up at its deadline");
    check(FirstOwed(7, 5000) == Owed::None && !IsBirthSlice(7), "given up once, its birth mark with it");
    g_owedFirst[8] = 5000;
    g_birthEids.insert(8);
    FirstSent(8);
    check(FirstOwed(8, 0) == Owed::None && !IsBirthSlice(8), "a sent first publication is owed no more");
    g_owedFirst[9] = 5000;
    Forget(9);
    check(FirstOwed(9, 0) == Owed::None, "a container that no longer resolves owes nothing");

    Reset();
    g_quiet = false;
    if (pass == total) {
        UE_LOGI("container_birth selftest: ALL PASS (%d checks)", total);
        return true;
    }
    UE_LOGE("container_birth selftest: %d/%d checks passed", pass, total);
    return false;
}

}  // namespace coop::props::container_birth
