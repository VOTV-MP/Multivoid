// coop/dev/meadow_selftest.cpp -- see coop/dev/meadow_selftest.h.

#include "coop/dev/meadow_selftest.h"

#include "coop/config/config.h"
#include "coop/interactables/meadow_db_hash.h"
#include "coop/interactables/meadow_db_sync.h"  // IsPrimed, SentLines, SetApplyObserver
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady

#include "ue_wrap/core/log.h"
#include "ue_wrap/desk/meadow_store.h"
#include "ue_wrap/desk/signal_dynamic.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <vector>

namespace coop::dev::meadow_selftest {
namespace {

namespace MH  = coop::meadow_db_hash;
namespace MS  = ue_wrap::meadow_store;
namespace SD  = ue_wrap::signal_dynamic;
namespace MDB = coop::meadow_db_sync;
using Clock = std::chrono::steady_clock;

// A step that does not reach its milestone within this ends its leg, said.
constexpr auto kStepBound = std::chrono::seconds(60);

// The drill's rows. The rename keeps a row's id and changes its name, so A and A2 are one row of the
// database under two contents.
enum RowId : int { kA, kB, kA2, kC, kRowCount };
const wchar_t* const kName[kRowCount] = {L"MEADOW-SELFTEST-A", L"MEADOW-SELFTEST-B", L"MEADOW-SELFTEST-A2",
                                         L"MEADOW-SELFTEST-C"};
const wchar_t* const kId[kRowCount] = {L"selftest-a", L"selftest-b", L"selftest-a", L"selftest-c"};

// What each peer's lane must have sent by the end: the host's two adds, the rename as a delete and an
// append, the order lines of the rename and the move, the two removals; the client's row and its removal.
constexpr uint64_t kHostAppends = 3, kHostDeletes = 3, kHostOrders = 2;
constexpr uint64_t kClientAppends = 1, kClientDeletes = 1, kClientOrders = 0;

enum class HostStep : uint8_t {
    Ready, Add, AddSent, Rename, RenameSent, Move, MoveSent, Remove, RemoveSent, ClientRow, Cleanup, Done
};
enum class ClientStep : uint8_t { Ready, Watch, Add, AddSent, Remove, RemoveSent, Cleanup, Done };

HostStep   g_host   = HostStep::Ready;
ClientStep g_client = ClientStep::Ready;
bool       g_isHost = false;
Clock::time_point g_stepAt{};
MDB::SentCounts g_sent0{};      // the lane's sent lines as this peer's leg armed
uint64_t   g_ordersBefore = 0;  // host: the lane's order lines before the move
int        g_readyTicks = 0;    // host: ticks since a client's world was first seen ready
int        g_session = 1;       // this process's sessions, counted by their ends
std::vector<uint64_t> g_expect[4];  // client: the host's four states, in turn
int        g_reached = 0;           // client: how many of them the database has passed through
bool       g_clientRowIn = false, g_clientRowOut = false;  // host: the client's row, seen

bool Enabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::meadow_selftest);
    return s;
}

// A drill row, built alike on both peers so its content hash is the same on each.
SD::Row MakeRow(RowId r) {
    SD::Row row;
    row.name = kName[r];
    row.id = kId[r];
    row.object.clear();  // empty is NAME_None; the literal "None" trips WriteFNameField's failed-intern check
    row.signal.clear();
    row.level = 1;
    row.size = 1.0f;
    row.decoded = 1.0f;
    row.hasData = true;
    return row;
}

uint64_t HashOf(RowId r) {
    static uint64_t h[kRowCount] = {};
    std::vector<uint8_t> scratch;
    if (!h[r]) h[r] = MH::HashRow(MakeRow(r), scratch);
    return h[r];
}

// The database as its rows' content hashes, in its order. False while it cannot be read.
bool ReadSequence(std::vector<uint64_t>& out) {
    std::map<uint64_t, int32_t> counts;
    return MH::HashStore(counts, &out);
}

int32_t IndexOf(const std::vector<uint64_t>& seq, uint64_t h) {
    for (size_t i = 0; i < seq.size(); ++i)
        if (seq[i] == h) return static_cast<int32_t>(i);
    return -1;
}

