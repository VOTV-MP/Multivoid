// coop/dev/container_parity.h -- [dev] the container join drill's wire half. The joiner sends the host one
// DevContainerParity report after its join: a chunked blob, [u32 count] then count net::DevContainerParityEntry rows,
// the content hash of every world container it holds. The host assembles it and judges it against its own containers
// (JudgeReport). Dropped at the host's door until a caller turns the module on (Enable). Game thread.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace coop::net { class Session; }

namespace coop::dev::container_parity {

// Turn the host's door on. The container join drill calls it; nothing else reads the report.
void Enable();

// CLIENT: one row per world container, then ship the report. False when a chunk was refused or the blob would pass
// the transport's ceiling; the caller says so, and the host then has no report to judge.
bool SendReport(coop::net::Session& session);

// HOST: one chunk off the wire. A no-op until Enable.
void OnChunk(const coop::net::BlobChunkPayload& chunk, uint8_t senderSlot);

// A finished report, with the slot that sent it.
struct Report {
    uint8_t slot = 0;
    std::vector<coop::net::DevContainerParityEntry> rows;
};

// HOST: the oldest finished report not yet taken; false when none.
bool TakeReport(Report& out);

// HOST: compare a report with this peer's own world containers and print each difference (at most 20 FAIL lines).
// True only when nothing is mismatched, missing or extra; `firstFail` then holds nothing, else the first FAIL line.
bool JudgeReport(const Report& report, std::string& firstFail);

// HOST: sweep half-assemblies that never finished. Every pump tick; cheap while none is held.
void Tick();

// The session ended: the assemblies and the finished reports belong to it.
void OnDisconnect();

}  // namespace coop::dev::container_parity
