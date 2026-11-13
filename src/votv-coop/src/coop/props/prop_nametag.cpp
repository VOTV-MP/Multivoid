// coop/props/prop_nametag.cpp -- see coop/props/prop_nametag.h.

#include "coop/props/prop_nametag.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/intent_authority.h"   // IntentTarget: the prop within the sender's reach
#include "coop/net/net_clock.h"              // NowMs: the budget of a sender's tags
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/hand_item.h"           // a nametag in the sender's hand, as its owner announced it
#include "coop/player/players_registry.h"
#include "coop/player/roster_ledger.h"       // PerSlotState: a waiting request goes with its sender
#include "coop/props/prop_key_index.h"

#include "ue_wrap/actors/nametag.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstring>
#include <map>
#include <string>

namespace coop::prop_nametag {
namespace {

namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;
namespace NT = ue_wrap::nametag;
namespace PR = ue_wrap::prop;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
using coop::net::PropNametagPayload;

// The item's left click, prop_nametag_C::playerHandUse_LMB. The any-class row the coin gun and the trash packer
// already watch: the callbacks keep the nametag item alone.
UE_FACT_FN(kRowHandUse, "prop_nametag", nullptr, L"playerHandUse_LMB", AnyClass);
constexpr int kTag = 0x4E544147;   // 'NTAG'
// The item's own reach is the player's interaction trace, armLength long; the authority adds its pads.
constexpr float    kReachUU     = 200.0f;
// One tag a second a client: an item is spent by its use, so a faster run is not one player's hand.
constexpr uint64_t kPerSlotMs   = 1000;
constexpr size_t   kMaxKeyChars = sizeof(coop::net::WireKey::data);
constexpr size_t   kMaxTagChars = coop::net::kPropNametagMaxChars;
constexpr uint8_t  kAuthorHost  = 0;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watchRegistered = false;
bool g_watchSettled = false;

// CLIENT: the request in flight -- the prop's key and the item in hand the host's answer spends. Released by the host's
// tag for it (OnPublish), its refusal (OnRefused) or the session's end: every request the host reads is answered.
struct Pending {
    std::wstring key;
    ue_wrap::CachedObjRef item;
};
Pending g_pending;
// HOST: the host player's own use, between the watch's entry and exit.
void*        g_hostItem = nullptr;
void*        g_hostProp = nullptr;
std::wstring g_hostTag;
// HOST: when each client slot's last tag was applied.
std::map<uint8_t, uint64_t> g_lastBySlot;
// HOST: a slot's request that waits for its owed hand state to be applied; at most one a slot.
struct Parked {
    bool         valid = false;
    std::wstring key;
    std::wstring tag;
};
coop::roster_ledger::PerSlotState<Parked> g_parked;
uint64_t g_applied = 0, g_refused = 0;

bool TagSane(const std::wstring& t) {
    if (t.empty() || t.size() > kMaxTagChars) return false;
    for (wchar_t c : t)
        if (c < 0x20 || c == 0x7F) return false;
    return true;
}

PropNametagPayload MakePayload(uint8_t op, const std::wstring& key, uint8_t author, const std::wstring& tag) {
    PropNametagPayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.op       = op;
    p.author   = author;
    p.tagChars = static_cast<uint8_t>(tag.size());
    for (size_t i = 0; i < tag.size(); ++i) p.tag[i] = static_cast<uint16_t>(tag[i]);
    return p;
}

std::wstring TagOf(const PropNametagPayload& p) {
    std::wstring t;
    for (size_t i = 0; i < p.tagChars; ++i) t.push_back(static_cast<wchar_t>(p.tag[i]));
    return t;
}

std::wstring KeyOf(void* prop) {
    std::wstring k = PR::GetInteractableKeyString(prop);
    return (k.empty() || k == L"None") ? std::wstring() : k;
}

// The player whose hand used the item: the call's own `player`, else the local player.
void* PlayerOf(const sg::Call& c) {
    const int32_t off = c.locals ? R::FindParamOffset(c.function, L"player") : -1;
    void* p = nullptr;
    if (off >= 0) std::memcpy(&p, c.locals + off, sizeof(p));
    return p ? p : coop::players::Registry::Get().Local();
}

// HOST: the prop of `key` carries `tag` now; every client is told.
void Publish(coop::net::Session& s, const std::wstring& key, uint8_t author, const std::wstring& tag) {
    if (tag.size() > kMaxTagChars) {
        UE_LOGW("prop_nametag: the tag of prop '%ls' is %zu characters, more than the wire carries (%zu) -- the clients "
                "keep the one they show", key.c_str(), tag.size(), kMaxTagChars);
        return;
    }
    const PropNametagPayload p = MakePayload(coop::net::kPropNametagPublish, key, author, tag);
    if (!s.SendReliable(coop::net::ReliableKind::PropNametag, &p, sizeof(p)))
        UE_LOGW("prop_nametag: the tag of prop '%ls' did not send -- the clients keep the one they show",
                key.c_str());
}

sg::Verdict OnUsePre(const sg::Call& c) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !NT::IsNametagItem(c.object)) return sg::Verdict::Run;
    void* player = PlayerOf(c);
    if (!player || !coop::players::Registry::Get().IsLocal(player)) return sg::Verdict::Run;
    // The body's own target: the prop the player's interaction trace hits. Anything else, the body leaves alone.
    void* prop = E::ReadMainPlayerHitActor(player);
    if (!prop || !PR::IsDescendantOfProp(prop)) return sg::Verdict::Run;
    std::wstring tag;
    if (!NT::ReadItemTag(c.object, tag)) return sg::Verdict::Run;
    if (s->role() == coop::net::Role::Host) {
        g_hostItem = c.object;
        g_hostProp = prop;
        g_hostTag  = tag;
        return sg::Verdict::Run;
    }
    static const bool s_clientRuns =
        coop::config::ResolveFlag(coop::config_registry::rows::nametag_client_runs);
    if (s_clientRuns) return sg::Verdict::Run;   // the nametag drill's red: the tag stays on this copy
    const std::wstring key = KeyOf(prop);
    if (key.empty() || key.size() > kMaxKeyChars || !TagSane(tag)) {
        UE_LOGW("prop_nametag: this client's tag '%ls' on prop %p cannot be named to the host (key '%ls') -- not "
                "applied", tag.c_str(), prop, key.c_str());
        return sg::Verdict::Cancel;
    }
    if (!g_pending.key.empty()) return sg::Verdict::Cancel;   // asked already: the host's answer comes
    const PropNametagPayload p = MakePayload(coop::net::kPropNametagRequest, key, 0, tag);
    if (!s->SendReliableToSlot(0, coop::net::ReliableKind::PropNametag, &p, sizeof(p))) {
        UE_LOGW("prop_nametag: the tag for prop '%ls' was not sent -- the item stays; use it again", key.c_str());
        return sg::Verdict::Cancel;
    }
    g_pending.key = key;
    g_pending.item.Set(c.object);
    UE_LOGI("prop_nametag: this client tagged prop '%ls' '%ls' -- asked of the host", key.c_str(), tag.c_str());
    return sg::Verdict::Cancel;
}

