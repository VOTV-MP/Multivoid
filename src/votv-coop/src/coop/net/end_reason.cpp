// coop/net/end_reason.cpp -- see coop/net/end_reason.h.

#include "coop/net/end_reason.h"

#include "ue_wrap/core/log.h"

#include <steam/steamnetworkingtypes.h>

#include <cstring>

namespace coop::net {
namespace {

struct Row {
    EndReason     code;
    EndReasonInfo info;
};

// One row per enumerator, the id a literal. A code's meaning lives here and nowhere else.
constexpr Row kRows[] = {
    // J -- the joiner decided
    {EndReason::MasterUnreachable,   {"MV-J01", "Could not reach the server list."}},
    {EndReason::BadAddress,          {"MV-J02", "The server list gave a bad address for this host."}},
    {EndReason::JoinError,           {"MV-J03", "The join failed before it started. The log has the error."}},
    {EndReason::CouldNotStart,       {"MV-J04", "Could not start the connection."}},
    {EndReason::JoinTimedOut,        {"MV-J05", "The join did not finish in time."}},
    {EndReason::ShuttingDown,        {"MV-J06", "The game is shutting down."}},
    {EndReason::GameVersionMismatch, {"MV-J07", "The host plays a different version of the game."}},
    {EndReason::HostNewer,           {"MV-J08", "The host runs a newer build of the mod. Update to join."}},
    {EndReason::HostOlder,           {"MV-J09", "The host runs an older build of the mod. They need to update."}},
    {EndReason::IdentityExchange,    {"MV-J10", "Could not start the identity exchange with this host."}},
    {EndReason::BadChallenge,        {"MV-J11", "The host sent an identity challenge this build cannot read."}},
    {EndReason::HostNotProved,       {"MV-J12", "The host could not prove its identity."}},
    {EndReason::PasswordUnbound,     {"MV-J13", "This server needs a password, and nothing told this machine which host it was dialling."}},
    {EndReason::PasswordMissing,     {"MV-J14", "This server needs a password."}},
    {EndReason::PasswordProof,       {"MV-J15", "Could not compute the password proof on this machine."}},
    {EndReason::CouldNotSendProof,   {"MV-J16", "Could not send the identity proof to the host."}},
    {EndReason::ClientBacklogFatal,  {"MV-J17", "The connection fell too far behind and was closed."}},
    {EndReason::LeftSession,         {"MV-J18", "Left the session."}},
    // H -- the host decided
    {EndReason::WrongPassword,           {"MV-H01", "Wrong password."}},
    {EndReason::PasswordRequired,        {"MV-H02", "This server needs a password."}},
    {EndReason::TooManyPasswordAttempts, {"MV-H03", "Too many password attempts. Try again in ten minutes."}},
    {EndReason::HostCannotCheckPassword, {"MV-H04", "The host could not check the password."}},
    {EndReason::IdentityNotProved,       {"MV-H05", "Your identity proof did not verify."}},
    {EndReason::NoKeyIdentity,           {"MV-H06", "Your connection presented no key identity."}},
    {EndReason::IdentityChanged,         {"MV-H07", "Your identity changed during the exchange."}},
    {EndReason::MalformedHello,          {"MV-H08", "The host could not read your hello."}},
    {EndReason::DuplicateHello,          {"MV-H09", "A second hello arrived on one connection."}},
    {EndReason::ProofBeforeHello,        {"MV-H10", "A proof arrived before a hello."}},
    {EndReason::MalformedProof,          {"MV-H11", "The host could not read your proof."}},
    {EndReason::SpokeBeforeProving,      {"MV-H12", "Something was sent before the identity was proved."}},
    {EndReason::MalformedPacket,         {"MV-H13", "The host received a malformed packet."}},
    {EndReason::HostNoRandomness,        {"MV-H14", "The host has no randomness to challenge with."}},
    {EndReason::CouldNotSendChallenge,   {"MV-H15", "The host could not send its challenge."}},
    {EndReason::IdentityProofTimedOut,   {"MV-H16", "The identity proof did not arrive in time."}},
    {EndReason::TooSlowToProve,          {"MV-H17", "The host was busy, and this connection was the slowest to prove itself."}},
    {EndReason::Banned,                  {"MV-H18", "You are banned from this server."}},
    {EndReason::BannedByHost,            {"MV-H19", "The host banned you."}},
    {EndReason::KickedByHost,            {"MV-H20", "The host kicked you."}},
    {EndReason::HostFull,                {"MV-H21", "The server is full."}},
    {EndReason::GameVersionRefused,      {"MV-H22", "The host plays a different version of the game."}},
    {EndReason::BuildMismatch,           {"MV-H23", "Your mod build differs from the host's."}},
    {EndReason::Superseded,              {"MV-H24", "A newer connection with your identity replaced this one."}},
    {EndReason::HostStopped,             {"MV-H25", "The host ended the session."}},
    {EndReason::HostBacklogFatal,        {"MV-H26", "The host closed the connection: it fell too far behind."}},
    {EndReason::AcceptFailed,            {"MV-H27", "The host could not accept the connection."}},
    {EndReason::HostClosed,              {"MV-H28", "The host closed the connection."}},
    {EndReason::ConnectFlood,            {"MV-H29", "Too many connections from you in a short time. Try again shortly."}},
    // T -- the transport decided
    {EndReason::Timeout,            {"MV-T01", "No answer from the host."}},
    {EndReason::NoRoute,            {"MV-T02", "No route to the host through its firewall or router."}},
    {EndReason::Rendezvous,         {"MV-T03", "The signaling server could not reach the host."}},
    {EndReason::TransportHandshake, {"MV-T04", "The transport handshake with the host failed."}},
    {EndReason::LinkLost,           {"MV-T05", "The connection was lost."}},
};

constexpr int kRowCount = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));
constexpr EndReasonInfo kNone{"", ""};

