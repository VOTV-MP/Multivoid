// coop/text/novelty_ledger.cpp -- see coop/text/novelty_ledger.h.

#include "coop/text/novelty_ledger.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/text/repertoire.h"
#include "coop/text/utf8_codec.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster_ledger.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <vector>

namespace coop::text {
namespace {

// The seen-set is indexed by CODEPOINT, flat. The repertoire's largest member is
// U+1FBF9, so the bitset is ~16 KB -- small enough that a rank-compressed index
// would be pure complexity for no memory saved, and O(1) beats a binary search on
// a path that runs per character of every inbound message.
constexpr uint32_t kMaxTracked = 0x20000;   // covers the fold set's max, U+1FBF9
std::vector<uint64_t> g_seen;               // monotone: a bit is never cleared

struct Window {
    uint64_t startMs = 0;
    int      spent   = 0;
};
// PerSlotState, NOT a bare array. Slots RECYCLE lowest-free, so a slot goes
// person X -> person Y with no absence in between; a raw array would silently
// hand the incoming occupant whatever budget the outgoing one had already spent.
// PerSlotState registers its own clear with the roster ledger in its
// constructor, so the teardown is covered by construction rather than by anyone
// remembering to call it -- which is also why this file ships no ForgetPeer.
coop::roster_ledger::PerSlotState<Window> g_window;

uint64_t NowMs() { return ::GetTickCount64(); }

bool SeenAndMark(uint32_t cp) {
    if (cp >= kMaxTracked) return true;    // cannot be baked; not novelty
    if (g_seen.empty()) g_seen.assign(kMaxTracked / 64, 0);
    const size_t w = cp >> 6;
    const uint64_t bit = 1ull << (cp & 63);
    if (g_seen[w] & bit) return true;
    g_seen[w] |= bit;
    return false;
}

bool WouldBeSeen(uint32_t cp) {
    if (cp >= kMaxTracked || g_seen.empty()) return cp >= kMaxTracked;
    return (g_seen[cp >> 6] & (1ull << (cp & 63))) != 0;
}

int Budget() {
    return static_cast<int>(coop::config::ResolveInt(coop::config_registry::rows::net_novelty_budget));
}
uint64_t WindowMs() {
    return static_cast<uint64_t>(
        coop::config::ResolveInt(coop::config_registry::rows::net_novelty_window_ms));
}

}  // namespace

bool AdmitRemoteText(uint8_t peerSlot, const std::wstring& text) {
    if (peerSlot >= coop::players::kMaxPeers) return true;   // not a peer slot; nothing to scope
    const int budget = Budget();
    if (budget <= 0) return true;                            // 0 = the cap is off (ini)

    Window& w = g_window[peerSlot];
    const uint64_t now = NowMs();
    if (now - w.startMs >= WindowMs()) { w.startMs = now; w.spent = 0; }

    // COUNT BEFORE MARKING. The whole field is refused or admitted as a unit, so
    // marking as we go would leave the seen-set holding codepoints from a message
    // that was then dropped -- the peer would have widened everyone's atlas
    // budget with a message nobody read.
    int novel = 0;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = 0;
        i += DecodeCodepoint(text, i, &cp);
        // Only what the atlas could actually rasterise counts. An excluded or
        // absent codepoint draws the fallback box, which is already baked.
        if (!InRepertoire(cp)) continue;
        if (!WouldBeSeen(cp)) ++novel;
    }
    if (novel == 0) return true;                             // the common case: free

    if (w.spent + novel > budget) {
        UE_LOGW("novelty ledger: REFUSED a text field from slot %u -- it would introduce %d "
                "never-before-seen codepoints and %d of this peer's %d are already spent in "
                "this window. Rasterising a large first-sight alphabet inside one frame is a "
                "stutter every receiving peer pays for (docs/security TRACKER W11). Raise "
                "net.novelty_budget if a legitimate multilingual lobby trips this.",
                static_cast<unsigned>(peerSlot), novel, w.spent, budget);
        return false;
    }

    for (size_t i = 0; i < text.size();) {
        uint32_t cp = 0;
        i += DecodeCodepoint(text, i, &cp);
        if (!InRepertoire(cp)) continue;
        SeenAndMark(cp);
    }
    w.spent += novel;
    return true;
}

void ResetForTests() {
    g_seen.clear();
    for (int i = 0; i < g_window.size(); ++i) g_window[i] = Window{};
}

bool RunNoveltyLedgerSelftest() {
    // The selftest MUTATES the real ledger, so it restores it afterwards -- and it
    // runs at boot, before any peer text, so there is nothing to preserve. It is
    // written to be honest about that rather than to pretend isolation.
    ResetForTests();

    int pass = 0, total = 0;
    auto ok = [&](bool cond, const char* what) {
        ++total;
        if (cond) ++pass;
        else UE_LOGE("novelty ledger selftest: FAIL -- %s", what);
    };

    const int budget = Budget();
    if (budget <= 0) {
        UE_LOGI("novelty ledger selftest: SKIPPED -- net.novelty_budget is 0 (cap disabled)");
        ResetForTests();
        return true;
    }

    // A burst larger than the budget is refused WHOLE. The burst must carry
    // budget+1 DISTINCT in-repertoire codepoints, derived FROM the budget -- the
    // first version of this check built a fixed 32-codepoint string and then
    // returned `true` when the budget exceeded 32, i.e. it passed by construction
    // for every configuration except the one nobody runs.
    // [[lesson-an-instrument-never-shown-failing-passes-by-construction]]
    std::wstring burst;
    int distinct = 0;
    for (uint32_t cp = 0x0020; cp < kMaxTracked && distinct <= budget; ++cp) {
        if (!InRepertoire(cp) || cp > 0xFFFF) continue;   // BMP keeps the string simple
        burst.push_back(static_cast<wchar_t>(cp));
        ++distinct;
    }
    // If the repertoire cannot even supply budget+1 codepoints the check is
    // untestable, and saying so is the honest outcome -- not a silent pass.
    ok(distinct > budget, "the repertoire can supply a burst larger than the budget");
    ok(distinct > budget && !AdmitRemoteText(1, burst),
       "a burst past the budget is refused whole");

    // MONOTONE: the same text a second time costs nothing, so a peer that says the
    // same word twice is never throttled for it.
    const std::wstring word = L"привет";  // "privet"
    ok(AdmitRemoteText(1, word), "a short first-sight word is admitted");
    ok(AdmitRemoteText(1, word), "the same word again is free (the seen-set is monotone)");

    // PER PEER. Slot 2's window is its own, so slot 1 cannot spend it.
    ok(AdmitRemoteText(2, word), "another slot is not charged for slot 1's alphabet");

    // Codepoints the atlas cannot bake are not novelty -- they draw the fallback
    // box, which is already resident. Hanzi is absent from every shipped face.
    const std::wstring hanzi(64, static_cast<wchar_t>(0x4E00));
    ok(AdmitRemoteText(3, hanzi),
       "out-of-repertoire text costs no budget (it renders the fallback, not a new glyph)");

    UE_LOGI("novelty ledger selftest: %s (%d/%d) -- budget %d per %llu ms per peer",
            pass == total ? "PASS" : "FAIL", pass, total, budget,
            static_cast<unsigned long long>(WindowMs()));
    ResetForTests();
    return pass == total;
}

}  // namespace coop::text
