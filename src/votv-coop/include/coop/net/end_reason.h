// coop/net/end_reason.h -- why a join or a session ended, as one stable code a player can read
// off the screen and paste into a report. Three families, by who decided: J, the joiner, before
// or without the host's say; H, the host, sent to the peer as the transport's application end
// reason, the integer GNS carries beside its close text; T, the transport itself, mapped from
// its own end-reason ranges. Every id is a literal per row, never the enumerator's position, so
// a row added later renumbers nothing and a code in an old report still names the same site.
// The sentence is the player's; the site's own text rides beside it as the detail. MTA: one
// reason enum on the wire and one literal code per case in CPacketHandler's
// Packet_ServerDisconnected, with CConnectManager mapping the transport's own errors the same way.
// Relay, Moddy's VOTV mod, publishes the two rules this follows in its README: show the reason a
// join is being held rather than one generic label, and give a player stable codes to quote
// (docs/credits.md).

#pragma once

#include <cstdint>
#include <string>

namespace coop::net {

enum class EndReason : uint8_t {
    None = 0,

    // J -- the joiner decided. Values 1..39.
    MasterUnreachable = 1,  // the server list did not answer the join request
    BadAddress,             // the server list answered with an address that does not parse
    JoinError,              // the join worker threw; the log has the error
    CouldNotStart,          // the session's Start returned false (no socket, no identity)
    // RESERVED, NEVER CONSTRUCTED. This was the whole-join failsafe (MV-J05), deleted with the
    // 240 s clock in WP-B2. The slot stays for two reasons: the value rides the wire as
    // App_Min + itself and a retired wire value is never reused, and an id's digits ARE its
    // position in this family, so removing the enumerator would renumber every code below it and
    // make every field report that quotes one point at a different failure.
    RetiredJoinTimedOut,
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
    // The two halves of a rendezvous dial that ended with nothing coming back. The transport
    // reports both as its own timeout, which is true of every dead dial and names neither side;
    // the joiner can tell them apart from what it owns, and only these two rows say so.
    RendezvousUnreachable,  // this machine had no connection to the relay while it dialled
    NoRendezvousAnswer,     // our registration was live and the dialled host never answered
    // The phase tokens. Each names the PHASE that stopped; whether the host went quiet or
    // answered all along and got nowhere is the detail's job, since both end the same wait and a
    // player quotes the code. This is what one budget over every phase could not do: it could only
    // ever say that the sum ran long, which is why the row that said so (MV-J05) is retired and
    // never reissued -- a code in a field report must keep meaning what it meant.
    HostWorldNotPrepared,   // the host never finished capturing the world it owed this joiner
    WorldDownloadStalled,   // no byte of the world blob arrived for the download's budget
    HostWorldNotSent,       // the world bracket never came: the host held it, or stopped answering
    // These four are LOCAL: the host did what it owed and this machine could not use it. The
    // first two used to boot a fresh world instead and finish the join in it, telling the player
    // nothing -- a joiner standing in a world that is not the host's; the third loaded anyway with
    // the host's items emptied out; the fourth is a wait that had no watcher at all.
    WorldWouldNotLoad,      // the engine never reached gameplay with the world we received
    WorldUnusable,          // the world arrived damaged: the CRC failed, or it could not be written
    ProfileNotSent,         // the host never sent this player's inventory, so the world would load without it
    WorldNeverSettled,      // the world came up and this machine never got ready to announce it
    // The player's ICE policy (coop/net/ice_policy.h) refused this machine's session before it
    // started: a join, or a host's own start, which the hosting window reports under the same code.
    IcePolicyUnreadable,    // net.ice is neither all nor relay, or multivoid.ini could not be read
    RelayWithoutServer,     // net.ice=relay and the session has no TURN server
    RelayRefusesDirect,     // net.ice=relay and the session is a direct dial

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
    kJoinerLast = RelayRefusesDirect,
    kHostFirst = WrongPassword,
    kHostLast = ConnectFlood,
    kTransportFirst = Timeout,
    kTransportLast = LinkLost,
};

struct EndReasonInfo {
    const char* id;    // "MV-H01"; empty for None
    const char* text;  // the player's sentence; empty for None
};

// A refusal handed back to the caller instead of carried on the wire: the code, and the deciding
// site's own detail beside it. None is no refusal.
struct Refusal {
    EndReason   code = EndReason::None;
    std::string detail;
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
