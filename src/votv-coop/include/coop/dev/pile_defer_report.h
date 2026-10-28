// coop/dev/pile_defer_report.h -- [dev] the pile deferral drill's wire half and its row (ini pile_defer_drill=
// off|check|giveup / env VOTVCOOP_PILE_DEFER_DRILL; BOTH peers, the same value). The joiner sends the host one
// DevParityReport after its join: a chunked blob, [u32 count] then count net::DevParityEntry rows, the pose of every
// trash element it has bound. The host assembles it and hands the finished report to the drill's judge. Dropped at
// the host's door unless the row is on. Game thread.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>
#include <vector>

namespace coop::net { class Session; }

namespace coop::dev::pile_defer_report {

enum class Arm { Off, Check, Giveup };

// The row, read once.
Arm ArmOf();

// CLIENT: ship the report. False when the blob would pass the transport's ceiling or a chunk was refused; the
// caller says so, and the host then has no report to judge.
bool Send(coop::net::Session& session, const std::vector<coop::net::DevParityEntry>& rows);

// HOST: one chunk off the wire. A no-op when the row is off.
void OnChunk(const coop::net::BlobChunkPayload& chunk, uint8_t senderSlot);

// A finished report, with the slot that sent it.
struct Report {
    uint8_t slot = 0;
    std::vector<coop::net::DevParityEntry> rows;
};

// HOST: the oldest finished report not yet taken; false when none.
bool TakeReport(Report& out);

// HOST: sweep half-assemblies that never finished. Every pump tick; cheap while none is held.
void Tick();

// The session ended: the assemblies and the finished reports belong to it.
void OnDisconnect();

}  // namespace coop::dev::pile_defer_report
