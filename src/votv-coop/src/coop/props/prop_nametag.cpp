// coop/props/prop_nametag.cpp -- see coop/props/prop_nametag.h.

#include "coop/props/prop_nametag.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/intent_authority.h"   // IntentTarget: the prop within the sender's reach
#include "coop/net/blob_chunks.h"
#include "coop/net/intent_bucket.h"          // a sender's budget of tags
#include "coop/net/net_clock.h"              // NowMs: the budget's clock
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"           // a nametag in the sender's hand, as its owner announced it
#include "coop/player/players_registry.h"
#include "coop/player/roster_ledger.h"       // PerSlotState: a waiting request goes with its sender
#include "coop/props/prop_key_index.h"

#include "ue_wrap/actors/nametag.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"   // UE_ASSERT_GAME_THREAD
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"   // Generation: the class table's stamp
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::prop_nametag {
namespace {

namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;
namespace NT = ue_wrap::nametag;
namespace PR = ue_wrap::prop;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace BC = coop::blob_chunks;

// The item's left click, prop_nametag_C::playerHandUse_LMB. The any-class row the coin gun and the trash packer
// already watch: the callbacks keep the nametag item alone.
UE_FACT_FN(kRowHandUse, "prop_nametag", nullptr, L"playerHandUse_LMB", AnyClass);
constexpr int kTag = 0x4E544147;   // 'NTAG'
// The item's own reach is the player's interaction trace, armLength long; the authority adds its pads.
constexpr float    kReachUU     = 200.0f;
constexpr coop::net::IntentBudget kTagBudget{2.0f, 1.0f};
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
    bool waitSaid = false;   // a click while it waits is said once
};
Pending g_pending;
// HOST: the host player's own use, between the watch's entry and exit.
void*        g_hostItem = nullptr;
void*        g_hostProp = nullptr;
std::wstring g_hostTag;
// HOST: each client slot's budget of tags, spent by every request judged and reset with the slot's occupant
// (prop_syncer_host's g_claimBuckets is the shape). A nametag is spent by its use, so a faster run is not one player's hand.
coop::roster_ledger::PerSlotState<coop::net::IntentBucket> g_budget;
// HOST: a slot's request that waits for its owed hand state to be applied; at most one a slot.
struct Parked {
    bool         valid = false;
    std::wstring key;
    std::wstring tag;
};
coop::roster_ledger::PerSlotState<Parked> g_parked;
uint64_t g_applied = 0, g_refused = 0;
// The events the drill judges on, each counted where it happens. HOST: the host player's own tags told to the clients.
// CLIENT, this session: the requests asked, the host's refusals of them, the host's tags for them (each spends its
// item), and the host's tags of any author.
uint64_t g_hostPublished = 0;
uint64_t g_asked = 0, g_refusedAnswers = 0, g_spent = 0, g_publishedSeen = 0;
// HOST: the keys of the props whose tag this session published, which a joiner's connect replay names to it. A joiner's
// save is cut at one instant and the host plays on while it loads; a publish is not sent to a slot before its
// world-ready (session.cpp's pre-world gate), and a prop's spawn row and record carry no nametag (prop_save_data.h). A
// set of keys: the tag itself is read off the prop at the replay, so it is the one then.
std::set<std::wstring> g_tagged;
constexpr size_t       kMaxTagged = 1024;
bool                   g_taggedFullSaid = false;
// CLIENT: tags for props this client does not hold yet, by key, latest wins. A joiner's replay reaches it ahead of the
// snapshot that births some of the props; the park is retried only when the key index moves (prop_key_index::Version).
// A new park sets the version to a value no index has, so the next Tick walks once and a move that came before the
// park (the index drained inside a Find) is not taken for one already seen. The park is the world's: it goes with the
// world generation, and with the session.
std::map<std::wstring, std::wstring> g_unbound;
constexpr uint64_t                   kWalkNext = ~0ull;
uint64_t                             g_unboundVersion = 0;
uint32_t                             g_unboundGen = 0;
constexpr size_t                     kMaxUnbound = 256;
// HOST: the keys of tagged props a joiner's replay could not read yet (the key index answered Pending), per slot, tried
// again each tick until the index answers.
coop::roster_ledger::PerSlotState<std::vector<std::wstring>> g_joinRetry;
bool                                                         g_joinRetryOwed = false;
// HOST: g_tagged is walked when the key index's table moves, which a prop's destroy does.
uint64_t g_taggedVersion = 0;

// Valid UTF-16 as the engine's string takes it: no NUL, no unpaired or reversed surrogate, no U+FFFE or U+FFFF.
bool ValidUtf16(const std::wstring& t) {
    for (size_t i = 0; i < t.size(); ++i) {
        const wchar_t c = t[i];
        if (c == 0 || c == 0xFFFE || c == 0xFFFF) return false;
        if (c >= 0xDC00 && c <= 0xDFFF) return false;   // a low surrogate with no high one before it
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 >= t.size() || t[i + 1] < 0xDC00 || t[i + 1] > 0xDFFF) return false;
            ++i;
        }
    }
    return true;
}

