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
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"  // Generation: the class table's stamp
#include "ue_wrap/facts/function_row.h"

#include "hook_sync_detail.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::hook_retrieve {
namespace {

namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
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

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watchRegistered = false;
bool g_watchSettled = false;

// CLIENT: the hooks asked of the host and not yet answered, by save key. A press on one of them is refused without a
// second request; the host's Gone or Refused for the key, or the session's end, releases it. A handful at most.
std::vector<std::wstring> g_inFlight;
// CLIENT: the copy whose pickup the host granted, while the game's own pickup runs on it.
void* g_replay = nullptr;
// HOST: the hook the host player's own pickup is running on, between the watch's entry and exit.
void*        g_hostPress = nullptr;
std::wstring g_hostPressKey;

// g_sent, g_refusedAnswers and g_taken are the client's: requests asked, refusals answered, pickups the host's grant
// ran here. g_granted and g_refused are the host's.
uint64_t g_sent = 0, g_granted = 0, g_refused = 0, g_taken = 0, g_refusedAnswers = 0;
// CLIENT: the copies the host's Gone took away from this client without its own pickup (the join's drill reads it).
uint64_t g_goneCopies = 0;
// CLIENT: grants that left no item: the taker's copy was missing, or the pickup did not dispatch on it.
uint64_t g_noItem = 0;
// HOST: per joiner slot, the keys of the anchored hooks the world held when its save was cut (CaptureJoinHooks).
std::vector<std::wstring> g_cut[coop::players::kMaxPeers];

// The latch is per world: an answer owed by a world that has since changed is never coming.
uint32_t g_inFlightGen = 0;

bool InFlight(const std::wstring& key) {
    const uint32_t gen = ue_wrap::world_identity::Generation();
    if (gen != g_inFlightGen) { g_inFlight.clear(); g_inFlightGen = gen; }
    return std::find(g_inFlight.begin(), g_inFlight.end(), key) != g_inFlight.end();
}

void Release(const std::wstring& key) {
    g_inFlight.erase(std::remove(g_inFlight.begin(), g_inFlight.end(), key), g_inFlight.end());
}

// The watch is on every class's actionOptionIndex, so a door, a toggle or a coin asks too. Whether a class sits under
// hook_C is a walk of its chain, so it runs once per distinct class (the floppy census's per-class verdict). hook_C is
// held as a CachedObjRef and found again by name when it is no longer alive, as prop_act_host does for its classes: a
// world reload can re-create a Blueprint class. The table is dropped when the world's generation or hook_C itself
// changes, and nothing is cached while hook_C is not loaded.
ue_wrap::CachedObjRef           g_hookBase;
void*                           g_hookBaseSeen = nullptr;
std::unordered_map<void*, bool> g_hookClass;
uint32_t                        g_hookClassGen = 0;

bool IsHookObject(void* obj) {
    void* cls = R::ClassOf(obj);
    if (!cls) return false;
    if (!g_hookBase.Alive()) g_hookBase.Set(ue_wrap::object_index::ClassByName(L"hook_C"));
    void* base = g_hookBase.Alive() ? g_hookBase.Raw() : nullptr;
    if (!base) return false;
    const uint32_t gen = ue_wrap::world_identity::Generation();
    if (gen != g_hookClassGen || base != g_hookBaseSeen) {
        g_hookClass.clear();
        g_hookClassGen = gen;
        g_hookBaseSeen = base;
    }
    auto it = g_hookClass.find(cls);
    if (it == g_hookClass.end()) it = g_hookClass.emplace(cls, PR::WalksToBase(cls, base)).first;
    return it->second;
}

std::string Red() {
    static const std::string r = coop::config::ResolveEnum(::coop::config_registry::rows::hook_retrieve_red);
    return r;
}

// A hook this peer owns and the host has not adopted -- its own, before the second end anchored -- is the game's to take
// back here: the owner lane announces the destroy. Once its commit is sent (hook_sync's `committed`) the host adopts it
// as the last chunk lands, and a request sent after the commit rides the same ordered channel, so the host holds the key
// by the time it judges: from then on the pickup goes through the host like any anchored hook, or the game's body would
// run here and leave the host's adopted copy standing beside the item.
bool OwnedHere(void* hook) {
    for (const auto& o : D::OwnedHooks())
        if (o.ref.Raw() == hook) return !o.committed;
    return false;
}

std::wstring KeyOf(void* hook) {
    std::wstring k = PR::GetActorSaveKeyString(hook);
    return (k.empty() || k == L"None") ? std::wstring() : k;
}

// `slot` < 0: every client; else that one.
void SendGone(coop::net::Session& s, const std::wstring& key, uint8_t taker, int slot = -1) {
    HookRetrievePayload g{};
    coop::net::WireKeyFromString(key, g.key);
    g.op    = coop::net::kHookRetrieveGone;
    g.taker = taker;
    const bool sent = slot < 0 ? s.SendReliable(coop::net::ReliableKind::HookRetrieve, &g, sizeof(g))
                               : s.SendReliableToSlot(slot, coop::net::ReliableKind::HookRetrieve, &g, sizeof(g));
    // A send with nobody to send to is no failure: a host alone picks its hook up with no word owed. A joiner not yet
    // world-ready is told at its connect replay (SendJoinGone).
    bool anyReady = slot >= 0 ? s.IsSlotWorldReady(slot) : false;
    for (int i = 1; slot < 0 && i < coop::players::kMaxPeers && !anyReady; ++i) anyReady = s.IsSlotWorldReady(i);
    if (!sent && anyReady)
        UE_LOGW("hook_retrieve: the word that hook '%ls' is gone did not send -- the clients keep their copies",
                key.c_str());
}

// HOST: a request the host will not grant is answered to its sender alone, which releases that client's latch.
void Refuse(coop::net::Session& s, uint8_t slot, const std::wstring& key, const char* why) {
    ++g_refused;
    if (g_refused <= 20 || g_refused % 50 == 0)
        UE_LOGI("hook_retrieve: slot %u's pickup of hook '%ls' refused (%s)", static_cast<unsigned>(slot), key.c_str(),
                why);
    HookRetrievePayload r{};
    coop::net::WireKeyFromString(key, r.key);
    r.op    = coop::net::kHookRetrieveRefused;
    r.taker = coop::net::kHookRetrieveNoTaker;
    if (!s.SendReliableToSlot(slot, coop::net::ReliableKind::HookRetrieve, &r, sizeof(r)))
        UE_LOGW("hook_retrieve: the refusal of slot %u's pickup of hook '%ls' did not send -- its client holds the "
                "request until its world changes or the session ends", static_cast<unsigned>(slot), key.c_str());
}

sg::Verdict OnActionPre(const sg::Call& c) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !IsHookObject(c.object)) return sg::Verdict::Run;
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
    if (InFlight(key)) return sg::Verdict::Cancel;   // asked already: the host's answer comes
    HookRetrievePayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.op    = coop::net::kHookRetrieveRequest;
    p.taker = coop::net::kHookRetrieveNoTaker;
    if (!s->SendReliableToSlot(0, coop::net::ReliableKind::HookRetrieve, &p, sizeof(p))) {
        UE_LOGW("hook_retrieve: the pickup of hook '%ls' was not sent -- the hook stays; use it again", key.c_str());
        return sg::Verdict::Cancel;
    }
    g_inFlight.push_back(key);
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

