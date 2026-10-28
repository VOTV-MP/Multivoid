// coop/props/container_park.cpp -- the container readiness pen; see coop/props/container_park.h.

#include "coop/props/container_park.h"

#include "ue_wrap/core/log.h"

#include <map>

namespace coop::props::container_park {
namespace {

struct Parked {
    std::vector<uint8_t> blob;
    uint64_t atMs = 0;
    uint8_t authorSlot = 0;
};

std::map<uint32_t, Parked> g_parked;

bool g_joinBracketOpen = false;

// The selftest drives the real functions, so it would otherwise print fifty admissions, nine
// evictions and thirty-two expiries into the session log -- lines a reader is supposed to treat as
// defects. Quiet for its duration only, the way connect_history's selftest instance is nameless.
bool g_quiet = false;

// How many entries this author holds, and which of them is the oldest. Bounded by the map, which
// is bounded by this cap times the peer count; a linear walk of that is cheaper than a second
// index. `outHaveOldest` rather than a zero eid: 0 is a legal element id, and a sentinel that can
// be a real value fails OPEN inside a cap.
size_t CountFor(uint8_t authorSlot, uint32_t& outOldestEid, bool& outHaveOldest) {
    size_t n = 0;
    uint64_t oldest = UINT64_MAX;
    outOldestEid = 0;
    outHaveOldest = false;
    for (const auto& kv : g_parked) {
        if (kv.second.authorSlot != authorSlot) continue;
        ++n;
        if (kv.second.atMs <= oldest) {
            oldest = kv.second.atMs;
            outOldestEid = kv.first;
            outHaveOldest = true;
        }
    }
    return n;
}

}  // namespace

void Admit(uint32_t eid, uint8_t authorSlot, std::vector<uint8_t>&& blob, uint64_t nowMs) {
    // Only a NEW eid grows the table: replacing this author's own entry costs nothing.
    if (authorSlot != 0 && g_parked.find(eid) == g_parked.end()) {
        uint32_t oldestEid = 0;
        bool haveOldest = false;
        if (CountFor(authorSlot, oldestEid, haveOldest) >= kMaxParksPerClient && haveOldest) {
            if (!g_quiet) {
                UE_LOGW("container_contents: slot %u holds %zu parked slices -- evicting its "
                        "oldest (eid=%u) to admit eid=%u", static_cast<unsigned>(authorSlot),
                        kMaxParksPerClient, oldestEid, eid);
            }
            g_parked.erase(oldestEid);
        }
    }
    g_parked[eid] = Parked{std::move(blob), nowMs, authorSlot};
    if (g_quiet) return;
    UE_LOGI("container_contents: eid=%u not ready yet -- parked in the readiness pen (TTL %llu ms, author slot %u)",
            eid, static_cast<unsigned long long>(kTtlMs), static_cast<unsigned>(authorSlot));
}

void Sweep(ReplayFn replay, uint64_t nowMs) {
    if (g_parked.empty() || !replay) return;
    for (auto it = g_parked.begin(); it != g_parked.end();) {
        if (replay(it->second.blob, it->second.authorSlot)) {
            it = g_parked.erase(it);
        } else if (!g_joinBracketOpen && nowMs - it->second.atMs > kTtlMs) {
            if (!g_quiet) {
                UE_LOGW("container_contents: parked eid=%u expired after %llu ms not ready -- "
                        "dropped", it->first, static_cast<unsigned long long>(kTtlMs));
            }
            it = g_parked.erase(it);
        } else {
            ++it;
        }
    }
}

void NoteJoinBracket(bool open, uint64_t nowMs) {
    if (g_joinBracketOpen == open) return;
    g_joinBracketOpen = open;
    if (open) return;
    for (auto& kv : g_parked) kv.second.atMs = nowMs;
    if (!g_parked.empty() && !g_quiet) {
        UE_LOGI("container_contents: snapshot bracket closed -- %zu park(s) re-stamped, "
                "TTL runs from now (leak-guard)", g_parked.size());
    }
}

void Erase(uint32_t eid) { g_parked.erase(eid); }

size_t Size() { return g_parked.size(); }

void Reset() {
    g_parked.clear();
    g_joinBracketOpen = false;  // session state must not survive the session
}

namespace {
// Replay arms for the selftest: one whose container is never ready, one that always deals with its entry.
bool ReplayNever(const std::vector<uint8_t>&, uint8_t) { return false; }
bool ReplayAlways(const std::vector<uint8_t>&, uint8_t) { return true; }
}  // namespace

bool RunSelftest() {
    Reset();
    g_quiet = true;
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("container_park selftest FAIL: %s", what);
    };
    auto admit = [](uint32_t eid, uint8_t slot, uint64_t at) {
        std::vector<uint8_t> b{static_cast<uint8_t>(eid & 0xFF)};
        Admit(eid, slot, std::move(b), at);
    };

