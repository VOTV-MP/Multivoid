// coop/player/stat_orders.cpp -- see coop/player/stat_orders.h.

#include "coop/player/stat_orders.h"

#include "coop/items/inventory_wire.h"
#include "coop/items/player_inventory_sync.h"
#include "coop/items/player_profile.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/player/player_profile_store.h"
#include "coop/session/net_pump.h"  // IsWorldSettled
#include "coop/session/player_handshake.h"

#include "ue_wrap/actors/effects.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

namespace coop::stat_orders {
namespace {

namespace V = ue_wrap::vitals;
namespace E = ue_wrap::effects;
using coop::net::ReliableKind;
using coop::net::Role;
using coop::net::Session;

static_assert(kStatRows == static_cast<int>(V::Field::Count), "the wire carries every row of the stat table");

constexpr float kStoredTolerance = 0.001f;
constexpr int kKinds = 2;   // Kind::Order, Kind::Query

PendingTable g_table;
Session* g_session = nullptr;

// One line per kind (a client) or per slot and kind (the host) for a message that is refused,
// unreadable or unmatched: a hostile or broken peer must not flood the log.
bool g_saidRefused[kKinds] = {};
bool g_saidBadBytes[kKinds] = {};
bool g_saidMalformed[coop::net::kMaxPeers][kKinds] = {};
bool g_saidUnmatched[coop::net::kMaxPeers][kKinds] = {};
uint32_t g_unmatched = 0;   // answers whose token the host did not hold

bool FirstTime(bool* flag) {
    if (*flag) return false;
    *flag = true;
    return true;
}

std::wstring Widen(const std::string& ascii) { return std::wstring(ascii.begin(), ascii.end()); }

// The wire carries 0x20-0x7E only; the game's own names are such in practice, and a name that is
// not reads '?' rather than becoming a different name.
std::string Narrow(const std::wstring& wide) {
    std::string out;
    out.reserve(wide.size());
    for (const wchar_t c : wide) out.push_back(c >= 0x20 && c <= 0x7E ? static_cast<char>(c) : '?');
    return out;
}

bool SameName(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

// The entries of `name` the list holds that are live.
int LiveCount(const std::vector<E::Entry>& list, const std::wstring& name) {
    int n = 0;
    for (const E::Entry& e : list)
        if (e.live && SameName(e.name, name)) ++n;
    return n;
}

// The live entries of `name` now; 0 when the list does not read.
float LiveCountNow(const std::wstring& name) {
    std::vector<E::Entry> list;
    return E::List(&list) ? static_cast<float>(LiveCount(list, name)) : 0.f;
}

Result ApplySet(const OrderWire& o, float* valueNow) {
    V::Field f;
    if (!V::FieldFromId(o.field, &f)) return Result::Refused;
    if (!V::Write(f, o.value)) {
        return (V::RowOf(f).write == V::WriteRule::ReadOnly || !std::isfinite(o.value)) ? Result::Refused
                                                                                       : Result::NotFound;
    }
    float now = 0.f;
    *valueNow = V::Read(f, &now) ? now : 0.f;
    return Result::Applied;
}

Result ApplyAdd(const OrderWire& o, float* valueNow) {
    if (!std::isfinite(o.strength) || !std::isfinite(o.seconds)) return Result::Refused;
    std::vector<std::wstring> names;
    if (!E::Names(&names)) return Result::NotFound;
    const std::wstring name = Widen(o.effect);
    const bool known = std::any_of(names.begin(), names.end(),
                                   [&name](const std::wstring& n) { return SameName(n, name); });
    if (!known) return Result::Refused;
    if (!E::Add(name, o.strength, o.seconds)) return Result::NotFound;
    *valueNow = LiveCountNow(name);
    return Result::Applied;
}

Result ApplyRemove(const OrderWire& o, float* valueNow) {
    const std::wstring name = Widen(o.effect);
    std::vector<E::Entry> list;
    if (!E::List(&list)) return Result::NotFound;
    const auto first = std::find_if(list.begin(), list.end(),
                                    [&name](const E::Entry& e) { return SameName(e.name, name); });
    if (first == list.end() || !first->live) return Result::Refused;
    if (!E::Remove(name)) return Result::NotFound;
    *valueNow = LiveCountNow(name);
    return Result::Applied;
}

// The apply, shared by a client's handler and the host's own player. valueNow is meaningful for
// Applied only (the caller's pack and answer drop it otherwise).
Result ApplyOrder(const OrderWire& o, float* valueNow) {
    *valueNow = 0.f;
    switch (o.op) {
        case Op::SetStat:      return ApplySet(o, valueNow);
        case Op::AddEffect:    return ApplyAdd(o, valueNow);
        case Op::RemoveEffect: return ApplyRemove(o, valueNow);
    }
    return Result::Refused;
}

// The read, shared by a client's handler and the host's own player: bit i of validMask when row i
// reads, the effect list as the game holds it.
TableView ReadTable() {
    TableView view{};
    for (int i = 0; i < kStatRows; ++i) {
        float v = 0.f;
        if (!V::Read(static_cast<V::Field>(i), &v)) continue;
        view.values[i] = v;
        view.validMask |= 1u << i;
    }
    std::vector<E::Entry> list;
    if (E::List(&list)) {
        view.effectTotal = static_cast<uint8_t>(std::min<size_t>(list.size(), 255));
        for (const E::Entry& e : list) view.effects.push_back(EffectView{Narrow(e.name), e.live, e.strength, e.time});
    }
    return view;
}

// A row the profile carries: the table's own answer, a member of the snapshot.
bool InProfile(uint8_t fieldId) {
    V::Field f;
    if (!V::FieldFromId(fieldId, &f)) return false;
    V::Snapshot scratch;
    return V::SnapshotField(scratch, f) != nullptr;
}

// The host's stored profile of `slot` holds `valueNow` in the row `fieldId`.
bool StoredMatches(uint8_t slot, uint8_t fieldId, float valueNow) {
    V::Field f;
    if (!V::FieldFromId(fieldId, &f)) return false;
    std::vector<uint8_t> blob;
    if (coop::player_profile_store::Get(coop::player_handshake::GuidForSlot(slot), blob) !=
        coop::player_profile_store::Found::Yes)
        return false;
    coop::player_profile::Profile profile;
    if (!coop::inventory_wire::Deserialize(blob, profile) || !profile.hasVitals) return false;
    const float* held = V::SnapshotField(profile.vitals, f);
    return held && std::fabs(*held - valueNow) <= kStoredTolerance;
}

void Deliver(const OrderDone& done, const OrderAnswer& answer) {
    if (done) done(answer);
}

void Deliver(const QueryDone& done, Result r, const TableView& view) {
    if (done) done(r, view);
}

void AnswerLeft(Pending& p) {
    if (p.kind == Kind::Order) Deliver(p.orderDone, OrderAnswer{Result::Left, 0.f, false, false});
    else Deliver(p.queryDone, Result::Left, TableView{});
}

// The host takes an answer only from a client slot; false (and one line per kind) otherwise.
bool AcceptFromClient(const Session& s, const Session::ReliableMessage& m, Kind kind) {
    if (s.role() == Role::Host && m.senderPeerSlot >= 1 && m.senderPeerSlot < coop::net::kMaxPeers) return true;
    if (FirstTime(&g_saidRefused[static_cast<int>(kind)]))
        UE_LOGW("[STAT-ORDER] %s answer refused: it came from slot %d to a %s",
                kind == Kind::Order ? "order" : "query", m.senderPeerSlot,
                s.role() == Role::Host ? "host" : "client");
    return false;
}

// What a client prints for an order it took: the row's name for a set, the effect's otherwise.
void LogClientApplied(const OrderWire& w, Result r, float now, bool profileSent) {
    V::Field f;
    const char* what = w.effect.c_str();
    if (w.op == Op::SetStat) what = V::FieldFromId(w.field, &f) ? V::RowOf(f).name : "?";
    UE_LOGI("[STAT-ORDER] client applied %s %s -> %s now=%.3f profile=%s", OpText(w.op), what, ResultText(r), now,
            profileSent ? "sent" : "not sent");
}

}  // namespace

Sent SendOrder(uint8_t slot, const Order& order, OrderDone done) {
    UE_ASSERT_GAME_THREAD("stat_orders::SendOrder");
    Session* s = g_session;
    if (!s || !s->running() || s->role() != Role::Host) return Sent::NotHost;
    if (order.op != Op::SetStat && !ValidEffectName(order.effect)) return Sent::BadName;
    OrderWire w{0, order.op, static_cast<uint8_t>(order.field), order.value, order.strength, order.seconds,
                order.effect};
    if (slot == 0) {
        float now = 0.f;
        const Result r = ApplyOrder(w, &now);
        Deliver(done, OrderAnswer{r, now, false, false});   // the host's own save holds its numbers: not storable
        return Sent::Ok;
    }
    if (slot >= coop::net::kMaxPeers || !s->IsSlotConnected(slot) || !s->IsSlotWorldReady(slot))
        return Sent::NotReady;
    const uint32_t generation = s->peerGenerationForSlot(slot);
    if (generation == 0) return Sent::NotReady;
    if (coop::player_handshake::GuidForSlot(slot).empty()) return Sent::NoGuid;
    Pending entry{};
    entry.slot = slot;
    entry.generation = generation;
    entry.kind = Kind::Order;
    entry.op = order.op;
    entry.field = w.field;
    entry.sentMs = coop::net::NowMs();
    entry.orderDone = std::move(done);
    uint32_t token = 0;
    if (!g_table.Add(std::move(entry), &token)) return Sent::Busy;
    w.token = token;
    coop::net::StatOrderPayload p{};
    Pending back;
    PackOrder(w, &p);   // cannot fail: an effect op's name passed ValidEffectName above
    // MTA stamps a synced set with a sync-time context; the host writes no copy at send time, so nothing is stamped.
    if (!s->SendReliableToSlot(slot, ReliableKind::StatOrder, &p, sizeof p)) {
        g_table.Take(token, slot, generation, Kind::Order, &back);
        return Sent::SendFailed;
    }
    return Sent::Ok;
}

Sent SendQuery(uint8_t slot, QueryDone done) {
    UE_ASSERT_GAME_THREAD("stat_orders::SendQuery");
    Session* s = g_session;
    if (!s || !s->running() || s->role() != Role::Host) return Sent::NotHost;
    if (slot == 0) {
        Deliver(done, Result::Applied, ReadTable());
        return Sent::Ok;
    }
    if (slot >= coop::net::kMaxPeers || !s->IsSlotConnected(slot) || !s->IsSlotWorldReady(slot))
        return Sent::NotReady;
    const uint32_t generation = s->peerGenerationForSlot(slot);
    if (generation == 0) return Sent::NotReady;
    if (coop::player_handshake::GuidForSlot(slot).empty()) return Sent::NoGuid;
    Pending entry{};
    entry.slot = slot;
    entry.generation = generation;
    entry.kind = Kind::Query;
    entry.sentMs = coop::net::NowMs();
    entry.queryDone = std::move(done);
    uint32_t token = 0;
    if (!g_table.Add(std::move(entry), &token)) return Sent::Busy;
    const coop::net::StatQueryPayload p = PackQuery(token);
    if (!s->SendReliableToSlot(slot, ReliableKind::StatQuery, &p, sizeof p)) {
        Pending back;
        g_table.Take(token, slot, generation, Kind::Query, &back);
        return Sent::SendFailed;
    }
    return Sent::Ok;
}

int PendingCount(uint8_t slot) { return g_table.Count(slot); }

void Install(coop::net::Session* session) { g_session = session; }

void OnSlotDisconnected(uint8_t slot) {
    if (slot < coop::net::kMaxPeers)
        for (int k = 0; k < kKinds; ++k) g_saidMalformed[slot][k] = g_saidUnmatched[slot][k] = false;
    std::vector<Pending> held = g_table.TakeSlot(slot);
    for (Pending& p : held) AnswerLeft(p);
}

void OnDisconnect() {
    std::vector<Pending> held = g_table.TakeAll();
    for (int k = 0; k < kKinds; ++k) {
        g_saidRefused[k] = g_saidBadBytes[k] = false;
        for (int slot = 0; slot < coop::net::kMaxPeers; ++slot) g_saidMalformed[slot][k] = g_saidUnmatched[slot][k] = false;
    }
    g_unmatched = 0;
    for (Pending& p : held) AnswerLeft(p);
}

void HandleOrder(Session& s, const Session::ReliableMessage& m) {
    UE_ASSERT_GAME_THREAD("stat_orders::HandleOrder");
    // From the host alone: the host's receive edge refuses a client's copy (client_authorship.h).
    OrderWire w;
    if (!UnpackOrder(m.payload, m.payloadLen, &w)) {
        if (FirstTime(&g_saidBadBytes[static_cast<int>(Kind::Order)]))
            UE_LOGW("[STAT-ORDER] an order of %u bytes is not a StatOrder -- dropped", static_cast<unsigned>(m.payloadLen));
        return;
    }
    Result r = Result::NotReady;
    float now = 0.f;
    bool profileSent = false;
    if (coop::net_pump::IsWorldSettled()) {
        r = ApplyOrder(w, &now);
        // The profile goes before the answer, on the lane the answer rides, so the host has stored it
        // when it handles the answer.
        if (r == Result::Applied && w.op == Op::SetStat && InProfile(w.field))
            profileSent = coop::player_inventory_sync::SendProfileNow(&s);
    }
    const coop::net::StatOrderReplyPayload p = PackOrderReply(w.token, r, w.op, now);
    LogClientApplied(w, r, r == Result::Applied ? now : 0.f, profileSent);
    if (!s.SendReliable(ReliableKind::StatOrderReply, &p, sizeof p))
        UE_LOGW("[STAT-ORDER] the answer to order %u was not sent", w.token);
}

void HandleQuery(Session& s, const Session::ReliableMessage& m) {
    UE_ASSERT_GAME_THREAD("stat_orders::HandleQuery");
    // From the host alone: the host's receive edge refuses a client's copy (client_authorship.h).
    uint32_t token = 0;
    if (!UnpackQuery(m.payload, m.payloadLen, &token)) {
        if (FirstTime(&g_saidBadBytes[static_cast<int>(Kind::Query)]))
            UE_LOGW("[STAT-ORDER] a query of %u bytes is not a StatQuery -- dropped", static_cast<unsigned>(m.payloadLen));
        return;
    }
    const bool ready = coop::net_pump::IsWorldSettled();
    const coop::net::StatQueryReplyPayload p =
        PackQueryReply(token, ready ? Result::Applied : Result::NotReady, ready ? ReadTable() : TableView{});
    if (!s.SendReliable(ReliableKind::StatQueryReply, &p, sizeof p))
        UE_LOGW("[STAT-ORDER] the answer to query %u was not sent", token);
}

void HandleOrderReply(Session& s, const Session::ReliableMessage& m) {
    UE_ASSERT_GAME_THREAD("stat_orders::HandleOrderReply");
    if (!AcceptFromClient(s, m, Kind::Order)) return;
    const uint8_t slot = static_cast<uint8_t>(m.senderPeerSlot);
    const uint32_t generation = s.peerGenerationForSlot(slot);
    const int k = static_cast<int>(Kind::Order);
    uint32_t token = 0;
    Result r = Result::Malformed;
    Op op = Op::SetStat;
    float now = 0.f;
    Pending pending;
    if (!UnpackOrderReply(m.payload, m.payloadLen, &token, &r, &op, &now)) {
        // Answers arrive in the order their orders went (one ordered lane), so a reply too broken to
        // carry a token answers the oldest order of that slot; nothing stays wedged.
        if (FirstTime(&g_saidMalformed[slot][k]))
            UE_LOGW("[STAT-ORDER] slot %u sent an order answer of %u bytes that fails its checks", slot,
                    static_cast<unsigned>(m.payloadLen));
        if (g_table.TakeOldest(slot, generation, Kind::Order, &pending))
            Deliver(pending.orderDone, OrderAnswer{Result::Malformed, 0.f, false, false});
        return;
    }
    if (!g_table.Take(token, slot, generation, Kind::Order, &pending)) {
        ++g_unmatched;
        if (FirstTime(&g_saidUnmatched[slot][k]))
            UE_LOGW("[STAT-ORDER] slot %u answered token %u, which no order of its holds (%u unmatched so far)", slot,
                    token, g_unmatched);
        return;
    }
    OrderAnswer answer{r, now, false, false};
    if (op != pending.op) {
        answer = OrderAnswer{Result::Malformed, 0.f, false, false};
    } else if (r == Result::Applied && pending.op == Op::SetStat && InProfile(pending.field)) {
        answer.storable = true;
        answer.stored = StoredMatches(slot, pending.field, now);
    }
    UE_LOGI("[STAT-ORDER] slot %u %s -> %s now=%.3f stored=%d", slot, OpText(pending.op), ResultText(answer.result),
            answer.valueNow, answer.stored ? 1 : 0);
    Deliver(pending.orderDone, answer);
}

void HandleQueryReply(Session& s, const Session::ReliableMessage& m) {
    UE_ASSERT_GAME_THREAD("stat_orders::HandleQueryReply");
    if (!AcceptFromClient(s, m, Kind::Query)) return;
    const uint8_t slot = static_cast<uint8_t>(m.senderPeerSlot);
    const uint32_t generation = s.peerGenerationForSlot(slot);
    const int k = static_cast<int>(Kind::Query);
    uint32_t token = 0;
    Result r = Result::Malformed;
    TableView view{};
    Pending pending;
    if (!UnpackQueryReply(m.payload, m.payloadLen, &token, &r, &view)) {
        if (FirstTime(&g_saidMalformed[slot][k]))
            UE_LOGW("[STAT-ORDER] slot %u sent a query answer of %u bytes that fails its checks", slot,
                    static_cast<unsigned>(m.payloadLen));
        if (g_table.TakeOldest(slot, generation, Kind::Query, &pending))
            Deliver(pending.queryDone, Result::Malformed, TableView{});
        return;
    }
    if (!g_table.Take(token, slot, generation, Kind::Query, &pending)) {
        ++g_unmatched;
        if (FirstTime(&g_saidUnmatched[slot][k]))
            UE_LOGW("[STAT-ORDER] slot %u answered token %u, which no query of its holds (%u unmatched so far)", slot,
                    token, g_unmatched);
        return;
    }
    // A query is polled at 2 Hz by a pane: only a failure is worth a line.
    if (r != Result::Applied) UE_LOGI("[STAT-ORDER] slot %u query -> %s", slot, ResultText(r));
    Deliver(pending.queryDone, r, view);
}

}  // namespace coop::stat_orders