void OnUsePost(const sg::Call& c) {
    if (!g_hostItem || c.object != g_hostItem) return;
    void* prop = g_hostProp;
    const std::wstring tag = std::move(g_hostTag);
    g_hostItem = g_hostProp = nullptr;
    g_hostTag.clear();
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host || !prop || !R::IsLive(prop)) return;
    std::wstring now;
    if (!NT::ReadPropNametag(prop, now) || now != tag) return;   // the body did not tag it
    const std::wstring key = KeyOf(prop);
    if (key.empty() || key.size() > kMaxKeyChars) return;
    Publish(*s, key, kAuthorHost, tag);
    UE_LOGI("prop_nametag: the host player tagged prop '%ls' '%ls' -- the clients are told", key.c_str(), tag.c_str());
}

// HOST: a request the host will not grant is answered to its sender alone, which releases that client's latch.
void Refuse(coop::net::Session& s, uint8_t slot, const std::wstring& key, const char* why) {
    ++g_refused;
    if (g_refused <= 20 || g_refused % 50 == 0)
        UE_LOGI("prop_nametag: slot %u's tag on prop '%ls' refused (%s)", static_cast<unsigned>(slot), key.c_str(),
                why);
    const PropNametagPayload r = MakePayload(coop::net::kPropNametagRefused, key, 0, std::wstring());
    if (!s.SendReliableToSlot(slot, coop::net::ReliableKind::PropNametag, &r, sizeof(r)))
        UE_LOGW("prop_nametag: the refusal of slot %u's tag on prop '%ls' did not send", static_cast<unsigned>(slot),
                key.c_str());
}

