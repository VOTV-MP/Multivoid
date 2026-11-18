// coop/interactables/oven_progress_sync.cpp -- see coop/interactables/oven_progress_sync.h.

#include "coop/interactables/oven_progress_sync.h"

#include "coop/interactables/interactable_sync.h"  // the oven lane's index: key <-> oven
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/oven_screen.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace coop::oven_progress_sync {
namespace {

namespace OS = ue_wrap::oven_screen;
namespace IS = coop::interactable_sync;
namespace sg = ue_wrap::script_gate;

// The repair screen's own repaint. UI_oven's graph writes a screw's or the lid's bool and calls it at once
// (UI_oven.cpp: TL, TR, BL, BR, lid), so its end is where the screen's mask has just changed.
UE_FACT_FN(kRowUpd, "oven_progress_sync", L"UI_oven_C", L"upd", Declared);
constexpr int kTagUpd = 0x4F564E01;  // 'OVN' 1
bool g_watched = false;
bool g_settled = false;

std::atomic<coop::net::Session*> g_session{nullptr};

// An oven's screen mask as this peer last sent or took it, by the key the oven lane indexes it under.
struct Known {
    std::wstring key;
    uint8_t mask = 0;
};
std::vector<Known> g_known;

// A mask that named an oven this peer cannot place yet: the oven is not indexed (a joiner's replay racing its
// world load) or has no screen. It waits for the oven lane's index and is applied when the oven answers.
struct Parked {
    std::wstring key;
    uint8_t mask = 0;
};
std::vector<Parked> g_parked;
constexpr size_t kMaxParked = 8;

// An oven whose screen took a step before the oven lane indexed it, so it had no key to send under: sent when
// the index first names it.
std::vector<ue_wrap::CachedObjRef> g_unsent;

uint64_t g_raised = 0;

int Bits(uint8_t m) {
    int n = 0;
    for (; m; m &= static_cast<uint8_t>(m - 1)) ++n;
    return n;
}

uint8_t& KnownOf(const std::wstring& key) {
    for (auto& k : g_known)
        if (k.key == key) return k.mask;
    g_known.push_back(Known{key, 0});
    return g_known.back().mask;
}

void Send(coop::net::Session* s, const std::wstring& key, uint8_t mask, int onlySlot = -1) {
    coop::net::OvenProgressStatePayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.mask = mask;
    if (onlySlot >= 0)
        s->SendReliableToSlot(onlySlot, coop::net::ReliableKind::OvenProgressState, &p, sizeof(p));
    else
        s->SendReliable(coop::net::ReliableKind::OvenProgressState, &p, sizeof(p));
}

// Raise on the oven's screen the bits of `mask` it lacks. False when the oven has no screen yet (the caller
// keeps the mask); an oven repaired in this save never has one.
bool Merge(void* oven, const std::wstring& key, uint8_t mask, const char* from) {
    uint8_t live = 0;
    if (!OS::TryReadProgress(oven, live)) return false;
    const uint8_t add = static_cast<uint8_t>(mask & OS::kAll & ~live);
    // Set before the raise: the screen's own upd, which the raise runs, then finds nothing new to send.
    const uint8_t now = static_cast<uint8_t>(KnownOf(key) | live | add);
    KnownOf(key) = now;
    if (!add) return true;
    if (!OS::RaiseProgress(oven, add)) {
        UE_LOGW("oven_progress: key='%ls' -- the screen did not take mask 0x%02X (%s)", key.c_str(), add, from);
        return true;
    }
    g_raised += static_cast<uint64_t>(Bits(add));
    UE_LOGI("oven_progress: key='%ls' raised 0x%02X on this screen, now 0x%02X (%s)", key.c_str(), add, now, from);
    return true;
}

// The screen's mask as it stands, sent under `key` when it holds bits this peer has not sent or taken.
void SendLive(coop::net::Session* s, void* oven, const std::wstring& key) {
    uint8_t live = 0;
    if (!OS::TryReadProgress(oven, live)) return;
    const uint8_t before = KnownOf(key);
    if ((live & ~before) == 0) return;
    const uint8_t now = static_cast<uint8_t>(before | live);
    KnownOf(key) = now;
    if (!s->connected()) return;
    UE_LOGI("oven_progress: key='%ls' local step 0x%02X->0x%02X -- sent", key.c_str(), before, now);
    Send(s, key, now);
}

// A screen's repaint just ran: the bits it gained since this peer last sent or took its mask go out. The
// screen's own writes and the lane's merges both end here; a merge set `known` first, so it sends nothing.
void OnUpdPost(const sg::Call& c) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    void* oven = OS::OvenOfScreen(c.object);
    if (!oven) return;
    const std::wstring key = IS::OvenKey(oven);
    if (key.empty()) {
        for (const auto& u : g_unsent)
            if (u.Get() == oven) return;
        ue_wrap::CachedObjRef ref;
        ref.Set(oven);
        g_unsent.push_back(ref);
        UE_LOGI("oven_progress: a screen's step on an oven the oven lane has not indexed yet -- sent once it has");
        return;
    }
    SendLive(s, oven, key);
}

// The oven lane's index was rebuilt: an oven it names now has a key. What waited for one is placed.
void OnOvenIndexed() {
    if (g_parked.empty() && g_unsent.empty()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    for (size_t i = g_parked.size(); i-- > 0;) {
        void* oven = IS::OvenActor(g_parked[i].key);
        if (!oven || !Merge(oven, g_parked[i].key, g_parked[i].mask, "parked")) continue;
        g_parked.erase(g_parked.begin() + static_cast<std::ptrdiff_t>(i));
    }
    for (size_t i = g_unsent.size(); i-- > 0;) {
        void* oven = g_unsent[i].Get();
        const std::wstring key = oven ? IS::OvenKey(oven) : std::wstring();
        if (oven && key.empty()) continue;
        if (oven && s && s->running()) SendLive(s, oven, key);
        g_unsent.erase(g_unsent.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_watched) return;
    g_watched = true;
    if (!sg::WatchName(kRowUpd, kTagUpd, nullptr, &OnUpdPost))
        UE_LOGE("oven_progress: the watch on the repair screen's upd was refused -- no step is sent");
    IS::SetOvenIndexedListener(&OnOvenIndexed);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !g_watched || g_settled) return;
    sg::ResolvePendingNames();
    if (!sg::NameWatchSettled(kRowUpd, kTagUpd)) return;
    g_settled = true;
    if (sg::NameWatchLive(kRowUpd, kTagUpd)) UE_LOGI("oven_progress: the watch on the repair screen is live");
    else UE_LOGE("oven_progress: the watch on the repair screen is dead -- no step is sent");
}

void OnProgress(const coop::net::OvenProgressStatePayload& p, uint8_t senderSlot) {
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    const uint8_t mask = static_cast<uint8_t>(p.mask & OS::kAll);
    if (key.empty() || !mask) return;
    if (void* oven = IS::OvenActor(key)) {
        char from[24];
        std::snprintf(from, sizeof(from), "slot %u", static_cast<unsigned>(senderSlot));
        if (Merge(oven, key, mask, from)) return;
    }
    for (auto& parked : g_parked)
        if (parked.key == key) { parked.mask |= mask; return; }
    if (g_parked.size() >= kMaxParked) {
        UE_LOGW("oven_progress: key='%ls' mask 0x%02X dropped -- %zu masks already wait for their ovens",
                key.c_str(), mask, g_parked.size());
        return;
    }
    g_parked.push_back(Parked{key, mask});
    UE_LOGI("oven_progress: key='%ls' mask 0x%02X parked (no indexed oven with a screen here yet)", key.c_str(), mask);
}

void QueueConnectBroadcastForSlot(int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    std::vector<std::pair<std::wstring, void*>> ovens;
    IS::LiveOvens(ovens);
    for (const auto& o : ovens) {
        uint8_t live = 0;
        if (!OS::TryReadProgress(o.second, live) || !live) continue;
        KnownOf(o.first) |= live;
        Send(s, o.first, live, slot);
        UE_LOGI("oven_progress: key='%ls' mask 0x%02X -> joiner slot %d", o.first.c_str(), live, slot);
    }
}

uint64_t BitsRaised() { return g_raised; }

void OnDisconnect() {
    g_known.clear();
    g_parked.clear();
    g_unsent.clear();
}

}  // namespace coop::oven_progress_sync
