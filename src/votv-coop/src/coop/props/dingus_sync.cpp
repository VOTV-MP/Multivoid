// coop/props/dingus_sync.cpp -- see coop/props/dingus_sync.h. The install, the watch on `touch`, the found set, the births the
// host drains and the client's hearing. The host's judge of a client's report is dingus_judge.cpp.
//
// prop_dingus_C's graph, from its bytecode (0.9.0n). `touch`: if `isUsed` is false it grants the finder's achievement and
// deactivates `ws1`; then `isUsed` = true. playerGrabbed and every actionOptionIndex call it, on the machine that ran the
// grab. `loadData` with `isUsed` set lowers `ws1`'s volume, gives it the small attenuation asset and deactivates it. Holding
// a dingus runs no `touch` (the hand copy is a new object written by loadData, `isUsed` unchanged).

// MTA keeps an element's data on the element and sends it whole at the join (CEntityAddPacket.cpp:178-181), and a client's
// change request is validated by the server, which owns the data. Here the datum is the host's set beside the copies, not on
// them: the record lane does not carry a dingus (the food claim covers every prop_food_C descendant with no exemption), and
// carving it out would move a food class's ownership off the food lane. The set is the element datum's stand-in, fed from the
// copies themselves.

#include "coop/props/dingus_sync.h"