// A tag is the first line of a notebook page, so a tab can stand in it; the other control units cannot be typed there,
// and a typed string is valid UTF-16.
bool TagSane(const std::wstring& t) {
    if (t.empty() || t.size() > kMaxTagChars) return false;
    for (wchar_t c : t)
        if ((c < 0x20 && c != L'\t') || c == 0x7F) return false;
    return ValidUtf16(t);
}

// The message body, little-endian: [op][reason][author][key length][key][tag length u16][tag, UTF-16]. Its longest is a key
// of 31 and a tag of kMaxTagChars; a chunk count above what that needs is refused before an assembly reserves for it.
struct Body {
    uint8_t      op = 0, reason = 0, author = 0;
    std::wstring key;
    std::wstring tag;
};
constexpr size_t kHeadBytes = 4;
constexpr size_t kMaxBody   = kHeadBytes + kMaxKeyChars + 2 + 2 * kMaxTagChars;
constexpr size_t kChunkData = sizeof(coop::net::BlobChunkPayload{}.data);
constexpr size_t kMaxChunks = (kMaxBody + kChunkData - 1) / kChunkData;
static_assert(kMaxBody <= coop::blob_chunks::MaxBlobBytes(), "a nametag message must fit the blob ceiling");
static_assert(kMaxChunks <= 255, "a nametag message's chunk count is a u8");

// Empty when the key or the tag is past what the body can say: nothing is cut.
std::vector<uint8_t> Pack(const Body& b) {
    if (b.key.size() > kMaxKeyChars || b.tag.size() > kMaxTagChars) return {};
    std::vector<uint8_t> out;
    out.reserve(kHeadBytes + b.key.size() + 2 + 2 * b.tag.size());
    out.push_back(b.op);
    out.push_back(b.reason);
    out.push_back(b.author);
    out.push_back(static_cast<uint8_t>(b.key.size()));
    for (wchar_t c : b.key) out.push_back(static_cast<uint8_t>(c & 0xFF));
    const size_t n = b.tag.size();
    out.push_back(static_cast<uint8_t>(n & 0xFF));
    out.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
    for (wchar_t c : b.tag) {
        out.push_back(static_cast<uint8_t>(c & 0xFF));
        out.push_back(static_cast<uint8_t>((c >> 8) & 0xFF));
    }
    return out;
}

