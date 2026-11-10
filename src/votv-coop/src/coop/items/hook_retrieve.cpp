// coop/items/hook_retrieve.cpp -- see coop/items/hook_retrieve.h.

#include "coop/items/hook_retrieve.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/intent_authority.h"   // IntentTarget: an end within the sender's reach
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/actors/hook.h"
#include "ue_wrap/actors/prop.h"              // GetActorSaveKeyString
#include "ue_wrap/actors/prop_use.h"          // CallActionOptionIndex: the granted pickup's replay
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include "hook_sync_detail.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace coop::hook_retrieve {
namespace {

namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;
namespace H  = ue_wrap::hook;
namespace PR = ue_wrap::prop;
namespace sg = ue_wrap::script_gate;
namespace D  = coop::hook_sync::detail;
using coop::net::HookRetrievePayload;

// The hook's one action, hook_C::actionOptionIndex, which every hook class inherits. The any-class row the door, toggle
// and coin lanes already watch: the callback keeps hooks alone.
UE_FACT_FN(kRowActionOptionIndex, "hook_retrieve", nullptr, L"actionOptionIndex", AnyClass);
constexpr int kTag = 0x484B5254;  // 'HKRT'
// The look that offers the pickup is the player's interaction trace, armLength long (200); the pads for the puppet's
// lag are the authority's own. Either end within it is enough: a player takes a hook by whichever sphere it looks at.
constexpr float kReachUU = 200.0f;
// CLIENT: one request per hook in this window; a press the host refused can be pressed again after it.
constexpr uint64_t kRepeatMs = 2000;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watchRegistered = false;
bool g_watchSettled = false;

// CLIENT: the last hook asked for, and when.
std::wstring g_pendingKey;
uint64_t     g_pendingMs = 0;
// CLIENT: the copy whose pickup the host granted, while the game's own pickup runs on it.
void* g_replay = nullptr;
// HOST: the hook the host player's own pickup is running on, between the watch's entry and exit.
void*        g_hostPress = nullptr;
std::wstring g_hostPressKey;

uint64_t g_sent = 0, g_granted = 0, g_refused = 0, g_taken = 0;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string Red() {
    static const std::string r = coop::config::ResolveEnum(::coop::config_registry::rows::hook_retrieve_red);
    return r;
}

// A hook this peer still owns -- its own, before the second end anchored -- is the game's to take back here: the owner
// lane announces the destroy.
bool OwnedHere(void* hook) {
    for (const auto& o : D::OwnedHooks())
        if (o.ref.Raw() == hook) return true;
    return false;
}

std::wstring KeyOf(void* hook) {
    std::wstring k = PR::GetActorSaveKeyString(hook);
    return (k.empty() || k == L"None") ? std::wstring() : k;
}

void SendGone(coop::net::Session& s, const std::wstring& key, uint8_t taker) {
    HookRetrievePayload g{};
    coop::net::WireKeyFromString(key, g.key);
    g.op    = coop::net::kHookRetrieveGone;
    g.taker = taker;
    if (!s.SendReliable(coop::net::ReliableKind::HookRetrieve, &g, sizeof(g)))
        UE_LOGW("hook_retrieve: the word that hook '%ls' is gone did not send -- the clients keep their copies",
                key.c_str());
}

void Refuse(uint8_t slot, const std::wstring& key, const char* why) {
    ++g_refused;
    if (g_refused <= 20 || g_refused % 50 == 0)
        UE_LOGI("hook_retrieve: slot %u's pickup of hook '%ls' refused (%s)", static_cast<unsigned>(slot), key.c_str(),
                why);
}

sg::Verdict OnActionPre(const sg::Call& c) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !H::IsHookFamily(c.object)) return sg::Verdict::Run;
    if (c.object == g_replay) return sg::Verdict::Run;   // the host's grant, replayed
    if (OwnedHere(c.object)) return sg::Verdict::Run;
    const std::wstring key = KeyOf(c.object);
    if (s->role() == coop::net::Role::Host) {
        // The host's own pickup is the game's. Its hook goes, and the clients are told after the body ran.
        if (!key.empty()) { g_hostPress = c.object; g_hostPressKey = key; }
        return sg::Verdict::Run;
    }
    if (Red() == "clientruns") return sg::Verdict::Run;
    if (key.empty() || key.size() > sizeof(coop::net::WireKey::data)) {
        UE_LOGW("hook_retrieve: this client's hook %p has no save key the wire can carry -- not taken (the host's "
                "stays)", c.object);
        return sg::Verdict::Cancel;
    }
    const uint64_t now = NowMs();
    if (key == g_pendingKey && now - g_pendingMs < kRepeatMs) return sg::Verdict::Cancel;
    HookRetrievePayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.op    = coop::net::kHookRetrieveRequest;
    p.taker = coop::net::kHookRetrieveNoTaker;
    if (!s->SendReliableToSlot(0, coop::net::ReliableKind::HookRetrieve, &p, sizeof(p))) {
        UE_LOGW("hook_retrieve: the pickup of hook '%ls' was not sent -- the hook stays; use it again", key.c_str());
        return sg::Verdict::Cancel;
    }
    g_pendingKey = key;
    g_pendingMs  = now;
    ++g_sent;
    UE_LOGI("hook_retrieve: this client took hook '%ls' -- asked of the host (%llu asked)", key.c_str(),
            static_cast<unsigned long long>(g_sent));
    return sg::Verdict::Cancel;
}

