// coop/world/ground_hose_sync.cpp -- see coop/world/ground_hose_sync.h.

#include "coop/world/ground_hose_sync.h"

#include "coop/items/save_record_wire.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_key_index.h"
#include "coop/session/net_pump.h"
#include "coop/session/poll_timer.h"

#include "ue_wrap/actors/ground_hose.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::ground_hose_sync {
namespace {

namespace E = ue_wrap::engine;
namespace H = ue_wrap::ground_hose;
namespace R = ue_wrap::reflection;
namespace W = coop::save_record_wire;
using Clock = std::chrono::steady_clock;
using SaveRecord = ue_wrap::save_record::SaveRecord;

constexpr uint8_t kOpState = 1;   // GroundHoseState: the record. GroundHoseIntent: a route, the client's record
constexpr uint8_t kOpGone = 2;    // the hose went (state) or was taken up (intent)

constexpr size_t kMaxPoints = 512;
constexpr float kRouteReachCm = 1000.f;   // a route's sender from its last point: the coil finishes where it stands
constexpr float kTakeReachCm = 800.f;     // a removal's sender from the nearest point
constexpr size_t kMaxClientHoses = 64;    // hoses the host lays for clients, all told
constexpr auto kStateLife = std::chrono::seconds(10);   // a state whose faucet is not here yet waits this long
constexpr auto kEchoLife = std::chrono::seconds(5);     // a client's route the host has not answered by then was refused
constexpr auto kSweepEvery = std::chrono::seconds(1);
constexpr auto kAssemblyTtl = std::chrono::seconds(20);

std::atomic<coop::net::Session*> g_session{nullptr};
coop::session::PollTimer g_poll{std::chrono::milliseconds(250)};
coop::blob_chunks::Assembler g_asmState, g_asmIntent;
Clock::time_point g_lastSweep{};
uint32_t g_blobSeq = 0;
uint32_t g_world = 0;

// HOST: what each finished hose last told the clients.
struct Row {
    ue_wrap::CachedObjRef ref;
    uint64_t print = 0;
    bool pending = true;
};
std::unordered_map<std::wstring, Row> g_rows;
struct Intent {
    int slot = 0;
    uint8_t op = 0;
    std::wstring key;
    SaveRecord rec;
};
std::vector<Intent> g_intents;
size_t g_clientLaid = 0;

// CLIENT: the host's hoses as last laid here, the states waiting to be laid and the hoses this player is laying.
struct Known {
    ue_wrap::CachedObjRef ref;
    uint64_t print = 0;
    uint64_t awaiting = 0;   // the print of this player's last route, until the host's state answers it
    Clock::time_point sentAt{};
};
std::unordered_map<std::wstring, Known> g_known;
struct Pending {
    uint8_t op = 0;
    SaveRecord rec;
    Clock::time_point at{};
};
std::unordered_map<std::wstring, Pending> g_inbox;
std::unordered_set<void*> g_laying;

uint32_t g_sent = 0, g_applied = 0, g_routes = 0, g_refused = 0;

coop::net::Session* Live() {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    return s && s->connected() ? s : nullptr;
}

bool ValidKey(const std::wstring& k) {
    if (k.empty() || k == L"None" || k.size() > 128) return false;
    for (wchar_t c : k)
        if (c < 32 || c > 126) return false;
    return true;
}

bool InWorld(float v) { return std::isfinite(v) && std::fabs(v) <= coop::net::kMaxCoord; }

// A hose's record as the wire may carry it: the class is the hose's, the two keys are there, every point is in the
// world and there are not too many.
bool RecordSane(const SaveRecord& r) {
    if (r.className != L"groundHose_C" || r.vectors.empty() || r.vectors[0].size() < 2 || r.vectors[0].size() > kMaxPoints)
        return false;
    std::wstring f, s;
    if (!H::RecordKeys(r, f, s)) return false;
    for (size_t i = 0; i < r.xform.size(); ++i)
        if (!std::isfinite(r.xform[i]) || (i >= 4 && i <= 6 && !InWorld(r.xform[i]))) return false;
    for (const auto& grp : r.vectors)
        for (const auto& v : grp)
            if (!InWorld(v[0]) || !InWorld(v[1]) || !InWorld(v[2])) return false;
    return true;
}

// What a hose is, for "has it changed" and "is this the host's answer to my route": its key, its points and its two
// keys. Not its transform, which a load and a read round by a bit, nor the base record, which each peer fills its own.
uint64_t Print(const SaveRecord& r) {
    std::vector<uint8_t> b;
    W::AppWStr(b, r.key);
    for (const auto& p : r.vectors.empty() ? std::vector<std::array<float, 3>>{} : r.vectors[0])
        for (float v : p) W::AppF32(b, v);
    for (const auto& n : r.names.empty() ? std::vector<std::wstring>{} : r.names[0]) W::AppWStr(b, n);
    return coop::blob_chunks::Fnv64(b);
}

std::vector<uint8_t> Blob(uint8_t op, const std::wstring& key, const SaveRecord* rec) {
    std::vector<uint8_t> b;
    W::AppU8(b, op);
    W::AppWStr(b, key);
    if (rec) W::SerSave(b, *rec);
    return b;
}

bool Parse(const std::vector<uint8_t>& b, uint8_t& op, std::wstring& key, SaveRecord& rec) {
    size_t o = 0;
    if (!W::RdU8(b, o, op) || !W::RdWStr(b, o, key) || !ValidKey(key)) return false;
    if (op == kOpGone) return o == b.size();
    return op == kOpState && W::DeSave(b, o, rec) && o == b.size() && rec.key == key && RecordSane(rec);
}

void* HoseByKey(const std::wstring& key) {
    std::vector<void*> hoses;
    H::ListHoses(hoses);
    for (void* h : hoses)
        if (H::KeyOf(h) == key) return h;
    return nullptr;
}

void* Body(int slot) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
    void* body = rp && rp->valid() ? rp->GetActor() : nullptr;
    return body && R::IsLive(body) ? body : nullptr;
}