constexpr int V(EndReason e) { return static_cast<int>(e); }

// The transport's application range has room for 1000 codes; the last row stays well inside it.
constexpr int kLast = 127;
static_assert(V(EndReason::kTransportLast) <= kLast, "the code range is 1..127");
static_assert(k_ESteamNetConnectionEnd_App_Min + kLast <= k_ESteamNetConnectionEnd_App_Max,
              "every code must fit the transport's application range");
// Every enumerator has a row: the families are contiguous, so the row count is the sum of the
// three spans (the self-test below rules out a duplicated code, which is the other way to reach
// this count).
static_assert(kRowCount == (V(EndReason::kJoinerLast) - V(EndReason::kJoinerFirst) + 1) +
                               (V(EndReason::kHostLast) - V(EndReason::kHostFirst) + 1) +
                               (V(EndReason::kTransportLast) - V(EndReason::kTransportFirst) + 1),
              "an enumerator without a row in the table");

char FamilyOf(EndReason code) {
    const int v = V(code);
    if (v >= V(EndReason::kTransportFirst) && v <= V(EndReason::kTransportLast)) return 'T';
    if (v >= V(EndReason::kHostFirst) && v <= V(EndReason::kHostLast)) return 'H';
    if (v >= V(EndReason::kJoinerFirst) && v <= V(EndReason::kJoinerLast)) return 'J';
    return '\0';
}

}  // namespace

const EndReasonInfo& Describe(EndReason code) {
    for (const Row& r : kRows)
        if (r.code == code) return r.info;
    return kNone;
}

int ToTransportEnd(EndReason code) {
    return k_ESteamNetConnectionEnd_App_Min + static_cast<int>(code);
}

EndReason FromTransportEnd(int transportEnd) {
    if (transportEnd > k_ESteamNetConnectionEnd_App_Min &&
        transportEnd <= k_ESteamNetConnectionEnd_App_Min + kLast) {
        const auto code = static_cast<EndReason>(transportEnd - k_ESteamNetConnectionEnd_App_Min);
        return Describe(code).id[0] ? code : EndReason::HostClosed;
    }
    // The rest of the two application ranges (the bare generic code, a value no row of ours
    // carries, the exceptional range): the peer's application decided, so a host close, not a
    // transport verdict.
    if (transportEnd >= k_ESteamNetConnectionEnd_App_Min &&
        transportEnd <= k_ESteamNetConnectionEnd_AppException_Max) {
        return EndReason::HostClosed;
    }
    switch (transportEnd) {
    case k_ESteamNetConnectionEnd_Misc_Timeout:
        return EndReason::Timeout;
    case k_ESteamNetConnectionEnd_Misc_P2P_NAT_Firewall:
    case k_ESteamNetConnectionEnd_Local_P2P_ICE_NoPublicAddresses:
    case k_ESteamNetConnectionEnd_Remote_P2P_ICE_NoPublicAddresses:
        return EndReason::NoRoute;
    case k_ESteamNetConnectionEnd_Misc_P2P_Rendezvous:
        return EndReason::Rendezvous;
    case k_ESteamNetConnectionEnd_Remote_BadCrypt:
    case k_ESteamNetConnectionEnd_Remote_BadCert:
    case k_ESteamNetConnectionEnd_Remote_BadProtocolVersion:
        return EndReason::TransportHandshake;
    default:
        return EndReason::LinkLost;
    }
}

namespace end_reason {

bool RunSelftest() {
    int total = 0, pass = 0;
    auto check = [&](bool ok, const char* what, const char* id) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("end_reason selftest: FAIL %s (%s)", what, id);
    };
    for (int i = 0; i < kRowCount; ++i) {
        const Row& r = kRows[i];
        check(r.info.id[0] != '\0' && r.info.text[0] != '\0', "row has an id and a sentence",
              r.info.id);
        // "MV-" then the family letter then two digits: the letter must match the value range.
        check(std::strlen(r.info.id) == 6 && std::strncmp(r.info.id, "MV-", 3) == 0 &&
                  r.info.id[3] == FamilyOf(r.code),
              "id names the family its value sits in", r.info.id);
        for (int j = 0; j < i; ++j) {
            check(std::strcmp(kRows[j].info.id, r.info.id) != 0, "id is unique", r.info.id);
            check(kRows[j].code != r.code, "code has one row", r.info.id);
        }
        check(FromTransportEnd(ToTransportEnd(r.code)) == r.code,
              "code survives the transport end reason", r.info.id);
    }
    check(Describe(EndReason::None).id[0] == '\0', "None describes as empty", "MV-");
    check(FromTransportEnd(k_ESteamNetConnectionEnd_App_Generic) == EndReason::HostClosed,
          "the bare application code reads as a host close", "MV-H28");
    check(FromTransportEnd(0) == EndReason::LinkLost, "an unknown end reads as a lost link",
          "MV-T05");
    check(FromTransportEnd(k_ESteamNetConnectionEnd_AppException_Generic) == EndReason::HostClosed,
          "an application end outside the table reads as a host close", "MV-H28");
    if (pass == total) {
        UE_LOGI("end_reason selftest: ALL PASS (%d checks, %d codes)", total, kRowCount);
        return true;
    }
    UE_LOGE("end_reason selftest: %d/%d checks passed", pass, total);
    return false;
}

}  // namespace end_reason
}  // namespace coop::net
