// coop/world/build_sync.cpp -- see coop/world/build_sync.h.

#include "coop/world/build_sync.h"

#include "coop/items/save_record_wire.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/local_streams.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/remote_prop.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/session/poll_timer.h"

#include "ue_wrap/actors/build_piece.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/world/keyed_objects.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::build_sync {
namespace {

namespace B = ue_wrap::build_piece;
namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;
namespace W = coop::save_record_wire;
using Clock = std::chrono::steady_clock;
using SaveRecord = ue_wrap::save_record::SaveRecord;

constexpr uint8_t kOpState = 1;   // BuildState: the record. BuildIntent: a piece this client made, its record
constexpr uint8_t kOpGone = 2;    // the piece went (state) or this client took it away (intent)
constexpr uint8_t kOpChange = 3;  // intent only: this client's player hit (below zero) or repaired a wall by this much
constexpr uint8_t kOpSnap = 4;    // intent only: this client's player's pull broke the nail's tie on its copy

constexpr size_t kMaxElements = 4096;      // every value of a record, all told
constexpr float kMakeReachCm = 3000.f;     // a client's new piece from its sender: a nail flies
constexpr float kTakeReachCm = 800.f;      // a removal's sender from the piece
constexpr float kChangeReachCm = 3000.f;   // a hit's or repair's sender from the wall
constexpr float kMaxChange = 100000.f;     // one hit or repair, at most
constexpr size_t kMaxClientPieces = 512;   // pieces the host makes for clients, all told
constexpr float kWallHealthScale = 10000.f;
constexpr auto kStateLife = std::chrono::seconds(10);   // a nail state whose ends are not here yet waits this long
constexpr auto kEchoLife = std::chrono::seconds(10);    // a piece of this player's the host has not answered was refused
constexpr auto kSaveLife = std::chrono::seconds(20);    // a piece of the save the host never names is not the host's
constexpr auto kSnapWait = std::chrono::milliseconds(1500);   // a snap the host's state has not named by then is tied again
constexpr auto kSweepEvery = std::chrono::seconds(1);
constexpr auto kAssemblyTtl = std::chrono::seconds(20);

std::atomic<coop::net::Session*> g_session{nullptr};
coop::session::PollTimer g_poll{std::chrono::milliseconds(500)};
coop::blob_chunks::Assembler g_asmState, g_asmIntent;
Clock::time_point g_lastSweep{};
uint32_t g_blobSeq = 0;
uint32_t g_world = 0;

// HOST: what each piece last told the clients.
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
    float change = 0.f;
};
std::vector<Intent> g_intents;
size_t g_clientMade = 0;

// CLIENT: the pieces this copy holds for the host, by key, with the record each was last made from.
struct Known {
    ue_wrap::CachedObjRef ref;
    SaveRecord rec;
    uint64_t print = 0;        // the print of the record last applied here, or of this player's own piece
    bool confirmed = false;    // the host's state has named it
    Clock::time_point since{};
    float health = 0.f;        // a wall's health as last applied here, scaled
    bool hasHealth = false;
    Clock::time_point snapped{};   // when this copy's nail was first seen broken, the host's not
    bool snapSent = false;         // this player's pull broke it here, and the host was told
};
std::unordered_map<std::wstring, Known> g_known;
struct Pending {
    uint8_t op = 0;
    SaveRecord rec;
    Clock::time_point at{};
};
std::unordered_map<std::wstring, Pending> g_inbox;
std::unordered_set<std::wstring> g_freed;   // host nails whose ties this copy let go: an end is in this player's hands
std::unordered_map<std::wstring, Clock::time_point> g_saveKeys;   // pieces the save made here, till the host names them
bool g_seeded = false;

uint32_t g_sent = 0, g_applied = 0, g_made = 0, g_refused = 0, g_taken = 0, g_changes = 0, g_reties = 0, g_snaps = 0;
std::atomic<unsigned> g_changesSent{0};

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

template <class G>
size_t Count(const G& groups) {
    size_t n = 0;
    for (const auto& g : groups) n += g.size();
    return n;
}