float DistSq(const ue_wrap::FVector& a, const std::array<float, 3>& b) {
    const float dx = a.X - b[0], dy = a.Y - b[1], dz = a.Z - b[2];
    return dx * dx + dy * dy + dz * dz;
}

// The faucet and the sprinkler a record names, in this world.
void Ends(const SaveRecord& r, void*& faucet, void*& sprinkler) {
    std::wstring f, s;
    H::RecordKeys(r, f, s);
    faucet = ValidKey(f) ? H::FindFaucet(f) : nullptr;
    sprinkler = ValidKey(s) ? coop::prop_key_index::Find(s) : nullptr;
}

// ---- HOST ----------------------------------------------------------------------------------------------------------

void HostRoute(coop::net::Session& s, const Intent& in) {
    void* body = Body(in.slot);
    ue_wrap::FVector by{};
    if (!s.IsSlotWorldReady(in.slot) || !body || !E::TryGetActorLocation(body, by)) return;
    const auto& pts = in.rec.vectors[0];
    if (DistSq(by, pts.back()) > kRouteReachCm * kRouteReachCm) {
        ++g_refused;
        UE_LOGW("ground_hose: slot %d's hose '%ls' ends out of its reach -- refused", in.slot, in.key.c_str());
        return;
    }
    void* faucet = nullptr;
    void* sprinkler = nullptr;
    Ends(in.rec, faucet, sprinkler);
    if (!faucet) {
        ++g_refused;
        UE_LOGW("ground_hose: slot %d's hose '%ls' names no faucet of this world -- refused", in.slot, in.key.c_str());
        return;
    }
    if (H::FindFaucet(in.key)) return;   // a key that is a faucet's is no hose's
    void* hose = HoseByKey(in.key);
    if (hose && H::IsLaying(hose)) return;   // the host's own player is laying it again: that edit stands
    const bool made = !hose;
    if (made) {
        if (g_clientLaid >= kMaxClientHoses) {
            ++g_refused;
            UE_LOGW("ground_hose: %zu hoses laid for clients -- slot %d's refused", g_clientLaid, in.slot);
            return;
        }
        hose = H::Spawn({pts.front()[0], pts.front()[1], pts.front()[2]});
        if (!hose) return;
    }
    if (!H::Apply(hose, in.rec, faucet, sprinkler)) {
        if (made) E::DestroyActor(hose);
        UE_LOGW("ground_hose: slot %d's hose '%ls' would not load", in.slot, in.key.c_str());
        return;
    }
    if (made) ++g_clientLaid;
    ++g_routes;
    g_rows[in.key].pending = true;
    UE_LOGI("ground_hose: slot %d %s '%ls' (%zu points, sprinkler %s) on the host", in.slot, made ? "laid" : "re-laid",
            in.key.c_str(), pts.size(), sprinkler ? "tied" : "none");
}

