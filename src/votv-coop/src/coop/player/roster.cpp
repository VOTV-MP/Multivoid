// coop/roster.cpp -- see coop/roster.h.

#include "coop/player/roster.h"

#include "coop/net/session.h"
#include "coop/session/player_handshake.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster_ledger.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace coop::roster {
namespace {

std::atomic<coop::net::Session*> g_session{nullptr};

std::mutex g_mutex;
Snapshot   g_snap;            // guarded by g_mutex
std::atomic<bool> g_localIsHost{false};  // lock-free mirror of g_snap.localIsHost
unsigned long long g_lastMs = 0;  // throttle stamp (game thread only)

}  // namespace

void SetSession(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Refresh() {
    // Roster changes are rare; throttle to ~6 Hz so a 125 Hz tick doesn't churn
    // the mutex / WideCharToMultiByte every iteration.
    const unsigned long long now = ::GetTickCount64();
    if (g_lastMs != 0 && now - g_lastMs < 160) return;
    g_lastMs = now;

    Snapshot snap;  // build locally, publish under the lock at the end
    auto* s = g_session.load(std::memory_order_acquire);
    const bool running = (s != nullptr && s->running());
    snap.inSession = running;

    if (!running) {
        // Not in a session: show just YOU so the board isn't blank (you would be
        // the host once you start one).
        snap.localIsHost = true;
        snap.count = 1;
        snap.rows[0].slot = 0;
        snap.rows[0].isLocal = true;
        snap.rows[0].isHost = true;
        snap.rows[0].connected = true;
        // NAMED EXCEPTION (arc A T14): with no session there is no ledger, so
        // this one row is synthesised from the local request and carries no ID.
        // "One derivation" holds WITHIN a session.
        snap.rows[0].playerNo = kNoPlayerNo;
        // Out of session there is no link to measure and never will be, which is
        // the "n/a" case -- NOT the "--" one, which means a sample has not landed
        // yet and implies it still might.
        snap.rows[0].linkKind = coop::net::LinkKind::Local;
        coop::text::CopyUtf8ToBuffer(snap.rows[0].nick, coop::player_handshake::LocalNickname());
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_snap = snap;
        }
        // Publish the lock-free mirror AFTER the snapshot is visible: a reader that
        // observes the new role via acquire then also sees the matching snapshot.
        g_localIsHost.store(snap.localIsHost, std::memory_order_release);
        return;
    }

    const bool isHost = (s->role() == coop::net::Role::Host);
    snap.localIsHost = isHost;
    // The local peer's slot: host is always slot 0; a client uses its assigned
    // LocalPeerId. (kPeerIdUnknown during the brief pre-AssignPeerSlot window ->
    // the local row is simply omitted until the slot resolves.)
    const int localSlot = isHost ? 0 : static_cast<int>(coop::players::Registry::Get().LocalPeerId());

    int idx = 0;
    for (int slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        // ONE DERIVATION (arc A). Presence comes from the LEDGER, which both
        // peers agree on, instead of from connection state -- which a client
        // simply does not have for other clients. That is why a client's board
        // used to list only itself and the host: IsSlotConnected(2) is false on
        // a client no matter who is in slot 2.
        const coop::roster_ledger::Row& led = coop::roster_ledger::Get(slot);
        if (!led.occupied()) continue;
        const bool rowIsLocal = (slot == localSlot);
        Row& r = snap.rows[idx];
        r.slot = slot;
        r.playerNo = led.playerNo;
        r.generation = led.bornGeneration;
        r.isLocal = rowIsLocal;
        r.isHost = (slot == 0);
        r.connected = true;  // an occupied row IS the presence fact now
        // ONE DERIVATION for the connection facts too (v131). Both come from the
        // ledger, i.e. from the HOST's measurement, on BOTH roles -- so the same
        // player reads the same on every board. There is deliberately no role
        // branch here: the previous five-way cascade asked "what can I measure
        // about you", which made one column answer transport on some rows and
        // routing on others, side by side (user: "It should be all the same, no
        // special treatment"). The local row is NOT special-cased -- your own
        // ping is a real host-measured number and belongs on your own row.
        r.ping = led.pingMs;
        r.linkKind = led.linkKind;
        // The display fallback lives in the ledger (ONE copy); the local row
        // still resolves through LocalNickname because our own name is ours
        // before any row exists.
        coop::text::CopyUtf8ToBuffer(r.nick,
                                     rowIsLocal ? coop::player_handshake::LocalNickname()
                                                : coop::roster_ledger::DisplayName(slot));
        ++idx;
    }
    snap.count = idx;

    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_snap = snap;
    }
    g_localIsHost.store(snap.localIsHost, std::memory_order_release);
}

void GetSnapshot(Snapshot& out) {
    std::lock_guard<std::mutex> lk(g_mutex);
    out = g_snap;
}

bool LocalIsHost() { return g_localIsHost.load(std::memory_order_acquire); }

}  // namespace coop::roster
