// coop/interactables/oven_progress_sync.cpp -- see coop/interactables/oven_progress_sync.h.

#include "coop/interactables/oven_progress_sync.h"

#include "coop/config/config.h"                    // [dev] oven_merge_off
#include "coop/config/config_registry.h"
#include "coop/interactables/interactable_sync.h"  // the oven lane's index: key <-> oven
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/appliance.h"  // TryReadOvenFixed
#include "ue_wrap/devices/oven_screen.h"
#include "ue_wrap/engine/world_identity.h"
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
// The oven's own BeginPlay creates its repair screen (kitchen.cpp: ReceiveBeginPlay creates the widget and sets its
// `kitchen`), so its end is the moment a screen first exists to take a mask.
UE_FACT_FN(kRowBegin, "oven_progress_sync", L"kitchen_C", L"ReceiveBeginPlay", Declared);
constexpr int kTagBegin = 0x4F564E02;  // 'OVN' 2
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
// world load) or has no screen. It is applied when the oven lane's index names the oven with a screen, or when
// the oven's BeginPlay makes the screen, and dropped, with a line, when the world changes.
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
uint64_t g_arrived = 0;

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

// To every peer but `skipSlot`, or to `onlySlot` alone.
void Send(coop::net::Session* s, const std::wstring& key, uint8_t mask, int onlySlot = -1, int skipSlot = -1) {
    coop::net::OvenProgressStatePayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.mask = mask;
    if (onlySlot >= 0)
        s->SendReliableToSlot(onlySlot, coop::net::ReliableKind::OvenProgressState, &p, sizeof(p));
    else
        s->SendReliable(coop::net::ReliableKind::OvenProgressState, &p, sizeof(p), skipSlot);
}

