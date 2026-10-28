// coop/interactables/cord_sync.cpp -- see coop/interactables/cord_sync.h. The lane: the wire, the host's judgement of a
// claim and its broadcasts, the client's pending rule and apply, the seed, the client's parks. The watches that raise
// its edges are cord_sync_watch.cpp.

#include "coop/interactables/cord_sync.h"

#include "cord_sync_detail.h"  // co-located private header (src tree, not include/)

#include "coop/element/intent_authority.h"
#include "coop/element/object_scan_hub.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_key_index.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/cord.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop::cord_sync::detail {
namespace {
Counters g_stats;
}  // namespace

Counters& Stat() { return g_stats; }

bool SameLink(const LinkId& a, const LinkId& b) {
    if (a.form != b.form) return false;
    if (a.form == 0 || a.form == kFormUnsyncable) return true;
    const auto same = [](const coop::net::WireKey& x, const coop::net::WireKey& y) {
        return x.len == y.len && std::memcmp(x.data, y.data, x.len) == 0;
    };
    return same(a.sock, b.sock) && (a.form == 1 || same(a.child, b.child));
}

}  // namespace coop::cord_sync::detail

namespace coop::cord_sync {
namespace {

namespace R = ue_wrap::reflection;
namespace CD = ue_wrap::cord;
namespace OI = ue_wrap::object_index;
namespace WI = ue_wrap::world_identity;
namespace EL = coop::element;
using detail::Counters;
using detail::LinkId;
using Payload = coop::net::CordLinkPayload;
using Kind = coop::net::ReliableKind;

constexpr uint8_t kOpClaim = 0, kOpCanonical = 1, kOpSeedEnd = 2;
constexpr uint64_t kStatsEveryMs = 60000;

std::atomic<coop::net::Session*> g_session{nullptr};

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// ---- the listing of corded props (scan hub) ----

using PropMap = std::unordered_map<std::wstring, void*>;
PropMap g_props, g_foundProps;
uint32_t g_indexGen = 0;

bool IndexCurrent() { return g_indexGen == ue_wrap::world_identity::Generation(); }

// ---- HOST state ----

// The link each cord last had on the wire, by its prop's key. An absent entry is unplugged. Read silently at the
// listing's first full pass of a world, so the cords the host's save plugged are not news.
std::unordered_map<std::wstring, LinkId> g_broadcast;
uint32_t g_tableGen = 0;
bool g_tableSeeded = false;
// Joiners owed the seed, by slot; it waits for the listing of the current world.
uint32_t g_seedOwed = 0;
static_assert(coop::net::kMaxPeers <= 32, "g_seedOwed is a bit per slot");
struct Rate { uint64_t windowStart = 0; int count = 0; };
Rate g_rate[coop::net::kMaxPeers];
// The slot whose claim the host is applying, -1 otherwise: the broadcast of the change that apply makes skips it, as
// MTA's skips the source (BroadcastOnlyJoined's pSkip, CGame.cpp:3883); the claimant has its answer.
int g_claimant = -1;

// ---- CLIENT state ----

// Mute until the host's seed ended (op 2, the end mark MTA's join gives with CPlayerJoinCompletePacket, CGame.cpp:1377):
// before it this peer's copies are its own save's, and a link read then is not its player's.
bool g_seeded = false;
uint32_t g_nextSeq = 1;
// Per cord by its prop's key: the seq of this client's latest unanswered claim; the last link this client reported or
// applied; the canonical held behind the local player's drag (its cord, to find it again at the release).
std::unordered_map<std::wstring, uint32_t> g_pending;
std::unordered_map<std::wstring, LinkId> g_register;
struct Held { Payload p; ue_wrap::CachedObjRef cord; };
std::unordered_map<std::wstring, Held> g_held;
// A canonical whose cord or socket is not here yet, newest per cord, oldest first. `prop` is set once the cord's prop
// has been seen, so its death ends the park; a park for a prop never born waits for the session's end. A held or a
// parked canonical has passed the pending rule when it arrived: it is dropped at its drain only if a claim was sent
// since.
struct Park { std::wstring key, sock, child; Payload p; ue_wrap::CachedObjRef prop; };
std::vector<Park> g_parks;
// What the parks last waited on: the key index's version and the world, so a retry runs when either moved.
uint64_t g_retryVersion = ~0ull;
uint32_t g_retryWorld = 0;

uint64_t g_nextStatsMs = 0;
Counters g_lastPrinted{};

// ---- link <-> wire ----

LinkId ReadLink(void* cord) {
    LinkId l{};
    void* socket = CD::SocketOf(cord);
    if (!socket) return l;
    std::wstring owner, child;
    if (CD::ChildSocketName(socket, owner, child)) {
        if (owner.size() > detail::kKeyMax || child.size() > detail::kKeyMax) { l.form = detail::kFormUnsyncable; return l; }
        l.form = 2;
        coop::net::WireKeyFromString(owner, l.sock);
        coop::net::WireKeyFromString(child, l.child);
        return l;
    }
    const std::wstring key = ue_wrap::prop::GetActorSaveKeyString(socket);
    if (key.empty() || key == L"None" || key.size() > detail::kKeyMax) { l.form = detail::kFormUnsyncable; return l; }
    l.form = 1;
    coop::net::WireKeyFromString(key, l.sock);
    return l;
}

LinkId LinkOf(const Payload& p) {
    LinkId l{};
    l.form = p.sockForm;
    l.sock = p.sock;
    l.child = p.child;
    return l;
}

Payload MakePayload(uint8_t op, uint32_t seq, const coop::net::WireKey& cord, const LinkId& link) {
    Payload p{};
    p.op = op;
    p.seq = seq;
    p.cord = cord;
    if (link.form == 1 || link.form == 2) {
        p.sockForm = link.form;
        p.sock = link.sock;
        if (link.form == 2) p.child = link.child;
    }
    return p;
}

// A corded prop's key as the wire carries it, or false: none, "None", or past the wire's length.
bool WireKeyOfProp(void* prop, std::wstring& key, coop::net::WireKey& wire) {
    key = ue_wrap::prop::GetKeyString(prop);
    if (key.empty() || key == L"None" || key.size() > detail::kKeyMax) return false;
    coop::net::WireKeyFromString(key, wire);
    return true;
}

// The corded prop a key names and its cord, or null.
void* CordedProp(const std::wstring& key) {
    void* prop = coop::prop_key_index::Find(key);
    return (prop && CD::IsCordedProp(prop)) ? prop : nullptr;
}

// The socket a link names here, or null for unplugged; `unresolved` says a named socket is not here. `ownerPending`
// (when given) says a child socket's owner lookup could not answer yet, which is not "gone".
void* ResolveSocket(uint8_t form, const std::wstring& sock, const std::wstring& child, bool& unresolved,
                    bool* ownerPending = nullptr) {
    unresolved = false;
    if (form == 0) return nullptr;
    void* socket = nullptr;
    if (form == 1) {
        socket = LevelSocket(sock);
    } else if (form == 2) {
        void* owner = nullptr;
        const auto read = coop::prop_key_index::Resolve(sock, &owner);
        if (read == coop::prop_key_index::Read::Pending && ownerPending) *ownerPending = true;
        socket = read == coop::prop_key_index::Read::Hit ? CD::ChildSocket(owner, child) : nullptr;
    }
    unresolved = socket == nullptr;
    return socket;
}

// `skipSlot` leaves one peer out of a broadcast (slot < 0 only).
bool Send(coop::net::Session* s, int slot, const Payload& p, int skipSlot = -1) {
    const bool ok = slot < 0
        ? s->SendReliable(Kind::CordLinkState, &p, static_cast<int>(sizeof(p)), skipSlot)
        : s->SendReliableToSlot(slot, Kind::CordLinkState, &p, static_cast<int>(sizeof(p)));
    if (!ok) ++detail::Stat().sendFailed;
    return ok;
}

// ---- scan-hub listing ----

void HubBegin(void*, bool) { g_foundProps.clear(); }

void HubMatch(void*, void* actor) {
    if (!R::IsLive(actor) || R::NameStartsWith(R::NameOf(actor), L"Default__")) return;
    if (!CD::IsCordedProp(actor)) return;
    std::wstring key = ue_wrap::prop::GetKeyString(actor);
    if (key.empty() || key == L"None") return;
    g_foundProps.emplace(std::move(key), actor);
}

// The host's table of last-sent links is read from the first full pass of a world, without sending anything.
void SeedBroadcastTable(uint32_t gen) {
    g_broadcast.clear();
    for (const auto& [key, prop] : g_props) {
        if (!R::IsLive(prop)) continue;
        void* cord = CD::CordOf(prop);
        if (!cord) continue;
        const LinkId l = ReadLink(cord);
        if (l.form != 0) g_broadcast[key] = l;
    }
    g_tableGen = gen;
    g_tableSeeded = true;
}

size_t HubComplete(void*, bool full, uint32_t gen) {
    if (full) g_props.swap(g_foundProps);
    else for (auto& [key, actor] : g_foundProps) g_props[key] = actor;
    for (auto it = g_props.begin(); it != g_props.end();) {
        if (R::IsLive(it->second)) ++it;
        else it = g_props.erase(it);
    }
    g_indexGen = gen;
    auto* s = g_session.load(std::memory_order_acquire);
    if (full && s && s->role() == coop::net::Role::Host && (!g_tableSeeded || g_tableGen != gen)) SeedBroadcastTable(gen);
    return g_props.size();
}

void Register() {
    static bool registered = false;
    if (registered) return;
    registered = true;
    coop::element::scan_hub::Register({"cord", nullptr, &CD::EnsureResolved,
        [](void* actor) { return CD::IsCordedProp(actor); },
        &HubBegin, &HubMatch, &HubComplete, 15});
}

// ---- HOST ----

// A link to one sender, in answer to its claim of that seq. A link the wire cannot name is not synced: the answer
// echoes the claim's own link, counted, so the client's pending claim clears and its copy stays as it is.
void AnswerLink(coop::net::Session* s, uint8_t slot, const Payload& claim, LinkId link) {
    if (link.form == detail::kFormUnsyncable) { link = LinkOf(claim); ++detail::Stat().unsyncable; }
    Send(s, slot, MakePayload(kOpCanonical, claim.seq, claim.cord, link));
    ++detail::Stat().answers;
}

// The cord's link as the host has it (unplugged when there is no cord).
void Answer(coop::net::Session* s, uint8_t slot, const Payload& claim, void* cord) {
    AnswerLink(s, slot, claim, cord ? ReadLink(cord) : LinkId{});
}

// The link the host last broadcast for that key, else `fallback`, for a claim answered without looking the cord up.
void AnswerLastBroadcast(coop::net::Session* s, uint8_t slot, const Payload& claim, const std::wstring& key,
                         const LinkId& fallback) {
    const auto it = g_broadcast.find(key);
    AnswerLink(s, slot, claim, it != g_broadcast.end() ? it->second : fallback);
}

// A claim is a person's plug: bounded per sender, and a claim past the bound is answered, not applied.
bool RateAllows(uint8_t slot) {
    if (slot >= coop::net::kMaxPeers) return false;
    const uint64_t now = NowMs();
    Rate& r = g_rate[slot];
    if (now - r.windowStart >= detail::kRateWindowMs) { r.windowStart = now; r.count = 0; }
    if (r.count >= detail::kMaxClaimsPerWindow) return false;
    ++r.count;
    return true;
}

// The sender is within reach of the cord's prop, or of the socket a plug names, or of the host's head of the cord for
// an unplug: the nearer one counts, the game's use distance doubled.
bool InReach(coop::net::Session* s, uint8_t slot, void* prop, void* cord, void* socket) {
    const auto token = EL::IntentTarget::ForClientIntent(*s, slot, detail::kReachUU);
    if (token.Authorize(prop)) return true;
    ue_wrap::FVector at{};
    const bool read = socket ? ue_wrap::engine::TryGetActorLocation(socket, at) : CD::HeadLocation(cord, at);
    return read && token.AuthorizeSegment(at, at);
}

// A client's claim: its player's plug or unplug already ran there, and this is its outcome. The host tests the sender
// and the state, then applies the game's own rule to its copy: connect into an occupied socket leaves the occupant
// plugged and the claiming cord unplugged (MTA's newcomer evicts the present partners instead, CGame.cpp:3845-3871; here
// the game's connect decides). Compare-first on both edges, since
// connect into the socket the cord already holds unplugs it. Every claim is answered; the rate check is first, so a
// flood costs no lookup.
void HostClaim(coop::net::Session* s, uint8_t slot, const Payload& p) {
    Counters& st = detail::Stat();
    ++st.claims;
    const std::wstring key = coop::net::StringFromWireKey(p.cord);
    if (!RateAllows(slot)) { ++st.refusedRate; AnswerLastBroadcast(s, slot, p, key, LinkId{}); return; }
    void* found = nullptr;
    const auto read = coop::prop_key_index::Resolve(key, &found);
    // A lookup that cannot answer yet is not "the cord is gone": the answer is what the host last said, else the
    // claim's own link.
    if (read == coop::prop_key_index::Read::Pending) {
        ++st.keyPending;
        AnswerLastBroadcast(s, slot, p, key, LinkOf(p));
        return;
    }
    void* prop = (read == coop::prop_key_index::Read::Hit && CD::IsCordedProp(found)) ? found : nullptr;
    void* cord = prop ? CD::CordOf(prop) : nullptr;
    if (coop::players::Registry::Get().LifeOf(slot) != coop::players::PuppetLife::Alive) {
        ++st.refusedDead;
        Answer(s, slot, p, cord);
        return;
    }
    bool unresolved = false, ownerPending = false;
    void* socket = nullptr;
    if (cord) socket = ResolveSocket(p.sockForm, coop::net::StringFromWireKey(p.sock),
                                     coop::net::StringFromWireKey(p.child), unresolved, &ownerPending);
    if (ownerPending) { ++st.keyPending; AnswerLastBroadcast(s, slot, p, key, LinkOf(p)); return; }
    if (!cord || unresolved) { ++st.unresolved; Answer(s, slot, p, cord); return; }
    if (!InReach(s, slot, prop, cord, socket)) { ++st.refusedReach; Answer(s, slot, p, cord); return; }
    if (!detail::LaneLive()) { ++st.laneOff; Answer(s, slot, p, cord); return; }
    if (detail::Dragging(cord)) { ++st.refusedHostDrag; Answer(s, slot, p, cord); return; }
    if (CD::SocketOf(cord) == socket) { ++st.claimsEqual; Answer(s, slot, p, cord); return; }
    {
        detail::ApplyScope lane;
        g_claimant = slot;
        if (!socket) {
            CD::Disconnect(cord);
        } else {
            if (CD::SocketOf(cord)) CD::Disconnect(cord);
            CD::Connect(cord, socket);
        }
        g_claimant = -1;
    }
    ++st.claimsApplied;
    Answer(s, slot, p, cord);
}

// The corded props alive in the running world, from the object index: its classes that are a prop_corded_C, then each
// class's instances. MTA's join walks the live container, not a cache (CMapManager.cpp:305-313).
std::vector<void*> LiveCordedProps() {
    std::vector<void*> classes, props;
    OI::ForEachClass(
        [](void* ctx, void* cls, void* anyInstance) {
            if (CD::IsCordedProp(anyInstance)) static_cast<std::vector<void*>*>(ctx)->push_back(cls);
        },
        &classes);
    struct Walk { std::vector<void*>* out; void* world; } walk{&props, WI::CurrentWorld()};
    for (void* cls : classes) {
        OI::ForEachInstance(
            cls,
            [](void* ctx, void* obj, int32_t idx) {
                const auto* w = static_cast<Walk*>(ctx);
                if (R::SlotFlags(idx) & (R::slot_flags::Dying | R::slot_flags::NotYetReadable)) return;
                if (!R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
                if (WI::WorldOf(obj) == w->world) w->out->push_back(obj);
            },
            &walk);
    }
    return props;
}

// Every corded prop's link to one joiner, then the seed's end. MTA sends only the attached links after the entity
// packet (CMapManager.cpp:305-313); this sends the unplugged ones too, since the joiner's own save plugs cords at its
// world load and an unplugged entry is what repairs one the host has since unplugged. Op 2 is the end mark that stands
// for MTA's joined state. Waits for the listing of this world.
bool SendSeed(coop::net::Session* s, int slot) {
    if (!IndexCurrent()) return false;
    Counters& st = detail::Stat();
    ++st.seeds;
    for (void* prop : LiveCordedProps()) {
        if (!R::IsLive(prop)) continue;
        void* cord = CD::CordOf(prop);
        if (!cord) continue;
        std::wstring key;
        coop::net::WireKey wire{};
        if (!WireKeyOfProp(prop, key, wire)) { ++st.unsyncable; continue; }
        const LinkId l = ReadLink(cord);
        if (l.form == detail::kFormUnsyncable) { ++st.unsyncable; continue; }
        Send(s, slot, MakePayload(kOpCanonical, 0, wire, l));
        ++st.seedLinks;
    }
    Payload end{};
    end.op = kOpSeedEnd;
    Send(s, slot, end);
    return true;
}

void DrainSeeds(coop::net::Session* s) {
    if (!g_seedOwed) return;
    for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers); ++slot) {
        if (!(g_seedOwed & (1u << slot))) continue;
        if (!s->IsSlotWorldReady(slot)) { g_seedOwed &= ~(1u << slot); continue; }  // gone before its seed
        if (SendSeed(s, slot)) g_seedOwed &= ~(1u << slot);
    }
}

// ---- CLIENT ----

// The pending rule. The answer and the host's broadcasts reach this client on one ordered lane, in the host's send
// order, so a broadcast that arrives while a claim is pending was sent before the host took that claim; an answer is
// the latest claim's or it is stale. (Source reconciles a predicted state the same way: a server snapshot supersedes
// the client's prediction once the command is acknowledged. An answer lost with a dying connection ends with it.)
bool Admissible(const std::wstring& key, uint32_t seq) {
    const auto it = g_pending.find(key);
    if (seq == 0) return it == g_pending.end();
    return it != g_pending.end() && it->second == seq;
}

void ParkIt(const std::wstring& key, const Payload& p, void* prop) {
    for (auto it = g_parks.begin(); it != g_parks.end(); ++it)
        if (it->key == key) { g_parks.erase(it); break; }
    if (g_parks.size() >= detail::kMaxParks) {
        g_parks.erase(g_parks.begin());
        ++detail::Stat().parkOverflow;
    }
    Park park{key, coop::net::StringFromWireKey(p.sock), coop::net::StringFromWireKey(p.child), p, {}};
    if (prop) park.prop.Set(prop);
    g_parks.push_back(std::move(park));
    ++detail::Stat().parked;
}

// What a canonical names here: the cord's prop, its cord, the socket (null for unplugged), and whether a named socket
// is missing. Resolved once and handed on.
struct Ends {
    void* prop = nullptr;
    void* cord = nullptr;
    void* socket = nullptr;
    bool unresolved = false;
};

Ends ResolveEnds(const std::wstring& key, uint8_t form, const std::wstring& sock, const std::wstring& child) {
    Ends e;
    e.prop = CordedProp(key);
    e.cord = e.prop ? CD::CordOf(e.prop) : nullptr;
    if (e.cord) e.socket = ResolveSocket(form, sock, child, e.unresolved);
    return e;
}

// A cord this lane unplugged to make room for another reports unplugged, so its player's replug of the same socket
// differs from the register and is claimed.
void RegisterUnplugged(void* cord) {
    void* prop = CD::PropOf(cord);
    if (!prop) return;
    const std::wstring key = ue_wrap::prop::GetKeyString(prop);
    if (!key.empty() && key != L"None") g_register[key] = LinkId{};
}

// Apply a canonical to this peer's copy: compare first, then the host's own rule. The socket's occupant here is
// unplugged first, as the host's rule left it (the occupant stays plugged there; MTA's newcomer would evict, this is
// the game's own connect), and the cord leaves any other socket before the plug, since connect never does. A link
// still differing after is counted, and the next canonical settles it.
void ApplyCanonical(const std::wstring& key, const Payload& p, const Ends& e) {
    Counters& st = detail::Stat();
    if (!e.cord || e.unresolved) { ParkIt(key, p, e.prop); return; }
    g_register[key] = LinkOf(p);
    void* cord = e.cord;
    void* target = e.socket;
    void* current = CD::SocketOf(cord);
    if (current == target) { ++st.equal; return; }
    {
        detail::ApplyScope lane;
        if (!target) {
            if (!CD::Disconnect(cord)) ++st.applyFailed;
        } else {
            void* occupant = CD::CordIn(target);
            if (occupant && occupant != cord) {
                CD::Disconnect(occupant);
                RegisterUnplugged(occupant);
            }
            if (current) CD::Disconnect(cord);
            if (!CD::Connect(cord, target)) ++st.applyFailed;
        }
    }
    ++st.applied;
    if (CD::SocketOf(cord) != target) ++st.stillDiffering;
}

// A canonical that passed the pending rule when it arrived: held behind the local player's drag of that cord, else
// applied; dropped here only when a claim has been sent since, whose answer is the newer word.
void Deliver(const std::wstring& key, const Payload& p, const Ends& e) {
    Counters& st = detail::Stat();
    if (g_pending.find(key) != g_pending.end()) { ++st.pendingDropped; return; }
    if (e.cord && detail::Dragging(e.cord)) {
        Held h{p, {}};
        h.cord.Set(e.cord);
        g_held[key] = std::move(h);
        ++st.held;
        return;
    }
    ApplyCanonical(key, p, e);
}

void ClientCanonical(const Payload& p) {
    const std::wstring key = coop::net::StringFromWireKey(p.cord);
    if (key.empty()) { ++detail::Stat().badWire; return; }
    if (!Admissible(key, p.seq)) { ++detail::Stat().pendingDropped; return; }
    // The latest answer has arrived: nothing is pending for that cord now, so a broadcast after it is admitted.
    if (p.seq != 0) g_pending.erase(key);
    // The newest canonical for a cord stands in for what was parked or held before it.
    for (auto it = g_parks.begin(); it != g_parks.end(); ++it)
        if (it->key == key) { g_parks.erase(it); break; }
    g_held.erase(key);
    Deliver(key, p, ResolveEnds(key, p.sockForm, coop::net::StringFromWireKey(p.sock),
                                coop::net::StringFromWireKey(p.child)));
}

// A parked canonical is retried when its prop and socket are here, dropped when the prop it was seen on is gone. It
// runs only when what the parks wait on moved: the key index's table or the world. A read that missed cannot answer
// otherwise.
void RetryParks() {
    const uint64_t version = coop::prop_key_index::Version();
    const uint32_t world = WI::Generation();
    if (version == g_retryVersion && world == g_retryWorld) return;
    g_retryVersion = version;
    g_retryWorld = world;
    size_t budget = g_parks.size();
    for (size_t i = 0; i < g_parks.size() && budget-- > 0;) {
        Park& pk = g_parks[i];
        const Ends e = ResolveEnds(pk.key, pk.p.sockForm, pk.sock, pk.child);
        if (e.prop && !pk.prop.Is(e.prop)) pk.prop.Set(e.prop);
        if (e.cord && !e.unresolved) {
            const std::wstring key = pk.key;
            const Payload p = pk.p;
            g_parks.erase(g_parks.begin() + static_cast<std::ptrdiff_t>(i));
            Deliver(key, p, e);
            continue;
        }
        if (pk.prop.Raw() && !pk.prop.Alive()) {
            ++detail::Stat().parkDead;
            g_parks.erase(g_parks.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

// ---- stats ----

void PrintStats(bool summary) {
    const Counters& c = detail::Stat();
    if (std::memcmp(&c, &g_lastPrinted, sizeof(c)) == 0) return;
    g_lastPrinted = c;
    UE_LOGI("cord_sync: %s claims=%u applied=%u equal=%u refusedRate=%u refusedDead=%u refusedReach=%u "
            "refusedHostDrag=%u unresolved=%u keyPending=%u laneOff=%u sockBuilds=%u broadcasts=%u answers=%u "
            "seeds=%u seedLinks=%u | claimsSent=%u "
            "applied=%u equal=%u stillDiffering=%u applyFailed=%u pendingDropped=%u held=%u parked=%u "
            "parkOverflow=%u parkDead=%u parkLeft=%u stretchRefused=%u | badWire=%u unsyncable=%u sendFailed=%u "
            "dropSize=%u dropKey=%u dropOp=%u dropField=%u",
            summary ? "summary" : "stats", c.claims, c.claimsApplied, c.claimsEqual, c.refusedRate, c.refusedDead,
            c.refusedReach, c.refusedHostDrag, c.unresolved, c.keyPending, c.laneOff, c.sockBuilds, c.broadcasts,
            c.answers, c.seeds, c.seedLinks,
            c.claimsSent, c.applied, c.equal, c.stillDiffering, c.applyFailed, c.pendingDropped, c.held, c.parked,
            c.parkOverflow, c.parkDead, c.parkLeft, c.stretchRefused, c.badWire, c.unsyncable, c.sendFailed,
            c.dropSize, c.dropKey, c.dropOp, c.dropField);
}

}  // namespace

// ---- the seam the watches call ----

namespace detail {

coop::net::Session* Connected() {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->connected()) ? s : nullptr;
}
bool IsHost(const coop::net::Session* s) { return s && s->role() == coop::net::Role::Host; }
bool IsClient(const coop::net::Session* s) { return s && s->role() == coop::net::Role::Client; }

void OnClientLinkEdge(void* cord) {
    auto* s = Connected();
    if (!s || !g_seeded || !LaneLive()) return;
    void* prop = CD::PropOf(cord);
    std::wstring key;
    coop::net::WireKey wire{};
    if (!prop || !CD::IsCordedProp(prop)) return;
    if (!WireKeyOfProp(prop, key, wire)) { ++Stat().unsyncable; return; }
    const LinkId now = ReadLink(cord);
    if (now.form == kFormUnsyncable) { ++Stat().unsyncable; return; }
    const auto reg = g_register.find(key);
    if (reg != g_register.end() && SameLink(reg->second, now)) return;
    // The register and the pending claim are set only once the claim is sent: a lost claim leaves both as they were.
    const uint32_t seq = g_nextSeq;
    if (!Send(s, -1, MakePayload(kOpClaim, seq, wire, now))) return;
    ++g_nextSeq;
    ++Stat().claimsSent;
    g_register[key] = now;
    g_pending[key] = seq;
}

void OnHostLinkEdge(void* cord) {
    auto* s = Connected();
    if (!s) return;
    void* prop = CD::PropOf(cord);
    std::wstring key;
    coop::net::WireKey wire{};
    if (!prop || !CD::IsCordedProp(prop)) return;
    if (!WireKeyOfProp(prop, key, wire)) { ++Stat().unsyncable; return; }
    const LinkId now = ReadLink(cord);
    const auto prev = g_broadcast.find(key);
    if (SameLink(prev == g_broadcast.end() ? LinkId{} : prev->second, now)) return;
    g_broadcast[key] = now;
    if (now.form == kFormUnsyncable) { ++Stat().unsyncable; return; }
    if (Send(s, -1, MakePayload(kOpCanonical, 0, wire, now), g_claimant)) ++Stat().broadcasts;
}

void OnDragEnded(void* cord) {
    if (!IsClient(Connected())) return;
    for (auto it = g_held.begin(); it != g_held.end(); ++it) {
        if (!it->second.cord.Is(cord)) continue;
        const std::wstring key = it->first;
        const Payload p = it->second.p;
        g_held.erase(it);
        Deliver(key, p, ResolveEnds(key, p.sockForm, coop::net::StringFromWireKey(p.sock),
                                    coop::net::StringFromWireKey(p.child)));
        return;
    }
}

void OnCordDying(void* cord) {
    for (auto it = g_held.begin(); it != g_held.end(); ++it) {
        if (it->second.cord.Raw() != cord) continue;
        g_held.erase(it);
        return;
    }
}

}  // namespace detail

// ---- the public surface ----

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    Register();
    detail::InstallWatches();
}

void Tick() {
    detail::TickWatches();
    auto* s = detail::Connected();
    if (!s) return;
    if (detail::IsHost(s)) DrainSeeds(s);
    else if (!g_parks.empty()) RetryParks();
    const uint64_t now = NowMs();
    if (now >= g_nextStatsMs) {
        g_nextStatsMs = now + kStatsEveryMs;
        PrintStats(false);
    }
}

void OnReliable(const coop::net::CordLinkPayload& payload, uint8_t senderSlot) {
    if (!ue_wrap::game_thread::IsGameThread()) { ++detail::Stat().badWire; return; }
    auto* s = detail::Connected();
    if (!s) return;
    if (payload.op == kOpClaim) {
        if (detail::IsHost(s)) HostClaim(s, senderSlot, payload);
    } else if (payload.op == kOpCanonical) {
        if (detail::IsClient(s)) ClientCanonical(payload);
    } else if (payload.op == kOpSeedEnd) {
        if (detail::IsClient(s)) g_seeded = true;
    }
}

void CountDropped(Drop why) {
    Counters& st = detail::Stat();
    switch (why) {
    case Drop::Size:  ++st.dropSize; break;
    case Drop::Key:   ++st.dropKey; break;
    case Drop::Op:    ++st.dropOp; break;
    case Drop::Field: ++st.dropField; break;
    }
}

DrillView ReadForDrill() {
    const Counters& c = detail::Stat();
    DrillView v;
    v.seeded = g_seeded;
    v.claimsSent = c.claimsSent;
    v.stretchRefused = c.stretchRefused;
    v.pendingDropped = c.pendingDropped;
    v.claims = c.claims;
    v.claimsApplied = c.claimsApplied;
    v.claimsEqual = c.claimsEqual;
    v.refusedRate = c.refusedRate;
    v.refusedDead = c.refusedDead;
    v.refusedReach = c.refusedReach;
    v.refusedHostDrag = c.refusedHostDrag;
    v.unresolved = c.unresolved;
    v.keyPending = c.keyPending;
    v.laneOff = c.laneOff;
    v.answers = c.answers;
    v.dropSize = c.dropSize;
    v.dropKey = c.dropKey;
    v.dropOp = c.dropOp;
    v.dropField = c.dropField;
    return v;
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = detail::Connected();
    if (!detail::IsHost(s) || peerSlot <= 0 || peerSlot >= static_cast<int>(coop::net::kMaxPeers)) return;
    g_seedOwed |= 1u << peerSlot;
    DrainSeeds(s);
}

void OnDisconnect() {
    detail::Stat().parkLeft += static_cast<uint32_t>(g_parks.size());
    PrintStats(true);
    g_session.store(nullptr, std::memory_order_release);
    g_props.clear(); g_foundProps.clear();
    g_indexGen = 0;
    g_broadcast.clear(); g_tableGen = 0; g_tableSeeded = false;
    g_seedOwed = 0;
    for (Rate& r : g_rate) r = Rate{};
    g_seeded = false;
    g_nextSeq = 1;
    g_pending.clear(); g_register.clear(); g_held.clear(); g_parks.clear();
    g_retryVersion = ~0ull; g_retryWorld = 0;
    g_claimant = -1;
    g_nextStatsMs = 0;
    detail::Stat() = Counters{};
    g_lastPrinted = Counters{};
    detail::ClearLatches();
    detail::ClearSockets();
}

}  // namespace coop::cord_sync