void OnActionPost(const sg::Call& c) {
    if (!g_hostPress || c.object != g_hostPress) return;
    const std::wstring key = std::move(g_hostPressKey);
    g_hostPress = nullptr;
    g_hostPressKey.clear();
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    SendGone(*s, key, coop::net::kHookRetrieveNoTaker);
    UE_LOGI("hook_retrieve: the host player took hook '%ls' -- the clients are told", key.c_str());
}

// HOST: a client's pickup.
void OnRequest(coop::net::Session& s, const std::wstring& key, uint8_t slot) {
    void* hook = D::FindHookByKey(key);
    // No hook is the ordinary end of a second press, or of two players taking one hook.
    if (!hook) { Refuse(slot, key, "no hook of that key here -- already taken"); return; }
    ue_wrap::FVector a{}, b{};
    if (!H::EndLocations(hook, a, b)) {
        if (!E::TryGetActorLocation(hook, a)) { Refuse(slot, key, "the hook's ends do not read"); return; }
        b = a;
    }
    const auto tok = coop::element::IntentTarget::ForClientIntent(s, slot, kReachUU);
    const auto atA = tok.AuthorizeSegment(a, a);
    if (!atA) {
        const auto atB = tok.AuthorizeSegment(b, b);
        if (!atB) {
            Refuse(slot, key, coop::element::OutcomeName(atA.outcome));
            return;
        }
    }
    // Taken away without the game's drop: the item is the taker's, and its own copy makes it.
    ue_wrap::CachedObjRef incarnation;
    incarnation.Set(hook);
    E::DestroyActor(hook);
    if (incarnation.Alive()) { Refuse(slot, key, "the hook is still live after its destroy"); return; }
    ++g_granted;
    SendGone(s, key, slot);
    UE_LOGI("hook_retrieve: HOST granted slot %u its pickup of hook '%ls' (%llu granted)", static_cast<unsigned>(slot),
            key.c_str(), static_cast<unsigned long long>(g_granted));
}