int32_t IndexOf(RowId r) {
    return MH::IndexOf(HashOf(r));
}

// The lane's own digest: the rows' hashes summed, whatever their order.
uint64_t Digest(const std::vector<uint64_t>& seq) {
    uint64_t d = 0;
    for (uint64_t h : seq) d += h;
    return d;
}

// Takes a drill row out if it is there; true once it is not.
bool TakeOut(RowId r) {
    std::vector<uint64_t> seq;
    if (!ReadSequence(seq)) return false;
    const int32_t idx = IndexOf(seq, HashOf(r));
    return idx < 0 || (MS::ApplyRemoveSignal(idx) && IndexOf(r) < 0);
}

void Enter(HostStep h) { g_host = h; g_stepAt = Clock::now(); }
void Enter(ClientStep c) { g_client = c; g_stepAt = Clock::now(); }
bool StepExpired() { return Clock::now() - g_stepAt >= kStepBound; }

// After every line this peer's lane applies: the client checks the host's states in turn, the host
// watches for the client's row.
void OnApplied() {
    std::vector<uint64_t> seq;
    if (!ReadSequence(seq)) return;
    if (!g_isHost && g_client == ClientStep::Watch && g_reached < 4 && seq == g_expect[g_reached]) {
        ++g_reached;
        UE_LOGI("[meadow_selftest] client: the database reached the host's state %d of 4 (%zu rows)", g_reached,
                seq.size());
        g_stepAt = Clock::now();
    }
    if (g_isHost && !g_clientRowOut) {
        const bool in = IndexOf(seq, HashOf(kC)) >= 0;
        if (in && !g_clientRowIn) {
            g_clientRowIn = true;
            UE_LOGI("[meadow_selftest] host: the client's row arrived");
        } else if (!in && g_clientRowIn) {
            g_clientRowOut = true;
            UE_LOGI("[meadow_selftest] host: the client's row left");
        }
    }
}

MDB::SentCounts SentSinceArm() {
    const MDB::SentCounts s = MDB::SentLines();
    return {s.appends - g_sent0.appends, s.deletes - g_sent0.deletes, s.orders - g_sent0.orders};
}