// Strict: every length is read from the body and must add up to it exactly. The key is filled as soon as it has been read
// whole, so a refusal can name it even when the tag is the part that is wrong.
bool Unpack(const std::vector<uint8_t>& b, Body& out, const char** why) {
    out = Body{};
    if (b.size() < kHeadBytes + 2) { *why = "shorter than its head"; return false; }
    out.op     = b[0];
    out.reason = b[1];
    out.author = b[2];
    const size_t keyLen = b[3];
    if (keyLen > kMaxKeyChars || b.size() < kHeadBytes + keyLen + 2) { *why = "a key length the body cannot hold"; return false; }
    size_t o = kHeadBytes;
    for (size_t i = 0; i < keyLen; ++i) out.key.push_back(static_cast<wchar_t>(b[o + i]));
    o += keyLen;
    const size_t tagLen = static_cast<size_t>(b[o]) | (static_cast<size_t>(b[o + 1]) << 8);
    o += 2;
    if (tagLen > kMaxTagChars) { *why = "a tag longer than the game's name holds"; return false; }
    if (o + tagLen * 2 != b.size()) { *why = "a tag length that does not add up to the body"; return false; }
    for (size_t i = 0; i < tagLen; ++i, o += 2)
        out.tag.push_back(static_cast<wchar_t>(b[o] | (b[o + 1] << 8)));
    if (!ValidUtf16(out.tag)) { *why = "a tag that is not valid UTF-16"; return false; }
    return true;
}

const char* ReasonName(uint8_t reason) {
    switch (reason) {
    case coop::net::kPropNametagBadTag:     return "the tag is empty, over 1023 units, holds a control character or invalid UTF-16, or the message is malformed";
    case coop::net::kPropNametagNoProp:     return "the host holds no prop of that key";
    case coop::net::kPropNametagNoItem:     return "no nametag in the sender's hand";
    case coop::net::kPropNametagTooFast:    return "the sender is past its budget";
    case coop::net::kPropNametagOutOfReach: return "the prop is not within the sender's reach";
    case coop::net::kPropNametagNotTaken:   return "the host's prop did not take the tag";
    case coop::net::kPropNametagBusy:       return "an earlier tag of the sender still waits for its hand state";
    default:                                return "a reason this build does not know";
    }
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

// The messages this lane sends are chunked blobs on the Default lane (blob_chunks), as PropSaveDataIntent's and
// KerfurCommand's are (kerfur_command_wire.cpp Send): ordered with the hand state and the spawns. `slot` < 0 is every
// peer with a world; else that peer alone. The assembly of the chunks that arrive is one per kind, keyed by sender.
BC::Assembler g_asm;
uint32_t      g_blobSeq = 1;
std::chrono::steady_clock::time_point g_nextSweep{};
constexpr std::chrono::seconds kSweepEvery{1};
constexpr std::chrono::seconds kAssemblyTtl{30};

bool SendBody(coop::net::Session& s, int slot, const Body& b) {
    const std::vector<uint8_t> blob = Pack(b);
    if (blob.empty()) return false;
    return slot < 0 ? BC::SendBlob(&s, coop::net::ReliableKind::PropNametag, g_blobSeq++, blob)
                    : BC::SendBlobToSlot(&s, slot, coop::net::ReliableKind::PropNametag, g_blobSeq++, blob);
}

// HOST: the prop of `key` carries `tag` now; every client is told.
void Publish(coop::net::Session& s, const std::wstring& key, uint8_t author, const std::wstring& tag) {
    if (tag.size() > kMaxTagChars || !ValidUtf16(tag)) {
        // The host player's own tag, put on by the game, that an FName cannot hold or that is not valid UTF-16: said once,
        // since a player who wrote it will write another.
        static bool s_said = false;
        if (!s_said) {
            s_said = true;
            UE_LOGW("prop_nametag: the host player's tag on prop '%ls' (%zu characters) is over the %zu a name holds or is "
                    "not valid UTF-16 -- the clients keep the one they show (said once)", key.c_str(), tag.size(),
                    kMaxTagChars);
        }
        return;
    }
    if (g_tagged.size() < kMaxTagged || g_tagged.count(key)) {
        g_tagged.insert(key);
    } else if (!g_taggedFullSaid) {
        g_taggedFullSaid = true;
        UE_LOGW("prop_nametag: %zu props carry a tag published this session -- a joiner is not told the tags after them",
                kMaxTagged);
    }
    // A host with no client that has a world has no one to tell; the joiner's connect replay names the tag.
    if (!s.AnyWorldReadyPeer()) return;
    if (!SendBody(s, -1, Body{coop::net::kPropNametagPublish, 0, author, key, tag}))
        UE_LOGW("prop_nametag: the tag of prop '%ls' did not send -- the clients keep the one they show",
                key.c_str());
}

// The watch is on every class's playerHandUse_LMB, so the coin gun or the trash packer calls it too. Whether a class sits
// under prop_nametag_C is a walk of its chain by names, so it runs once per distinct class (hook_retrieve's per-class
// verdict). A Blueprint class dies with its world: the table goes with the world's generation.
std::unordered_map<void*, bool> g_itemClass;
uint32_t                        g_itemClassGen = 0;

bool IsNametagObject(void* obj) {
    void* cls = obj ? R::ClassOf(obj) : nullptr;
    if (!cls) return false;
    const uint32_t gen = ue_wrap::world_identity::Generation();
    if (gen != g_itemClassGen) { g_itemClass.clear(); g_itemClassGen = gen; }
    auto it = g_itemClass.find(cls);
    if (it == g_itemClass.end()) it = g_itemClass.emplace(cls, NT::IsNametagClass(cls)).first;
    return it->second;
}

sg::Verdict OnUsePre(const sg::Call& c) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !IsNametagObject(c.object)) return sg::Verdict::Run;
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
        UE_LOGW("prop_nametag: this client's tag of %zu characters on prop %p cannot be named to the host (%s) -- not "
                "applied, the item stays", tag.size(), prop,
                (key.empty() || key.size() > kMaxKeyChars) ? "the prop has no key the wire can carry"
                                                           : ReasonName(coop::net::kPropNametagBadTag));
        return sg::Verdict::Cancel;
    }
    if (!g_pending.key.empty()) {   // asked already: the host's answer comes; said once, since a latch that stuck looks like this
        if (!g_pending.waitSaid) {
            g_pending.waitSaid = true;
            UE_LOGI("prop_nametag: this client's tag on prop '%ls' still waits for the host's answer -- a click meanwhile "
                    "is not sent", g_pending.key.c_str());
        }
        return sg::Verdict::Cancel;
    }
    if (!SendBody(*s, 0, Body{coop::net::kPropNametagRequest, 0, 0, key, tag})) {
        UE_LOGW("prop_nametag: the tag for prop '%ls' was not sent -- the item stays; use it again", key.c_str());
        return sg::Verdict::Cancel;
    }
    g_pending.key = key;
    g_pending.item.Set(c.object);
    ++g_asked;
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
    if (key.empty() || key.size() > kMaxKeyChars) {
        static bool s_said = false;
        if (!s_said) {
            s_said = true;
            UE_LOGW("prop_nametag: the host player's tag on prop %p has no key the wire can carry ('%ls') -- the clients "
                    "keep the one they show (said once)", prop, key.c_str());
        }
        return;
    }
    Publish(*s, key, kAuthorHost, tag);
    ++g_hostPublished;
    UE_LOGI("prop_nametag: the host player tagged prop '%ls' '%ls' -- the clients are told", key.c_str(), tag.c_str());
}

