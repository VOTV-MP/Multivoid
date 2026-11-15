// coop/interactables/atv_trunk_sync.cpp -- see coop/interactables/atv_trunk_sync.h.

#include "coop/interactables/atv_trunk_sync.h"

#include "coop/interactables/atv_sync.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/props/container_slice_wire.h"
#include "coop/props/container_target.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/facts/function_row.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace coop::atv_trunk_sync {
namespace {

namespace A  = ue_wrap::atv;
namespace sg = ue_wrap::script_gate;
namespace cw = coop::props::container_slice_wire;
namespace ct = coop::props::container_target;
using Items = std::vector<ue_wrap::inventory_item::Item>;

constexpr uint64_t kPollMs  = 500;    // the host reads its trunks this often; a change goes at the next read
constexpr uint64_t kSweepMs = 1000;   // a client drops a half-assembled blob this long after its last chunk
constexpr size_t   kMaxHeld = 32;     // blobs a client holds for ATVs it has not got (a host's word is bounded by its ATVs)
constexpr int      kTagAddAsObject = 0x41545431;   // "ATT1"

// The verb that files a held item in the trunk (the ATV's playerUsedOn drops the main hand, then adds the dropped actor).
UE_FACT_FN(kRowAddAsObject, "atv_trunk_sync", L"inventoryComponent_C", L"addItemAsObject", Declared);

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watchInstalled = false;
bool g_refusalSaid = false;

// ---- host ------------------------------------------------------------------------------------
uint64_t g_nextPollMs = 0;
std::map<uint32_t, uint64_t> g_sentHash;      // the hash last sent for each trunk
uint32_t g_seq = 1;
bool     g_unreadSaid = false;
bool     g_tooBigSaid = false;
std::set<uint32_t> g_clashSaid;
std::map<std::pair<uint32_t, int>, uint64_t> g_truthMs;   // when the truth last went to each (trunk, slot)

// ---- client ----------------------------------------------------------------------------------
coop::blob_chunks::Assembler g_asm;
uint64_t g_nextSweepMs = 0;
struct Held {
    Items items;
    uint64_t hash = 0;
};
std::map<uint32_t, Held> g_pending;           // the newest blob per trunk not applied yet
std::map<uint32_t, uint64_t> g_appliedHash;   // the hash of what was last applied per trunk

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool IsHost(const coop::net::Session* s) { return s && s->role() == coop::net::Role::Host; }

uint32_t Fnv32(const std::wstring& s) {
    uint32_t h = 2166136261u;
    for (wchar_t c : s) {
        h ^= static_cast<uint32_t>(c) & 0xFFu;
        h *= 16777619u;
        h ^= (static_cast<uint32_t>(c) >> 8) & 0xFFu;
        h *= 16777619u;
    }
    return h;
}

struct Trunk {
    uint32_t eid;
    void* atv;
};

// Every live ATV with a trunk id of its own. Two ATVs that map to one id are neither (said once).
std::vector<Trunk> Trunks() {
    std::vector<Trunk> all;
    coop::atv_sync::ForEachAnnounceableAtv([](void* ctx, void* actor, const std::wstring& key) {
        static_cast<std::vector<Trunk>*>(ctx)->push_back({EidOfKey(key), actor});
    }, &all);
    std::vector<Trunk> out;
    for (const Trunk& t : all) {
        const auto clashes = std::count_if(all.begin(), all.end(), [&](const Trunk& o) { return o.eid == t.eid; });
        if (clashes > 1) {
            if (g_clashSaid.insert(t.eid).second)
                UE_LOGW("atv_trunk: %lld ATVs map to the trunk id %08x -- none of them is shared",
                        static_cast<long long>(clashes), t.eid);
            continue;
        }
        out.push_back(t);
    }
    return out;
}

bool ReadTrunk(void* atv, Items& out) {
    if (A::ReadTrunk(atv, out, cw::kMaxRecords)) return true;
    if (!g_unreadSaid) {
        g_unreadSaid = true;
        UE_LOGW("atv_trunk: an ATV's trunk does not read (no inventory component, an unresolved entry layout or more "
                "than %zu entries) -- it is not sent", cw::kMaxRecords);
    }
    return false;
}

// Ship the entries whole to every peer, or to one slot (slot >= 0). True when every chunk was accepted.
bool Send(coop::net::Session* s, uint32_t eid, const Items& items, int slot) {
    if (cw::SliceBytes(items) > coop::blob_chunks::MaxBlobBytes()) {
        if (!g_tooBigSaid) {
            g_tooBigSaid = true;
            UE_LOGW("atv_trunk: %zu entries are past the blob ceiling -- not sent", items.size());
        }
        return false;
    }
    const std::vector<uint8_t> blob = cw::Pack(eid, items);
    const uint32_t seq = g_seq++;
    return slot < 0 ? coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::AtvTrunk, seq, blob)
                    : coop::blob_chunks::SendBlobToSlot(s, slot, coop::net::ReliableKind::AtvTrunk, seq, blob);
}

