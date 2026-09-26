// coop/dev/meadow_selftest.cpp -- see coop/dev/meadow_selftest.h.

#include "coop/dev/meadow_selftest.h"

#include "coop/config/config.h"
#include "coop/interactables/meadow_db_sync.h"  // IsPrimed, SentLines
#include "coop/interactables/signal_wire.h"
#include "coop/net/session.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady

#include "ue_wrap/core/log.h"
#include "ue_wrap/desk/meadow_store.h"
#include "ue_wrap/desk/signal_dynamic.h"

#include <chrono>
#include <cstdint>

namespace coop::dev::meadow_selftest {
namespace {

namespace MS  = ue_wrap::meadow_store;
namespace SD  = ue_wrap::signal_dynamic;
namespace MDB = coop::meadow_db_sync;
using Clock = std::chrono::steady_clock;

// A step that does not reach its milestone within this ends its leg, said.
constexpr auto kStepBound = std::chrono::seconds(60);

enum class HostStep : uint8_t { Ready, Add, AddSent, Remove, RemoveSent, Done };
enum class ClientStep : uint8_t { Ready, Arrive, Leave, Done };

HostStep   g_host   = HostStep::Ready;
ClientStep g_client = ClientStep::Ready;
Clock::time_point g_stepAt{};
uint64_t g_sentBefore  = 0;      // host: the lane's sent lines of the step's kind as its verb ran
bool     g_hostFailed  = false;  // host: the add went unsent; the removal still runs, its DONE line does not
int      g_readyTicks  = 0;      // host: ticks since a client's world was first seen ready
int32_t  g_countBefore = -1;     // client: the database's rows before the host's row
int32_t  g_countSeen   = -1;     // client: the count its last look saw
int      g_session     = 1;      // this process's sessions, counted by their ends

bool Enabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::meadow_selftest);
    return s;
}

// The row both peers build alike, so its content hash is the same on each.
SD::Row TestRow() {
    SD::Row row;
    row.name = L"MEADOW-SELFTEST";
    row.id = L"selftest-0";
    row.object.clear();  // empty is NAME_None; the literal "None" trips WriteFNameField's failed-intern check
    row.signal.clear();
    row.level = 1;
    row.size = 1.0f;
    row.decoded = 1.0f;
    row.hasData = true;
    return row;
}

uint64_t TestHash() {
    static const uint64_t h =
        coop::signal_wire::ContentHash(coop::signal_wire::Serialize(TestRow(), /*adopt=*/false));
    return h;
}

// The database index of the test row, or -1.
int32_t FindTestRow() {
    const int32_t n = MS::Count();
    SD::Row r;
    for (int32_t i = 0; i < n; ++i) {
        if (MS::ReadRow(i, r) &&
            coop::signal_wire::ContentHash(coop::signal_wire::Serialize(r, /*adopt=*/false)) == TestHash())
            return i;
    }
    return -1;
}