// HOST: a request's judgement, with the sender's hand as the hand lane has applied it.
void Judge(coop::net::Session& s, const std::wstring& key, const std::wstring& tag, uint8_t slot) {
    if (!TagSane(tag)) { Refuse(s, slot, key, "the tag is empty, too long or carries a control character"); return; }
    void* prop = coop::prop_key_index::Find(key);
    if (!prop || !PR::IsDescendantOfProp(prop)) { Refuse(s, slot, key, "no prop of that key here"); return; }
    const uint64_t now = coop::net::NowMs();
    if (!coop::hand_item::HeldClassIs(slot, L"prop_nametag_C")) {
        Refuse(s, slot, key, "no nametag in the sender's hand");
        return;
    }
    auto last = g_lastBySlot.find(slot);
    if (last != g_lastBySlot.end() && now - last->second < kPerSlotMs) { Refuse(s, slot, key, "a second tag within 1 s"); return; }
    const auto sub = coop::element::IntentTarget::ForClientIntent(s, slot, kReachUU).Authorize(prop);
    if (!sub) { Refuse(s, slot, key, coop::element::OutcomeName(sub.outcome)); return; }
    if (!NT::ApplyPropNametag(prop, tag)) { Refuse(s, slot, key, "the host's prop did not take the tag"); return; }
    g_lastBySlot[slot] = now;
    ++g_applied;
    Publish(s, key, slot, tag);
    UE_LOGI("prop_nametag: HOST tagged prop '%ls' '%ls' for slot %u (%llu for clients)", key.c_str(), tag.c_str(),
            static_cast<unsigned>(slot), static_cast<unsigned long long>(g_applied));
}

// HOST: a client's request. The hand state of the sender rode the same ordered lane ahead of it (a nametag is held
// before it is clicked), but the host applies a client's hand states at a pace (hand_item's LatestWins), so the
// state applied may be older than the one that arrived. While one is owed the request waits for its application,
// the lane's own edge (OnHandApplied), and is judged against the hand then.
void OnRequest(coop::net::Session& s, const std::wstring& key, const std::wstring& tag, uint8_t slot) {
    Parked& waiting = g_parked[slot];
    if (waiting.valid) { Refuse(s, slot, key, "an earlier tag of this slot still waits for its hand state"); return; }
    if (coop::hand_item::HandStateOwed(slot)) {
        waiting.valid = true;
        waiting.key   = key;
        waiting.tag   = tag;
        return;
    }
    Judge(s, key, tag, slot);
}

// The hand lane applied a state of `slot` (or settled an owed one): its waiting request is judged now.
void OnHandApplied(uint8_t slot) {
    if (slot >= coop::players::kMaxPeers) return;
    Parked& waiting = g_parked[slot];
    if (!waiting.valid || coop::hand_item::HandStateOwed(slot)) return;
    auto* s = g_session.load(std::memory_order_acquire);
    const Parked request = std::move(waiting);
    waiting = Parked{};
    if (!s || s->role() != coop::net::Role::Host) return;
    Judge(*s, request.key, request.tag, slot);
}

