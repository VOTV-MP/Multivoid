// coop/roster.cpp -- see coop/roster.h.

#include "coop/roster.h"

#include "coop/net/session.h"
#include "coop/player_handshake.h"
#include "coop/players_registry.h"

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

// Peer nicknames are ASCII-sanitized upstream (player_handshake::SanitizeNickname),
// so a straight UTF-8 narrowing into the fixed buffer is lossless + bounded.
void NarrowNick(const std::wstring& w, char out[24]) {
    out[0] = '\0';
    if (w.empty()) return;
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                  out, 23, nullptr, nullptr);
    if (n < 0) n = 0;
    out[n] = '\0';
}

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
        NarrowNick(coop::player_handshake::LocalNickname(), snap.rows[0].nick);
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
        const bool rowIsLocal = (slot == localSlot);
        const bool connected  = rowIsLocal ? true : s->IsSlotConnected(slot);
        if (!rowIsLocal && !connected) continue;  // empty client slot
        Row& r = snap.rows[idx];
        r.slot = slot;
        r.isLocal = rowIsLocal;
        r.isHost = (slot == 0);
        r.connected = connected;
        // Per-peer ping for the scoreboard. The local row has no self-ping (-1); a
        // remote row shows the GNS RTT to that peer (0 on a sub-ms LAN link).
        r.ping = rowIsLocal ? -1 : s->rttMsForSlot(slot);
        NarrowNick(rowIsLocal ? coop::player_handshake::LocalNickname()
                              : coop::player_handshake::NicknameForSlot(slot),
                   r.nick);
        // Connection-type column (user 2026-06-10): each board shows what THIS
        // peer truthfully knows. The HOST owns every client conn -> real
        // transport per client row + a hosting-mode label on its own row. A
        // CLIENT owns only its host link -> that label on the host row; other
        // clients' traffic reaches it relayed by the host -> "VIA HOST".
        if (rowIsLocal && isHost) {
            std::snprintf(r.link, sizeof(r.link), "%s",
                          s->topology() == coop::net::Topology::P2P ? "P2P HOST" : "LAN HOST");
        } else if (isHost) {
            s->LinkLabelForSlot(slot, r.link, sizeof(r.link));  // a client's real link
        } else if (slot == 0) {
            s->LinkLabelForSlot(0, r.link, sizeof(r.link));     // my own link to the host
        } else if (!rowIsLocal) {
            std::snprintf(r.link, sizeof(r.link), "VIA HOST");
        }
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