// A piece's record as the wire may carry it: a class this lane carries, its transform in the world, its values few and
// finite, and a nail's two keys.
bool RecordSane(const SaveRecord& r) {
    const B::Kind kind = B::KindOfClass(r.className);
    if (kind == B::Kind::None) return false;
    for (size_t i = 0; i < r.xform.size(); ++i)
        if (!std::isfinite(r.xform[i]) || (i >= 4 && i <= 6 && !InWorld(r.xform[i]))) return false;
    const size_t n = Count(r.bools) + Count(r.floats) + Count(r.ints) + Count(r.strings) + r.signals.size() +
                     Count(r.classes) + Count(r.vectors) + Count(r.rotators) + Count(r.transforms) + Count(r.bytes) +
                     Count(r.names);
    if (n > kMaxElements) return false;
    for (const auto& g : r.floats)
        for (float v : g)
            if (!std::isfinite(v)) return false;
    for (const auto& g : r.vectors)
        for (const auto& v : g)
            if (!InWorld(v[0]) || !InWorld(v[1]) || !InWorld(v[2])) return false;
    std::wstring a, b;
    return kind != B::Kind::Nail || B::NailKeys(r, a, b);
}

// What a piece is, for "has it changed": its whole record but its transform, which a nail on a moving prop changes
// every frame and the prop's own lane carries.
uint64_t Print(const SaveRecord& r) {
    SaveRecord c = r;
    c.xform = {};
    std::vector<uint8_t> b;
    W::SerSave(b, c);
    return coop::blob_chunks::Fnv64(b);
}

std::vector<uint8_t> Blob(uint8_t op, const std::wstring& key, const SaveRecord* rec, float change = 0.f) {
    std::vector<uint8_t> b;
    W::AppU8(b, op);
    W::AppWStr(b, key);
    if (rec) W::SerSave(b, *rec);
    if (op == kOpChange) W::AppF32(b, change);
    return b;
}

bool Parse(const std::vector<uint8_t>& b, uint8_t& op, std::wstring& key, SaveRecord& rec, float& change) {
    size_t o = 0;
    if (!W::RdU8(b, o, op) || !W::RdWStr(b, o, key) || !ValidKey(key)) return false;
    if (op == kOpGone || op == kOpSnap) return o == b.size();
    if (op == kOpChange)
        return W::RdF32(b, o, change) && o == b.size() && std::isfinite(change) && change != 0.f &&
               std::fabs(change) <= kMaxChange;
    return op == kOpState && W::DeSave(b, o, rec) && o == b.size() && rec.key == key && RecordSane(rec);
}

// Whether a nail's record has it broken (bools[0] = broken, static, nailed).
bool RecordBroken(const SaveRecord& r) { return !r.bools.empty() && !r.bools[0].empty() && r.bools[0][0]; }

std::unordered_map<std::wstring, void*> PiecesByKey() {
    std::vector<void*> pieces;
    B::List(pieces);
    std::unordered_map<std::wstring, void*> out;
    for (void* p : pieces) {
        const std::wstring key = B::KeyOf(p);
        if (ValidKey(key)) out.emplace(key, p);
    }
    return out;
}

void* PieceByKey(const std::wstring& key) {
    const auto all = PiecesByKey();
    const auto it = all.find(key);
    return it == all.end() ? nullptr : it->second;
}

// A key's actor in this world: a keyed prop, then whatever the game's own map holds.
void* ResolveKey(const std::wstring& key) {
    if (!ValidKey(key)) return nullptr;
    if (void* p = coop::prop_key_index::Find(key)) return p;
    void* a = ue_wrap::keyed_objects::Available() ? ue_wrap::keyed_objects::Resolve(key.c_str()) : nullptr;
    return a && R::IsLive(a) ? a : nullptr;
}

// A nail's two ends in this world. False when an end its record names is not here (yet).
bool NailEnds(const SaveRecord& r, void*& a, void*& b) {
    std::wstring ka, kb;
    a = b = nullptr;
    if (!B::NailKeys(r, ka, kb)) return true;
    a = ResolveKey(ka);
    b = ResolveKey(kb);
    return (!ValidKey(ka) || a) && (!ValidKey(kb) || b);
}

bool ApplyHere(void* piece, const SaveRecord& r, ue_wrap::save_record::ClassResolver resolve) {
    void* a = nullptr;
    void* b = nullptr;
    NailEnds(r, a, b);
    return B::Apply(piece, r, a, b, resolve);
}

