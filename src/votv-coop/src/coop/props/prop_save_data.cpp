// coop/props/prop_save_data.cpp -- see coop/props/prop_save_data.h.

#include "coop/props/prop_save_data.h"

#include "coop/items/save_record_wire.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"
#include "coop/props/prop_element_tracker.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <chrono>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace coop::prop_save_data {
namespace {

namespace R  = ue_wrap::reflection;
namespace SR = ue_wrap::save_record;
namespace SW = coop::save_record_wire;
namespace PT = coop::prop_element_tracker;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// The body grammar, version-tagged so a future field is a parse branch and not a guess.
constexpr uint8_t kBodyVersion = 1;

// A Key is an Aprop_C save key: ASCII, at most WireKey's 31 chars.
constexpr size_t kMaxKeyChars = 31;

// The record ceiling. The transport refuses a blob past MaxBlobBytes() outright, so this sits
// below it with room for the head; a record over the cap is REFUSED and logged, never truncated --
// half a save record applied through loadData is worse than none, and the peer that still holds
// the whole prop can re-publish.
size_t MaxRecordBytes() { return coop::blob_chunks::MaxBlobBytes() - 64; }

// The park: records whose prop has not arrived, or whose apply has not had a budget slot yet.
// Keyed by Key and NEVER expired (the header says why). Sized for a legitimate join: one record
// per covered prop, measured at 766 in the test world.
//
// The cap is PER SENDER, for the reason blob_chunks states about its own table: a global one lets
// one flooding peer evict every other peer's rows. A sender's records are its own to lose. Both a
// count and a byte ceiling, since a record's size is the sender's to choose.
constexpr size_t kMaxParkedPerSender  = 2048;
constexpr size_t kMaxParkBytesPerSender = 4u * 1024u * 1024u;

// An apply is a ProcessEvent into the prop's own loadData, which on the Aprop_C lineage re-runs
// init(), physicsImpact->init() and setNametag() -- the same work a save load does. Measured at
// ~0.7 ms: applying a whole join's worth in the drain frame that closes the snapshot put 519 ms on
// a frame already 1.5 s long. So applies are BUDGETED per frame and the surplus waits in the park,
// which is the queue this lane already has. An apply costs TWO dispatches, the base capture and
// loadData itself, so 8 a frame is about the same ~11 ms and a 766-record join converges in a
// second or two with every prop already correct in everything but its save payload.
constexpr int    kAppliesPerFrame = 8;
// How far into the park one frame looks. The park is ordered by Key, so the cursor rotates through
// it and nothing starves; bounded so a large park is never a per-frame walk of thousands.
constexpr size_t kParkScanPerFrame = 64;

struct Parked {
    SR::SaveRecord rec;
    uint8_t        senderSlot = 0;
    uint64_t       seq        = 0;   // arrival order, for the oldest-first eviction
    size_t         bytes      = 0;   // the body this came from, for the per-sender byte ceiling
};
std::map<std::wstring, Parked> g_parked;
uint64_t g_parkSeq = 1;
struct SenderUse { size_t count = 0; size_t bytes = 0; };
std::map<uint8_t, SenderUse> g_parkUse;   // per-sender occupancy, so one peer cannot evict another's

// Keys whose publish was refused by the transport. A refused record is a permanent divergence
// unless it is re-sent, and it can be: the record is derivable from the live actor at any time.
// Bounded, and retried a few per frame from Drive().
constexpr size_t kMaxRetry        = 512;
constexpr int    kRetriesPerFrame = 4;
struct Refused { int peerSlot = -1; };            // -1 = it was a broadcast
std::map<std::wstring, Refused> g_retry;
coop::net::Session* g_session = nullptr;          // for the retry, which has no caller of its own

// Keys the host spawned from a client's intent and whose record is still in flight behind it.
constexpr size_t kMaxAwaiting = 256;
std::set<std::wstring> g_awaiting;
// Classes a dedicated lane owns, by name, and the resolved answer per UClass.
std::set<std::wstring> g_ownedElsewhere;
std::map<void*, bool>  g_ownedCache;

std::wstring g_parkCursor;      // where the next frame's park scan resumes
int g_applyBudget = kAppliesPerFrame;
uint64_t g_nextSweepMs = 0;
// Applied since connect, both paths. Per-record lines are not logged -- a join lands hundreds --
// so this is the number a run is read for, printed when the park empties.
uint64_t g_appliedTotal = 0;

coop::blob_chunks::Assembler g_asmCanonical;
coop::blob_chunks::Assembler g_asmIntent;
uint32_t g_blobSeq = 1;

std::vector<uint8_t> BuildBody(const std::wstring& key, const SR::SaveRecord& rec) {
    std::vector<uint8_t> body;
    body.push_back(kBodyVersion);
    body.push_back(static_cast<uint8_t>(key.size()));
    for (wchar_t c : key) body.push_back(static_cast<uint8_t>(c & 0x7F));
    SW::SerSave(body, rec);
    return body;
}

// Strict on format: an ill-formed body is refused whole (the receive-boundary rule -- strict on
// format, permissive on state).
bool ParseBody(const std::vector<uint8_t>& body, std::wstring& key, SR::SaveRecord& rec) {
    size_t o = 0;
    uint8_t ver = 0, keyLen = 0;
    if (!SW::RdU8(body, o, ver) || ver != kBodyVersion) return false;
    if (!SW::RdU8(body, o, keyLen) || keyLen == 0 || keyLen > kMaxKeyChars) return false;
    if (o + keyLen > body.size()) return false;
    key.assign(body.begin() + static_cast<long>(o), body.begin() + static_cast<long>(o + keyLen));
    o += keyLen;
    if (!SW::DeSave(body, o, rec)) return false;
    // The body ADDRESSES a prop by the head key and CARRIES a record whose own key field the
    // game's loadData would write onto that prop. If the two disagree the message is asking to
    // rename one prop to another's identity, so it is refused whole rather than reconciled.
    return rec.key.empty() || rec.key == key;
}

// Drop one parked row and give its sender the room back.
void Unpark(std::map<std::wstring, Parked>::iterator it) {
    SenderUse& u = g_parkUse[it->second.senderSlot];
    if (u.count) --u.count;
    u.bytes = (u.bytes > it->second.bytes) ? (u.bytes - it->second.bytes) : 0;
    g_parked.erase(it);
}

void Park(const std::wstring& key, SR::SaveRecord&& rec, uint8_t senderSlot, size_t bytes) {
    auto existing = g_parked.find(key);
    if (existing != g_parked.end()) Unpark(existing);   // a newer record replaces its own predecessor
    SenderUse& use = g_parkUse[senderSlot];
    while (use.count >= kMaxParkedPerSender || use.bytes + bytes > kMaxParkBytesPerSender) {
        // This sender's own oldest row goes, never another sender's.
        auto oldest = g_parked.end();
        for (auto it = g_parked.begin(); it != g_parked.end(); ++it) {
            if (it->second.senderSlot != senderSlot) continue;
            if (oldest == g_parked.end() || it->second.seq < oldest->second.seq) oldest = it;
        }
        if (oldest == g_parked.end()) break;   // nothing of this sender's left to give back
        UE_LOGW("prop_save_data: slot %u is at its park ceiling (%zu rows, %zu B) -- evicting its "
                "oldest record (key '%ls'); that prop's save state is now whatever its peer holds",
                static_cast<unsigned>(senderSlot), use.count, use.bytes, oldest->first.c_str());
        Unpark(oldest);
    }
    Parked p;
    p.rec = std::move(rec);
    p.senderSlot = senderSlot;
    p.seq = g_parkSeq++;
    p.bytes = bytes;
    use.count += 1;
    use.bytes += bytes;
    g_parked[key] = std::move(p);
}

// A client's intent is a write into the host's world, so its RATE is bounded per sender the way
// its size is. The ceiling is generous against real play -- an eject, a drop, a hand switch are
// human-rate -- and refuses a stream. A refusal is loud once per window, not per message, because
// a flood must not become a log flood.
constexpr int      kIntentsPerWindow = 60;
constexpr uint64_t kIntentWindowMs   = 10000;
struct IntentRate { uint64_t windowStartMs = 0; int count = 0; bool warned = false; };
std::map<uint8_t, IntentRate> g_intentRate;

bool AllowIntent(uint8_t senderSlot) {
    IntentRate& r = g_intentRate[senderSlot];
    const uint64_t now = NowMs();
    if (now - r.windowStartMs >= kIntentWindowMs) {
        r.windowStartMs = now;
        r.count = 0;
        r.warned = false;
    }
    if (++r.count <= kIntentsPerWindow) return true;
    if (!r.warned) {
        r.warned = true;
        UE_LOGW("prop_save_data: slot %u is past %d save-record intents in %llu ms -- refusing the "
                "rest of this window", static_cast<unsigned>(senderSlot), kIntentsPerWindow,
                static_cast<unsigned long long>(kIntentWindowMs));
    }
    return false;
}

// Apply to the live actor if there is one, park by Key if there is not.
void LandRecord(const std::wstring& key, SR::SaveRecord&& rec, uint8_t senderSlot, size_t bytes) {
    // The index only, never ResolveLiveActorByKey: that one falls back to a cold GUObjectArray
    // scan on a miss, and a join hands this lane hundreds of arrivals at once. A prop the index
    // does not know yet is parked and retried at O(1) on the next frame instead.
    void* actor = (g_applyBudget > 0) ? PT::FindLiveActorByKey(key) : nullptr;
    if (actor && SR::ApplyRecord(actor, rec)) {
        --g_applyBudget;
        ++g_appliedTotal;
        return;
    }
    Park(key, std::move(rec), senderSlot, bytes);
}

bool SendBody(coop::net::Session* s, int peerSlot, const std::wstring& key,
              const SR::SaveRecord& rec) {
    const std::vector<uint8_t> body = BuildBody(key, rec);
    if (body.size() > MaxRecordBytes()) {
        UE_LOGW("prop_save_data: record for key '%ls' is %zu B, over the %zu cap -- REFUSED, not "
                "truncated; that prop's state stays where it is",
                key.c_str(), body.size(), MaxRecordBytes());
        return false;
    }
    const bool host = PT::SessionIsHost();
    const coop::net::ReliableKind kind = host ? coop::net::ReliableKind::PropSaveData
                                              : coop::net::ReliableKind::PropSaveDataIntent;
    const bool ok = (peerSlot < 0)
        ? coop::blob_chunks::SendBlob(s, kind, g_blobSeq++, body)
        : coop::blob_chunks::SendBlobToSlot(s, peerSlot, kind, g_blobSeq++, body);
    if (!ok) {
        // blob_chunks' own contract: a refused blob is retried whole under a fresh seq. Doing that
        // is what makes the header's "a dropped record is a permanent divergence" false, so the
        // key goes on the retry register rather than into a warning nobody can act on.
        if (g_retry.size() < kMaxRetry) g_retry[key] = Refused{peerSlot};
        else UE_LOGW("prop_save_data: the retry register is full (%zu) -- the record for key "
                     "'%ls' is dropped and that peer's copy of this prop stays divergent",
                     kMaxRetry, key.c_str());
    }
    return ok;
}

}  // namespace