// CLIENT: the host's tag. The author's held item goes, as the game's own use takes it.
void OnPublish(const std::wstring& key, const std::wstring& tag, uint8_t author) {
    void* prop = coop::prop_key_index::Find(key);
    if (!prop || !NT::ApplyPropNametag(prop, tag)) {
        UE_LOGW("prop_nametag: the host's tag '%ls' for prop '%ls' did not apply here (%s)", tag.c_str(), key.c_str(),
                prop ? "the copy did not take it" : "no copy of that key");
    }
    if (author != coop::players::Registry::Get().LocalPeerId() || g_pending.key != key) return;
    if (void* item = g_pending.item.Get()) E::DestroyActor(item);
    UE_LOGI("prop_nametag: this client's tag on prop '%ls' applied by the host -- its nametag is spent", key.c_str());
    g_pending = Pending{};
}

// CLIENT: the host refused this client's request: the item stays and the client may use it again.
void OnRefused(const std::wstring& key) {
    if (g_pending.key != key) return;
    UE_LOGI("prop_nametag: the host refused this client's tag on prop '%ls' -- the item stays", key.c_str());
    g_pending = Pending{};
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!coop::hand_item::SubscribeHandApplied(&OnHandApplied))
        UE_LOGE("prop_nametag: the hand lane's subscriber table is full -- a request waiting for its hand state is not released");
}

void Tick() {
    if (g_watchSettled) return;
    if (!g_watchRegistered) {
        g_watchRegistered = sg::WatchName(kRowHandUse, kTag, &OnUsePre, &OnUsePost);
        if (!g_watchRegistered) {
            g_watchSettled = true;
            UE_LOGE("prop_nametag: the gate took no watch on %ls -- a tag stays on the screen that applied it",
                    kRowHandUse.Name());
            return;
        }
    }
    sg::ResolvePendingNames();
    if (!sg::NameWatchSettled(kRowHandUse, kTag)) return;
    g_watchSettled = true;
    if (sg::NameWatchLive(kRowHandUse, kTag))
        UE_LOGI("prop_nametag: the watch on %ls is live", kRowHandUse.Name());
    else
        UE_LOGE("prop_nametag: the watch on %ls settled dead -- a tag stays on the screen that applied it",
                kRowHandUse.Name());
}

void OnMsg(const PropNametagPayload& p, int senderPeerSlot) {
    if (!GT::IsGameThread()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    if (senderPeerSlot < 0 || senderPeerSlot >= coop::players::kMaxPeers) return;
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    if (key.empty()) return;
    if (p.op == coop::net::kPropNametagRequest) {
        if (s->role() != coop::net::Role::Host || senderPeerSlot == 0) return;
        const uint8_t slot = static_cast<uint8_t>(senderPeerSlot);
        // Every request that names a prop is answered: a grant by the publish, anything else by the refusal.
        if (p.tagChars > kMaxTagChars) { Refuse(*s, slot, key, "the tag is longer than the wire carries"); return; }
        OnRequest(*s, key, TagOf(p), slot);
        return;
    }
    if (s->role() != coop::net::Role::Client || senderPeerSlot != 0) return;   // the host's word alone
    if (p.op == coop::net::kPropNametagRefused) { OnRefused(key); return; }
    if (p.op != coop::net::kPropNametagPublish || p.tagChars > kMaxTagChars) return;
    OnPublish(key, TagOf(p), p.author);
}

uint64_t AppliedForClients() { return g_applied; }

void OnDisconnect() {
    if (g_applied || g_refused)
        UE_LOGI("prop_nametag: session end -- applied for clients=%llu refused=%llu",
                static_cast<unsigned long long>(g_applied), static_cast<unsigned long long>(g_refused));
    g_refused = 0;
    g_pending = Pending{};
    g_hostItem = g_hostProp = nullptr;
    g_hostTag.clear();
    g_lastBySlot.clear();
    for (int i = 0; i < g_parked.size(); ++i) g_parked[i] = Parked{};
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::prop_nametag