void* Body(int slot) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
    void* body = rp && rp->valid() ? rp->GetActor() : nullptr;
    return body && R::IsLive(body) ? body : nullptr;
}

bool Within(void* body, float x, float y, float z, float reach) {
    ue_wrap::FVector at{};
    if (!body || !E::TryGetActorLocation(body, at)) return false;
    const float dx = at.X - x, dy = at.Y - y, dz = at.Z - z;
    return dx * dx + dy * dy + dz * dz <= reach * reach;
}

const char* KindName(B::Kind k) {
    switch (k) {
    case B::Kind::Nail: return "nail";
    case B::Kind::Unfinished: return "unfinished wall";
    case B::Kind::Wall: return "wall";
    case B::Kind::WetWall: return "wet wall";
    default: return "piece";
    }
}

// ---- HOST ----------------------------------------------------------------------------------------------------------

void HostMake(coop::net::Session& s, const Intent& in) {
    if (!s.IsSlotWorldReady(in.slot)) return;
    const B::Kind kind = B::KindOfClass(in.rec.className);
    if (PieceByKey(in.key)) return;   // the host has it: a repeat, or a key that is not this client's to make
    if (!Within(Body(in.slot), in.rec.xform[4], in.rec.xform[5], in.rec.xform[6], kMakeReachCm)) {
        ++g_refused;
        UE_LOGW("build_sync: slot %d's %s '%ls' lies out of its reach -- refused", in.slot, KindName(kind), in.key.c_str());
        return;
    }
    if (g_clientMade >= kMaxClientPieces) {
        ++g_refused;
        UE_LOGW("build_sync: %zu pieces made for clients -- slot %d's refused", g_clientMade, in.slot);
        return;
    }
    void* piece = B::Spawn(in.rec);
    if (!piece) return;
    if (!ApplyHere(piece, in.rec, &ue_wrap::object_index::ClassByName)) {
        E::DestroyActor(piece);
        UE_LOGW("build_sync: slot %d's %s '%ls' would not load", in.slot, KindName(kind), in.key.c_str());
        return;
    }
    ++g_clientMade;
    ++g_made;
    g_rows[in.key].pending = true;
    UE_LOGI("build_sync: slot %d made %s '%ls' on the host", in.slot, KindName(kind), in.key.c_str());
}

void HostTake(coop::net::Session& s, const Intent& in) {
    void* piece = PieceByKey(in.key);
    ue_wrap::FVector at{};
    if (!s.IsSlotWorldReady(in.slot) || !piece || !E::TryGetActorLocation(piece, at)) return;
    if (!Within(Body(in.slot), at.X, at.Y, at.Z, kTakeReachCm)) {
        ++g_refused;
        UE_LOGW("build_sync: slot %d took '%ls' out of reach -- refused", in.slot, in.key.c_str());
        return;
    }
    ++g_taken;
    UE_LOGI("build_sync: slot %d took %s '%ls' away", in.slot, KindName(B::KindOf(piece)), in.key.c_str());
    E::DestroyActor(piece);   // the game's own drop, which the prop lanes carry
}

// The piece's record to every client when it is new or changed since it was last sent; `key` is set to its key. True
// when the piece is keyed and its record is sane, sent or not.
bool SendPiece(coop::net::Session& s, void* p, std::wstring& key) {
    SaveRecord rec;
    if (!B::Capture(p, rec)) return false;   // getData keys a piece that had no key yet
    key = B::KeyOf(p);
    if (!ValidKey(key) || rec.key != key || !RecordSane(rec)) return false;
    Row& row = g_rows[key];
    const uint64_t print = Print(rec);
    if (!row.pending && row.print == print && row.ref.Is(p)) return true;
    const std::vector<uint8_t> blob = Blob(kOpState, key, &rec);
    if (!coop::blob_chunks::SendBlob(&s, coop::net::ReliableKind::BuildState, ++g_blobSeq, blob)) return true;
    if (!row.print) UE_LOGI("build_sync: %s '%ls' is the host's", KindName(B::KindOf(p)), key.c_str());
    row.ref.Set(p);
    row.print = print;
    row.pending = false;
    ++g_sent;
    return true;
}

