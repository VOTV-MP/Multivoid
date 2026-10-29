// coop/props/prop_drive_rows.h -- IMPLEMENTATION-PRIVATE: the driven-prop row and the verbs on the table.
//
// NOT a public header (it lives under src/, not include/). The table is prop_drive_host's g_driven, one row
// per prop under a drive; the row says WHO authors the drive (`syncer`: 0 = the host, n = client slot n).
// prop_syncer_host judges a client's rows through the declarations below, which prop_drive_host.cpp defines
// because it owns the table; prop_syncer_host also ends them (EndClientRow), called back by the row's readers.
//
// Game-thread only (same contract as prop_drive_host).

#pragma once

#include "coop/net/intent_bucket.h"
#include "coop/net/protocol.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/types.h"

#include <cstdint>
#include <vector>

namespace coop::prop_drive_host {

struct Driven {
    ue_wrap::CachedObjRef ref;
    uint32_t              eid = 0;
    coop::net::WireKey    key{};
    uint8_t               gen = 0;
    uint8_t               syncer   = 0;      // 0 = the host authors the row; n = client slot n
    bool                  claimed  = true;   // false: coasting after the verb let go
    bool                  everSent = false;  // a pose of this drive went out; sentLoc/sentRot hold the last
    bool                  resend   = false;  // a joiner needs the pose again, moved or not
    bool                  unreadSaid = false;  // the unreadable-location line went out for this row
    bool                  dead     = false;  // closed this tick; erased after the pass
    ue_wrap::FVector      sentLoc{};
    ue_wrap::FRotator     sentRot{};
    uint64_t              openMs      = 0;   // when the row opened: the host's rows count their sendTimeMs from here
    uint64_t              lastMoveMs  = 0;
    uint64_t              nextProbeMs = 0;   // 0 = every tick; set while resting

    // A client row (syncer != 0): the last pose the host admitted from the syncer, when, and whether
    // the row's turn in Tick published it yet. The host never reads the actor for such a row.
    ue_wrap::FVector      admitLoc{};
    ue_wrap::FRotator     admitRot{};
    ue_wrap::FVector      admitVel{};            // that row's velocity (cm/s) and its sender-clock stamp
    uint32_t              admitSendMs = 0;
    uint64_t              admitMs     = 0;
    bool                  admittedEver = false;  // a pose of this gen was admitted
    bool                  freshAdmit   = false;  // admitted, not yet published
    coop::net::IntentBucket streamBucket;        // the syncer's rows only
    uint16_t              claimSeq    = 0;       // the claim that opened the row, echoed in its answers

    // A thrown prop the host elected its thrower to sync (prop_syncer_host::ElectSyncer): the throw's sequence, which the
    // grant and a Revoked echo, and the release the first row is measured from. A row that fails the step test is
    // displayed and counted (failedRun, failedTotal); a run of kMaxRefusedRows of them revokes the right.
    bool                  elected     = false;
    uint16_t              throwSeq    = 0;
    ue_wrap::FVector      startLoc{};
    uint64_t              startMs     = 0;
    uint8_t               failedRun   = 0;      // consecutive rows that failed the step test, reset by a passing row
    uint16_t              failedTotal = 0;      // the same, over the flight
    uint64_t              electedMs   = 0;      // the election: the grant timeout counts from here, startMs from the release
    bool                  passedEver  = false;  // a row passed the step test
    ue_wrap::FVector      passLoc{};            // the last row that PASSED (admit* is the last one displayed, passing or not):
    ue_wrap::FRotator     passRot{};            // the host takes the prop back from it
    ue_wrap::FVector      passVel{};
    uint32_t              passSendMs  = 0;
    uint64_t              passMs      = 0;
};

// The live row of the prop with element id `eid`, null if none. Game thread.
Driven* FindByEid(uint32_t eid);

// The next claim generation of `eid`, one counter per prop for both authors; 0 is skipped, the stream's
// "no generation". Game thread.
uint8_t BumpGen(uint32_t eid);

// The PropDriveEnd of generation `gen` of `eid`, read off `actor` now and placed at `loc`/`rot`: this peer's
// physics flags and the body's velocity, zero under kRestVelCmS. The one build for the host's End and a syncing
// client's. `*speedCmS`, when given, gets the speed read (0 when unreadable). Game thread.
coop::net::PropDriveEndPayload EndFromBody(void* actor, uint32_t eid, uint8_t gen, const ue_wrap::FVector& loc,
                                           const ue_wrap::FRotator& rot, float* speedCmS = nullptr);

// A row for `actor` authored by client `slot`, under the Open refusals (a keyed prop with a host element id,
// in no hand, of a class peers hold); null otherwise. Fresh admission state: nothing admitted, a full
// bucket, the silence clock at now. Game thread.
Driven* OpenClientRow(void* actor, uint8_t slot, const char* why);

// A client row for `actor` when a host row already stands on it (a release's override coasted a client's): that row
// becomes the client's under a new generation, nothing admitted. A row a host verb holds (claimed, syncer 0) is not
// converted: null, as OnClaim refuses it. Another client's row is ended HostVerb first, so that client is told
// Revoked. With no row, OpenClientRow's. Null on the refusals Open has. Game thread.
Driven* ElectClientRow(void* actor, uint8_t slot, const char* why);

// Close `row`: it is skipped by every reader at once and swept after the tick's pass. Game thread.
void EraseRow(Driven& row);

// Every row, closed ones not yet swept included (skip `dead`). Rows are only marked closed outside Tick, so a
// pass over them may call EraseRow; a new row (a Claim, a grant) invalidates the references. Game thread.
std::vector<Driven>& Rows();

}  // namespace coop::prop_drive_host

namespace coop::prop_syncer_host {

// Why a client's row ends on the host. Hand, HostVerb and Died are met by the row's own readers in
// prop_drive_host; the rest are prop_syncer_host's own passes. A rest End comes from the client itself (OnClientEnd)
// and is not one of these. FlightBound (an elected row past kMaxThrowFlightMs), RowsUntrusted (kMaxRefusedRows rows in
// a row failed the step test) and GrantTimeout (no row admitted kGrantTimeoutMs after the election) hand the prop to
// the host the way HostVerb does: its copy continues from the last passing row, or from its own state when none was.
// So does every ending of an elected row but Died and Hand when nothing was admitted, and SlotLeft of an elected row.
enum class Ending : uint8_t { Hand, HostVerb, Died, Silence, Range, SlotLeft, FlightBound, RowsUntrusted, GrantTimeout, kCount };

// End client row `row` in the one order every ending uses: release the host's copy at the last admitted pose and
// velocity, send the End built from it (none for an ending that hands the prop to the host, none with nothing
// admitted), tell the syncer `Revoked` (none when its slot left), then erase the row, or for those endings hand it to
// the host under a new generation. Defined in prop_syncer_host.cpp; prop_drive_host.cpp calls it. Game thread.
void EndClientRow(coop::prop_drive_host::Driven& row, Ending why);

}  // namespace coop::prop_syncer_host