// HOST: a request the host will not grant is answered to its sender alone, with the reason, which releases that
// client's latch. `detail` adds what the reason alone does not say.
void Refuse(coop::net::Session& s, uint8_t slot, const std::wstring& key, uint8_t reason, const char* detail = nullptr) {
    ++g_refused;
    if (g_refused <= 20 || g_refused % 50 == 0)
        UE_LOGI("prop_nametag: slot %u's tag on prop '%ls' refused (%s%s%s)", static_cast<unsigned>(slot), key.c_str(),
                ReasonName(reason), detail ? ": " : "", detail ? detail : "");
    if (!SendBody(s, slot, Body{coop::net::kPropNametagRefused, reason, 0, key, std::wstring()}))
        UE_LOGW("prop_nametag: the refusal of slot %u's tag on prop '%ls' did not send", static_cast<unsigned>(slot),
                key.c_str());
}

// A message this peer does not act on, at the first twenty and each fiftieth after: no honest peer sends one. `op` is -1
// (kNoOp) when the drop came before any op was read.
constexpr int kNoOp = -1;
void DropLogged(const char* why, int slot, int op) {
    static uint64_t s_dropped = 0;
    ++s_dropped;
    if (s_dropped > 20 && s_dropped % 50 != 0) return;
    if (op < 0)
        UE_LOGW("prop_nametag: a message from slot %d dropped before its op was read (%s); %llu so far", slot, why,
                static_cast<unsigned long long>(s_dropped));
    else
        UE_LOGW("prop_nametag: op %u from slot %d dropped (%s); %llu so far", static_cast<unsigned>(op), slot, why,
                static_cast<unsigned long long>(s_dropped));
}