void ExpectRecordFor(const std::wstring& key) {
    if (key.empty()) return;
    if (g_awaiting.size() >= kMaxAwaiting) g_awaiting.erase(g_awaiting.begin());
    g_awaiting.insert(key);
}

void DeclareClassOwnedElsewhere(const wchar_t* className) {
    if (className && *className) g_ownedElsewhere.insert(className);
}

bool Covers(void* actor) {
    if (!actor) return false;
    void* cls = R::ClassOf(actor);
    if (!SR::OverridesGetData(cls)) return false;
    if (g_ownedElsewhere.empty()) return true;
    // The claim is by NAME because a lane declares its class before that class is loaded; the
    // resolved answer is cached per UClass, so the name compare happens once per class.
    const auto it = g_ownedCache.find(cls);
    if (it != g_ownedCache.end()) return !it->second;
    bool owned = false;
    for (const std::wstring& n : g_ownedElsewhere) {
        void* oc = R::FindClass(n.c_str());
        if (oc && (cls == oc || R::IsDescendantOfAny(cls, &oc, 1))) { owned = true; break; }
    }
    g_ownedCache[cls] = owned;
    return !owned;
}

bool Publish(coop::net::Session* s, void* actor, const std::wstring& key) {
    if (!s || !actor || key.empty() || key.size() > kMaxKeyChars) return false;
    // Nobody to publish to: return before the capture, not at the send. A host loading its own
    // world births every keyed prop it owns, and paying getData plus a serialize per prop for a
    // send that cannot land is the whole cost of the lane spent on nothing.
    if (!s->connected() || !s->AnyWorldReadyPeer()) return false;
    if (g_awaiting.count(key)) return false;   // the author's own record is still in flight
    g_session = s;
    if (!Covers(actor)) return false;
    SR::SaveRecord rec;
    if (!SR::CaptureRecord(actor, rec)) {
        UE_LOGW("prop_save_data: getData failed on the prop keyed '%ls' -- no record published",
                key.c_str());
        return false;
    }
    return SendBody(s, -1, key, rec);
}