void Enter(HostStep h) { g_host = h; g_stepAt = Clock::now(); }
void Enter(ClientStep c) { g_client = c; g_stepAt = Clock::now(); }
bool StepExpired() { return Clock::now() - g_stepAt >= kStepBound; }

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
        UE_LOGI("[meadow_selftest] host ARMED in session %d: a client's world is ready", g_session);
        Enter(HostStep::Add);
        return;
    }
    case HostStep::Add:
        // The laptop widget's device pointer can come late in a world; the add waits for it.
        if (MS::EnsureResolved() && MS::Widget()) {
            g_sentBefore = MDB::SentLines().appends;
            if (!MS::ApplyAddSignal(TestRow())) {
                UE_LOGW("[meadow_selftest] ABANDONED in session %d: the laptop refused addSignal", g_session);
                Enter(HostStep::Done);
                return;
            }
            UE_LOGI("[meadow_selftest] host added the row (hash %016llx)",
                    static_cast<unsigned long long>(TestHash()));
            Enter(HostStep::AddSent);
        } else if (StepExpired()) {
            UE_LOGW("[meadow_selftest] ABANDONED in session %d: the laptop widget did not come within 60 s",
                    g_session);
            Enter(HostStep::Done);
        }
        return;
    case HostStep::AddSent:
        if (MDB::SentLines().appends > g_sentBefore) {
            UE_LOGI("[meadow_selftest] host: the lane sent the row");
            Enter(HostStep::Remove);
        } else if (StepExpired()) {
            UE_LOGW("[meadow_selftest] FAIL in session %d: the lane did not send the host's row within 60 s -- "
                    "removing it", g_session);
            g_hostFailed = true;
            Enter(HostStep::Remove);
        }
        return;
    case HostStep::Remove: {
        // Retried until it lands: a row left behind would be saved into this game's database.
        const int32_t idx = FindTestRow();
        g_sentBefore = MDB::SentLines().deletes;
        if (idx >= 0 && MS::ApplyRemoveSignal(idx)) {
            UE_LOGI("[meadow_selftest] host removed the row");
            Enter(g_hostFailed ? HostStep::Done : HostStep::RemoveSent);
        } else if (idx < 0) {
            UE_LOGW("[meadow_selftest] FAIL in session %d: the host's row left its database before the removal",
                    g_session);
            Enter(HostStep::Done);
        } else if (StepExpired()) {
            UE_LOGW("[meadow_selftest] FAIL in session %d: removeSignal did not take the row (index %d) within "
                    "60 s -- it may be saved into this game's database", g_session, idx);
            Enter(HostStep::Done);
        }
        return;
    }
    case HostStep::RemoveSent:
        if (MDB::SentLines().deletes > g_sentBefore) {
            UE_LOGI("[meadow_selftest] host DONE in session %d: the lane sent the row and its removal", g_session);
            Enter(HostStep::Done);
        } else if (StepExpired()) {
            UE_LOGW("[meadow_selftest] FAIL in session %d: the lane did not send the removal within 60 s",
                    g_session);
            Enter(HostStep::Done);
        }
        return;
    case HostStep::Done:
        return;
    }
}

void ClientTick() {
    switch (g_client) {
    case ClientStep::Ready: {
        if (!coop::net_pump::HasAnnouncedWorldReady() || !MS::EnsureResolved()) return;
        const int32_t n = MS::Count();
        if (n < 0) return;
        g_countSeen = n;
        const bool here = FindTestRow() >= 0;  // it came before this leg looked
        g_countBefore = here ? n - 1 : n;
        UE_LOGI("[meadow_selftest] client ARMED in session %d: the database holds %d row(s)%s", g_session, n,
                here ? ", the host's row among them" : "");
        Enter(here ? ClientStep::Leave : ClientStep::Arrive);
        return;
    }
    case ClientStep::Arrive: {
        const int32_t n = MS::Count();
        if (n != g_countSeen) {
            g_countSeen = n;
            if (n >= 0 && FindTestRow() >= 0) {
                UE_LOGI("[meadow_selftest] client: the host's row arrived (%d -> %d rows)", g_countBefore, n);
                Enter(ClientStep::Leave);
                return;
            }
        }
        if (StepExpired()) {
            UE_LOGW("[meadow_selftest] FAIL in session %d: the host's row did not arrive within 60 s", g_session);
            Enter(ClientStep::Done);
        }
        return;
    }
    case ClientStep::Leave: {
        const int32_t n = MS::Count();
        if (n != g_countSeen) {
            g_countSeen = n;
            if (n >= 0 && FindTestRow() < 0) {
                if (n == g_countBefore)
                    UE_LOGI("[meadow_selftest] client DONE in session %d: the host's row arrived and left, and "
                            "the database is back at %d row(s) -- PASS", g_session, n);
                else
                    UE_LOGW("[meadow_selftest] FAIL in session %d: the host's row left, and the database holds %d "
                            "row(s) where it held %d", g_session, n, g_countBefore);
                Enter(ClientStep::Done);
                return;
            }
        }
        if (StepExpired()) {
            UE_LOGW("[meadow_selftest] FAIL in session %d: the host's row did not leave within 60 s", g_session);
            Enter(ClientStep::Done);
        }
        return;
    }
    case ClientStep::Done:
        return;
    }
}

}  // namespace

void Tick(coop::net::Session* s) {
    if (!Enabled() || !s || !s->connected()) return;
    if (s->role() == coop::net::Role::Host) HostTick(*s);
    else ClientTick();
}

void OnDisconnect() {
    g_host = HostStep::Ready;
    g_client = ClientStep::Ready;
    g_hostFailed = false;
    g_readyTicks = 0;
    g_countBefore = g_countSeen = -1;
    ++g_session;
}

}  // namespace coop::dev::meadow_selftest
