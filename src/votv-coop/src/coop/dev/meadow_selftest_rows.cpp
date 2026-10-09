// coop/dev/meadow_selftest_rows.cpp -- see coop/dev/meadow_selftest_rows.h.

#include "coop/dev/meadow_selftest_rows.h"

#include "coop/interactables/meadow_db_hash.h"

#include "ue_wrap/desk/meadow_store.h"

#include <map>

namespace coop::dev::meadow_selftest_rows {
namespace {

namespace MH = coop::meadow_db_hash;
namespace MS = ue_wrap::meadow_store;

const wchar_t* const kName[kRowCount] = {L"MEADOW-SELFTEST-A", L"MEADOW-SELFTEST-B", L"MEADOW-SELFTEST-A2",
                                         L"MEADOW-SELFTEST-C", L"MEADOW-SELFTEST-X", L"MEADOW-SELFTEST-Y",
                                         L"MEADOW-SELFTEST-X2", L"MEADOW-SELFTEST-W", L"MEADOW-SELFTEST-Z"};
const wchar_t* const kId[kRowCount] = {L"selftest-a", L"selftest-b", L"selftest-a", L"selftest-c", L"selftest-x",
                                       L"selftest-y", L"selftest-x2", L"selftest-w", L"selftest-z"};

}  // namespace

namespace SD = ue_wrap::signal_dynamic;

const wchar_t* Name(RowId r) {
    return kName[r];
}

// A drill row, built alike on both peers so its content hash is the same on each.
SD::Row MakeRow(RowId r) {
    SD::Row row;
    row.name = kName[r];
    row.id = kId[r];
    row.object.clear();  // empty is NAME_None; the literal "None" fails LeafToFName's failed-intern check
    row.signal.clear();
    row.level = 1;
    row.size = 1.0f;
    row.decoded = 1.0f;
    row.hasData = true;
    return row;
}

uint64_t HashOf(RowId r) {
    static uint64_t h[kRowCount] = {};
    std::vector<uint8_t> scratch;
    if (!h[r]) h[r] = MH::HashRow(MakeRow(r), scratch);
    return h[r];
}

bool ReadSequence(std::vector<uint64_t>& out) {
    std::map<uint64_t, int32_t> counts;
    return MH::HashStore(counts, &out);
}

int32_t IndexOf(const std::vector<uint64_t>& seq, uint64_t h) {
    for (size_t i = 0; i < seq.size(); ++i)
        if (seq[i] == h) return static_cast<int32_t>(i);
    return -1;
}

int32_t IndexOf(RowId r) {
    return MH::IndexOf(HashOf(r));
}

uint64_t Digest(const std::vector<uint64_t>& seq) {
    uint64_t d = 0;
    for (uint64_t h : seq) d += h;
    return d;
}

bool TakeOut(RowId r) {
    std::vector<uint64_t> seq;
    if (!ReadSequence(seq)) return false;
    const int32_t idx = IndexOf(seq, HashOf(r));
    return idx < 0 || (MS::ApplyRemoveSignal(idx) && IndexOf(r) < 0);
}

bool TakeOutAll() {
    bool out = true;
    for (RowId r : kAllRows) out = TakeOut(r) && out;
    return out;
}

}  // namespace coop::dev::meadow_selftest_rows