void HostTick(coop::net::Session& s) {
    switch (g_host) {
    case HostStep::Ready: {
        bool anyReady = false;
        for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers) && !anyReady; ++slot)
            anyReady = s.IsSlotWorldReady(slot);
        // A row added before the lane holds this world's database would be taken into the lane's first
        // picture of it, and never sent.
        if (!anyReady || !MDB::IsPrimed()) return;
        if (++g_readyTicks < 2) return;  // a tick after, so the client's world-ready replay went first
        std::vector<uint64_t> seq;
        if (!MS::EnsureResolved() || !ReadSequence(seq)) return;
        for (RowId r : {kA, kB, kA2, kC}) {
            if (IndexOf(seq, HashOf(r)) >= 0) {
                UE_LOGW("[meadow_selftest] ABANDONED in session %d: the database already holds a drill row (%ls), "
                        "a previous run's -- taking the drill's rows out; run again", g_session, kName[r]);
                Enter(HostStep::Cleanup);
                return;
            }
        }
        g_sent0 = MDB::SentLines();
        MDB::SetApplyObserver(&OnApplied);
        UE_LOGI("[meadow_selftest] host ARMED in session %d: a client's world is ready; the database holds %zu "
                "row(s), digest %016llx", g_session, seq.size(), static_cast<unsigned long long>(Digest(seq)));
        Enter(HostStep::Add);
        return;
    }
    case HostStep::Add:
        // The laptop widget's device pointer can come late in a world; the adds wait for it.
        if (!MS::EnsureResolved() || !MS::Widget()) {
            if (StepExpired()) {
                UE_LOGW("[meadow_selftest] ABANDONED in session %d: the laptop widget did not come within 60 s",
                        g_session);
                Enter(HostStep::Done);
            }
            return;
        }
        if (!MS::ApplyAddSignal(MakeRow(kA)) || !MS::ApplyAddSignal(MakeRow(kB))) {
            UE_LOGW("[meadow_selftest] ABANDONED in session %d: the laptop refused addSignal", g_session);
            Enter(HostStep::Cleanup);
            return;
        }
        UE_LOGI("[meadow_selftest] host added rows A and B");
        Enter(HostStep::AddSent);
        return;
    case HostStep::AddSent:
        if (SentSinceArm().appends >= 2) {
            UE_LOGI("[meadow_selftest] host: the lane sent both rows");
            Enter(HostStep::Rename);
        } else if (StepExpired()) {
            UE_LOGW("[meadow_selftest] FAIL in session %d: the lane did not send the host's two rows within 60 s",
                    g_session);
            Enter(HostStep::Cleanup);
        }
        return;
    case HostStep::Rename: {
        const int32_t idx = IndexOf(kA);
        if (idx < 0 || !MS::RenameRow(coop::players::Registry::Get().Local(), idx, kName[kA2])) {
            UE_LOGW("[meadow_selftest] ABANDONED in session %d: the rename window could not be driven on row A "
                    "(row %d)", g_session, idx);
            Enter(HostStep::Cleanup);
            return;
        }
        if (IndexOf(kA2) != idx) {
            SD::Row r;
            const bool read = MS::ReadRow(idx, r);
            UE_LOGW("[meadow_selftest] ABANDONED in session %d: after the rename, row %d reads '%ls', not the drill's "
                    "row A2", g_session, idx, read ? r.name.c_str() : L"(unreadable)");
            Enter(HostStep::Cleanup);
            return;
        }
        UE_LOGI("[meadow_selftest] host renamed row A to A2 through the rename window (row %d)", idx);
        Enter(HostStep::RenameSent);
        return;
    }
    case HostStep::RenameSent: {
        // A rename is a new content: the lane sends the old one's delete and the new one's append.
        const MDB::SentCounts d = SentSinceArm();
        if (d.appends >= 3 && d.deletes >= 1) {
            UE_LOGI("[meadow_selftest] host: the lane sent the rename");
            Enter(HostStep::Move);
        } else if (StepExpired()) {
            UE_LOGW("[meadow_selftest] FAIL in session %d: the lane did not send the rename within 60 s", g_session);
            Enter(HostStep::Cleanup);
        }
        return;
    }
    case HostStep::Move: {
        const int32_t a2 = IndexOf(kA2), b = IndexOf(kB);
        g_ordersBefore = MDB::SentLines().orders;
        if (a2 < 0 || b != a2 + 1 || !MS::MoveRow(b, -1) || IndexOf(kB) != a2 || IndexOf(kA2) != a2 + 1) {
            UE_LOGW("[meadow_selftest] ABANDONED in session %d: sortSignal did not move row B above A2 (rows %d, %d)",
                    g_session, a2, b);
            Enter(HostStep::Cleanup);
            return;
        }
        UE_LOGI("[meadow_selftest] host moved row B above A2 through the list's arrows");
        Enter(HostStep::MoveSent);
        return;
    }
    case HostStep::MoveSent:
        if (MDB::SentLines().orders > g_ordersBefore) {
            UE_LOGI("[meadow_selftest] host: the lane sent the move");
            Enter(HostStep::Remove);
        } else if (StepExpired()) {
            UE_LOGW("[meadow_selftest] FAIL in session %d: the lane did not send the move within 60 s", g_session);
            Enter(HostStep::Cleanup);
        }
        return;
    case HostStep::Remove:
        if (TakeOut(kB) && TakeOut(kA2)) {
            UE_LOGI("[meadow_selftest] host removed rows B and A2");
            Enter(HostStep::RemoveSent);
        } else if (StepExpired()) {
            UE_LOGW("[meadow_selftest] FAIL in session %d: removeSignal did not take the host's rows out within "
                    "60 s -- they may be saved into this game's database", g_session);
            Enter(HostStep::Done);
        }
        return;
    case HostStep::RemoveSent:
        if (SentSinceArm().deletes >= 3) {
            UE_LOGI("[meadow_selftest] host: the lane sent the removals; waiting for the client's row");
            Enter(HostStep::ClientRow);
        } else if (StepExpired()) {
            UE_LOGW("[meadow_selftest] FAIL in session %d: the lane did not send the removals within 60 s",
                    g_session);
            Enter(HostStep::Done);
        }
        return;
    case HostStep::ClientRow: {
        if (!g_clientRowOut) {
            if (StepExpired()) {
                UE_LOGW("[meadow_selftest] FAIL in session %d: the client's row did not %s within 60 s", g_session,
                        g_clientRowIn ? "leave" : "arrive");
                Enter(HostStep::Done);
            }
            return;
        }
        const MDB::SentCounts d = SentSinceArm();
        if (d.appends == kHostAppends && d.deletes == kHostDeletes && d.orders == kHostOrders)
            UE_LOGI("[meadow_selftest] host DONE in session %d: the lane sent its verbs' lines and nothing else "
                    "(%llu appends, %llu deletes, %llu order lines), and the client's row arrived and left -- PASS",
                    g_session, static_cast<unsigned long long>(d.appends),
                    static_cast<unsigned long long>(d.deletes), static_cast<unsigned long long>(d.orders));
        else
            UE_LOGW("[meadow_selftest] FAIL in session %d: the host's lane sent %llu appends, %llu deletes and %llu "
                    "order lines, where its verbs make %llu, %llu and %llu", g_session,
                    static_cast<unsigned long long>(d.appends), static_cast<unsigned long long>(d.deletes),
                    static_cast<unsigned long long>(d.orders), static_cast<unsigned long long>(kHostAppends),
                    static_cast<unsigned long long>(kHostDeletes), static_cast<unsigned long long>(kHostOrders));
        Enter(HostStep::Done);
        return;
    }
    case HostStep::Cleanup: {
        // A leg that ended early takes the drill's rows back out of the save's database, retried.
        const bool out = TakeOut(kA) && TakeOut(kB) && TakeOut(kA2) && TakeOut(kC);
        if (out) {
            UE_LOGI("[meadow_selftest] host took the drill's rows out of the database");
            Enter(HostStep::Done);
        } else if (StepExpired()) {
            UE_LOGW("[meadow_selftest] host could not take the drill's rows out within 60 s -- they may be saved "
                    "into this game's database");
            Enter(HostStep::Done);
        }
        return;
    }
    case HostStep::Done:
        return;
    }
}