void HostTake(coop::net::Session& s, const Intent& in) {
    void* body = Body(in.slot);
    void* hose = HoseByKey(in.key);
    ue_wrap::FVector by{};
    SaveRecord rec;
    if (!s.IsSlotWorldReady(in.slot) || !body || !hose || H::IsLaying(hose) || !E::TryGetActorLocation(body, by) ||
        !H::Capture(hose, rec) || rec.vectors.empty())
        return;
    float best = -1.f;
    for (const auto& p : rec.vectors[0]) {
        const float d = DistSq(by, p);
        if (best < 0.f || d < best) best = d;
    }
    if (best < 0.f || best > kTakeReachCm * kTakeReachCm) {
        ++g_refused;
        UE_LOGW("ground_hose: slot %d took up '%ls' out of reach -- refused", in.slot, in.key.c_str());
        return;
    }
    E::DestroyActor(hose);
    UE_LOGI("ground_hose: slot %d took up '%ls'", in.slot, in.key.c_str());
}

void HostPoll(coop::net::Session& s) {
    std::vector<Intent> intents;
    intents.swap(g_intents);
    for (const Intent& in : intents) {
        if (in.op == kOpState) HostRoute(s, in);
        else HostTake(s, in);
    }
    std::vector<void*> hoses;
    H::ListHoses(hoses);
    std::unordered_set<std::wstring> seen;
    for (void* h : hoses) {
        std::wstring key = H::KeyOf(h);
        if (ValidKey(key)) seen.insert(key);
        if (H::IsLaying(h)) continue;
        SaveRecord rec;
        if (!H::Capture(h, rec)) continue;   // getData keys a hose that had no key yet
        key = H::KeyOf(h);
        if (!ValidKey(key) || rec.key != key || !RecordSane(rec)) continue;
        seen.insert(key);
        Row& row = g_rows[key];
        const uint64_t print = Print(rec);
        if (!row.pending && row.print == print && row.ref.Is(h)) continue;
        const std::vector<uint8_t> blob = Blob(kOpState, key, &rec);
        if (!coop::blob_chunks::SendBlob(&s, coop::net::ReliableKind::GroundHoseState, ++g_blobSeq, blob)) continue;
        if (!row.print) UE_LOGI("ground_hose: '%ls' laid (%zu points)", key.c_str(), rec.vectors[0].size());
        row.ref.Set(h);
        row.print = print;
        row.pending = false;
        ++g_sent;
    }
    for (auto it = g_rows.begin(); it != g_rows.end();) {
        if (seen.count(it->first)) {
            ++it;
            continue;
        }
        if (it->second.print) {
            const std::vector<uint8_t> blob = Blob(kOpGone, it->first, nullptr);
            if (!coop::blob_chunks::SendBlob(&s, coop::net::ReliableKind::GroundHoseState, ++g_blobSeq, blob)) {
                ++it;
                continue;
            }
            UE_LOGI("ground_hose: '%ls' went", it->first.c_str());
        }
        it = g_rows.erase(it);
    }
}