bool PublishToSlot(coop::net::Session* s, int peerSlot, void* actor, const std::wstring& key) {
    if (!s || !actor || peerSlot < 0 || key.empty() || key.size() > kMaxKeyChars) return false;
    if (!s->IsSlotWorldReady(peerSlot)) return false;
    if (g_awaiting.count(key)) return false;   // the author's own record is still in flight
    g_session = s;
    if (!Covers(actor)) return false;
    SR::SaveRecord rec;
    if (!SR::CaptureRecord(actor, rec)) return false;
    return SendBody(s, peerSlot, key, rec);
}

bool PublishWithSpawn(coop::net::Session* s, void* actor, const coop::net::WireKey& key,
                      int peerSlot) {
    if (key.len == 0 || key.len > kMaxKeyChars) return false;   // a keyless prop has no identity to address
    std::wstring w;
    w.reserve(key.len);
    for (uint8_t i = 0; i < key.len; ++i) w.push_back(static_cast<wchar_t>(key.data[i]));
    return peerSlot < 0 ? Publish(s, actor, w) : PublishToSlot(s, peerSlot, actor, w);
}

void OnChunk(coop::net::Session& s, const coop::net::BlobChunkPayload& p, uint8_t senderSlot,
             bool intent) {
    // An intent is a claim addressed to the authority; a client receiving one has nothing to do
    // with it, and honouring it would make every peer an author. A canonical is the other half of
    // the same rule: only the host writes one, so the host refuses a client's and a client refuses
    // anything not from slot 0. Both are format-level refusals of a message sent by the wrong
    // role, not a gate on state -- the record itself is never second-guessed.
    const bool host = PT::SessionIsHost();
    if (intent && !host) return;
    if (!intent && host) {
        UE_LOGW("prop_save_data: canonical record from slot %u refused -- a client does not "
                "publish canonicals; its own records arrive as intents",
                static_cast<unsigned>(senderSlot));
        return;
    }
    if (!intent && senderSlot != 0) {
        UE_LOGW("prop_save_data: canonical record from slot %u refused -- only the host publishes "
                "one", static_cast<unsigned>(senderSlot));
        return;
    }
    std::vector<uint8_t> body;
    if (!(intent ? g_asmIntent : g_asmCanonical).OnChunk(p, senderSlot, body)) return;
    // The same ceiling the sender enforces, applied on receive: a body between it and the
    // transport's own limit would be applied here and then REFUSED on the re-publish, leaving this
    // peer holding state no other peer can be given.
    if (body.size() > MaxRecordBytes()) {
        UE_LOGW("prop_save_data: %s body from slot %u is %zu B, over the %zu ceiling -- dropped",
                intent ? "intent" : "record", static_cast<unsigned>(senderSlot), body.size(),
                MaxRecordBytes());
        return;
    }
    if (intent && !AllowIntent(senderSlot)) return;

    std::wstring key;
    SR::SaveRecord rec;
    if (!ParseBody(body, key, rec)) {
        UE_LOGW("prop_save_data: ill-formed %s body from slot %u (%zu B) -- dropped whole",
                intent ? "intent" : "record", static_cast<unsigned>(senderSlot), body.size());
        return;
    }
    if (!intent) {
        LandRecord(key, std::move(rec), senderSlot, body.size());
        return;
    }
    // The claim has landed, so the key is no longer awaited and the host may publish it again.
    g_awaiting.erase(key);
    // The host takes a client's claim: apply or park it locally, then re-publish the result as the
    // canonical so every peer -- the author included, as the acknowledgement -- takes the same
    // bytes from the same authority.
    void* actor = PT::FindLiveActorByKey(key);
    if (actor && SR::ApplyRecord(actor, rec)) {
        UE_LOGI("prop_save_data: HOST took a client record (key '%ls', slot %u) -- republishing",
                key.c_str(), static_cast<unsigned>(senderSlot));
    } else {
        UE_LOGI("prop_save_data: HOST parked a client record (key '%ls', slot %u) -- "
                "republishing", key.c_str(), static_cast<unsigned>(senderSlot));
        Park(key, SR::SaveRecord(rec), senderSlot, body.size());
    }
    SendBody(&s, -1, key, rec);
}

