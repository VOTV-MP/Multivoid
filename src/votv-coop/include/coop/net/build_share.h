// coop/net/build_share.h -- a joiner whose Multivoid build differs from the host's can take the host's.
//
// Two builds that differ cannot talk: another protocol number ends the connection at the first packet,
// and the same number over different code misbehaves in silence. This exchange runs beside the
// protocol, in frames of its own (`MVUP`, a magic no build's packet header carries, so a build without
// this module drops them unread) whose layout never changes with the protocol number. The host tells
// each connection its build's hash; a client whose own differs asks, the host answers with an offer
// signed by its identity key over the client's nonce, and on the client's word streams its main.dll, deflated.
// The client checks the signature against the identity the transport reports for the connection and
// the file against the offered hash, then puts it in place of its own, which stays as main.dll.old.
// Whose build a client takes is the client's choice: one that verifies under a release key, one from
// a host the player has trusted, or one the player accepts at the prompt (`net.build_updates`).

#pragma once

#include <cstdint>
#include <string>

namespace coop::net::build_share {

// A frame of this exchange, whatever protocol version the sender speaks. Any thread.
bool IsFrame(const void* data, int len);

// ---- host, net thread ----

// A pending connection's packet arrived. The host's notice goes out once per connection. `mismatch`
// says the packet named another protocol version: true comes back when the connection is held for an
// exchange and must not be closed for the mismatch yet.
bool HostOnPendingSpoke(uint32_t hConn, bool mismatch);

void HostOnFrame(uint32_t hConn, const void* data, int len);

// What the pending sweep does with a connection: nothing of this module's (None), leave it alone
// (Busy), or close it for the build mismatch it was held over (Lapsed, said once).
enum class Hold : uint8_t { None, Busy, Lapsed };
Hold HostHold(uint32_t hConn);

// Feeds the transfers in flight. Once a net-thread pass. `pace` says how a connection may be sent to now:
// kWait while a seated joiner is still loading the world (its world download owns the connection), kGentle
// for a seated player in the world (the game's own messages share the queue), kFull for a connection held
// for nothing else.
inline constexpr int kWait = 0;
inline constexpr int kGentle = 1;
inline constexpr int kFull = 2;
void HostPump(int (*pace)(void* ctx, uint32_t hConn), void* ctx);

// ---- client ----

// Net thread: a frame from the host.
void ClientOnFrame(uint32_t hConn, const void* data, int len);

// Net thread, once a pass: a download that stopped arriving is ended.
void ClientPump();

enum class State : uint8_t { Idle, Asking, Offered, Downloading, Installed, Failed };

// What the prompt draws. Any thread.
struct View {
    State       state = State::Idle;
    bool        official = false;      // the offered build verifies under a release key
    uint16_t    hostProto = 0;
    uint32_t    size = 0;              // the build's size
    uint32_t    packed = 0;            // the download's size, once the host has said it
    uint32_t    received = 0;          // of the download
    std::string hostGuid;              // the host's proved identity
    std::string shaHex;                // the offered build's hash, first 16 hex digits
    std::string error;                 // Failed: why
};
View Snapshot();

// The state alone, for a per-frame gate.
State Peek();

// The player's answer to an offer, and the prompt's close after Installed or Failed. Any thread.
void Accept(bool trustHost);
void Decline();
void Dismiss();

// The player's setting allows taking a host's build at all: a join to a host on another build
// number is then dialled instead of refused from the browser row.
bool Wanted();

// The session ended: an exchange in flight is over. Installed and Failed stay for the prompt.
void OnDisconnect();

}  // namespace coop::net::build_share