// ---- CLIENT --------------------------------------------------------------------------------------------------------

bool SendIntent(coop::net::Session& s, uint8_t op, const std::wstring& key, const SaveRecord* rec) {
    const std::vector<uint8_t> blob = Blob(op, key, rec);
    return coop::blob_chunks::SendBlob(&s, coop::net::ReliableKind::GroundHoseIntent, ++g_blobSeq, blob);
}

// The host's hose on this copy. False to try again on a later poll.
bool Lay(const std::wstring& key, const SaveRecord& rec, bool lastTry, Clock::time_point now) {
    void* hose = HoseByKey(key);
    if (hose && H::IsLaying(hose)) return true;   // this player is laying it: its route answers
    auto known = g_known.find(key);
    if (known != g_known.end() && known->second.awaiting) {
        // A state sent before the host had this player's last route would undo it here: the answer is still to come.
        if (Print(rec) != known->second.awaiting && now - known->second.sentAt < kEchoLife) return true;
        known->second.awaiting = 0;
    }
    void* faucet = nullptr;
    void* sprinkler = nullptr;
    Ends(rec, faucet, sprinkler);
    std::wstring f, sp;
    H::RecordKeys(rec, f, sp);
    if (!lastTry && ((ValidKey(f) && !faucet) || (ValidKey(sp) && !sprinkler))) return false;
    const bool made = !hose;
    if (made) {
        const auto& p = rec.vectors[0].front();
        hose = H::Spawn({p[0], p[1], p[2]});
        if (!hose) return false;
    }
    if (!H::Apply(hose, rec, faucet, sprinkler)) {
        if (made) E::DestroyActor(hose);
        return false;
    }
    SaveRecord here;
    Known& k = g_known[key];
    k.ref.Set(hose);
    k.print = H::Capture(hose, here) ? Print(here) : Print(rec);
    ++g_applied;
    UE_LOGI("ground_hose: the host's '%ls' laid here (%zu points, faucet %s, sprinkler %s)", key.c_str(),
            rec.vectors[0].size(), faucet ? "tied" : "none", sprinkler ? "tied" : "none");
    return true;
}

void ClientPoll(coop::net::Session& s, Clock::time_point now) {
    if (!coop::net_pump::HasAnnouncedWorldReady()) return;
    std::vector<void*> hoses;
    H::ListHoses(hoses);
    std::unordered_set<void*> live(hoses.begin(), hoses.end());
    for (auto it = g_laying.begin(); it != g_laying.end();) it = live.count(*it) ? std::next(it) : g_laying.erase(it);
    for (void* h : hoses) {
        if (H::IsLaying(h)) {
            g_laying.insert(h);
            continue;
        }
        std::wstring key = H::KeyOf(h);
        const bool laid = g_laying.erase(h) > 0;
        auto known = ValidKey(key) ? g_known.find(key) : g_known.end();
        if (!laid && known == g_known.end()) continue;   // a hose of the save, which the host's state will name
        SaveRecord rec;
        if (!H::Capture(h, rec)) continue;
        key = H::KeyOf(h);
        if (!ValidKey(key) || rec.key != key) continue;
        const uint64_t print = Print(rec);
        Known& k = g_known[key];
        if (!laid && k.print == print) continue;
        if (!RecordSane(rec) || !SendIntent(s, kOpState, key, &rec)) continue;
        k.ref.Set(h);
        k.print = print;
        k.awaiting = print;
        k.sentAt = now;
        UE_LOGI("ground_hose: this player %s '%ls' (%zu points) -- the host lays it", laid ? "laid" : "changed", key.c_str(),
                rec.vectors[0].size());
    }
    for (auto it = g_known.begin(); it != g_known.end();) {
        if (it->second.ref.Get() || g_inbox.count(it->first)) {
            ++it;
            continue;
        }
        if (!SendIntent(s, kOpGone, it->first, nullptr)) break;
        UE_LOGI("ground_hose: this player took up '%ls' -- the host takes it up", it->first.c_str());
        it = g_known.erase(it);
    }
    // The host's states last: a local edit since the last poll is sent first, so an older state cannot undo it.
    for (auto it = g_inbox.begin(); it != g_inbox.end();) {
        if (it->second.op == kOpGone) {
            if (void* hose = HoseByKey(it->first)) {
                if (!H::IsLaying(hose)) E::DestroyActor(hose);
            }
            g_known.erase(it->first);
            UE_LOGI("ground_hose: the host's '%ls' went here", it->first.c_str());
            it = g_inbox.erase(it);
            continue;
        }
        const bool last = now - it->second.at > kStateLife;
        it = Lay(it->first, it->second.rec, last, now) || last ? g_inbox.erase(it) : std::next(it);
    }
}