#include "dingus_internal.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_key_index.h"
#include "ue_wrap/actors/dingus.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace coop::dingus_sync {
namespace {

namespace D  = ue_wrap::dingus;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace EL = coop::element;
namespace N  = coop::net;

UE_FACT_FN(kRowTouch, "dingus_sync", L"prop_dingus_C", L"touch", Declared);
constexpr int kTagTouch = 0x44475331;  // 'DGS1'

std::atomic<N::Session*> g_session{nullptr};
bool g_installed = false, g_watched = false, g_settled = false, g_taken = false, g_subscribed = false;
bool g_subscribeSaid = false;  // the full-table error, said once a session
bool g_catchUp = false;        // the four classes were just found: one pass over the live copies

// The keys found in this world, by this peer's knowledge of them. Game thread only.
std::unordered_set<std::wstring> g_found;
bool g_fullSaid = false;
bool g_seedSaid = false;

struct Counters {
    uint32_t full = 0, longKey = 0, badKey = 0, markFail = 0, dropped = 0, sendFailed = 0, marked = 0, queueFull = 0;
};
Counters g_n;

bool PowerOfTwo(uint32_t n) { return (n & (n - 1)) == 0; }

// The dingus drill's red arms: one dev row, read once, each value withholding one effect (a send or a mark) and never a refusal.
enum class Red : uint8_t { Off, Report, Broadcast, BirthMark, BindMark, BirthSeed, Snapshot };
Red RedOf() {
    static const Red red = [] {
        const std::string v = coop::config::ResolveEnum(coop::config_registry::rows::dingus_red);
        if (v == "report") return Red::Report;
        if (v == "broadcast") return Red::Broadcast;
        if (v == "birth_mark") return Red::BirthMark;
        if (v == "bind_mark") return Red::BindMark;
        if (v == "birth_seed") return Red::BirthSeed;
        if (v == "snapshot") return Red::Snapshot;
        return Red::Off;
    }();
    return red;
}
bool RedIs(Red r) { return RedOf() == r; }

const dev::Observer* g_observer = nullptr;

// The births the host's callback saw, any thread, drained on the game thread.
constexpr size_t kBornCap = 16;
struct Born {
    void* actor;
    EL::ElementId eid;
};
std::mutex g_bornMutex;
Born g_born[kBornCap];
size_t g_bornCount = 0;
std::atomic<size_t> g_bornPending{0};
std::atomic<bool> g_bornOverflow{false};

// The key as a wire key, or false: a key too long is never truncated (counted and said once), and a keyless dingus or a key with a
// byte outside printable ASCII cannot be named.
bool ToWire(const std::wstring& key, N::WireKey& out) {
    if (key.empty() || key == L"None") {
        if (PowerOfTwo(++g_n.badKey)) UE_LOGW("dingus_sync: a dingus with no key cannot be shared (#%u)", g_n.badKey);
        return false;
    }
    if (key.size() > sizeof(out.data)) {
        if (++g_n.longKey == 1) UE_LOGW("dingus_sync: the key '%ls' is longer than the wire's %zu and is not sent", key.c_str(), sizeof(out.data));
        return false;
    }
    N::WireKeyFromString(key, out);
    if (!WireKeyWellFormed(out)) {
        if (PowerOfTwo(++g_n.badKey)) UE_LOGW("dingus_sync: the key '%ls' has a character the wire does not carry (#%u)", key.c_str(), g_n.badKey);
        return false;
    }
    return true;
}

void OnWorldChange() noexcept {
    g_found.clear();
    g_fullSaid = false;
    g_seedSaid = false;
}

// The key of a copy, read from the engine.
std::wstring KeyOf(void* actor) { return ue_wrap::prop::GetInteractableKeyString(actor); }

// ---- the find on this peer's own copy ------------------------------------------------------------------------------------

// `touch` ended on a dingus on this machine. The host's find is its own; a client's is asked of the host unless its set holds the
// key already: `touch` runs once per grab or action, and the judge's budget bounds the asks for a key not yet acked.
void OnTouchPost(const sg::Call& c) {
    N::Session* const s = detail::SessionPtr();
    if (!s || !s->running() || !D::IsDingus(c.object)) return;
    const std::wstring key = KeyOf(c.object);
    N::WireKey wire{};
    if (!ToWire(key, wire)) return;
    if (detail::IsHost()) {
        if (detail::AddKey(key) != detail::Add::Added) return;
        UE_LOGI("dingus_sync: HOST found the dingus '%ls' -- every player's copy goes quiet", key.c_str());
        detail::Broadcast(key);
        return;
    }
    if (!s->connected()) return;
    // MTA's client sends only a changed value (CStaticFunctionDefinitions.cpp:1032): a key the set holds was acked by the host.
    if (detail::Has(key)) return;
    if (RedIs(Red::Report)) {
        UE_LOGW("dingus_sync: dingus_red=report -- CLIENT's report of the dingus '%ls' is not sent", key.c_str());
        return;
    }
    N::DingusReportPayload p{};
    p.key = wire;
    if (!s->SendReliable(N::ReliableKind::DingusReport, &p, sizeof(p))) {
        if (PowerOfTwo(++g_n.sendFailed)) UE_LOGW("dingus_sync: CLIENT's report of the dingus '%ls' was not sent (#%u)", key.c_str(), g_n.sendFailed);
        return;
    }
    UE_LOGI("dingus_sync: CLIENT found the dingus '%ls' -- asked the host", key.c_str());
}

// ---- the host's births ---------------------------------------------------------------------------------------------------

// MarkPropElement's callback, any thread: a dingus is recognised by its class pointer alone and queued with its element id.
// Anything else costs a pointer compare.
void OnExpressed(void* actor, EL::ElementId eid) {
    if (!D::IsDingus(actor)) return;
    std::lock_guard<std::mutex> lk(g_bornMutex);
    if (g_bornCount >= kBornCap) {
        g_bornOverflow.store(true, std::memory_order_release);
        return;
    }
    g_born[g_bornCount++] = Born{actor, eid};
    g_bornPending.store(g_bornCount, std::memory_order_release);
}

struct Pass {
    int read = 0, found = 0;
};

// One dingus copy the host has: a copy the save loaded found is a find nobody told of (its key joins the set and every client
// hears it); a copy born under a key of the set that reads unfound (a drop of a found dingus) is marked.
void HandleBirth(void* actor, Pass& pass) {
    const std::wstring key = KeyOf(actor);
    N::WireKey wire{};
    if (!ToWire(key, wire)) return;
    ++pass.read;
    const int found = D::ReadFound(actor);
    if (found == 1) ++pass.found;
    if (found == 1 && !detail::Has(key) && !RedIs(Red::BirthSeed)) {
        if (detail::AddKey(key) == detail::Add::Added) {
            UE_LOGI("dingus_sync: HOST's copy of the dingus '%ls' is found as it was born or loaded -- every player's copy goes quiet",
                    key.c_str());
            detail::Broadcast(key);
        }
    } else if (found == 0 && detail::Has(key) && !RedIs(Red::BirthMark)) {
        detail::MarkCopy(actor, key, "born under a found key");
    }
    if (g_observer && g_observer->born) g_observer->born(actor, key);
}

// Every live dingus copy, collected first: a mark loads state and sends, which a walk of the object index must not do.
std::vector<void*> LiveCopies() {
    std::vector<void*> live;
    D::ForEachDingus([](void* ctx, void* actor) { static_cast<std::vector<void*>*>(ctx)->push_back(actor); }, &live);
    return live;
}

// CLIENT: the copies alive when the four classes were found, marked when their key is in the set (a copy bound before its class
// was known was passed over at its bind).
void MarkLiveCopies() {
    for (void* const actor : LiveCopies()) {
        if (!R::IsLive(actor)) continue;
        const std::wstring key = KeyOf(actor);
        if (detail::Has(key) && D::ReadFound(actor) == 0) detail::MarkCopy(actor, key, "its class was found after its bind");
    }
}

// HOST: the births queued since the last tick, or every live copy when the queue overflowed or the four classes were just found.
void DrainBorn(bool catchUp) {
    Born taken[kBornCap];
    size_t n = 0;
    bool overflow = false;
    {
        std::lock_guard<std::mutex> lk(g_bornMutex);
        for (; n < g_bornCount; ++n) taken[n] = g_born[n];
        g_bornCount = 0;
        g_bornPending.store(0, std::memory_order_release);
        overflow = g_bornOverflow.exchange(false, std::memory_order_acq_rel);
    }
    Pass pass;
    if (overflow || catchUp) {
        // More births than the queue holds in one tick (the seed of a big save), or a class found after its first copy was
        // enrolled: one pass over the live copies, each treated as a birth. An event-driven pass, never a timed one.
        if (overflow && PowerOfTwo(++g_n.queueFull))
            UE_LOGW("dingus_sync: the birth queue overflowed -- one pass over the live copies (#%u)", g_n.queueFull);
        for (void* const actor : LiveCopies())
            if (R::IsLive(actor)) HandleBirth(actor, pass);
    } else {
        for (size_t i = 0; i < n; ++i)
            if (EL::Registry::Get().EidForActor(taken[i].actor) == taken[i].eid && R::IsLive(taken[i].actor)) HandleBirth(taken[i].actor, pass);
    }
    if (!g_seedSaid) {
        g_seedSaid = true;
        UE_LOGI("dingus_sync: the first drain read %d dingus copies, %d found", pass.read, pass.found);
    }
}

// ---- the watch's settle --------------------------------------------------------------------------------------------------

void SettleWatch() {
    if (!g_watched || g_settled) return;
    sg::ResolvePendingNames();
    if (g_taken && !sg::NameWatchSettled(kRowTouch, kTagTouch)) return;
    g_settled = true;
    if (g_taken && sg::NameWatchLive(kRowTouch, kTagTouch)) UE_LOGI("dingus_sync: the watch on prop_dingus_C's touch is live");
    else UE_LOGE("dingus_sync: the watch on prop_dingus_C's touch is dead -- a dingus found on this machine is not shared");
}

}  // namespace