void HostTick(coop::net::Session* s) {
    const uint64_t now = NowMs();
    if (now < g_nextPollMs) return;
    g_nextPollMs = now + kPollMs;
    // A broadcast reaches world-ready peers only; a joiner takes the trunks at its own connect replay.
    if (!s->AnyWorldReadyPeer()) return;
    for (const Trunk& t : Trunks()) {
        Items items;
        if (!ReadTrunk(t.atv, items)) continue;
        const uint64_t hash = cw::RecordsHash(t.eid, items);
        const auto sent = g_sentHash.find(t.eid);
        if (sent != g_sentHash.end() && sent->second == hash) continue;
        if (!Send(s, t.eid, items, -1)) continue;   // not accepted: the next read sends it again
        g_sentHash[t.eid] = hash;
        UE_LOGI("atv_trunk: sent trunk %08x, %zu entries (hash %016llx)", t.eid, items.size(),
                static_cast<unsigned long long>(hash));
    }
}

void ClientTick() {
    const uint64_t now = NowMs();
    if (now >= g_nextSweepMs) {
        g_nextSweepMs = now + kSweepMs;
        if (!g_asm.Idle()) g_asm.Sweep(std::chrono::steady_clock::now(), std::chrono::seconds(10));
    }
    if (g_pending.empty()) return;
    const std::vector<Trunk> trunks = Trunks();
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        const uint32_t eid = it->first;
        Held& held = it->second;
        const auto applied = g_appliedHash.find(eid);
        if (applied != g_appliedHash.end() && applied->second == held.hash) {   // the copy already holds this
            it = g_pending.erase(it);
            continue;
        }
        const auto t = std::find_if(trunks.begin(), trunks.end(), [&](const Trunk& x) { return x.eid == eid; });
        if (t == trunks.end()) { ++it; continue; }   // the ATV resolves later; the blob waits
        switch (A::WriteTrunk(t->atv, held.items)) {
            case A::TrunkWrite::Written:
                g_appliedHash[eid] = held.hash;
                UE_LOGI("atv_trunk: applied trunk %08x, %zu entries (hash %016llx)", eid, held.items.size(),
                        static_cast<unsigned long long>(held.hash));
                break;
            case A::TrunkWrite::NotReady:
                ++it;
                continue;   // the entry layout or the component is not up yet; try again next tick
            case A::TrunkWrite::Refused:
                UE_LOGW("atv_trunk: the host's %zu entries for trunk %08x were refused by the ATV's inventory",
                        held.items.size(), eid);
                break;
        }
        it = g_pending.erase(it);
    }
}

// ---- the client's refusal --------------------------------------------------------------------
// A hand item put into a trunk files the dropped actor with addItemAsObject on the ATV's component. It writes a copy the
// host never sees, so a client's call is cancelled (the dropped item stays in the world); the host's own put runs.
sg::Verdict OnAddAsObjectPre(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Client || !call.object) return sg::Verdict::Run;
    if (EidOfInventory(call.object) == 0) return sg::Verdict::Run;
    if (!g_refusalSaid) {
        g_refusalSaid = true;
        UE_LOGI("atv_trunk: this client's hand put into an ATV trunk is refused -- the trunk is the host's; drag the item "
                "onto the open trunk to ask for it");
    }
    return sg::Verdict::Cancel;
}

void EnsureWatch() {
    sg::ResolvePendingNames();
    if (g_watchInstalled) return;
    g_watchInstalled = sg::WatchName(kRowAddAsObject, kTagAddAsObject, &OnAddAsObjectPre, nullptr);
}

}  // namespace

