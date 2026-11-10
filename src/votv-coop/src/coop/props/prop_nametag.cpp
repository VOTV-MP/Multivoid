// coop/props/prop_nametag.cpp -- see coop/props/prop_nametag.h.

#include "coop/props/prop_nametag.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/intent_authority.h"   // IntentTarget: the prop within the sender's reach
#include "coop/net/net_clock.h"              // NowMs: the hand history's clock
#include "coop/net/session.h"
#include "coop/player/hand_history.h"        // a nametag in the sender's hand
#include "coop/player/players_registry.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/prop_save_data.h"       // SendRawBody: the lane's kinds and its blob sequence

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

// The item's left click, prop_nametag_C::playerHandUse_LMB. The any-class row the coin gun and the trash packer
// already watch: the callbacks keep the nametag item alone.
UE_FACT_FN(kRowHandUse, "prop_nametag", nullptr, L"playerHandUse_LMB", AnyClass);
constexpr int kTag = 0x4E544147;   // 'NTAG'
// The item's own reach is the player's interaction trace, armLength long; the authority adds its pads.
constexpr float    kReachUU     = 200.0f;
// The hand lane paces what it shows of a client's hand; a click lands within this of the item being held.
constexpr uint64_t kHandLagMs   = 3000;
// One tag a second a client: an item is spent by its use, so a faster run is not one player's hand.
constexpr uint64_t kPerSlotMs   = 1000;
// CLIENT: a request the host did not answer in this long was refused; the item stays.
constexpr uint64_t kPendingMs   = 5000;
constexpr size_t   kMaxKeyChars = 31;
constexpr size_t   kMaxTagChars = 64;
constexpr uint8_t  kAuthorHost  = 0;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watchRegistered = false;
bool g_watchSettled = false;

// CLIENT: the request in flight -- the prop's key, the tag, and the item in hand the host's answer spends.
struct Pending {
    std::wstring key;
    std::wstring tag;
    ue_wrap::CachedObjRef item;
    uint64_t sentMs = 0;
};
Pending g_pending;
// HOST: the host player's own use, between the watch's entry and exit.
void*        g_hostItem = nullptr;
void*        g_hostProp = nullptr;
std::wstring g_hostTag;
// HOST: when each client slot's last tag was applied.
std::map<uint8_t, uint64_t> g_lastBySlot;
uint64_t g_applied = 0, g_refused = 0;

bool TagSane(const std::wstring& t) {
    if (t.empty() || t.size() > kMaxTagChars) return false;
    for (wchar_t c : t)
        if (c < 0x20 || c == 0x7F) return false;
    return true;
}

std::vector<uint8_t> BuildBody(const std::wstring& key, uint8_t author, const std::wstring& tag) {
    std::vector<uint8_t> b;
    b.push_back(kBodyVersion);
    b.push_back(static_cast<uint8_t>(key.size()));
    for (wchar_t c : key) b.push_back(static_cast<uint8_t>(c & 0x7F));
    b.push_back(author);
    b.push_back(static_cast<uint8_t>(tag.size()));
    for (wchar_t c : tag) {
        b.push_back(static_cast<uint8_t>(c & 0xFF));
        b.push_back(static_cast<uint8_t>((c >> 8) & 0xFF));
    }
    return b;
}