namespace detail {

N::Session* SessionPtr() { return g_session.load(std::memory_order_acquire); }
bool IsHost() {
    N::Session* const s = SessionPtr();
    return s && s->running() && s->role() == N::Role::Host;
}
bool IsClient() {
    N::Session* const s = SessionPtr();
    return s && s->running() && s->role() == N::Role::Client;
}

bool Has(const std::wstring& key) { return g_found.count(key) != 0; }

Add AddKey(const std::wstring& key) {
    if (g_found.count(key)) return Add::Known;
    if (g_found.size() >= kMaxFound) {
        ++g_n.full;
        if (!g_fullSaid) {
            g_fullSaid = true;
            UE_LOGW("dingus_sync: the found set holds %zu keys, the most there is -- a further dingus is marked here and not shared", kMaxFound);
        }
        return Add::Full;
    }
    g_found.insert(key);
    return Add::Added;
}

// MTA's server tells every joined player except the source (CGame.cpp:2757-2768), the source having written the datum on its own
// element. Only the host writes a finder's set here, so the finder is told too.
bool Broadcast(const std::wstring& key) {
    N::Session* const s = SessionPtr();
    N::DingusFoundPayload p{};
    if (!s || !ToWire(key, p.key)) return false;
    if (RedIs(Red::Broadcast)) {
        UE_LOGW("dingus_sync: dingus_red=broadcast -- HOST's word of the dingus '%ls' is not sent", key.c_str());
        return false;
    }
    return s->SendReliable(N::ReliableKind::DingusFound, &p, sizeof(p));
}

bool SayToSlot(int slot, const std::wstring& key) {
    N::Session* const s = SessionPtr();
    N::DingusFoundPayload p{};
    if (!s || !ToWire(key, p.key)) return false;
    if (RedIs(Red::Broadcast)) {
        UE_LOGW("dingus_sync: dingus_red=broadcast -- HOST's word of the dingus '%ls' to slot %d is not sent", key.c_str(), slot);
        return false;
    }
    return s->SendReliableToSlot(slot, N::ReliableKind::DingusFound, &p, sizeof(p));
}

void MarkCopy(void* actor, const std::wstring& key, const char* why) {
    if (D::MarkFound(actor)) {
        ++g_n.marked;
        UE_LOGI("dingus_sync: a copy of the found dingus '%ls' marked found (%s)", key.c_str(), why);
        return;
    }
    if (PowerOfTwo(++g_n.markFail))
        UE_LOGW("dingus_sync: a copy of the found dingus '%ls' could not be marked found (%s) (#%u)", key.c_str(), why, g_n.markFail);
}

void AcceptFound(int slot, const std::wstring& key, void* actor) {
    const Add added = AddKey(key);
    MarkCopy(actor, key, "the host's own copy");
    if (added != Add::Added) return;
    UE_LOGI("dingus_sync: HOST slot %d found the dingus '%ls' -- every player's copy goes quiet", slot, key.c_str());
    Broadcast(key);
}

}  // namespace detail