// A client's hit or repair on its copy of a wall, as it comes: the host's wall takes it, a hit through its own Damage,
// and its state goes out at once. A refused one sends the state anyway, which takes back what the client showed.
void HostChange(coop::net::Session& s, const Intent& in) {
    void* wall = PieceByKey(in.key);
    float h = 0.f;
    ue_wrap::FVector at{};
    if (!s.IsSlotWorldReady(in.slot) || !B::WallHealth(wall, h) || !E::TryGetActorLocation(wall, at)) return;
    if (!Within(Body(in.slot), at.X, at.Y, at.Z, kChangeReachCm)) {
        ++g_refused;
        UE_LOGW("build_sync: slot %d's change to '%ls' out of reach -- refused", in.slot, in.key.c_str());
    } else {
        ++g_changes;
        UE_LOGI("build_sync: slot %d %s wall '%ls' by %.2f (health %.2f)", in.slot, in.change < 0.f ? "hit" : "repaired",
                in.key.c_str(), std::fabs(in.change), h);
        if (in.change < 0.f) B::HitWall(wall, -in.change);
        else B::RepairWall(wall, in.change);
    }
    if (!R::IsLive(wall)) return;   // broken: the poll sends its going
    g_rows[in.key].pending = true;
    std::wstring key;
    SendPiece(s, wall, key);
}

// A client's pull broke its copy of a nail in the board its player holds. The host judges it: its nail snaps, as force
// snaps one, when that player's hold drives one of the nail's ends here and the nail is in its reach; the nail's state
// goes out at once either way, so a refused snap is tied again there.
void HostSnap(coop::net::Session& s, const Intent& in) {
    void* nail = PieceByKey(in.key);
    ue_wrap::FVector at{};
    if (!s.IsSlotWorldReady(in.slot) || B::KindOf(nail) != B::Kind::Nail || !E::TryGetActorLocation(nail, at)) return;
    void* a = B::NailEndA(nail);
    void* b = B::NailEndB(nail);
    const bool drives = (a && coop::remote_prop::DrivingSlotOf(a) == in.slot) ||
                        (b && coop::remote_prop::DrivingSlotOf(b) == in.slot);
    if (!drives || !Within(Body(in.slot), at.X, at.Y, at.Z, kTakeReachCm)) {
        ++g_refused;
        UE_LOGW("build_sync: slot %d's snap of '%ls' -- %s, refused", in.slot, in.key.c_str(),
                drives ? "out of reach" : "its hold drives neither end");
    } else if (B::SnapNail(nail)) {
        ++g_snaps;
        UE_LOGI("build_sync: slot %d's pull snapped nail '%ls'", in.slot, in.key.c_str());
    }
    if (!R::IsLive(nail)) return;   // a pin went as it snapped: the poll sends its going
    g_rows[in.key].pending = true;
    std::wstring key;
    SendPiece(s, nail, key);
}

void HostPoll(coop::net::Session& s) {
    std::vector<Intent> intents;
    intents.swap(g_intents);
    for (const Intent& in : intents) {
        if (in.op == kOpState) HostMake(s, in);
        else HostTake(s, in);
    }
    // No client to send to yet: a joiner gets every piece at its ready edge.
    bool anyReady = false;
    for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers) && !anyReady; ++slot)
        anyReady = s.IsSlotWorldReady(slot);
    if (!anyReady) return;
    std::vector<void*> pieces;
    B::List(pieces);
    std::unordered_set<std::wstring> seen;
    for (void* p : pieces) {
        std::wstring key = B::KeyOf(p);
        if (ValidKey(key)) seen.insert(key);
        if (SendPiece(s, p, key)) seen.insert(key);
    }
    for (auto it = g_rows.begin(); it != g_rows.end();) {
        if (seen.count(it->first)) {
            ++it;
            continue;
        }
        if (it->second.print) {
            const std::vector<uint8_t> blob = Blob(kOpGone, it->first, nullptr);
            if (!coop::blob_chunks::SendBlob(&s, coop::net::ReliableKind::BuildState, ++g_blobSeq, blob)) {
                ++it;
                continue;
            }
            UE_LOGI("build_sync: '%ls' went", it->first.c_str());
        }
        it = g_rows.erase(it);
    }
}

// ---- CLIENT --------------------------------------------------------------------------------------------------------

