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
#include <utility>
#include <vector>

namespace coop::prop_save_data {
namespace {

namespace R  = ue_wrap::reflection;
namespace SR = ue_wrap::save_record;
namespace SW = coop::save_record_wire;
namespace PT = coop::prop_element_tracker;

// The body grammar, version-tagged so a future field is a parse branch and not a guess.
constexpr uint8_t kBodyVersion = 1;

// A Key is an Aprop_C save key: ASCII, at most WireKey's 31 chars.
constexpr size_t kMaxKeyChars = 31;

// The record ceiling. The transport refuses a blob past MaxBlobBytes() outright, so this sits
// below it with room for the head; a record over the cap is REFUSED and logged, never truncated --
// half a save record applied through loadData is worse than none, and the peer that still holds
// the whole prop can re-publish.
size_t MaxRecordBytes() { return coop::blob_chunks::MaxBlobBytes() - 64; }

// The park: records whose prop has not arrived. Keyed by Key and NEVER expired (the header says
// why). Capped by count so a peer streaming garbage cannot grow it without bound; an eviction
// drops the OLDEST and is loud.
constexpr size_t kMaxParked = 256;

struct Parked {
    SR::SaveRecord rec;
    uint8_t        senderSlot = 0;
    uint64_t       seq        = 0;  // arrival order, for the oldest-first eviction
};
std::map<std::wstring, Parked> g_parked;
uint64_t g_parkSeq = 1;

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
    return SW::DeSave(body, o, rec);
}

void Park(const std::wstring& key, SR::SaveRecord&& rec, uint8_t senderSlot) {
    if (g_parked.size() >= kMaxParked && !g_parked.count(key)) {
        auto oldest = g_parked.begin();
        for (auto it = g_parked.begin(); it != g_parked.end(); ++it)
            if (it->second.seq < oldest->second.seq) oldest = it;
        UE_LOGW("prop_save_data: park at the %zu cap -- evicting the oldest record (key '%ls'); "
                "that prop's save state is now whatever its peer holds",
                kMaxParked, oldest->first.c_str());
        g_parked.erase(oldest);
    }
    Parked p;
    p.rec = std::move(rec);
    p.senderSlot = senderSlot;
    p.seq = g_parkSeq++;
    g_parked[key] = std::move(p);
}

// Apply to the live actor if there is one, park by Key if there is not.
void LandRecord(const std::wstring& key, SR::SaveRecord&& rec, uint8_t senderSlot) {
    void* actor = PT::ResolveLiveActorByKey(key);
    if (actor && SR::ApplyRecord(actor, rec)) {
        UE_LOGI("prop_save_data: record applied to live prop (key '%ls', from slot %u)",
                key.c_str(), static_cast<unsigned>(senderSlot));
        return;
    }
    Park(key, std::move(rec), senderSlot);
    UE_LOGI("prop_save_data: record parked by key (key '%ls', from slot %u, %zu parked) -- "
            "applies when that prop appears", key.c_str(), static_cast<unsigned>(senderSlot),
            g_parked.size());
}

// Would a reliable of this kind reach anyone right now? Session::SendReliable skips a slot that is
// not world-ready for a kind that is not pre-world sendable, and this lane is not one, so during a
// joiner's load window every send is refused. That is not a divergence and must not be reported as
// one: the joiner takes each record behind its own spawn row when the prop snapshot drains.
bool AnyPeerWouldTake(const coop::net::Session* s) {
    for (int i = 0; i < static_cast<int>(coop::net::kMaxPeers); ++i)
        if (s->IsSlotWorldReady(i)) return true;
    return false;
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
        UE_LOGW("prop_save_data: record for key '%ls' (%zu B) was not fully sent -- that peer's "
                "copy of this prop is now divergent", key.c_str(), body.size());
    }
    return ok;
}

}  // namespace

bool Covers(void* actor) {
    return actor && SR::OverridesGetData(R::ClassOf(actor));
}

bool Publish(coop::net::Session* s, void* actor, const std::wstring& key) {
    if (!s || !actor || key.empty() || key.size() > kMaxKeyChars) return false;
    // Nobody to publish to: return before the capture, not at the send. A host loading its own
    // world births every keyed prop it owns, and paying getData plus a serialize per prop for a
    // send that cannot land is the whole cost of the lane spent on nothing.
    if (!s->connected() || !AnyPeerWouldTake(s)) return false;
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

    std::wstring key;
    SR::SaveRecord rec;
    if (!ParseBody(body, key, rec)) {
        UE_LOGW("prop_save_data: ill-formed %s body from slot %u (%zu B) -- dropped whole",
                intent ? "intent" : "record", static_cast<unsigned>(senderSlot), body.size());
        return;
    }
    if (!intent) {
        LandRecord(key, std::move(rec), senderSlot);
        return;
    }
    // The host takes a client's claim: apply or park it locally, then re-publish the result as the
    // canonical so every peer -- the author included, as the acknowledgement -- takes the same
    // bytes from the same authority.
    void* actor = PT::ResolveLiveActorByKey(key);
    if (actor && SR::ApplyRecord(actor, rec)) {
        UE_LOGI("prop_save_data: HOST took a client record (key '%ls', slot %u) -- republishing",
                key.c_str(), static_cast<unsigned>(senderSlot));
    } else {
        UE_LOGI("prop_save_data: HOST parked a client record (key '%ls', slot %u) -- "
                "republishing", key.c_str(), static_cast<unsigned>(senderSlot));
        Park(key, SR::SaveRecord(rec), senderSlot);
    }
    SendBody(&s, -1, key, rec);
}

bool ApplyParked(void* actor, const std::wstring& key) {
    if (!actor || key.empty()) return false;
    auto it = g_parked.find(key);
    if (it == g_parked.end()) return false;
    const bool ok = SR::ApplyRecord(actor, it->second.rec);
    if (ok) {
        UE_LOGI("prop_save_data: parked record applied at birth (key '%ls')", key.c_str());
        g_parked.erase(it);
    } else {
        UE_LOGW("prop_save_data: loadData refused the parked record for key '%ls' -- kept parked",
                key.c_str());
    }
    return ok;
}

void Drive() {
    const auto now = std::chrono::steady_clock::now();
    g_asmCanonical.Sweep(now, std::chrono::seconds(30));
    g_asmIntent.Sweep(now, std::chrono::seconds(30));
}

void OnDisconnect() {
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
