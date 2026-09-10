// coop/dev/prop_birth_key_probe.cpp -- see header.

#include "coop/dev/prop_birth_key_probe.h"

#include "coop/config/config.h"
#include "ue_wrap/core/log.h"

#include <chrono>
#include <map>
#include <string>
#include <unordered_map>

namespace coop::dev::prop_birth_key_probe {
namespace {

// All state client game-thread-only (the finish-spawn post-hook and the drain both are). No mutex.
constexpr int    kTriesBuckets  = 10;   // 0..8 drain ticks, plus an overflow bucket
constexpr size_t kLiveCap       = 4096; // backstop on the per-actor side table
constexpr int    kPerExitLines  = 6;    // event lines per exit before the tally carries it alone
// The rig kills its peers rather than disconnecting them, so a teardown-only summary prints
// nothing in the runs this exists for. Tick() prints on a period instead; the first print lands
// as soon as anything is recorded.
constexpr auto   kVerdictPeriod = std::chrono::seconds(15);

struct Entry {
    std::wstring cls;
    std::wstring seamKey;          // as read at the seam; empty when the Key was not there yet
    bool         containerExtract = false;
};
std::unordered_map<void*, Entry> g_live;  // enqueued, not yet drained

int g_enqueued     = 0;
int g_keyAtSeam    = 0;   // ... of which the Key read back with no waiting
int g_capHit       = 0;   // refused at enqueue, pending vector full
int g_parkEvict    = 0;   // a parked key evicted before a place consumed it
int g_keyNeverRead = 0;   // left the vector with the Key still unreadable
int g_seamKeyChanged = 0; // the Key at the seam was NOT the one the entry left with
int g_tries[kTriesBuckets] = {};  // drain tick on which the Key became readable

// Ordered, so the verdict block prints the same exits in the same order run to run.
std::map<std::string, int> g_exits;      // verdict -> count
std::map<std::string, int> g_exitLines;  // verdict -> event lines already printed

std::chrono::steady_clock::time_point g_lastVerdict{};
bool g_anyRecorded = false;

bool g_dirty = false;  // something recorded since the last print

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::prop_birth_key_probe);
    return s;
}

void NoteEnqueue(void* actor, const std::wstring& cls, const std::wstring& seamKey,
                 bool containerExtract) {
    if (!IsEnabled() || !actor) return;
    g_anyRecorded = true;
    ++g_enqueued;
    const bool present = !(seamKey.empty() || seamKey == L"None");
    if (present) ++g_keyAtSeam;
    if (g_live.size() < kLiveCap)
        g_live[actor] = Entry{cls, present ? seamKey : std::wstring(), containerExtract};
    UE_LOGW("prop_birth_key_probe: ENQUEUE actor=%p cls='%ls' key-at-seam='%ls' container-extract=%d",
            actor, cls.c_str(), present ? seamKey.c_str() : L"<none>", containerExtract ? 1 : 0);
    g_dirty = true;
}

void NotePendingCapHit(void* actor) {
    if (!IsEnabled()) return;
    g_anyRecorded = true;
    ++g_capHit;
    UE_LOGW("prop_birth_key_probe: PENDING-CAP refused actor=%p -- this spawn never reached the drain",
            actor);
    g_dirty = true;
}

void NoteParkEvict(const std::wstring& key) {
    if (!IsEnabled()) return;
    g_anyRecorded = true;
    ++g_parkEvict;
    UE_LOGW("prop_birth_key_probe: PARK-EVICT key='%ls' -- a pickup's key aged out of the park with no "
            "place having consumed it", key.c_str());
    g_dirty = true;
}

void NoteKeyReadable(void* actor, int tries) {
    if (!IsEnabled()) return;
    g_anyRecorded = true;
    const int b = (tries <= 0) ? 0 : (tries >= kTriesBuckets ? kTriesBuckets - 1 : tries);
    ++g_tries[b];
    if (tries > 0) {
        UE_LOGW("prop_birth_key_probe: KEY-RESTORED actor=%p after %d drain tick(s) -- unreadable at the "
                "seam, readable once loadData had run", actor, tries);
    }
    g_dirty = true;
}