// HOST: a client's pickup. MTA's server alone decides a pickup: CPickup::CanUse and Use (CPickup.cpp:379-405) and
// Callback_OnCollision (:483), Source's server traces the use on its own copy: CBasePlayer::PlayerUse
// (baseplayer_shared.cpp:1274). Here the look is a Blueprint trace on the client's copy that the host cannot replay, so
// the client names the hook by its key and the host judges identity, reach and existence: it is still the only one
// that destroys the shared hook.
// A request is judged and answered inside this one call, so a slot has no second request in flight to refuse.
void OnRequest(coop::net::Session& s, const std::wstring& key, uint8_t slot) {
    void* hook = D::FindHookByKey(key);
    // The host holds no hook of that key: the second press of a taken hook, two players taking one, or a copy the
    // client keeps that the host no longer has. The host's absence is authoritative, so the sender's copy goes with the
    // word that frees its latch; no taker is named, so no item is made.
    if (!hook) {
        ++g_refused;
        UE_LOGI("hook_retrieve: slot %u asked for hook '%ls', which the host does not hold -- its copy is told gone",
                static_cast<unsigned>(slot), key.c_str());
        SendGone(s, key, coop::net::kHookRetrieveNoTaker, slot);
        return;
    }
    // A hook the host is still flying is its own player's, not an anchored one a client may take.
    if (OwnedHere(hook)) { Refuse(s, slot, key, "the hook is the host's own, not yet anchored"); return; }
    ue_wrap::FVector a{}, b{};
    if (!H::EndLocations(hook, a, b)) {
        if (!E::TryGetActorLocation(hook, a)) { Refuse(s, slot, key, "the hook's ends do not read"); return; }
        b = a;
    }
    const auto tok = coop::element::IntentTarget::ForClientIntent(s, slot, kReachUU);
    const auto atA = tok.AuthorizeSegment(a, a);
    if (!atA) {
        const auto atB = tok.AuthorizeSegment(b, b);
        if (!atB) {
            Refuse(s, slot, key, coop::element::OutcomeName(atA.outcome));
            return;
        }
    }
    // Taken away without the game's drop: the item is the taker's, and its own copy makes it.
    ue_wrap::CachedObjRef incarnation;
    incarnation.Set(hook);
    E::DestroyActor(hook);
    if (incarnation.Alive()) { Refuse(s, slot, key, "the hook is still live after its destroy"); return; }
    ++g_granted;
    SendGone(s, key, slot);
    UE_LOGI("hook_retrieve: HOST granted slot %u its pickup of hook '%ls' (%llu granted)", static_cast<unsigned>(slot),
            key.c_str(), static_cast<unsigned long long>(g_granted));
}