bool ApplyParked(void* actor, const std::wstring& key) {
    if (!actor || key.empty()) return false;
    auto it = g_parked.find(key);
    if (it == g_parked.end()) return false;
    // ApplyRecord's bool is the DISPATCH's, not the Blueprint's: Aprop_C::loadData never assigns
    // its `return` out-param at all, so there is no refusal to read. A failure here is a codec or
    // reflection failure, and the record stays parked for the next frame to retry.
    if (!SR::ApplyRecord(actor, it->second.rec)) return false;
    ++g_appliedTotal;
    UE_LOGI("prop_save_data: parked record applied at birth (key '%ls')", key.c_str());
    Unpark(it);
    return true;
}

// Re-send a few refused records per frame, re-captured from the live actor so what goes out is
// current rather than a stale copy of what was refused.
void DriveRetries() {
    if (g_retry.empty() || !g_session) return;
    int sent = 0;
    for (auto it = g_retry.begin(); it != g_retry.end() && sent < kRetriesPerFrame;) {
        void* actor = PT::FindLiveActorByKey(it->first);
        if (!actor) { it = g_retry.erase(it); continue; }   // the prop is gone; nothing to re-send
        const int slot = it->second.peerSlot;
        const std::wstring key = it->first;
        it = g_retry.erase(it);
        ++sent;
        if (slot < 0) Publish(g_session, actor, key);
        else          PublishToSlot(g_session, slot, actor, key);
    }
}