// HOST: a request's judgement, with the sender's hand as the hand lane has applied it.
// MTA's shape for a client changing a named property of a world element is Packet_CustomData
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:2715-2796): the server resolves the element, judges the
// sender, relays to everyone but the source and, on a refusal, sends its own value back to the source. Ours differs in
// three ways. The publish goes to every peer, the author included, because the client cancelled its own apply where
// MTA's source had applied it. The refusal carries a reason and no value, for the same reason. A prop that does not
// resolve is answered (op 3) where MTA returns silently, because a client keeps one request in flight on that answer.
// The trust gate is the sender's reach and the item in its hand, where MTA's is a per-element flag.
void Judge(coop::net::Session& s, const std::wstring& key, const std::wstring& tag, uint8_t slot) {
    if (!g_budget[slot].Take(kTagBudget, coop::net::NowMs())) { Refuse(s, slot, key, coop::net::kPropNametagTooFast); return; }
    if (!TagSane(tag)) { Refuse(s, slot, key, coop::net::kPropNametagBadTag); return; }
    void* prop = coop::prop_key_index::Find(key);
    if (!prop || !PR::IsDescendantOfProp(prop)) { Refuse(s, slot, key, coop::net::kPropNametagNoProp); return; }
    if (!coop::hand_item::HeldClassIs(slot, L"prop_nametag_C")) {
        Refuse(s, slot, key, coop::net::kPropNametagNoItem);
        return;
    }
    const auto sub = coop::element::IntentTarget::ForClientIntent(s, slot, kReachUU).Authorize(prop);
    if (!sub) { Refuse(s, slot, key, coop::net::kPropNametagOutOfReach, coop::element::OutcomeName(sub.outcome)); return; }
    if (!NT::ApplyPropNametag(prop, tag)) { Refuse(s, slot, key, coop::net::kPropNametagNotTaken); return; }
    ++g_applied;
    Publish(s, key, slot, tag);
    UE_LOGI("prop_nametag: HOST tagged prop '%ls' '%ls' for slot %u (%llu for clients)", key.c_str(), tag.c_str(),
            static_cast<unsigned>(slot), static_cast<unsigned long long>(g_applied));
}