// CLIENT: the hook is gone on the host. The taker's copy runs the game's own pickup; every other copy goes.
void OnGone(const std::wstring& key, uint8_t taker) {
    void* copy = D::FindHookByKey(key);
    const bool mine = taker == coop::players::Registry::Get().LocalPeerId();
    if (!mine) {
        if (copy) { E::DestroyActor(copy); ++g_goneCopies; }
        return;
    }
    void* player = coop::players::Registry::Get().Local();
    if (!player) {
        UE_LOGW("hook_retrieve: the host granted this client hook '%ls' with no local player -- no item", key.c_str());
        if (copy) E::DestroyActor(copy);
        return;
    }
    // The item is the game's own pickup's, run on the copy and nowhere else: with no copy here (none is known to occur:
    // the request was made through it) nothing is made by hand, and a copy the table missed would be left a ghost.
    if (!copy) {
        ++g_noItem;
        UE_LOGW("hook_retrieve: the host granted this client hook '%ls' but no copy of it stands here -- no item "
                "(%llu such grants)", key.c_str(), static_cast<unsigned long long>(g_noItem));
        return;
    }
    // hook_C::actionOptionIndex reads only the player: drophook spawns the item at them, putObjectInventory2 pockets it
    // (or leaves it on the floor when the inventory is full, as the game does), and the hook destroys itself.
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
    ++g_noItem;
    UE_LOGW("hook_retrieve: the pickup of hook '%ls' did not dispatch on this client's copy -- no item (%llu such "
            "grants)", key.c_str(), static_cast<unsigned long long>(g_noItem));
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
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    if (key.empty()) return;
    if (p.op == coop::net::kHookRetrieveRequest) {
        if (s->role() != coop::net::Role::Host || senderPeerSlot == 0) return;
        // Every request that reaches the host is answered: a grant by Gone, anything else by Refused.
        if (!H::EnsureResolved()) {
            Refuse(*s, static_cast<uint8_t>(senderPeerSlot), key, "the hook wrapper is not resolved on the host");
            return;
        }
        OnRequest(*s, key, static_cast<uint8_t>(senderPeerSlot));
        return;
    }
    if (s->role() != coop::net::Role::Client || senderPeerSlot != 0) return;   // the host's word alone
    if (p.op == coop::net::kHookRetrieveRefused) {
        Release(key);
        ++g_refusedAnswers;
        UE_LOGI("hook_retrieve: the host refused this client's pickup of hook '%ls' -- the copy stays", key.c_str());
        return;
    }
    if (p.op == coop::net::kHookRetrieveGone) {
        Release(key);   // before anything that can return: the answer has come
        if (!H::EnsureResolved()) return;
        OnGone(key, p.taker);
    }
}