void ClientTick() {
    switch (g_client) {
    case ClientStep::Ready: {
        std::vector<uint64_t> seq;
        if (!coop::net_pump::HasAnnouncedWorldReady() || !MS::EnsureResolved() || !ReadSequence(seq)) return;
        for (RowId r : {kA, kB, kA2, kC}) {
            if (IndexOf(seq, HashOf(r)) >= 0) {
                UE_LOGW("[meadow_selftest] ABANDONED in session %d: the database already holds a drill row (%ls), a "
                        "previous run's; the host takes it out", g_session, kName[r]);
                Enter(ClientStep::Done);
                return;
            }
        }
        g_expect[0] = seq;
        g_expect[0].push_back(HashOf(kA));
        g_expect[0].push_back(HashOf(kB));
        g_expect[1] = seq;
        g_expect[1].push_back(HashOf(kA2));
        g_expect[1].push_back(HashOf(kB));
        g_expect[2] = seq;
        g_expect[2].push_back(HashOf(kB));
        g_expect[2].push_back(HashOf(kA2));
        g_expect[3] = seq;
        g_reached = 0;
        g_sent0 = MDB::SentLines();
        MDB::SetApplyObserver(&OnApplied);
        UE_LOGI("[meadow_selftest] client ARMED in session %d: the database holds %zu row(s), digest %016llx",
                g_session, seq.size(), static_cast<unsigned long long>(Digest(seq)));
        Enter(ClientStep::Watch);
        return;
    }
    case ClientStep::Watch:
        // The observer moves through the host's states; here only the bound, counted from the last one.
        if (g_reached >= 4) {
            UE_LOGI("[meadow_selftest] client: the host's rows came, were renamed and moved in the host's order, "
                    "and left");
            Enter(ClientStep::Add);
        } else if (StepExpired()) {
            std::vector<uint64_t> seq;
            ReadSequence(seq);
            UE_LOGW("[meadow_selftest] FAIL in session %d: the database stopped short of the host's state %d of 4 "
                    "within 60 s; it holds %zu row(s), digest %016llx", g_session, g_reached + 1, seq.size(),
                    static_cast<unsigned long long>(Digest(seq)));
            Enter(ClientStep::Done);
        }
        return;
    case ClientStep::Add:
        if (!MS::EnsureResolved() || !MS::Widget()) {
            if (StepExpired()) {
                UE_LOGW("[meadow_selftest] ABANDONED in session %d: the laptop widget did not come within 60 s",
                        g_session);
                Enter(ClientStep::Done);
            }
            return;
        }
        if (!MS::ApplyAddSignal(MakeRow(kC))) {
            UE_LOGW("[meadow_selftest] ABANDONED in session %d: the laptop refused addSignal", g_session);
            Enter(ClientStep::Cleanup);
            return;
        }
        UE_LOGI("[meadow_selftest] client added row C");
        Enter(ClientStep::AddSent);
        return;
    case ClientStep::AddSent:
        if (SentSinceArm().appends >= 1) {
            Enter(ClientStep::Remove);
        } else if (StepExpired()) {
            UE_LOGW("[meadow_selftest] FAIL in session %d: the lane did not send the client's row within 60 s",
                    g_session);
            Enter(ClientStep::Cleanup);
        }
        return;
    case ClientStep::Remove:
        if (TakeOut(kC)) {
            UE_LOGI("[meadow_selftest] client removed row C");
            Enter(ClientStep::RemoveSent);
        } else if (StepExpired()) {
            UE_LOGW("[meadow_selftest] FAIL in session %d: removeSignal did not take the client's row out within 60 s",
                    g_session);
            Enter(ClientStep::Done);
        }
        return;
    case ClientStep::RemoveSent: {
        const MDB::SentCounts d = SentSinceArm();
        if (d.deletes < 1) {
            if (StepExpired()) {
                UE_LOGW("[meadow_selftest] FAIL in session %d: the lane did not send the client's removal within 60 s",
                        g_session);
                Enter(ClientStep::Done);
            }
            return;
        }
        if (d.appends == kClientAppends && d.deletes == kClientDeletes && d.orders == kClientOrders)
            UE_LOGI("[meadow_selftest] client DONE in session %d: the host's rows came, were renamed and moved in "
                    "the host's order and left, and this client sent its own row and its removal and nothing else "
                    "-- PASS", g_session);
        else
            UE_LOGW("[meadow_selftest] FAIL in session %d: the client's lane sent %llu appends, %llu deletes and %llu "
                    "order lines, where its verbs make %llu, %llu and %llu -- a line it applied went back out",
                    g_session, static_cast<unsigned long long>(d.appends),
                    static_cast<unsigned long long>(d.deletes), static_cast<unsigned long long>(d.orders),
                    static_cast<unsigned long long>(kClientAppends), static_cast<unsigned long long>(kClientDeletes),
                    static_cast<unsigned long long>(kClientOrders));
        Enter(ClientStep::Done);
        return;
    }
    case ClientStep::Cleanup:
        if (TakeOut(kC)) {
            Enter(ClientStep::Done);
        } else if (StepExpired()) {
            UE_LOGW("[meadow_selftest] client could not take its row out within 60 s");
            Enter(ClientStep::Done);
        }
        return;
    case ClientStep::Done:
        return;
    }
}

}  // namespace

void Tick(coop::net::Session* s) {
    if (!Enabled() || !s || !s->connected()) return;
    g_isHost = s->role() == coop::net::Role::Host;
    if (g_isHost) HostTick(*s);
    else ClientTick();
}

void OnDisconnect() {
    MDB::SetApplyObserver(nullptr);
    g_host = HostStep::Ready;
    g_client = ClientStep::Ready;
    g_readyTicks = 0;
    g_reached = 0;
    g_clientRowIn = g_clientRowOut = false;
    for (auto& e : g_expect) e.clear();
    ++g_session;
}

}  // namespace coop::dev::meadow_selftest
