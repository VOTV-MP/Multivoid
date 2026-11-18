// coop/interactables/oven_progress_sync.cpp -- see coop/interactables/oven_progress_sync.h.

#include "coop/interactables/oven_progress_sync.h"

#include "coop/interactables/interactable_sync.h"  // OvenKey: the oven's name on the wire
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/oven_screen.h"
#include "ue_wrap/facts/function_row.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::oven_progress_sync {
namespace {

namespace OS = ue_wrap::oven_screen;
namespace sg = ue_wrap::script_gate;
using Clock = std::chrono::steady_clock;

// The repair screen's own repaint. UI_oven's graph writes a screw's or the lid's bool and calls it at once
// (UI_oven.cpp: TL, TR, BL, BR, lid), so its end is where the screen's mask has just changed.
UE_FACT_FN(kRowUpd, "oven_progress_sync", L"UI_oven_C", L"upd", Declared);
constexpr int kTagUpd = 0x4F564E01;  // 'OVN' 1
bool g_watched = false;
bool g_settled = false;

std::atomic<coop::net::Session*> g_session{nullptr};

// An oven of this world and its screen's mask as this peer last sent or took it.
struct Oven {
    ue_wrap::CachedObjRef ref;
    std::wstring key;
    uint8_t known = 0;
};
std::vector<Oven> g_ovens;

// A mask that named an oven this peer had not found yet (a joiner's replay racing its world load).
struct Parked {
    std::wstring key;
    uint8_t mask = 0;
};
std::vector<Parked> g_parked;
constexpr size_t kMaxParked = 8;

// The scan walks the object array, a few milliseconds of one frame: every 3 s while no oven is known (a world
// still loading), once a minute after, and at once when a known oven dies (a new world).
constexpr auto kScanEvery = std::chrono::seconds(3);
constexpr auto kRescanEvery = std::chrono::seconds(60);
Clock::time_point g_nextScan{};
uint64_t g_raised = 0;

int Bits(uint8_t m) {
    int n = 0;
    for (; m; m &= static_cast<uint8_t>(m - 1)) ++n;
    return n;
}

void Send(coop::net::Session* s, const Oven& o, int onlySlot = -1) {
    coop::net::OvenProgressStatePayload p{};
    coop::net::WireKeyFromString(o.key, p.key);
    p.mask = o.known;
    if (onlySlot >= 0)
        s->SendReliableToSlot(onlySlot, coop::net::ReliableKind::OvenProgressState, &p, sizeof(p));
    else
        s->SendReliable(coop::net::ReliableKind::OvenProgressState, &p, sizeof(p));
}

// Raise on the oven's screen the bits of `mask` it lacks. An oven with no screen is repaired, and
// takes nothing.
void Merge(Oven& o, uint8_t mask, const char* from) {
    void* oven = o.ref.Get();
    uint8_t live = 0;
    if (!oven || !OS::TryReadProgress(oven, live)) return;
    const uint8_t add = static_cast<uint8_t>(mask & OS::kAll & ~live);
    o.known = static_cast<uint8_t>(o.known | live | add);
    if (!add) return;
    if (!OS::RaiseProgress(oven, add)) {
        UE_LOGW("oven_progress: key='%ls' -- the screen did not take mask 0x%02X (%s)", o.key.c_str(), add, from);
        return;
    }
    g_raised += static_cast<uint64_t>(Bits(add));
    UE_LOGI("oven_progress: key='%ls' raised 0x%02X on this screen, now 0x%02X (%s)", o.key.c_str(), add,
            o.known, from);
}

Oven* ByKey(const std::wstring& key) {
    for (auto& o : g_ovens)
        if (o.key == key) return &o;
    return nullptr;
}

// The ovens this world holds now: a dead one is dropped, a found one is added with its screen's mask
// as known, since a screen's first read is its own start and no edit.
void Scan() {
    for (size_t i = g_ovens.size(); i-- > 0;)
        if (!g_ovens[i].ref.Get()) g_ovens.erase(g_ovens.begin() + static_cast<std::ptrdiff_t>(i));
    for (void* oven : OS::FindOvens()) {
        bool have = false;
        for (auto& o : g_ovens) have = have || o.ref.Get() == oven;
        if (have) continue;
        Oven o;
        o.ref.Set(oven);
        o.key = coop::interactable_sync::OvenKey(oven);
        if (o.key.empty() || o.key == L"None") continue;
        OS::TryReadProgress(oven, o.known);
        g_ovens.push_back(std::move(o));
    }
    for (size_t i = g_parked.size(); i-- > 0;) {
        Oven* o = ByKey(g_parked[i].key);
        if (!o) continue;
        Merge(*o, g_parked[i].mask, "parked");
        g_parked.erase(g_parked.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

Oven* ByOven(void* oven) {
    for (auto& o : g_ovens)
        if (o.ref.Get() == oven) return &o;
    return nullptr;
}

// A screen's repaint just ran: the bits it gained since this peer last sent or took its mask go out. The
// screen's own writes and the lane's merges both end here; a merge set `known` first, so it sends nothing.
void OnUpdPost(const sg::Call& c) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    void* oven = OS::OvenOfScreen(c.object);
    if (!oven) return;
    Oven* o = ByOven(oven);
    if (!o) {
        Scan();
        o = ByOven(oven);
    }
    if (!o) {
        UE_LOGW("oven_progress: a screen's step on an oven with no key here -- not sent");
        return;
    }
    uint8_t live = 0;
    if (!OS::TryReadProgress(oven, live) || (live & ~o->known) == 0) return;
    const uint8_t before = o->known;
    o->known = static_cast<uint8_t>(o->known | live);
    if (!s->connected()) return;
    UE_LOGI("oven_progress: key='%ls' local step 0x%02X->0x%02X -- sent", o->key.c_str(), before, o->known);
    Send(s, *o);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_watched) return;
    g_watched = true;
    if (!sg::WatchName(kRowUpd, kTagUpd, nullptr, &OnUpdPost))
        UE_LOGE("oven_progress: the watch on the repair screen's upd was refused -- no step is sent");
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    if (g_watched && !g_settled) {
        sg::ResolvePendingNames();
        if (sg::NameWatchSettled(kRowUpd, kTagUpd)) {
            g_settled = true;
            if (sg::NameWatchLive(kRowUpd, kTagUpd)) UE_LOGI("oven_progress: the watch on the repair screen is live");
            else UE_LOGE("oven_progress: the watch on the repair screen is dead -- no step is sent");
        }
    }
    const auto now = Clock::now();
    for (const auto& o : g_ovens)
        if (!o.ref.Get()) {
            g_nextScan = {};  // its world is gone: scan the new one
            break;
        }
    if (now < g_nextScan) return;
    Scan();
    g_nextScan = now + ((g_ovens.empty() || !g_parked.empty()) ? kScanEvery : kRescanEvery);
}

void OnProgress(const coop::net::OvenProgressStatePayload& p, uint8_t senderSlot) {
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    const uint8_t mask = static_cast<uint8_t>(p.mask & OS::kAll);
    if (key.empty() || !mask) return;
    if (Oven* o = ByKey(key)) {
        char from[24];
        std::snprintf(from, sizeof(from), "slot %u", static_cast<unsigned>(senderSlot));
        Merge(*o, mask, from);
        return;
    }
    for (auto& parked : g_parked)
        if (parked.key == key) { parked.mask |= mask; return; }
    if (g_parked.size() >= kMaxParked) {
        UE_LOGW("oven_progress: key='%ls' mask 0x%02X dropped -- %zu masks already wait for their ovens",
                key.c_str(), mask, g_parked.size());
        return;
    }
    g_parked.push_back(Parked{key, mask});
    g_nextScan = {};  // look for the oven on the next pass
    UE_LOGI("oven_progress: key='%ls' mask 0x%02X parked (no such oven here yet)", key.c_str(), mask);
}

void QueueConnectBroadcastForSlot(int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    for (const auto& o : g_ovens) {
        if (!o.known) continue;
        Send(s, o, slot);
        UE_LOGI("oven_progress: key='%ls' mask 0x%02X -> joiner slot %d", o.key.c_str(), o.known, slot);
    }
}

uint64_t BitsRaised() { return g_raised; }

void OnDisconnect() {
    g_ovens.clear();
    g_parked.clear();
    g_nextScan = {};
}

}  // namespace coop::oven_progress_sync
