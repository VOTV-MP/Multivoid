// coop/net/end_reason.h -- why a join or a session ended, as one stable code a player can read
// off the screen and paste into a report. Three families, by who decided: J, the joiner, before
// or without the host's say; H, the host, sent to the peer as the transport's application end
// reason, the integer GNS carries beside its close text; T, the transport itself, mapped from
// its own end-reason ranges. Every id is a literal per row, never the enumerator's position, so
// a row added later renumbers nothing and a code in an old report still names the same site.
// The sentence is the player's; the site's own text rides beside it as the detail. MTA: one
// reason enum on the wire and one literal code per case in CPacketHandler's
// Packet_ServerDisconnected, with CConnectManager mapping the transport's own errors the same way.

#pragma once

#include <cstdint>

namespace coop::net {

enum class EndReason : uint8_t {
    None = 0,

    // J -- the joiner decided. Values 1..39.
    MasterUnreachable = 1,  // the server list did not answer the join request
    BadAddress,             // the server list answered with an address that does not parse
    JoinError,              // the join worker threw; the log has the error
    CouldNotStart,          // the session's Start returned false (no socket, no identity)
    JoinTimedOut,           // the cover's failsafe: the join ran past every real duration
    ShuttingDown,           // the process is exiting; never shown
    GameVersionMismatch,    // the host's game target differs (pre-flight, or its Join)
    HostNewer,              // the host's build is newer (pre-flight, from the browser row)
    HostOlder,              // the host's build is older (pre-flight, from the browser row)
    IdentityExchange,       // the client could not open the identity exchange
    BadChallenge,           // the host's challenge was unexpected, duplicated or malformed
    HostNotProved,          // the host's signature did not verify, or it seated us unproved
    PasswordUnbound,        // a password is wanted and nothing bound the destination host
    PasswordMissing,        // a password is wanted and the player gave none
    PasswordProof,          // the password proof could not be computed on this machine
    CouldNotSendProof,      // the identity proof could not be sent
    ClientBacklogFatal,     // the client's own send backlog tripped its fatal bound
    LeftSession,            // this peer stopped its own session; the host logs it, the leaver never sees it

    // H -- the host decided; rides the transport's application end reason. Values 40..89.
    WrongPassword = 40,
    PasswordRequired,
    TooManyPasswordAttempts,
    HostCannotCheckPassword,
    IdentityNotProved,
    NoKeyIdentity,
    IdentityChanged,
    MalformedHello,
    DuplicateHello,
    ProofBeforeHello,
    MalformedProof,
    SpokeBeforeProving,
    MalformedPacket,
    HostNoRandomness,
    CouldNotSendChallenge,
    IdentityProofTimedOut,
    TooSlowToProve,
    Banned,
    BannedByHost,
    KickedByHost,
    HostFull,
    GameVersionRefused,
    BuildMismatch,
    Superseded,
    HostStopped,
    HostBacklogFatal,
    AcceptFailed,
    HostClosed,             // a close with no code of ours behind it; the text says what it said
    ConnectFlood,           // over the per-source connection cap; refused before any handshake

    // T -- the transport decided. Values 90..127.
    Timeout = 90,           // no answer from the host, at the dial or later
    NoRoute,                // ICE found no path: a firewall or a NAT on either side
    Rendezvous,             // the signaling server could not reach the host
    TransportHandshake,     // the transport's own handshake failed (crypt, cert, protocol)
    LinkLost,               // everything else the transport reports

    // Each family's bounds, for the table's completeness check: a family is contiguous from its
    // first enumerator, so the row count must equal the sum of the three spans.
    kJoinerFirst = MasterUnreachable,
    kJoinerLast = LeftSession,
    kHostFirst = WrongPassword,
    kHostLast = ConnectFlood,
    kTransportFirst = Timeout,
    kTransportLast = LinkLost,
};

struct EndReasonInfo {
    const char* id;    // "MV-H01"; empty for None
    const char* text;  // the player's sentence; empty for None
};

// The id and the sentence for a code; None gives two empty strings.
const EndReasonInfo& Describe(EndReason code);

// The nReason for CloseConnection: the transport's application range plus the code, so the peer's
// status callback reads it back through FromTransportEnd.
int ToTransportEnd(EndReason code);

// The code from a status callback's end reason: our own application range decodes to the code it
// carries, any other application-decided value (the bare generic code, a value outside the table)
// becomes HostClosed, the transport's system ranges map onto the T rows, and anything else is
// LinkLost.
EndReason FromTransportEnd(int transportEnd);

namespace end_reason {
// The table's own check, run once per session start beside the identity ones: every row has a
// non-empty id and sentence, ids are unique, the family letter matches the value range, and a
// code survives the trip through the transport's end reason. Logs; true when every check passed.
bool RunSelftest();
}  // namespace end_reason

}  // namespace coop::net