// CLIENT: the hook is gone on the host. The taker's copy runs the game's own pickup; every other copy goes.
void OnGone(const std::wstring& key, uint8_t taker) {
    if (key == g_pendingKey) g_pendingKey.clear();
    void* copy = D::FindHookByKey(key);
    const bool mine = taker == coop::players::Registry::Get().LocalPeerId();
    if (!mine) {
        if (copy) E::DestroyActor(copy);
        return;
    }
    void* player = coop::players::Registry::Get().Local();
    if (!player) {
        UE_LOGW("hook_retrieve: the host granted this client hook '%ls' with no local player -- no item", key.c_str());
        if (copy) E::DestroyActor(copy);
        return;
    }
    if (copy) {
        // hook_C::actionOptionIndex reads only the player: drophook spawns the item at them, putObjectInventory2
        // pockets it (or leaves it on the floor when the inventory is full, as the game does), and the hook destroys
        // itself.
        ue_wrap::CachedObjRef ref;
        ref.Set(copy);
        g_replay = copy;
        const bool ran = ue_wrap::prop_use::CallActionOptionIndex(copy, player, nullptr, ue_wrap::FVector{}, 4, nullptr);
        g_replay = nullptr;
        if (ref.Alive()) E::DestroyActor(copy);
        if (ran) {
            ++g_taken;
            UE_LOGI("hook_retrieve: this client's pickup of hook '%ls' granted -- the game's own pickup ran on its copy",
                    key.c_str());
            return;
        }
        UE_LOGW("hook_retrieve: the pickup of hook '%ls' did not dispatch on this client's copy -- the item is "
                "made by hand", key.c_str());
    }
    // No copy to run it on: the item the game's pickup would have made, made here.
    void* cls = ue_wrap::object_index::ClassByName(L"prop_hook_C");
    ue_wrap::FVector at{};
    void* item = (cls && E::TryGetActorLocation(player, at)) ? E::SpawnActor(cls, at) : nullptr;
    bool pocketed = false;
    if (item) {
        ue_wrap::CachedObjRef ref;
        ref.Set(item);
        E::CallPutObjectInventory2(player, item, false, pocketed);
        if (pocketed && ref.Alive()) E::DestroyActor(item);   // the store holds its record now, as the game's pickup leaves it
    }
    ++g_taken;
    UE_LOGW("hook_retrieve: hook '%ls' was granted to this client with no copy here -- its item %s", key.c_str(),
            !item ? "could not be made" : pocketed ? "was put in the inventory" : "lies at the player");
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    if (g_watchSettled) return;
    if (!g_watchRegistered) {
        g_watchRegistered = sg::WatchName(kRowActionOptionIndex, kTag, &OnActionPre, &OnActionPost);
        if (!g_watchRegistered) {
            g_watchSettled = true;
            UE_LOGE("hook_retrieve: the gate took no watch on %ls -- a client's pickup of an anchored hook stays on "
                    "its own screen", kRowActionOptionIndex.Name());
            return;
        }
    }
    sg::ResolvePendingNames();
    if (!sg::NameWatchSettled(kRowActionOptionIndex, kTag)) return;
    g_watchSettled = true;
    if (sg::NameWatchLive(kRowActionOptionIndex, kTag))
        UE_LOGI("hook_retrieve: the watch on %ls is live", kRowActionOptionIndex.Name());
    else
        UE_LOGE("hook_retrieve: the watch on %ls settled dead -- a client's pickup of an anchored hook stays on its "
                "own screen", kRowActionOptionIndex.Name());
}

void OnMsg(const HookRetrievePayload& p, int senderPeerSlot) {
    if (!GT::IsGameThread()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    if (senderPeerSlot < 0 || senderPeerSlot >= coop::players::kMaxPeers) return;
    if (!H::EnsureResolved()) return;
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    if (key.empty()) return;
    if (p.op == coop::net::kHookRetrieveRequest) {
        if (s->role() != coop::net::Role::Host || senderPeerSlot == 0) return;
        OnRequest(*s, key, static_cast<uint8_t>(senderPeerSlot));
        return;
    }
    if (p.op == coop::net::kHookRetrieveGone) {
        if (s->role() != coop::net::Role::Client || senderPeerSlot != 0) return;   // the host's word alone
        OnGone(key, p.taker);
    }
}

void PrepareAnchoredCopy(void* mirror) {
    if (!mirror) return;
    E::SetActorEnableCollision(mirror, Red() != "nocollision");
}

uint64_t GrantedCount() { return g_granted; }

void OnDisconnect() {
    if (g_sent || g_granted || g_refused || g_taken)
        UE_LOGI("hook_retrieve: session end -- asked=%llu granted=%llu refused=%llu taken=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_granted),
                static_cast<unsigned long long>(g_refused), static_cast<unsigned long long>(g_taken));
    g_sent = g_refused = g_taken = 0;
    g_pendingKey.clear();
    g_pendingMs = 0;
    g_replay = nullptr;
    g_hostPress = nullptr;
    g_hostPressKey.clear();
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::hook_retrieve