uint32_t EidOfKey(const std::wstring& key) { return ct::kAtvTrunkBase | (Fnv32(key) & ~ct::kAtvTrunkMask); }

void* AtvOfEid(uint32_t eid) {
    if (!ct::IsAtvTrunk(eid)) return nullptr;
    for (const Trunk& t : Trunks())
        if (t.eid == eid) return t.atv;
    return nullptr;
}

uint32_t EidOfInventory(void* inventory) {
    if (!inventory) return 0;
    for (const Trunk& t : Trunks())
        if (A::InventoryOf(t.atv) == inventory) return t.eid;
    return 0;
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    EnsureWatch();
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    EnsureWatch();
    if (!s || !s->connected()) return;
    if (IsHost(s)) HostTick(s); else ClientTick();
}

void OnChunk(const coop::net::BlobChunkPayload& chunk, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || IsHost(s)) return;   // a client's copy is the host's to refuse at its receive edge
    std::vector<uint8_t> blob;
    if (!g_asm.OnChunk(chunk, senderSlot, blob)) return;
    size_t o = 0;
    uint32_t eid = 0;
    uint16_t count = 0;
    Items items;
    const char* why = nullptr;
    if (!cw::ParseHeader(blob, o, eid, count) || !ct::IsAtvTrunk(eid) ||
        !cw::ParseRecords(blob, o, count, items, &why)) {
        UE_LOGW("atv_trunk: a malformed trunk blob was dropped (%s)", why ? why : "header");
        return;
    }
    if (g_pending.size() >= kMaxHeld && g_pending.find(eid) == g_pending.end()) {
        UE_LOGW("atv_trunk: %zu blobs are held for ATVs this world lacks -- the blob for %08x was dropped",
                g_pending.size(), eid);
        return;
    }
    Held& held = g_pending[eid];   // the newest word replaces an older one still waiting
    held.hash = cw::RecordsHash(eid, items);
    held.items = std::move(items);
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!IsHost(s) || peerSlot < 1 || peerSlot >= static_cast<int>(coop::net::kMaxPeers)) return;
    for (const Trunk& t : Trunks()) {
        Items items;
        if (!ReadTrunk(t.atv, items)) continue;
        if (!Send(s, t.eid, items, peerSlot))
            UE_LOGW("atv_trunk: trunk %08x was not accepted for slot %d at its connect", t.eid, peerSlot);
    }
}

void MarkHostWrite(uint32_t) { g_nextPollMs = 0; }

void ForgetApplied(uint32_t eid) { g_appliedHash.erase(eid); }

bool SendTruthToSlot(uint32_t eid, int peerSlot, bool bounded) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!IsHost(s) || peerSlot < 1 || peerSlot >= static_cast<int>(coop::net::kMaxPeers)) return false;
    const uint64_t now = NowMs();
    uint64_t& last = g_truthMs[{eid, peerSlot}];
    if (bounded && last != 0 && now < last + kTruthEveryMs) return false;
    void* const atv = AtvOfEid(eid);
    Items items;
    if (!atv || !ReadTrunk(atv, items) || !Send(s, eid, items, peerSlot)) return false;
    last = now;
    return true;
}

void OnDisconnect() {
    g_asm.Clear();
    g_pending.clear();
    g_appliedHash.clear();
    g_sentHash.clear();
    g_truthMs.clear();
    g_clashSaid.clear();
    g_nextPollMs = 0;
    g_nextSweepMs = 0;
    g_refusalSaid = false;
}

int TrunkCount(uint32_t eid) {
    Items items;
    void* const atv = AtvOfEid(eid);
    if (!atv || !A::ReadTrunk(atv, items, cw::kMaxRecords)) return -1;
    int n = 0;
    for (const auto& it : items)
        if (!ue_wrap::inventory_item::IsEmpty(it)) ++n;
    return n;
}

void ForEachTrunk(void (*fn)(void* ctx, uint32_t eid, void* atv), void* ctx) {
    if (!fn) return;
    for (const Trunk& t : Trunks())
        if (A::InventoryOf(t.atv)) fn(ctx, t.eid, t.atv);
}

}  // namespace coop::atv_trunk_sync