// HOST: a client's request. The hand state of the sender rode the same ordered Default lane ahead of it (a nametag is held
// before it is clicked), and the drain applies each message in arrival order, the request inline as PropSaveData's chunks
// are. But the host applies a client's hand states at a pace (hand_item's LatestWins), so the state applied may be older
// than the one that arrived. While one is owed the request waits for its application, the lane's own edge
// (OnHandApplied), and is judged against the hand then. One residual, answered by op 3 with the item kept: a click made
// before its owner's announce (the client does not wait for OwnAnnounced; the drill does).
void OnRequest(coop::net::Session& s, const std::wstring& key, const std::wstring& tag, uint8_t slot) {
    Parked& waiting = g_parked[slot];
    if (waiting.valid) { Refuse(s, slot, key, coop::net::kPropNametagBusy); return; }
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
    ++g_publishedSeen;
    void* prop = coop::prop_key_index::Find(key);
    if (!prop) {
        const uint32_t gen = ue_wrap::world_identity::Generation();
        if (gen != g_unboundGen) { g_unbound.clear(); g_unboundGen = gen; }
        if (g_unbound.size() < kMaxUnbound || g_unbound.count(key)) {
            g_unbound[key] = tag;
            g_unboundVersion = kWalkNext;
            UE_LOGI("prop_nametag: the host's tag '%ls' for prop '%ls' waits -- no copy of it here yet (%zu waiting)",
                    tag.c_str(), key.c_str(), g_unbound.size());
        } else {
            UE_LOGW("prop_nametag: the host's tag '%ls' for prop '%ls' is dropped -- no copy of it here and %zu tags "
                    "already wait", tag.c_str(), key.c_str(), kMaxUnbound);
        }
    } else {
        g_unbound.erase(key);   // a newer word for a prop that is here: an older wait must not land over it
        if (!NT::ApplyPropNametag(prop, tag))
            UE_LOGW("prop_nametag: the host's tag '%ls' for prop '%ls' did not apply here (the copy did not take it)",
                    tag.c_str(), key.c_str());
    }
    if (author != coop::players::Registry::Get().LocalPeerId() || g_pending.key != key) return;
    // The item is this client's own, in an inventory the host cannot see, so it is spent here, by the client whose
    // request it was; the host's judgement cannot spend it. A modified client that keeps it is bounded by the budget.
    if (void* item = g_pending.item.Get()) E::DestroyActor(item);
    UE_LOGI("prop_nametag: this client's tag on prop '%ls' applied by the host -- its nametag is spent", key.c_str());
    g_pending = Pending{};
    ++g_spent;
}

// CLIENT: the host refused this client's request: the item stays and the client may use it again.
void OnRefused(const std::wstring& key, uint8_t reason) {
    if (g_pending.key.empty() || (!key.empty() && g_pending.key != key)) return;   // an empty key answers the one request in flight
    ++g_refusedAnswers;
    UE_LOGW("prop_nametag: the host refused this client's tag on prop '%ls' (%s) -- the item stays", key.c_str(),
            ReasonName(reason));
    g_pending = Pending{};
}

// One whole message. The host answers a request it cannot read with the refusal, naming the key when it could be read, as
// it answers every request it can; anything else a peer should not send is dropped and logged.
void OnBody(coop::net::Session& s, const std::vector<uint8_t>& blob, int senderPeerSlot) {
    const bool host = s.role() == coop::net::Role::Host;
    Body b;
    const char* why = "";
    if (!Unpack(blob, b, &why)) {
        if (!host) { DropLogged(why, senderPeerSlot, b.op); return; }
        const uint8_t slot = static_cast<uint8_t>(senderPeerSlot);
        // A refusal costs the host a send, so it is paid from the sender's budget like a judgement.
        if (g_budget[slot].Take(kTagBudget, coop::net::NowMs())) Refuse(s, slot, b.key, coop::net::kPropNametagBadTag, why);
        else DropLogged("a malformed message past the sender's budget", senderPeerSlot, b.op);
        return;
    }
    if (host) {
        if (b.op != coop::net::kPropNametagRequest || b.key.empty()) {
            DropLogged("a client may only ask, and by a prop's key", senderPeerSlot, b.op);
            return;
        }
        OnRequest(s, b.key, b.tag, static_cast<uint8_t>(senderPeerSlot));
        return;
    }
    if (b.op == coop::net::kPropNametagRefused) { OnRefused(b.key, b.reason); return; }
    if (b.op != coop::net::kPropNametagPublish || b.key.empty()) {
        DropLogged("an op this build does not know, or a tag with no prop", senderPeerSlot, b.op);
        return;
    }
    OnPublish(b.key, b.tag, b.author);
}

// The half-assembled messages of a peer that stopped sending are reclaimed after a TTL.
void SweepAssemblies() {
    if (g_asm.Idle()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextSweep) return;
    g_nextSweep = now + kSweepEvery;
    g_asm.Sweep(now, kAssemblyTtl);
}