void PrepareAnchoredCopy(void* mirror) {
    if (!mirror) return;
    E::SetActorEnableCollision(mirror, Red() != "nocollision");
}

// The join's answer, the shape of join_window_baseline's removes: the joiner's save is a photograph cut at one instant,
// the host plays on while it loads, and a Gone is not sent to a slot before its world-ready (session.cpp: not queued,
// the connect replay rebuilds). So the keys of the anchored hooks the world held at the cut are kept per slot, and at
// the slot's connect replay each one the host no longer holds is named gone to that slot alone.
void CaptureJoinHooks(int slot) {
    if (!GT::IsGameThread() || slot < 1 || slot >= coop::players::kMaxPeers) return;
    auto& cut = g_cut[slot];
    cut.clear();
    D::CollectAnchoredKeys(cut);
    std::sort(cut.begin(), cut.end());
    cut.erase(std::unique(cut.begin(), cut.end()), cut.end());
    UE_LOGI("hook_retrieve: slot %d's save cut with %zu anchored hook(s) standing", slot, cut.size());
}

void SendJoinGone(int slot) {
    if (!GT::IsGameThread() || slot < 1 || slot >= coop::players::kMaxPeers) return;
    auto* s = g_session.load(std::memory_order_acquire);
    std::vector<std::wstring> cut;
    cut.swap(g_cut[slot]);
    if (!s || s->role() != coop::net::Role::Host || cut.empty()) return;
    size_t sent = 0;
    for (const std::wstring& key : cut) {
        if (D::FindHookByKey(key)) continue;   // still stands: the joiner's copy is right
        if (Red() == "nojoingone") continue;   // the drill's red: the join as it was before this answer
        SendGone(*s, key, coop::net::kHookRetrieveNoTaker, slot);
        ++sent;
    }
    UE_LOGI("hook_retrieve: slot %d world-ready -- %zu of its %zu anchored hook(s) were taken since its save was cut",
            slot, sent, cut.size());
}

void OnPeerLeftSlot(int slot) {
    if (slot >= 0 && slot < coop::players::kMaxPeers) g_cut[slot].clear();
}

uint64_t GrantedCount() { return g_granted; }
uint64_t GoneCopyCount() { return g_goneCopies; }

void* FindHook(const std::wstring& key) { return D::FindHookByKey(key); }
uint64_t AskedCount() { return g_sent; }
uint64_t TakenCount() { return g_taken; }
uint64_t RefusedAnswerCount() { return g_refusedAnswers; }

void OnDisconnect() {
    if (g_sent || g_granted || g_refused || g_taken || g_noItem)
        UE_LOGI("hook_retrieve: session end -- asked=%llu granted=%llu refused=%llu taken=%llu no-item=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_granted),
                static_cast<unsigned long long>(g_refused), static_cast<unsigned long long>(g_taken),
                static_cast<unsigned long long>(g_noItem));
    g_sent = g_refused = g_taken = g_refusedAnswers = g_goneCopies = g_noItem = 0;
    for (auto& cut : g_cut) cut.clear();
    g_inFlight.clear();
    g_replay = nullptr;
    g_hostPress = nullptr;
    g_hostPressKey.clear();
    g_hookClass.clear();
    g_hookBase.Reset();
    g_hookBaseSeen = nullptr;
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::hook_retrieve