void Drive() {
    DriveRetries();
    // A fresh apply budget per frame, then spend what is left of it on the park. The scan resumes
    // at the cursor and wraps, so a record whose prop appears late is reached in bounded time
    // however large the park is.
    g_applyBudget = kAppliesPerFrame;
    size_t looked = 0;
    int    applied = 0;
    auto it = g_parked.lower_bound(g_parkCursor);
    while (looked < kParkScanPerFrame && g_applyBudget > 0 && !g_parked.empty()) {
        if (it == g_parked.end()) { it = g_parked.begin(); g_parkCursor.clear(); }
        ++looked;
        void* actor = PT::FindLiveActorByKey(it->first);
        if (actor && SR::ApplyRecord(actor, it->second.rec)) {
            --g_applyBudget;
            ++g_appliedTotal;
            ++applied;
            const auto dead = it++;
            Unpark(dead);
            continue;
        }
        g_parkCursor = it->first;
        ++it;
    }
    if (applied && !g_parked.empty()) {
        UE_LOGI("prop_save_data: applied %d parked record(s) this frame (%zu still parked)",
                applied, g_parked.size());
    } else if (applied) {
        UE_LOGI("prop_save_data: park drained -- %llu record(s) applied since connect",
                static_cast<unsigned long long>(g_appliedTotal));
    }

    // The assembler TTL sweep is 1 Hz, not per frame: a half-assembly is minutes old before it is
    // stale, and this runs from the gameplay tick.
    const uint64_t nowMs = NowMs();
    if (nowMs < g_nextSweepMs) return;
    g_nextSweepMs = nowMs + 1000;
    const auto now = std::chrono::steady_clock::now();
    g_asmCanonical.Sweep(now, std::chrono::seconds(30));
    g_asmIntent.Sweep(now, std::chrono::seconds(30));
}

void OnDisconnect() {
    g_parkCursor.clear();
    g_applyBudget = kAppliesPerFrame;
    g_nextSweepMs = 0;
    g_appliedTotal = 0;
    g_retry.clear();
    g_intentRate.clear();
    g_parkUse.clear();
    g_session = nullptr;
    g_awaiting.clear();
    // The codec's cached prop_C and per-class override answers are world-scoped: a Blueprint class
    // can be unloaded and reloaded across a level change, and a stale UClass pointer would answer
    // the membership test for a class that no longer exists.
    SR::ResetCodecCache();
    g_parked.clear();
    g_parkSeq = 1;
    g_asmCanonical.Clear();
    g_asmIntent.Clear();
    g_blobSeq = 1;
}

void OnPeerGone(uint8_t senderSlot) {
    g_asmCanonical.ClearSlot(senderSlot);
    g_asmIntent.ClearSlot(senderSlot);
    for (auto it = g_parked.begin(); it != g_parked.end();) {
        if (it->second.senderSlot == senderSlot) it = g_parked.erase(it);
        else ++it;
    }
}

}  // namespace coop::prop_save_data