bool SendIntent(coop::net::Session& s, uint8_t op, const std::wstring& key, const SaveRecord* rec,
                float change = 0.f) {
    const std::vector<uint8_t> blob = Blob(op, key, rec, change);
    return coop::blob_chunks::SendBlob(&s, coop::net::ReliableKind::BuildIntent, ++g_blobSeq, blob);
}

// The host's piece on this copy. False to try again on a later poll.
bool Make(const std::wstring& key, const SaveRecord& rec, void* piece, bool lastTry, Clock::time_point now) {
    Known& k = g_known[key];
    const uint64_t print = Print(rec);
    if (piece && k.confirmed && k.print == print) return true;
    void* a = nullptr;
    void* b = nullptr;
    if (!NailEnds(rec, a, b) && !lastTry) return false;
    const bool made = !piece;
    if (made) {
        piece = B::Spawn(rec);
        if (!piece) return false;
    }
    if (!B::Apply(piece, rec, a, b)) {
        if (made) B::DestroyQuiet(piece);
        return false;
    }
    B::ScaleWallHealth(piece, kWallHealthScale);
    B::Park(piece, !g_freed.count(key));
    k.hasHealth = B::WallHealth(piece, k.health);
    k.snapped = {};
    k.snapSent = false;
    k.ref.Set(piece);
    k.rec = rec;
    k.print = print;
    k.confirmed = true;
    k.since = now;
    g_saveKeys.erase(key);
    ++g_applied;
    if (made) UE_LOGI("build_sync: the host's %s '%ls' made here", KindName(B::KindOf(piece)), key.c_str());
    return true;
}

// A piece this copy held for the host that is no longer here: this player took it away, or it went with something else.
// True when it is dealt with.
bool Lost(coop::net::Session& s, const std::wstring& key, Known& k, Clock::time_point now) {
    const B::Kind kind = B::KindOfClass(k.rec.className);
    g_freed.erase(key);
    if (k.snapSent) return true;   // a pin this player's pull snapped: the host's answer makes it again or names it gone
    void* body = coop::players::Registry::Get().Local();
    const bool near = Within(body, k.rec.xform[4], k.rec.xform[5], k.rec.xform[6], kTakeReachCm);
    void* a = nullptr;
    void* b = nullptr;
    // A nail whose ends are gone went with them; an end the host still has comes back, and the nail with it.
    const bool takeable = kind == B::Kind::Unfinished || (kind == B::Kind::Nail && NailEnds(k.rec, a, b));
    if (k.confirmed && takeable && near) {
        if (!SendIntent(s, kOpGone, key, nullptr)) return false;
        UE_LOGI("build_sync: this player took %s '%ls' away -- the host takes it", KindName(kind), key.c_str());
        return true;
    }
    if (!k.confirmed) return true;   // this player's own, which the host never had
    const SaveRecord rec = k.rec;   // made again from its record, once its ends are here
    if (Make(key, rec, nullptr, false, now))
        UE_LOGI("build_sync: the host's %s '%ls' was lost here -- made again", KindName(kind), key.c_str());
    return false;
}

void SendSnap(coop::net::Session& s, const std::wstring& key, Known& k) {
    if (!SendIntent(s, kOpSnap, key, nullptr)) return;
    UE_LOGI("build_sync: this player's pull snapped nail '%ls' here -- the host judges it", key.c_str());
    k.snapSent = true;
    k.print = 0;   // the host's answer is applied even when it is the record this copy last had
}

// The host's nails in the board in this player's hands: their ties are let go here, as the game has them, so the pull
// breaks one as it would, and the break goes to the host. A tie no hand pulls is held again.
void WatchTies(coop::net::Session& s) {
    void* held = coop::local_streams::LastHeldActor();
    if (!held && g_freed.empty()) return;
    for (auto& e : g_known) {
        Known& k = e.second;
        if (!k.confirmed || B::KindOfClass(k.rec.className) != B::Kind::Nail || g_inbox.count(e.first)) continue;
        const bool freed = g_freed.count(e.first) > 0;
        void* nail = k.ref.Get();
        if (!nail) {
            if (freed && !k.snapSent) SendSnap(s, e.first, k);   // a pin goes as it snaps
            continue;
        }
        if (RecordBroken(k.rec)) {   // the host's is broken: no tie here to hold or let go
            g_freed.erase(e.first);
            continue;
        }
        const bool inHand = held && (B::NailEndA(nail) == held || B::NailEndB(nail) == held);
        if (inHand && !freed) {
            B::FreeNail(nail);
            g_freed.insert(e.first);
        } else if (!inHand && freed && !B::NailBroken(nail)) {
            B::HoldNail(nail);
            g_freed.erase(e.first);
        } else if (freed && !k.snapSent && B::NailBroken(nail)) {
            SendSnap(s, e.first, k);
        }
    }
}