    // A client author fills its budget, and the ninth evicts its OWN oldest.
    for (uint32_t i = 0; i < kMaxParksPerClient; ++i) admit(100 + i, 1, 1000 + i);
    check(Size() == kMaxParksPerClient, "a client author fills its budget");
    admit(200, 1, 2000);
    check(Size() == kMaxParksPerClient, "the ninth entry does not grow the table");
    check(g_parked.find(100) == g_parked.end(), "the evicted entry is that author's oldest");
    check(g_parked.find(101) != g_parked.end() && g_parked.find(200) != g_parked.end(),
          "everything else it holds survives");

    // A SECOND author is unaffected by the first one being full, and cannot be evicted by it.
    admit(300, 2, 2100);
    check(Size() == kMaxParksPerClient + 1, "another author gets its own row");
    for (uint32_t i = 0; i < kMaxParksPerClient; ++i) admit(400 + i, 1, 2200 + i);
    check(g_parked.find(300) != g_parked.end(), "one author's cap never evicts another's entry");

    // Re-parking an eid already held replaces it and does not spend the budget.
    const size_t before = Size();
    admit(400, 1, 3000);
    check(Size() == before, "re-parking a held eid does not grow the table");

    // The HOST author is not capped at all -- a client parks hundreds of host slices in one join.
    Reset();
    for (uint32_t i = 0; i < kMaxParksPerClient * 4; ++i) admit(500 + i, 0, 1000);
    check(Size() == kMaxParksPerClient * 4, "host-authored parks are not capped");

    // The TTL runs only outside a bracket, and a replay that dealt with an entry retires it.
    Sweep(&ReplayNever, 1000 + kTtlMs);
    check(Size() == kMaxParksPerClient * 4, "an entry inside its TTL is kept");
    NoteJoinBracket(true, 1000);
    Sweep(&ReplayNever, 1000 + kTtlMs * 10);
    check(Size() == kMaxParksPerClient * 4, "parks do not age while a join bracket is open");
    NoteJoinBracket(false, 5000);
    Sweep(&ReplayNever, 5000 + kTtlMs);
    check(Size() == kMaxParksPerClient * 4, "the bracket's close re-stamps every entry");
    Sweep(&ReplayNever, 5001 + kTtlMs);
    check(Size() == 0, "and the TTL drops them once it is past");
    admit(900, 1, 100);
    Sweep(&ReplayAlways, 100);
    check(Size() == 0, "a replay that dealt with an entry retires it");
    admit(901, 1, 100);
    admit(902, 1, 100);
    Erase(901);
    Erase(903);
    check(Size() == 1 && g_parked.find(902) != g_parked.end(), "an erase takes its own eid and no other");

    Reset();
    g_quiet = false;
    if (pass == total) {
        UE_LOGI("container_park selftest: ALL PASS (%d checks)", total);
        return true;
    }
    UE_LOGE("container_park selftest: %d/%d checks passed", pass, total);
    return false;
}

}  // namespace coop::props::container_park
