// coop/net/ice_policy.h -- the player's ICE policy, net.ice, and the sessions it refuses.
//
// The policy says what this machine shows a peer. `all` (the default) offers every ICE candidate:
// private, public and relayed; `relay` offers only the TURN relay's, so a peer sees the relay's
// address instead of ours. It therefore governs every P2P session, host and join, and a direct
// dial too, which shows the host our address with no ICE in between. A direct listen shows
// nothing by itself, since a direct host hands its own address out. A value that is neither, or
// an ini that cannot be read, refuses what the policy governs instead of falling back to `all`,
// the most disclosing policy: the setting is a privacy promise, and a mistyped value or a locked
// file must not quietly void it (a CFG_ENUM_FAILCLOSED row). MTA's CConnectManager::Connect
// refuses a connect it cannot honour the same way, before the network starts, each case under its
// code.

#pragma once

#include "coop/net/end_reason.h"

#include <string>

namespace coop::net {

struct Config;

struct IcePolicy {
    bool        readable = true;        // false: a refused value, or an unreadable ini
    bool        fileUnreadable = false; // the ini could not be read, so nothing was refused
    bool        relayOnly = false;      // net.ice=relay
    std::string refused;                // the refused value, possibly empty
    std::string origin;                 // where it came from: multivoid.ini, or the env var's name
    std::string fileFault;              // why the ini could not be read: the words after its name
};

// The policy as configured now: env, then ini, then `all`. Reads the ini, so once per session
// start or host action, never per frame. The verdict's self-test runs at the first call, before
// the first verdict it vouches for, whichever path asks first.
IcePolicy ResolveIcePolicy();

// The refusal a session start owes under `policy`, or None. An unreadable policy refuses whatever
// it governs; relay-only refuses a P2P session with no TURN server, and any direct dial.
Refusal IcePolicyRefusal(const IcePolicy& policy, const Config& cfg);

// The verdict's own check (ResolveIcePolicy runs it once per process): every arm, each against the
// session kinds that must and must not reach it. Logs; true when every case passed.
bool RunIcePolicySelftest();

}  // namespace coop::net