// Every frame: a host wall whose health moved on this copy by this player's hit or repair goes to the host, and this
// copy shows it at once, unscaled, until the host's state answers; a host wall's state is applied as it comes.
void ClientWatch(coop::net::Session& s, Clock::time_point now) {
    if (!coop::net_pump::HasAnnouncedWorldReady() ||
        coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
        return;
    for (auto it = g_inbox.begin(); it != g_inbox.end();) {
        const auto k = g_known.find(it->first);
        void* wall = k == g_known.end() || !k->second.hasHealth ? nullptr : k->second.ref.Get();
        if (!wall || it->second.op != kOpState) {
            ++it;
            continue;
        }
        Make(it->first, it->second.rec, wall, true, now);
        it = g_inbox.erase(it);
    }
    WatchTies(s);
    for (auto& e : g_known) {
        Known& k = e.second;
        void* wall = k.confirmed && k.hasHealth ? k.ref.Get() : nullptr;
        float h = 0.f;
        if (!wall || !B::WallHealth(wall, h) || std::fabs(h - k.health) <= 0.01f) continue;
        const float change = h - k.health;
        if (!SendIntent(s, kOpChange, e.first, nullptr, change)) continue;
        UE_LOGI("build_sync: this player %s wall '%ls' by %.2f -- the host takes it", change < 0.f ? "hit" : "repaired",
                e.first.c_str(), std::fabs(change));
        // A hit that would break it here stays just above nothing: the host's wall breaks, and its going answers.
        float shown = k.health + change * kWallHealthScale;
        if (shown < kWallHealthScale) shown = kWallHealthScale;
        B::SetWallHealth(wall, shown);
        B::WallHealth(wall, k.health);
        k.print = 0;   // the host's answer is applied even when it is the record this copy last had
        ++g_changesSent;
    }
}

void ClientPoll(coop::net::Session& s, Clock::time_point now) {
    if (!coop::net_pump::HasAnnouncedWorldReady() ||
        coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
        return;
    auto here = PiecesByKey();
    if (!g_seeded) {
        g_seeded = true;
        for (const auto& e : here) g_saveKeys.emplace(e.first, now);
    }
    for (const auto& e : here) {
        B::Park(e.second, !g_freed.count(e.first));
        if (g_known.count(e.first) || g_saveKeys.count(e.first)) continue;
        // A piece this player made: the host makes its own under this key, and its state answers.
        SaveRecord rec;
        if (!B::Capture(e.second, rec) || rec.key != e.first || !RecordSane(rec)) continue;
        if (!SendIntent(s, kOpState, e.first, &rec)) continue;
        Known& k = g_known[e.first];
        k.ref.Set(e.second);
        k.rec = rec;
        k.print = Print(rec);
        k.since = now;
        UE_LOGI("build_sync: this player made %s '%ls' -- the host makes it", KindName(B::KindOf(e.second)),
                e.first.c_str());
    }
    // A host nail whose tie broke here alone (a copy's tie is held, so only something that breaks it outright) is tied
    // again once the host has had time to name a snap.
    for (auto& e : g_known) {
        Known& k = e.second;
        const auto p = here.find(e.first);
        if (!k.confirmed || p == here.end() || g_inbox.count(e.first)) continue;
        if (!B::NailBroken(p->second) || RecordBroken(k.rec)) {
            k.snapped = {};
            continue;
        }
        if (k.snapped == Clock::time_point{}) {
            k.snapped = now;
            continue;
        }
        if (now - k.snapped < kSnapWait) continue;
        k.print = 0;
        const SaveRecord rec = k.rec;
        if (Make(e.first, rec, p->second, true, now)) {
            ++g_reties;
            UE_LOGI("build_sync: the host's nail '%ls' snapped here alone -- tied again", e.first.c_str());
        }
    }
    for (auto it = g_known.begin(); it != g_known.end();) {
        Known& k = it->second;
        const bool present = here.count(it->first) > 0;
        if (!k.confirmed && now - k.since > kEchoLife) {
            if (present) B::DestroyQuiet(here[it->first]);
            ++g_refused;
            UE_LOGW("build_sync: the host never answered this player's '%ls' -- it goes", it->first.c_str());
            it = g_known.erase(it);
            continue;
        }
        if (present || g_inbox.count(it->first)) {
            ++it;
            continue;
        }
        it = Lost(s, it->first, k, now) ? g_known.erase(it) : std::next(it);
    }
    for (auto it = g_saveKeys.begin(); it != g_saveKeys.end();) {
        if (now - it->second < kSaveLife) {
            ++it;
            continue;
        }
        const auto p = here.find(it->first);
        if (p != here.end()) B::DestroyQuiet(p->second);
        UE_LOGI("build_sync: '%ls' of the save is not the host's -- it goes here", it->first.c_str());
        it = g_saveKeys.erase(it);
    }
    // The host's states last: a piece this player made since the last poll is sent first.
    for (auto it = g_inbox.begin(); it != g_inbox.end();) {
        const auto p = here.find(it->first);
        void* piece = p == here.end() ? nullptr : p->second;
        if (it->second.op == kOpGone) {
            if (piece) B::DestroyQuiet(piece);
            g_known.erase(it->first);
            g_saveKeys.erase(it->first);
            UE_LOGI("build_sync: the host's '%ls' went here", it->first.c_str());
            it = g_inbox.erase(it);
            continue;
        }
        const bool last = now - it->second.at > kStateLife;
        it = Make(it->first, it->second.rec, piece, last, now) || last ? g_inbox.erase(it) : std::next(it);
    }
}

void Reset() {
    g_rows.clear();
    g_intents.clear();
    g_known.clear();
    g_inbox.clear();
    g_saveKeys.clear();
    g_freed.clear();
    g_seeded = false;
    g_clientMade = 0;
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
    if (s->role() == coop::net::Role::Client && ue_wrap::world_identity::Generation() == g_world) ClientWatch(*s, now);
    if (!g_poll.Due(now)) return;
    const uint32_t world = ue_wrap::world_identity::Generation();
    if (world != g_world) {
        g_world = world;
        Reset();
    }
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
    float change = 0.f;
    if (!Parse(blob, op, key, rec, change) || op == kOpChange || op == kOpSnap) {
        UE_LOGW("build_sync: a malformed piece from the host -- dropped");
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
    if (!Parse(blob, in.op, in.key, in.rec, in.change)) {
        UE_LOGW("build_sync: a malformed piece from slot %d -- dropped", senderSlot);
        return;
    }
    in.slot = senderSlot;
    if (in.op == kOpChange || in.op == kOpSnap) {
        if (in.op == kOpChange) HostChange(*s, in);
        else HostSnap(*s, in);
        return;
    }
    if (g_intents.size() >= 64) return;
    g_intents.push_back(std::move(in));
}

unsigned WallChangesSent() { return g_changesSent.load(); }

void QueueConnectBroadcastForSlot(int /*slot*/) {
    for (auto& entry : g_rows) entry.second.pending = true;
}

void OnPeerLeftSlot(int slot) {
    if (slot <= 0 || slot >= static_cast<int>(coop::net::kMaxPeers)) return;
    g_asmIntent.ClearSlot(static_cast<uint8_t>(slot));
}

void OnDisconnect() {
    if (g_sent || g_applied || g_made || g_refused || g_taken || g_changes || g_reties || g_snaps)
        UE_LOGI("build_sync: session end -- states sent=%u made here=%u client pieces=%u taken=%u wall changes=%u "
                "snaps=%u re-tied=%u refused=%u", g_sent, g_applied, g_made, g_taken, g_changes, g_snaps, g_reties,
                g_refused);
    g_sent = g_applied = g_made = g_refused = g_taken = g_changes = g_reties = g_snaps = 0;
    Reset();
    g_asmState.Clear();
    g_asmIntent.Clear();
    g_session.store(nullptr);
}

}  // namespace coop::build_sync