// Strict on format: an ill-formed body is dropped whole.
bool ParseBody(const std::vector<uint8_t>& b, std::wstring& key, uint8_t& author, std::wstring& tag) {
    size_t o = 0;
    if (b.size() < 4 || b[o++] != kBodyVersion) return false;
    const size_t keyLen = b[o++];
    if (keyLen == 0 || keyLen > kMaxKeyChars || o + keyLen + 2 > b.size()) return false;
    key.assign(b.begin() + static_cast<long>(o), b.begin() + static_cast<long>(o + keyLen));
    o += keyLen;
    author = b[o++];
    const size_t tagLen = b[o++];
    if (tagLen > kMaxTagChars || o + tagLen * 2 != b.size()) return false;
    tag.clear();
    for (size_t i = 0; i < tagLen; ++i, o += 2)
        tag.push_back(static_cast<wchar_t>(b[o] | (b[o + 1] << 8)));
    return true;
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

void Publish(coop::net::Session& s, const std::wstring& key, uint8_t author, const std::wstring& tag) {
    if (!coop::prop_save_data::SendRawBody(&s, -1, BuildBody(key, author, tag)))
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
    const uint64_t now = coop::net::NowMs();
    if (g_pending.key == key && now - g_pending.sentMs < kPendingMs) return sg::Verdict::Cancel;   // asked already
    if (!coop::prop_save_data::SendRawBody(s, 0, BuildBody(key, kAuthorHost, tag))) {
        UE_LOGW("prop_nametag: the tag for prop '%ls' was not sent -- the item stays; use it again", key.c_str());
        return sg::Verdict::Cancel;
    }
    g_pending.key = key;
    g_pending.tag = tag;
    g_pending.item.Set(c.object);
    g_pending.sentMs = now;
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

void Refuse(uint8_t slot, const std::wstring& key, const char* why) {
    ++g_refused;
    if (g_refused <= 20 || g_refused % 50 == 0)
        UE_LOGI("prop_nametag: slot %u's tag on prop '%ls' refused (%s)", static_cast<unsigned>(slot), key.c_str(),
                why);
}

// HOST: a client's request.
void OnRequest(coop::net::Session& s, const std::wstring& key, const std::wstring& tag, uint8_t slot) {
    if (slot == 0) return;
    if (!TagSane(tag)) { Refuse(slot, key, "the tag is empty, too long or carries a control character"); return; }
    void* prop = coop::prop_key_index::Find(key);
    if (!prop || !PR::IsDescendantOfProp(prop)) { Refuse(slot, key, "no prop of that key here"); return; }
    const uint64_t now = coop::net::NowMs();
    if (!coop::hand_history::HeldWithin(slot, L"prop_nametag_C", kHandLagMs, now)) {
        Refuse(slot, key, "no nametag in the sender's hand");
        return;
    }
    auto last = g_lastBySlot.find(slot);
    if (last != g_lastBySlot.end() && now - last->second < kPerSlotMs) { Refuse(slot, key, "a second tag within 1 s"); return; }
    const auto sub = coop::element::IntentTarget::ForClientIntent(s, slot, kReachUU).Authorize(prop);
    if (!sub) { Refuse(slot, key, coop::element::OutcomeName(sub.outcome)); return; }
    if (!NT::ApplyPropNametag(prop, tag)) { Refuse(slot, key, "the host's prop did not take the tag"); return; }
    g_lastBySlot[slot] = now;
    ++g_applied;
    Publish(s, key, slot, tag);
    UE_LOGI("prop_nametag: HOST tagged prop '%ls' '%ls' for slot %u (%llu for clients)", key.c_str(), tag.c_str(),
            static_cast<unsigned>(slot), static_cast<unsigned long long>(g_applied));
}

// CLIENT: the host's tag. The author's held item goes, as the game's own use takes it.
void OnTag(const std::wstring& key, const std::wstring& tag, uint8_t author) {
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

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    if (g_pending.sentMs && coop::net::NowMs() - g_pending.sentMs > kPendingMs) {
        UE_LOGI("prop_nametag: no answer for the tag on prop '%ls' -- refused; the item stays", g_pending.key.c_str());
        g_pending = Pending{};
    }
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

void OnBody(coop::net::Session& s, const std::vector<uint8_t>& body, uint8_t senderSlot, bool intent) {
    if (!GT::IsGameThread()) return;
    std::wstring key, tag;
    uint8_t author = 0;
    if (!ParseBody(body, key, author, tag)) {
        UE_LOGW("prop_nametag: ill-formed tag body from slot %u (%zu B) -- dropped whole",
                static_cast<unsigned>(senderSlot), body.size());
        return;
    }
    if (intent) {
        if (s.role() == coop::net::Role::Host) OnRequest(s, key, tag, senderSlot);
        return;
    }
    if (s.role() == coop::net::Role::Client && senderSlot == 0) OnTag(key, tag, author);
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
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::prop_nametag