bool WireKeyWellFormed(const N::WireKey& key) {
    if (key.len == 0 || key.len > sizeof(key.data)) return false;
    for (size_t i = 0; i < key.len; ++i) {
        const unsigned char c = static_cast<unsigned char>(key.data[i]);
        if (c < 0x21 || c > 0x7E) return false;
    }
    return true;
}

void Install(N::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!g_installed) {
        g_installed = true;
        if (D::RefreshClasses()) g_catchUp = true;  // the table is asked here once, ahead of the subscription; Tick asks it each frame
        g_taken = sg::WatchName(kRowTouch, kTagTouch, nullptr, &OnTouchPost);
        g_watched = true;
        if (!g_taken) UE_LOGE("dingus_sync: the watch on prop_dingus_C's touch was refused");
        if (!ue_wrap::world_identity::AddWorldChangeListener(&OnWorldChange))
            UE_LOGE("dingus_sync: the world-change list is full -- the found set is dropped at the session's end only");
    }
    if (!g_subscribed && session && session->role() == N::Role::Host) {
        g_subscribed = coop::prop_element_tracker::SubscribeHostPropExpressed(&OnExpressed);
        if (!g_subscribed && !g_subscribeSaid) {
            g_subscribeSaid = true;
            UE_LOGE("dingus_sync: the prop lane's subscriber table is full -- the host's dingi are not read at their birth");
        }
    }
}