// CLIENT: the waiting tags whose props have appeared. Nothing but the key index's own table moving can change an
// answer, so the walk runs when its Version has.
void RetryUnbound() {
    if (g_unbound.empty()) return;
    if (ue_wrap::world_identity::Generation() != g_unboundGen) {
        UE_LOGI("prop_nametag: the world changed -- %zu waiting tag(s) dropped with it", g_unbound.size());
        g_unbound.clear();
        return;
    }
    const uint64_t version = coop::prop_key_index::Version();
    if (version == g_unboundVersion) return;
    g_unboundVersion = version;
    for (auto it = g_unbound.begin(); it != g_unbound.end();) {
        void* prop = coop::prop_key_index::Find(it->first);
        if (!prop) { ++it; continue; }
        if (NT::ApplyPropNametag(prop, it->second))
            UE_LOGI("prop_nametag: prop '%ls' is here now -- the host's tag '%ls' applied", it->first.c_str(),
                    it->second.c_str());
        else
            UE_LOGW("prop_nametag: prop '%ls' is here now but did not take the host's tag '%ls'", it->first.c_str(),
                    it->second.c_str());
        it = g_unbound.erase(it);
    }
}

// HOST: one tagged prop named to one joiner, with the tag it carries now. Later: not yet answerable or not yet accepted.
enum class Named { Sent, Gone, Later, Failed };
Named NameTo(coop::net::Session& s, int slot, const std::wstring& key) {
    void*      prop = nullptr;
    const auto read = coop::prop_key_index::Resolve(key, &prop);
    if (read == coop::prop_key_index::Read::Miss) return Named::Gone;    // the prop is gone, and its tag with it
    if (read == coop::prop_key_index::Read::Pending) return Named::Later;   // the index cannot answer yet
    std::wstring tag;
    if (!NT::ReadPropNametag(prop, tag) || tag.size() > kMaxTagChars || !ValidUtf16(tag)) {
        UE_LOGW("prop_nametag: prop '%ls' could not be read at slot %d's connect replay -- its tag is not named",
                key.c_str(), slot);
        return Named::Failed;
    }
    if (SendBody(s, slot, Body{coop::net::kPropNametagPublish, 0, kAuthorHost, key, tag})) return Named::Sent;
    // The transport refused it (the send backlog is full): asked again next tick, as a Pending answer is. A slot that
    // leaves clears its list, and a session's end clears them all.
    return Named::Later;
}

// HOST: the replay's props the index could not answer for, or the transport did not accept, asked again each tick.
void RetryJoinTags() {
    if (!g_joinRetryOwed) return;
    auto* s = g_session.load(std::memory_order_acquire);
    g_joinRetryOwed = false;
    if (!s || s->role() != coop::net::Role::Host) return;
    for (int slot = 1; slot < g_joinRetry.size(); ++slot) {
        std::vector<std::wstring>& owed = g_joinRetry[slot];
        if (owed.empty()) continue;
        std::vector<std::wstring> still;
        for (const std::wstring& key : owed) {
            if (!g_tagged.count(key)) continue;
            const Named n = NameTo(*s, slot, key);
            if (n == Named::Later) still.push_back(key);
            else if (n == Named::Gone) g_tagged.erase(key);
        }
        owed.swap(still);
        if (!owed.empty()) g_joinRetryOwed = true;
    }
}

// HOST: a tagged prop that was destroyed leaves g_tagged. The key index's table moves when a prop dies, so the walk runs
// then; a Pending answer keeps the key.
void PruneTagged() {
    if (g_tagged.empty()) return;
    const uint64_t version = coop::prop_key_index::Version();
    if (version == g_taggedVersion) return;
    g_taggedVersion = version;
    for (auto it = g_tagged.begin(); it != g_tagged.end();) {
        void* prop = nullptr;
        if (coop::prop_key_index::Resolve(*it, &prop) == coop::prop_key_index::Read::Miss) it = g_tagged.erase(it);
        else ++it;
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!coop::hand_item::SubscribeHandApplied(&OnHandApplied))
        UE_LOGE("prop_nametag: the hand lane's subscriber table is full -- a request waiting for its hand state is not released");
}

