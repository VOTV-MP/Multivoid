// coop/net/join_wire.h -- the Join payload's bytes: one encoder and one walker, with no game
// behind them. The DLL's handshake and the load peer both send a Join through EncodeJoin, and the
// host's version gate reads one through ExtractJoinVersionFields. The handshake's own field-by-field
// apply of the first fields (HandleJoinMessage, which stores them into the roster) is a third reader.
//
// THE THREE MUST AGREE, field for field: EncodeJoin, the walker below and HandleJoinMessage's apply.
// A field added or removed is changed in all three together. When the guid field was deleted, one
// walker still consumed two length-prefixed fields where one remained, read the flags byte as a
// length, ran off the end and refused every Join with "version field missing" on both peers; the
// pose stream then never started. The failure is loud but names the wrong field.
//
// The chain: [u32 eid][u8 nicklen][nick UTF-8][u8 skinlen][skin][u8 prefs flags][u8 has][r][g][b]
// [u8 gamelen][game][sha256 32][u8 build flags][u8 keylen][game build key].

#pragma once

#include "coop/build_trust/build_trust.h"  // kShaBytes (constants only)

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::net {

// What a peer says about itself. EncodeJoin caps each text field on the wire's own limit: the nick
// at coop::text::kNickMaxBytes on a character boundary, the skin at 48 bytes, the game target at
// 23. `nick` is UTF-8.
struct JoinFields {
    uint32_t eid = 0;
    std::string nick;
    std::string skin;
    uint8_t prefsFlags = 0;     // bit 0 = nameplate visible
    bool colorCustom = false;   // the [has] byte; r, g, b ride either way
    uint8_t colorR = 0;
    uint8_t colorG = 0;
    uint8_t colorB = 0;
    std::string game;           // the game target (version::kGameTarget)
    uint8_t buildSha[coop::build_trust::kShaBytes] = {};  // the build claim's hash
    uint8_t buildFlags = 0;     // bit 0 = this build verified its own release signature
    std::string gameBuild;      // the game build key
};

std::vector<uint8_t> EncodeJoin(const JoinFields& f);

// Pure pre-pass over a received Join, with no side effect: extracts the game target, the nick's
// UTF-8 bytes (when a non-empty nick was sent; `outNickUtf8` may be null), the build claim (a
// pointer into `payload` and its official bit) and the game build key. False on a malformed chain,
// any length prefix overrunning the payload, so a caller refuses it; `len` must be at least 4
// (checked by the caller). Whether the key has a key's shape is the caller's to judge.
bool ExtractJoinVersionFields(const uint8_t* payload, size_t len, std::string* outGame,
                              std::string* outNickUtf8, bool* outNickGiven, const uint8_t** outSha,
                              bool* outOfficial, std::string* outGameBuild);

}  // namespace coop::net
