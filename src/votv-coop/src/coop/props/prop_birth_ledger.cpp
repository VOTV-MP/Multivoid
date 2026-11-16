// coop/props/prop_birth_ledger.cpp -- see coop/props/prop_birth_ledger.h.

#include "coop/props/prop_birth_ledger.h"

#include "coop/props/prop_element_tracker.h"
#include "ue_wrap/core/log.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace coop::prop_birth_ledger {
namespace {

namespace PT = coop::prop_element_tracker;

struct BirthTally { uint64_t records = 0; uint64_t bytes = 0; };
struct BirthLedger {
    BirthTally total;
    std::map<std::wstring, BirthTally> byClass;
    uint64_t declined  = 0;   // a covered birth the publisher sent nothing for
    uint64_t uncovered = 0;   // a birth whose class carries no save state of its own
};
BirthLedger g_birthWindow;
BirthLedger g_birthSession;
uint64_t g_birthWindowStartMs = 0;
uint64_t g_birthSessionStartMs = 0;
constexpr uint64_t kBirthReportMs = 60000;

std::string TopClasses(const BirthLedger& l) {
    std::vector<std::pair<std::wstring, BirthTally>> v(l.byClass.begin(), l.byClass.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
        return a.second.bytes > b.second.bytes;
    });
    std::string out;
    for (size_t i = 0; i < v.size() && i < 5; ++i) {
        char row[160];
        std::snprintf(row, sizeof(row), "%s%ls %llu/%lluB", i ? ", " : "", v[i].first.c_str(),
                      static_cast<unsigned long long>(v[i].second.records),
                      static_cast<unsigned long long>(v[i].second.bytes));
        out += row;
    }
    return out.empty() ? std::string("none") : out;
}

void ReportBirthLedger(const char* when, const BirthLedger& l, uint64_t spanMs) {
    const double secs = spanMs > 0 ? static_cast<double>(spanMs) / 1000.0 : 1.0;
    UE_LOGI("prop_save_data: HOST BIRTH RECORDS %s (%.0f s) -- %llu sent, %llu B (%.1f B/s, %.1f "
            "records/min), %llu declined, %llu births with no save state of their own; top by "
            "bytes: %s", when, secs, static_cast<unsigned long long>(l.total.records),
            static_cast<unsigned long long>(l.total.bytes),
            static_cast<double>(l.total.bytes) / secs,
            static_cast<double>(l.total.records) * 60.0 / secs,
            static_cast<unsigned long long>(l.declined),
            static_cast<unsigned long long>(l.uncovered), TopClasses(l).c_str());
}

}  // namespace

void NoteUncovered() {
    ++g_birthWindow.uncovered;
    ++g_birthSession.uncovered;
}

void NoteDeclined() {
    ++g_birthWindow.declined;
    ++g_birthSession.declined;
}

void NoteSent(const std::wstring& className, size_t bytes) {
    for (BirthLedger* l : {&g_birthWindow, &g_birthSession}) {
        l->total.records += 1;
        l->total.bytes += bytes;
        BirthTally& t = l->byClass[className];
        t.records += 1;
        t.bytes += bytes;
    }
}

void Tick(uint64_t now) {
    if (!PT::SessionIsHost()) return;
    if (!g_birthWindowStartMs) { g_birthWindowStartMs = g_birthSessionStartMs = now; return; }
    if (now - g_birthWindowStartMs < kBirthReportMs) return;
    // A minute with no host birth says nothing, and the session line under it has not moved.
    const bool quiet = !g_birthWindow.total.records && !g_birthWindow.declined && !g_birthWindow.uncovered;
    if (quiet) { g_birthWindowStartMs = now; return; }
    ReportBirthLedger("last minute", g_birthWindow, now - g_birthWindowStartMs);
    ReportBirthLedger("session so far", g_birthSession, now - g_birthSessionStartMs);
    g_birthWindow = BirthLedger{};
    g_birthWindowStartMs = now;
}

void OnDisconnect(uint64_t now) {
    if (g_birthSessionStartMs &&
        (g_birthSession.total.records || g_birthSession.declined || g_birthSession.uncovered))
        ReportBirthLedger("SESSION", g_birthSession, now - g_birthSessionStartMs);
    g_birthWindow = BirthLedger{};
    g_birthSession = BirthLedger{};
    g_birthWindowStartMs = g_birthSessionStartMs = 0;
}

}  // namespace coop::prop_birth_ledger