void Tick() {
    SweepAssemblies();
    RetryUnbound();
    RetryJoinTags();
    PruneTagged();
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

void OnChunk(coop::net::Session& session, const coop::net::BlobChunkPayload& chunk, int senderPeerSlot) {
    UE_ASSERT_GAME_THREAD("prop_nametag::OnChunk");
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s != &session || !s->connected()) return;
    if (senderPeerSlot < 0 || senderPeerSlot >= coop::players::kMaxPeers) return;
    // A request reaches the host from a client alone, and the host's word reaches a client from the host alone.
    if ((s->role() == coop::net::Role::Host) == (senderPeerSlot == 0)) {
        DropLogged("a message from the wrong side of the link", senderPeerSlot, kNoOp);
        return;
    }
    // A header that declares more chunks than a nametag needs is refused before the assembler reserves for it, as MTA's
    // latent receiver refuses a declared size at the first part (CLatentReceiver.cpp:128-129).
    if (chunk.chunks > kMaxChunks) {
        DropLogged("a header that declares more chunks than a nametag needs", senderPeerSlot, kNoOp);
        return;
    }
    std::vector<uint8_t> blob;
    if (!g_asm.OnChunk(chunk, static_cast<uint8_t>(senderPeerSlot), blob)) return;
    OnBody(*s, blob, senderPeerSlot);
}

void OnPeerGone(uint8_t slot) { g_asm.ClearSlot(slot); }

// The join's answer, the shape of hook_retrieve's SendJoinGone and join_window_baseline's removes: a joiner's save is a
// photograph, the host plays on while it loads, and a tag is not sent to a slot before its world-ready. So at the slot's
// connect replay each prop whose tag this session published is named to that slot alone, with the tag it carries now.
void SendJoinTags(int slot) {
    if (!GT::IsGameThread() || slot < 1 || slot >= coop::players::kMaxPeers) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host || g_tagged.empty()) return;
    static const bool s_off = coop::config::ResolveFlag(coop::config_registry::rows::nametag_no_join_replay);
    if (s_off) return;   // the nametag drill's red: the join as it was before this answer
    size_t sent = 0, later = 0;
    g_joinRetry[slot].clear();
    for (auto it = g_tagged.begin(); it != g_tagged.end();) {
        switch (NameTo(*s, slot, *it)) {
        case Named::Sent:   ++sent; ++it; break;
        case Named::Gone:   it = g_tagged.erase(it); break;
        case Named::Later:  g_joinRetry[slot].push_back(*it); g_joinRetryOwed = true; ++later; ++it; break;
        case Named::Failed: ++it; break;
        }
    }
    UE_LOGI("prop_nametag: slot %d world-ready -- %zu of %zu tagged prop(s) named to it, %zu to be asked again", slot,
            sent, g_tagged.size(), later);
}

uint64_t AppliedForClients() { return g_applied; }
uint64_t HostPublishedCount() { return g_hostPublished; }
uint64_t AskedCount() { return g_asked; }
uint64_t RefusedAnswerCount() { return g_refusedAnswers; }
uint64_t SpentCount() { return g_spent; }
uint64_t PublishedCount() { return g_publishedSeen; }

void OnDisconnect() {
    if (g_applied || g_refused)
        UE_LOGI("prop_nametag: session end -- applied for clients=%llu refused=%llu",
                static_cast<unsigned long long>(g_applied), static_cast<unsigned long long>(g_refused));
    g_refused = 0;
    g_hostPublished = g_asked = g_refusedAnswers = g_spent = g_publishedSeen = 0;
    g_pending = Pending{};
    g_hostItem = g_hostProp = nullptr;
    g_hostTag.clear();
    g_itemClass.clear();
    g_tagged.clear();
    g_taggedFullSaid = false;
    g_unbound.clear();
    g_unboundVersion = 0;
    g_unboundGen = 0;
    g_taggedVersion = 0;
    g_asm.Clear();
    g_blobSeq = 1;
    g_joinRetryOwed = false;
    for (int i = 0; i < g_joinRetry.size(); ++i) g_joinRetry[i].clear();
    for (int i = 0; i < g_budget.size(); ++i) g_budget[i].Reset();
    for (int i = 0; i < g_parked.size(); ++i) g_parked[i] = Parked{};
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::prop_nametag
