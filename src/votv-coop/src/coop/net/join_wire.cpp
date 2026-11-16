// coop/net/join_wire.cpp -- see coop/net/join_wire.h.

#include "coop/net/join_wire.h"

#include "coop/text/utf8_codec.h"
#include "ue_wrap/core/game_build.h"

#include <algorithm>
#include <cstring>

namespace coop::net {

namespace {
constexpr size_t kSkinMax = 48;
constexpr size_t kGameMax = 23;
constexpr size_t kShaBytes = coop::build_trust::kShaBytes;

void PutText(std::vector<uint8_t>& out, const std::string& s, size_t cap) {
    const size_t n = std::min(s.size(), cap);
    out.push_back(static_cast<uint8_t>(n));
    out.insert(out.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
}
}  // namespace

std::vector<uint8_t> EncodeJoin(const JoinFields& f) {
    std::vector<uint8_t> out(4);
    std::memcpy(out.data(), &f.eid, 4);
    // Capped on a character boundary: a raw resize manufactures the ill-formed tail the receive
    // boundary refuses.
    PutText(out, coop::text::CapUtf8Bytes(f.nick, coop::text::kNickMaxBytes), coop::text::kNickMaxBytes);
    PutText(out, f.skin, kSkinMax);
    out.push_back(f.prefsFlags);
    out.push_back(f.colorCustom ? 1 : 0);
    out.push_back(f.colorR);
    out.push_back(f.colorG);
    out.push_back(f.colorB);
    PutText(out, f.game, kGameMax);
    out.insert(out.end(), f.buildSha, f.buildSha + kShaBytes);
    out.push_back(f.buildFlags);
    PutText(out, f.gameBuild, ue_wrap::game_build::kKeyMax);
    return out;
}

bool ExtractJoinVersionFields(const uint8_t* payload, size_t len, std::string* outGame,
                              std::string* outNickUtf8, bool* outNickGiven, const uint8_t** outSha,
                              bool* outOfficial, std::string* outGameBuild) {
    size_t off = 4;  // [u32 eid]
    // [u8 nicklen][nick]
    if (off + 1 > len) return false;
    {
        const size_t n = payload[off];
        if (off + 1 + n > len) return false;
        if (n > 0 && outNickUtf8) {
            outNickUtf8->assign(reinterpret_cast<const char*>(payload + off + 1), n);
            *outNickGiven = true;
        }
        off += 1 + n;
    }
    // [u8 skinlen][skin]
    {
        if (off + 1 > len) return false;
        const size_t n = payload[off];
        if (off + 1 + n > len) return false;
        off += 1 + n;
    }
    // [u8 flags] + [u8 has][r][g][b]
    if (off + 1 + 4 > len) return false;
    off += 1 + 4;
    // [u8 gamelen][game]
    if (off + 1 > len) return false;
    const size_t n = payload[off];
    if (off + 1 + n > len || n > kGameMax) return false;
    outGame->assign(reinterpret_cast<const char*>(payload + off + 1), n);
    off += 1 + n;
    // [sha256 32][u8 flags] -- the build claim; bit 0 = the build verified its own release signature.
    if (off + kShaBytes + 1 > len) return false;
    *outSha = payload + off;
    *outOfficial = (payload[off + kShaBytes] & 1) != 0;
    off += kShaBytes + 1;
    // [u8 len][game build key]
    if (off + 1 > len) return false;
    const size_t k = payload[off];
    if (off + 1 + k > len || k > ue_wrap::game_build::kKeyMax) return false;
    outGameBuild->assign(reinterpret_cast<const char*>(payload + off + 1), k);
    return true;
}

}  // namespace coop::net