void NoteDrainExit(void* actor, const char* verdict, int tries, const std::wstring& key) {
    if (!IsEnabled()) return;
    g_anyRecorded = true;
    const std::string v = verdict ? verdict : "?";
    ++g_exits[v];
    if (key.empty()) ++g_keyNeverRead;

    Entry e;
    if (auto it = g_live.find(actor); it != g_live.end()) { e = it->second; g_live.erase(it); }
    // A Key that was there at the seam and is a DIFFERENT one at the exit is the case a flag hides:
    // the identity was not ready, it was merely occupied.
    const bool seamKeyChanged = !e.seamKey.empty() && !key.empty() && e.seamKey != key;
    if (seamKeyChanged) ++g_seamKeyChanged;

    if (int& printed = g_exitLines[v]; printed < kPerExitLines) {
        ++printed;
        UE_LOGW("prop_birth_key_probe: EXIT %s actor=%p cls='%ls' key='%ls' waited=%d tick(s) "
                "key-at-seam='%ls'%s container-extract=%d",
                v.c_str(), actor, e.cls.empty() ? L"?" : e.cls.c_str(),
                key.empty() ? L"<unread>" : key.c_str(), tries,
                e.seamKey.empty() ? L"<none>" : e.seamKey.c_str(),
                seamKeyChanged ? " CHANGED-BY-EXIT" : "", e.containerExtract ? 1 : 0);
    }

    g_dirty = true;
}

void Tick() {
    if (!IsEnabled() || !g_dirty) return;
    const auto now = std::chrono::steady_clock::now();
    if (g_lastVerdict.time_since_epoch().count() != 0 && now - g_lastVerdict < kVerdictPeriod) return;
    EmitVerdict();
}

void EmitVerdict() {
    if (!IsEnabled() || !g_anyRecorded) return;
    g_lastVerdict = std::chrono::steady_clock::now();
    g_dirty = false;
    int observed     = g_tries[0];  // entries that reached the Key read at all
    int restoredLate = 0;           // ... of which the Key needed at least one drain tick
    int atCeiling    = 0;           // ... and needed 7 or more, within one tick of the wait's end
    for (int i = 1; i < kTriesBuckets; ++i) {
        observed     += g_tries[i];
        restoredLate += g_tries[i];
        if (i >= 7) atCeiling += g_tries[i];
    }
    // Totals are cumulative: each print is the whole run so far, and the last one is the run.
    UE_LOGW("prop_birth_key_probe: VERDICT enqueued=%d key-at-seam=%d seam-key-changed=%d "
            "key-observed-at-drain=%d restored-late=%d at-ceiling(>=7)=%d key-never-read=%d "
            "cap-hit=%d park-evict=%d",
            g_enqueued, g_keyAtSeam, g_seamKeyChanged, observed, restoredLate, atCeiling,
            g_keyNeverRead, g_capHit, g_parkEvict);
    UE_LOGW("prop_birth_key_probe: tick histogram 0=%d 1=%d 2=%d 3=%d 4=%d 5=%d 6=%d 7=%d 8=%d 9+=%d",
            g_tries[0], g_tries[1], g_tries[2], g_tries[3], g_tries[4],
            g_tries[5], g_tries[6], g_tries[7], g_tries[8], g_tries[9]);
    for (const auto& [verdict, n] : g_exits) {
        UE_LOGW("prop_birth_key_probe: exit '%s' = %d", verdict.c_str(), n);
    }
    // What the histogram decides: whether the wait is a live bound on the loss, or slack. An EMPTY
    // histogram must never read as a clean one -- every entry can exit before the Key is ever read,
    // and "none waited" would then be a claim about nothing.
    const char* reading =
        (g_enqueued == 0)     ? "NO DATA -- no client keyed spawn was enqueued; drive the seam (place, eject, extract) first"
        : (observed == 0)     ? "NO KEY OBSERVED -- entries were enqueued but every one exited before the Key was read, so nothing here rules the wait in or out; read the exit tally above"
        : (g_seamKeyChanged)  ? "KEY AT THE SEAM IS NOT THE IDENTITY -- a Key was there and the entry left under a different one, so a lane reading identity at the seam reads the wrong prop"
        : (restoredLate == 0) ? "KEY IS READY AT THE SEAM -- every Key read back with zero waiting, and the one at the seam is the one the entry left with, so the wait never ran and cannot be the loss"
        : (atCeiling == 0)    ? "KEY RESTORES EARLY -- late restores exist, none near the ceiling; the wait has headroom and is not the loss"
                              : "KEY RESTORES AT THE CEILING -- the wait is a live bound, and a declared readiness point removes it at the root";
    UE_LOGW("prop_birth_key_probe: reading :: %s", reading);
}

}  // namespace coop::dev::prop_birth_key_probe
