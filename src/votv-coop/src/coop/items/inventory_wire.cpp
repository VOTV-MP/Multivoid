// coop/items/inventory_wire.cpp -- see coop/items/inventory_wire.h.
//
// The byte primitives live in coop/items/save_record_wire; what stays here is the player-scoped
// envelope: the version byte, the deflated body (the record, the vitals and the pose), and the parts a body past one
// transport blob travels as.

#include "coop/items/inventory_wire.h"

#include "coop/items/save_record_wire.h"

#include "miniz.h"

#include <cstddef>
#include <cstring>

namespace coop::inventory_wire {
namespace {

namespace W = coop::save_record_wire;

using coop::player_profile::Profile;
using ue_wrap::vitals::Snapshot;

// The vitals' scalar order on the wire. Appending is a layout change (bump kVersion).
constexpr float Snapshot::* kVitalScalars[] = {
    &Snapshot::health, &Snapshot::maxHealth, &Snapshot::food, &Snapshot::sleep,
    &Snapshot::coffeePower, &Snapshot::gasolinepilled, &Snapshot::strength,
};
constexpr uint32_t kMaxFoods = 4096;  // distinct foods a tolerance list may name

// The profile's own bytes, the layout of version 4 less its version byte: the record, the vitals (behind a present byte)
// and the pose.
std::vector<uint8_t> SerializeBody(const Profile& p) {
    std::vector<uint8_t> b;
    b.reserve(p.record.size() + 128);
    W::AppU32(b, static_cast<uint32_t>(p.record.size()));
    b.insert(b.end(), p.record.begin(), p.record.end());
    W::AppU8(b, p.hasVitals ? 1 : 0);
    if (p.hasVitals) {
        const Snapshot& vitals = p.vitals;
        for (auto m : kVitalScalars) W::AppF32(b, vitals.*m);
        const size_t foods = vitals.foodConsumed.size() < vitals.foodTolerance.size()
                                 ? vitals.foodConsumed.size() : vitals.foodTolerance.size();
        W::AppU32(b, static_cast<uint32_t>(foods));
        for (size_t i = 0; i < foods; ++i) {
            W::AppWStr(b, vitals.foodConsumed[i]);
            W::AppF32(b, vitals.foodTolerance[i]);
        }
    }
    W::AppU8(b, p.pose.valid ? 1 : 0);
    W::AppF32(b, p.pose.x); W::AppF32(b, p.pose.y); W::AppF32(b, p.pose.z); W::AppF32(b, p.pose.yaw);
    return b;
}

// Parse a body (`b` from `o`, which must run to the end of the vector). False on any malformed byte.
bool ParseBody(const std::vector<uint8_t>& b, size_t o, Profile& p) {
    uint32_t n;
    if (!W::RdU32(b, o, n) || n > b.size() - o) return false;
    p.record.assign(b.begin() + static_cast<std::ptrdiff_t>(o), b.begin() + static_cast<std::ptrdiff_t>(o + n));
    o += n;
    uint8_t hasVitals = 0;
    if (!W::RdU8(b, o, hasVitals)) return false;
    p.hasVitals = hasVitals != 0;
    if (p.hasVitals) {
        for (auto m : kVitalScalars) if (!W::RdF32(b, o, p.vitals.*m)) return false;
        if (!W::RdU32(b, o, n) || n > kMaxFoods || !W::Feasible(n, b, o)) return false;
        p.vitals.foodConsumed.resize(n);
        p.vitals.foodTolerance.resize(n);
        for (uint32_t i = 0; i < n; ++i) {
            if (!W::RdWStr(b, o, p.vitals.foodConsumed[i])) return false;
            if (!W::RdF32(b, o, p.vitals.foodTolerance[i])) return false;
        }
    }
    uint8_t valid = 0;
    if (!W::RdU8(b, o, valid)) return false;
    p.pose.valid = valid != 0;
    return W::RdF32(b, o, p.pose.x) && W::RdF32(b, o, p.pose.y) && W::RdF32(b, o, p.pose.z) &&
           W::RdF32(b, o, p.pose.yaw) && o == b.size();  // nothing may follow the pose
}

}  // namespace

std::vector<uint8_t> Serialize(const Profile& p) {
    const std::vector<uint8_t> body = SerializeBody(p);
    // Deflate: the record is the game's tagged-property stream, in which every item repeats the same property names
    // (with their GUID suffixes) and class paths, so a full inventory shrinks about forty times. Level 9 while the body
    // is small (the usual case), 6 for a huge one, where the last few percent cost more time than they save.
    const int level = body.size() <= (1u << 20) ? 9 : 6;
    mz_ulong cap = mz_compressBound(static_cast<mz_ulong>(body.size()));
    std::vector<uint8_t> b;
    b.push_back(kVersion);
    W::AppU32(b, static_cast<uint32_t>(body.size()));
    const size_t head = b.size();
    b.resize(head + cap);
    if (mz_compress2(b.data() + head, &cap, body.data(), static_cast<mz_ulong>(body.size()), level) != MZ_OK) {
        // Deflate cannot fail on memory it was given the bound of; if it does, the plain form is still valid.
        std::vector<uint8_t> plain;
        plain.reserve(1 + body.size());
        plain.push_back(kVersionPlain);
        plain.insert(plain.end(), body.begin(), body.end());
        return plain;
    }
    b.resize(head + cap);
    return b;
}

bool Deserialize(const std::vector<uint8_t>& b, Profile& p) {
    p = Profile{};
    size_t o = 0;
    uint8_t ver;
    if (!W::RdU8(b, o, ver)) return false;
    if (ver == kVersionPlain) return ParseBody(b, o, p);
    if (ver != kVersion) return false;
    uint32_t rawLen;
    if (!W::RdU32(b, o, rawLen) || rawLen > kMaxBodyBytes) return false;
    const size_t packed = b.size() - o;
    // Deflate cannot expand a byte past 1032 bytes of output: a header that claims more is a lie told to make this
    // allocate, and it is refused before it does.
    if (packed == 0 || static_cast<uint64_t>(rawLen) > static_cast<uint64_t>(packed) * 1032u + 64u) return false;
    std::vector<uint8_t> body(rawLen);
    mz_ulong outLen = rawLen;
    if (mz_uncompress(body.data(), &outLen, b.data() + o, static_cast<mz_ulong>(packed)) != MZ_OK || outLen != rawLen)
        return false;
    return ParseBody(body, 0, p);
}

std::vector<std::vector<uint8_t>> SplitParts(const std::vector<uint8_t>& blob, size_t maxPart, uint16_t group) {
    std::vector<std::vector<uint8_t>> parts;
    if (blob.size() <= maxPart) {
        parts.push_back(blob);
        return parts;
    }
    if (maxPart <= kPartHeader) return parts;
    const size_t per = maxPart - kPartHeader;
    const size_t count = (blob.size() + per - 1) / per;
    if (count > kMaxParts) return parts;
    for (size_t i = 0; i < count; ++i) {
        const size_t from = i * per;
        const size_t take = blob.size() - from < per ? blob.size() - from : per;
        std::vector<uint8_t> part;
        part.reserve(kPartHeader + take);
        part.push_back(kPartMarker);
        W::AppU16(part, group);
        part.push_back(static_cast<uint8_t>(i));
        part.push_back(static_cast<uint8_t>(count));
        part.insert(part.end(), blob.begin() + static_cast<std::ptrdiff_t>(from),
                    blob.begin() + static_cast<std::ptrdiff_t>(from + take));
        parts.push_back(std::move(part));
    }
    return parts;
}

void PartJoiner::ClearSlot(uint8_t slot) {
    for (auto it = slots_.begin(); it != slots_.end(); ++it)
        if (it->first == slot) { slots_.erase(it); return; }
}

bool PartJoiner::Feed(uint8_t slot, std::vector<uint8_t>& blob, std::vector<uint8_t>& out) {
    if (blob.empty() || blob[0] != kPartMarker) {
        out = std::move(blob);
        return true;
    }
    size_t o = 1;
    uint16_t id = 0;
    uint8_t idx = 0, count = 0;
    if (blob.size() < kPartHeader || !W::RdU16(blob, o, id) || !W::RdU8(blob, o, idx) || !W::RdU8(blob, o, count) ||
        count < 2 || count > kMaxParts || idx >= count) {
        ClearSlot(slot);
        return false;
    }
    Group* g = nullptr;
    for (auto& kv : slots_)
        if (kv.first == slot) { g = &kv.second; break; }
    if (idx == 0) {
        if (!g) { slots_.emplace_back(slot, Group{}); g = &slots_.back().second; }
        *g = Group{};
        g->id = id;
        g->count = count;
    } else if (!g || g->id != id || g->count != count || g->got != idx) {
        ClearSlot(slot);
        return false;
    }
    g->bytes.insert(g->bytes.end(), blob.begin() + static_cast<std::ptrdiff_t>(kPartHeader), blob.end());
    ++g->got;
    if (g->got < g->count) return false;
    out = std::move(g->bytes);
    ClearSlot(slot);
    return true;
}

}  // namespace coop::inventory_wire