void Tick() {
    if (D::RefreshClasses()) g_catchUp = true;
    SettleWatch();
    const bool catchUp = g_catchUp && (detail::IsHost() || detail::IsClient());
    if (catchUp) g_catchUp = false;
    if (!catchUp && g_bornPending.load(std::memory_order_acquire) == 0 && !g_bornOverflow.load(std::memory_order_acquire)) return;
    if (detail::IsHost()) DrainBorn(catchUp);
    else if (catchUp) MarkLiveCopies();
}

void OnBound(void* actor, const std::wstring& key) {
    if (!D::IsDingus(actor)) return;
    if (detail::Has(key) && D::ReadFound(actor) != 1 && !RedIs(Red::BindMark))
        detail::MarkCopy(actor, key, "bound under a found key");
    if (g_observer && g_observer->bound) g_observer->bound(actor, key);
}

void OnFound(const N::DingusFoundPayload& p) {
    if (!detail::IsClient()) return;
    const std::wstring key = N::StringFromWireKey(p.key);
    detail::AddKey(key);
    void* const actor = coop::prop_key_index::Find(key);
    if (!D::IsDingus(actor) || D::ReadFound(actor) == 1) return;
    detail::MarkCopy(actor, key, "the host's word");
}

void OnReport(int senderSlot, const N::DingusReportPayload& p) {
    if (!detail::IsHost()) return;
    detail::JudgeReport(senderSlot, N::StringFromWireKey(p.key));
}

void CountDropped(int senderSlot, size_t payloadLen) {
    if (PowerOfTwo(++g_n.dropped))
        UE_LOGW("dingus_sync: a message dropped (the wrong way, of %zu bytes or with a malformed key), from slot %d: %u so far", payloadLen,
                senderSlot, g_n.dropped);
}

void SendJoinFound(int slot) {
    N::Session* const s = detail::SessionPtr();
    if (!s || !detail::IsHost()) return;
    if (RedIs(Red::Snapshot)) {
        UE_LOGW("dingus_sync: dingus_red=snapshot -- HOST sends the joiner in slot %d none of its %zu keys", slot, g_found.size());
        return;
    }
    // MTA sends an element's data inside its add packet (CEntityAddPacket.cpp:178-181); here the set goes ahead of the props and each
    // copy is marked when it binds (OnBound), or when the key is heard if the copy is already alive (OnFound).
    for (const std::wstring& key : g_found) {  // every key of the host's set passed ToWire when it was added
        N::DingusFoundPayload p{};
        N::WireKeyFromString(key, p.key);
        s->SendReliableToSlot(slot, N::ReliableKind::DingusFound, &p, sizeof(p));
    }
}

namespace dev {
bool FoundHas(const std::wstring& key) { return g_found.count(key) != 0; }
size_t FoundKeyCount() { return g_found.size(); }
void SetObserver(const Observer* observer) { g_observer = observer; }
}  // namespace dev

namespace detail {
uint32_t DroppedCount() { return g_n.dropped; }
}  // namespace detail

void OnDisconnect() {
    if (g_n.full || g_n.longKey || g_n.badKey || g_n.markFail || g_n.dropped || g_n.sendFailed || g_n.marked)
        UE_LOGI("dingus_sync: session end -- copies marked %u, set full %u, keys too long %u, keys unusable %u, marks failed %u, sends failed %u, "
                "dropped %u", g_n.marked, g_n.full, g_n.longKey, g_n.badKey, g_n.markFail, g_n.sendFailed, g_n.dropped);
    g_n = Counters{};
    g_found.clear();
    g_fullSaid = false;
    g_seedSaid = false;
    g_subscribeSaid = false;
    {
        std::lock_guard<std::mutex> lk(g_bornMutex);
        g_bornCount = 0;
        g_bornPending.store(0, std::memory_order_release);
        g_bornOverflow.store(false, std::memory_order_release);
    }
    detail::ClearJudge();
}

}  // namespace coop::dingus_sync