void Reset() {
    g_rows.clear();
    g_intents.clear();
    g_known.clear();
    g_inbox.clear();
    g_laying.clear();
    g_clientLaid = 0;
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    coop::net::Session* s = Live();
    if (!s) return;
    const Clock::time_point now = Clock::now();
    if (now - g_lastSweep >= kSweepEvery) {
        g_lastSweep = now;
        g_asmState.Sweep(now, kAssemblyTtl);
        g_asmIntent.Sweep(now, kAssemblyTtl);
    }
    if (!g_poll.Due(now)) return;
    const uint32_t world = ue_wrap::world_identity::Generation();
    if (world != g_world) {
        g_world = world;
        Reset();
    }
    if (!H::HoseClass()) return;   // nothing has loaded a hose yet
    if (s->role() == coop::net::Role::Host) HostPoll(*s);
    else if (s->role() == coop::net::Role::Client) ClientPoll(*s, now);
}

void OnStateChunk(const coop::net::BlobChunkPayload& p, int senderSlot) {
    coop::net::Session* s = Live();
    if (!s || s->role() != coop::net::Role::Client || senderSlot != 0) return;
    std::vector<uint8_t> blob;
    if (!g_asmState.OnChunk(p, 0, blob)) return;
    uint8_t op = 0;
    std::wstring key;
    SaveRecord rec;
    if (!Parse(blob, op, key, rec)) {
        UE_LOGW("ground_hose: a malformed hose state from the host -- dropped");
        return;
    }
    Pending& pd = g_inbox[key];
    pd.op = op;
    pd.rec = std::move(rec);
    pd.at = Clock::now();
}

void OnIntentChunk(const coop::net::BlobChunkPayload& p, int senderSlot) {
    coop::net::Session* s = Live();
    if (!s || s->role() != coop::net::Role::Host || senderSlot <= 0 || senderSlot >= static_cast<int>(coop::net::kMaxPeers))
        return;
    std::vector<uint8_t> blob;
    if (!g_asmIntent.OnChunk(p, static_cast<uint8_t>(senderSlot), blob)) return;
    Intent in;
    if (!Parse(blob, in.op, in.key, in.rec)) {
        UE_LOGW("ground_hose: a malformed hose from slot %d -- dropped", senderSlot);
        return;
    }
    if (g_intents.size() >= 32) return;
    in.slot = senderSlot;
    g_intents.push_back(std::move(in));
}

void QueueConnectBroadcastForSlot(int /*slot*/) {
    for (auto& entry : g_rows) entry.second.pending = true;
}

void OnPeerLeftSlot(int slot) {
    if (slot <= 0 || slot >= static_cast<int>(coop::net::kMaxPeers)) return;
    g_asmIntent.ClearSlot(static_cast<uint8_t>(slot));
}

void OnDisconnect() {
    if (g_sent || g_applied || g_routes || g_refused)
        UE_LOGI("ground_hose: session end -- states sent=%u laid here=%u client hoses=%u refused=%u", g_sent, g_applied,
                g_routes, g_refused);
    g_sent = g_applied = g_routes = g_refused = 0;
    Reset();
    g_asmState.Clear();
    g_asmIntent.Clear();
    g_session.store(nullptr);
}

}  // namespace coop::ground_hose_sync
