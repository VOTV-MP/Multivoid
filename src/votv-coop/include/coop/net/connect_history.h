// coop/net/connect_history.h -- the per-source history behind the host's edge limits: how many
// times one source did a thing inside a sliding window, and a timed refusal once it crosses the
// policy's count. MTA: CConnectHistory, one class carrying four different limits (the join
// flood, the login brute force, the HTTP flood, the query flood); here it carries the connection
// cap at the accept edge and the lobby-password guess bound at the proof. A source is the remote
// address where the transport knows one, else the public key the peer has PROVED it holds, never
// a claimed one: a claimed identity costs nothing to rotate and nothing to forge, so a history
// keyed on it would let anyone fill an honest player's row. Every instance runs on the net thread
// with no lock, like the admission state beside it. The table is bounded where MTA's is a hash
// map that grows without limit (CConnectHistory.h:14, a server-class machine): a fixed table
// keeps the net thread allocation-free, and a full table stops counting, not checking, and says
// so once: a refusal here can only ever deny, so failing closed would lock honest players out
// and failing open admits nobody who has not still to pass every other gate.

#pragma once

#include <cstdint>

#include "coop/net/peer_identity.h"

namespace coop::net::connect_history {

enum class KeyKind : uint8_t { Address = 1, Identity = 2 };

// What a source is: 16 bytes and the kind that says how to read them. The kind is part of the
// key, so an address can never collide with the first half of a public key.
struct Key {
    uint8_t bytes[16]{};
    KeyKind kind = KeyKind::Address;
};

// The remote address of a connection, in the 16-byte form the transport stores (IPv4 arrives
// mapped), without the port: a retrying source gets a fresh port per connection. False when the
// transport reports none, which is every connection that is not plain direct UDP.
bool KeyFromAddress(uint32_t hConn, Key& out);
// The same from the 16 address bytes a caller already holds; false when they are all zero.
bool KeyFromAddressBytes(const uint8_t ipv6[16], Key& out);
// The first half of a public key. Only a key the peer has proved it holds may be passed.
Key KeyFromIdentity(const peer_identity::PubKey& pub);

// One policy: refuse a source that has recorded `max` events inside `windowMs`, for `ignoreMs`
// from the moment it crossed the line -- CConnectHistory's three constructor constants, which
// MTA sets per limit (join flood 4 / 30 s / 30 s, CGame.cpp:163; login brute force 6 / 30 s /
// 60 s, CAccountManager.cpp:24; HTTP 4 / 30 s / 300 s, CHTTPD.cpp:34). With the refusal as long
// as the window, every stamp that counted has expired when the refusal lifts, so the source
// starts clean. A max of 0 disables the policy: nothing is refused and nothing is kept.
struct Policy {
    int      max      = 4;
    uint64_t windowMs = 30'000;
    uint64_t ignoreMs = 30'000;
};

// The verdict of a check, for the log line and the close text.
struct Verdict {
    bool     refused = false;
    int      count   = 0;  // events inside the window; -1 when the source could not be bucketed
    uint64_t retryMs = 0;  // how long the refusal lasts from now, when refused
};

class History {
public:
    // `name` labels the one log line a full table writes; a nameless instance (the selftest's)
    // writes none.
    explicit History(Policy p, const char* name = nullptr) : name_(name) { SetPolicy(p); }

    // A new policy empties the table: rows counted under the old numbers mean nothing under new
    // ones. The max is clamped to the stamps a row can hold.
    void SetPolicy(Policy p);
    const Policy& GetPolicy() const { return policy_; }
    void Clear();

    // Is this source refused right now? Starts the timed refusal the moment the window's count
    // has reached the policy's max. Never records.
    Verdict Blocked(const Key& key, uint64_t nowMs);
    // Record one event for this source. Never refuses.
    void Record(const Key& key, uint64_t nowMs);
    // Blocked, else Record: MTA's AddConnect. A refused event is not recorded, so a source
    // hammering a closed door does not extend its own refusal, and the door reopens on the
    // policy's clock alone.
    Verdict Note(const Key& key, uint64_t nowMs);

    // Rows that are live right now (inside a window or a refusal); the selftest reads it.
    int LiveRows(uint64_t nowMs) const;

    static constexpr int kBuckets   = 64;
    static constexpr int kMaxStamps = 16;

private:
    struct Bucket {
        Key      key{};
        bool     used = false;
        uint64_t ignoreUntilMs = 0;
        uint64_t stamps[kMaxStamps]{};  // a ring, oldest at `head`, `count` live
        int      head = 0;
        int      count = 0;
        uint64_t newestMs = 0;
    };
    bool    IsLive(const Bucket& b, uint64_t nowMs) const;
    void    Expire(Bucket& b, uint64_t nowMs);
    Bucket* Find(const Key& key, uint64_t nowMs);  // found or claimed; null when every row is live

    const char* name_ = nullptr;
    bool   fullLogged_ = false;  // one line per full-to-free cycle, not one per arrival
    Policy policy_;
    Bucket buckets_[kBuckets];
};

// The connection cap at the host's edge, one instance for the two sites that feed it: the accept
// edge by address, and the proof by the identity just proved where the transport had no address.
History& Connects();
// The host's session start: the two ini rows applied to Connects(), which also empties it. A cap
// of 0 disables it. The refusal lasts one window.
void ConfigureConnects(long cap, long windowS);

// The arithmetic selftest, run once per session start beside the identity ones: the count that
// refuses, the refusal that lifts, a window that slides, a refused event that is not recorded,
// keys of two kinds that do not collide, and a full table that refuses nobody.
bool RunSelftest();

}  // namespace coop::net::connect_history
