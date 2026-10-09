// coop/props/container_birth.cpp -- see coop/props/container_birth.h.

#include "coop/props/container_birth.h"

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
        UE_LOGW("container_birth: slot %u has %zu container transfers in flight -- its oldest is given up",
                static_cast<unsigned>(authorSlot), mine);
        births.erase(oldest);
    }
    Birth b;
    b.actor.Set(actor);
    b.authorSlot = authorSlot;
    b.deadlineMs = NowMs() + kTtlMs;
    births.push_back(std::move(b));
}

}  // namespace

void NoteAuthoredBirth(void* actor) {
    if (IsHost() || !ci::IsContainer(actor)) return;
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
        UE_LOGW("container_birth: slot %u left before the contents of a container it threw came",
                static_cast<unsigned>(slot));
        g_awaited.erase(g_awaited.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

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
        if (live)
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

}  // namespace coop::props::container_birth