// Take the bits of `mask` the screen lacks: the record first, then the screen's bools. False when the oven has no
// screen yet and `screenless` is not set (the caller keeps the mask); an oven repaired in this save never has one.
// `screenless` is the host's judged grant: its record is the state, so an oven with no screen records the bits and
// raises none.
bool Merge(void* oven, const std::wstring& key, uint8_t mask, const char* from, bool screenless = false) {
    uint8_t live = 0;
    const bool screen = OS::TryReadProgress(oven, live);
    if (!screen && !screenless) return false;
    const uint8_t add = static_cast<uint8_t>(mask & OS::kAll & ~live);
    if (add) ++g_arrived;
    // [dev] oven_merge_off, the red arm of coop/dev/oven_progress_drill: the arrival is counted and the bit is not raised.
    static const bool sMergeOff = coop::config::ResolveFlag(coop::config_registry::rows::oven_merge_off);
    if (add && sMergeOff) {
        UE_LOGW("oven_progress: [dev] oven_merge_off -- key='%ls' mask 0x%02X not raised (%s)", key.c_str(), add, from);
        return true;
    }
    // Set before the raise: the screen's own upd, which the raise runs, then finds nothing new to send.
    const uint8_t now = static_cast<uint8_t>(KnownOf(key) | live | add);
    KnownOf(key) = now;
    if (!add) return true;
    if (!screen) {
        UE_LOGI("oven_progress: key='%ls' recorded 0x%02X, now 0x%02X -- no screen to raise it on (%s)", key.c_str(), add,
                now, from);
        return true;
    }
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
    // This peer's own unsent steps go first: a parked mask merged before them would count them as known, and they would never be sent.
    for (size_t i = g_unsent.size(); i-- > 0;) {
        void* oven = g_unsent[i].Get();
        const std::wstring key = oven ? IS::OvenKey(oven) : std::wstring();
        if (oven && key.empty()) continue;
        if (oven && s && s->running()) SendLive(s, oven, key);
        g_unsent.erase(g_unsent.begin() + static_cast<std::ptrdiff_t>(i));
    }
    for (size_t i = g_parked.size(); i-- > 0;) {
        void* oven = IS::OvenActor(g_parked[i].key);
        if (!oven || !Merge(oven, g_parked[i].key, g_parked[i].mask, "parked")) continue;
        g_parked.erase(g_parked.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

// An oven's BeginPlay ended, so its screen exists: a mask parked for it because it had none is applied now. An oven
// the oven lane has not indexed yet has no key to match; the index's rebuild places its mask.
void OnBeginPlayPost(const sg::Call& c) {
    if (g_parked.empty() || !c.object) return;
    const std::wstring key = IS::OvenKey(c.object);
    if (key.empty()) return;
    for (size_t i = 0; i < g_parked.size(); ++i) {
        if (g_parked[i].key != key) continue;
        if (Merge(c.object, key, g_parked[i].mask, "parked, screen made"))
            g_parked.erase(g_parked.begin() + static_cast<std::ptrdiff_t>(i));
        return;
    }
}

// What this peer kept for the world that went. A parked mask whose oven never appeared in it has no oven to wait
// for any more.
void Forget(const char* why) {
    if (!g_parked.empty())
        UE_LOGI("oven_progress: %zu parked mask(s) dropped (%s): their ovens are not in this world", g_parked.size(),
                why);
    g_known.clear();
    g_parked.clear();
    g_unsent.clear();
}

void OnWorldChange() noexcept { Forget("world change"); }

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_watched) return;
    g_watched = true;
    const bool u = sg::WatchName(kRowUpd, kTagUpd, nullptr, &OnUpdPost);
    const bool b = sg::WatchName(kRowBegin, kTagBegin, nullptr, &OnBeginPlayPost);
    if (!u || !b)
        UE_LOGE("oven_progress: a watch was refused (screen upd=%d oven begin=%d)", u ? 1 : 0, b ? 1 : 0);
    IS::SetOvenIndexedListener(&OnOvenIndexed);
    if (!ue_wrap::world_identity::AddWorldChangeListener(&OnWorldChange))
        UE_LOGE("oven_progress: the world-change list is full -- parked masks are dropped at the session's end only");
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !g_watched || g_settled) return;
    sg::ResolvePendingNames();
    if (!sg::NameWatchSettled(kRowUpd, kTagUpd) || !sg::NameWatchSettled(kRowBegin, kTagBegin)) return;
    g_settled = true;
    const bool u = sg::NameWatchLive(kRowUpd, kTagUpd), b = sg::NameWatchLive(kRowBegin, kTagBegin);
    if (u && b) UE_LOGI("oven_progress: the watches on the repair screen and the oven's start are live");
    else UE_LOGE("oven_progress: a watch is dead (screen upd=%d oven begin=%d) -- the steps are not kept", u ? 1 : 0,
                 b ? 1 : 0);
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
        // The record is the state: a granted step whose raise failed, or an oven with no screen open, still reaches the
        // joiner. A repaired oven has no steps to take, so it sends none.
        bool fixed = false;
        if (ue_wrap::appliance::TryReadOvenFixed(o.second, fixed) && fixed) continue;
        uint8_t live = 0;
        OS::TryReadProgress(o.second, live);
        const uint8_t now = static_cast<uint8_t>(KnownOf(o.first) | live);
        if (!now) continue;
        KnownOf(o.first) = now;
        Send(s, o.first, now, slot);
        UE_LOGI("oven_progress: key='%ls' mask 0x%02X -> joiner slot %d", o.first.c_str(), now, slot);
    }
}

uint8_t KnownMask(const std::wstring& key) {
    for (const auto& k : g_known)
        if (k.key == key) return k.mask;
    return 0;
}

void GrantSteps(coop::net::Session& s, void* oven, const std::wstring& key, uint8_t add, uint8_t exceptSlot) {
    char from[24];
    std::snprintf(from, sizeof(from), "granted, slot %u", static_cast<unsigned>(exceptSlot));
    Merge(oven, key, add, from, true);
    // The asker's own screen holds what it asked; the others take the record's whole mask.
    if (const uint8_t now = KnownMask(key)) Send(&s, key, now, -1, exceptSlot);
}

void ResendCumulative(coop::net::Session& s, void* oven, const std::wstring& key) {
    uint8_t live = 0;
    OS::TryReadProgress(oven, live);
    const uint8_t now = static_cast<uint8_t>(KnownOf(key) | live);
    KnownOf(key) = now;
    if (!now) return;
    UE_LOGI("oven_progress: key='%ls' mask 0x%02X sent again ahead of the repair ask", key.c_str(), now);
    Send(&s, key, now);
}

uint64_t BitsRaised() { return g_raised; }

uint64_t MasksArrived() { return g_arrived; }

void OnDisconnect() { Forget("session end"); }

}  // namespace coop::oven_progress_sync
