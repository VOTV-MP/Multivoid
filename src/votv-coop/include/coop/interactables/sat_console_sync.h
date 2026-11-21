// coop/interactables/sat_console_sync.h -- the SAT consoles' shared commands run on the host, each console its own.
//
// The game gives a machine one terminal, the ui_console every SAT console panel shows. A line typed there whose
// command rests on the shared world (coop/interactables/sat_console_table) is refused at its enterCommand while
// the terminal is not busy, its input consumed as the body's tail does, and run on the host with the terminal's
// context: its dish or ROOT, its name, and the panel last used, which names the console. The host keeps a
// terminal of the game's class for each console, never shown, and runs every line typed at that console there,
// the host's own included, so a command happens once, in the host's world, at its own console's dish and server,
// and a busy console holds only itself. Each console's printed lines and busy flag go to every peer, and a
// machine's terminal shows the console its player last used: that console's lines, then its own as they come.
// The world's broken-device alerts are the ROOT console's alone.
#pragma once

#include <cstddef>
#include <cstdint>

namespace coop::net {
class Session;
struct BlobChunkPayload;
}

namespace coop::sat_console_sync {

void Install(coop::net::Session* s);
void Tick();
void OnChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);
// A peer's world is ready: the host sends it every console's lines and flag. A peer's slot is gone: its
// chunks, owed blobs and pace are forgotten; the consoles it used keep running what it started.
void OnPeerWorldReady(int slot);
void OnPeerGone(uint8_t slot);
void OnDisconnect();

// [dev] What the lane did, for the drill: the lines this client sent to run on the host, and on the host
// the lines it ran for clients, the consoles' lines it sent, the consoles sent to a joiner at its
// world-ready, and those of them that were busy then; and the terminals the host keeps.
struct Counts {
    uint64_t linesSent = 0;
    uint64_t linesRun = 0;
    uint64_t linesReturned = 0;
    uint64_t rebinds = 0;
    uint64_t busyRebinds = 0;
};
Counts LaneCounts();
size_t KeptTerminals(void** out, size_t cap);

}  // namespace coop::sat_console_sync
