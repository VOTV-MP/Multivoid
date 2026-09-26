// coop/interactables/meadow_db_hash.cpp -- see coop/interactables/meadow_db_hash.h.

#include "coop/interactables/meadow_db_hash.h"

#include "coop/interactables/signal_wire.h"

#include "ue_wrap/desk/meadow_store.h"

#include <cstring>

namespace coop::meadow_db_hash {

namespace MS = ue_wrap::meadow_store;
namespace SD = ue_wrap::signal_dynamic;

uint64_t HashRow(const SD::Row& r, std::vector<uint8_t>& scratch) {
    scratch = coop::signal_wire::Serialize(r, /*adopt=*/false);
    return coop::signal_wire::ContentHash(scratch);
}

bool HashStore(std::map<uint64_t, int32_t>& counts, std::vector<uint64_t>* seq) {
    counts.clear();
    if (seq) seq->clear();
    const int32_t n = MS::Count();
    if (n < 0) return false;
    std::vector<uint8_t> scratch;
    SD::Row r;
    if (seq) seq->reserve(static_cast<size_t>(n));
    for (int32_t i = 0; i < n; ++i) {
        if (!MS::ReadRow(i, r)) return false;
        const uint64_t h = HashRow(r, scratch);
        ++counts[h];
        if (seq) seq->push_back(h);
    }
    return true;
}

int32_t IndexOf(uint64_t hash) {
    const int32_t n = MS::Count();
    if (n < 0) return -1;
    std::vector<uint8_t> scratch;
    SD::Row r;
    for (int32_t i = 0; i < n; ++i) {
        if (!MS::ReadRow(i, r)) continue;
        if (HashRow(r, scratch) == hash) return i;
    }
    return -1;
}

std::vector<uint8_t> OrderBlob(const std::vector<uint64_t>& seq) {
    const uint16_t n = static_cast<uint16_t>(seq.size() > 0xFFFF ? 0xFFFF : seq.size());
    std::vector<uint8_t> b(2 + static_cast<size_t>(n) * 8);
    std::memcpy(b.data(), &n, 2);
    for (uint16_t i = 0; i < n; ++i)
        std::memcpy(b.data() + 2 + static_cast<size_t>(i) * 8, &seq[i], 8);
    return b;
}

bool ParseOrderBlob(const std::vector<uint8_t>& b, std::vector<uint64_t>& out) {
    if (b.size() < 2) return false;
    uint16_t n = 0;
    std::memcpy(&n, b.data(), 2);
    if (b.size() < 2 + static_cast<size_t>(n) * 8) return false;
    out.resize(n);
    for (uint16_t i = 0; i < n; ++i)
        std::memcpy(&out[i], b.data() + 2 + static_cast<size_t>(i) * 8, 8);
    return true;
}

std::vector<int32_t> PermutationTo(const std::vector<uint64_t>& live, const std::vector<uint64_t>& target) {
    const int32_t n = static_cast<int32_t>(live.size());
    std::vector<int32_t> perm;
    perm.reserve(static_cast<size_t>(n));
    std::vector<bool> used(static_cast<size_t>(n), false);
    for (uint64_t h : target) {
        for (int32_t i = 0; i < n; ++i) {
            if (!used[static_cast<size_t>(i)] && live[static_cast<size_t>(i)] == h) {
                used[static_cast<size_t>(i)] = true;
                perm.push_back(i);
                break;
            }
        }
    }
    for (int32_t i = 0; i < n; ++i)
        if (!used[static_cast<size_t>(i)]) perm.push_back(i);
    return perm;
}

}  // namespace coop::meadow_db_hash
