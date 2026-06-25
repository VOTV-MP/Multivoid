// coop/dev/eid_lifetime_trace.cpp -- see header.

#include "coop/dev/eid_lifetime_trace.h"

#include "coop/ini_config.h"
#include "ue_wrap/log.h"

#include <unordered_map>
#include <unordered_set>

namespace coop::dev::eid_lifetime_trace {
namespace {

// All state HOST game-thread-only (capture + the snapshot drain are GT). No mutex.
std::unordered_map<void*, uint32_t> g_captureEid;  // actor -> eid recorded at save-capture
std::unordered_set<void*> g_checked;               // actors already wire-checked this join (dedup)
int g_captured = 0;     // capture-eid records
int g_wireChecked = 0;  // wire expressions that HAD a recorded capture-eid (the comparable set)
int g_matched = 0;      // ... of which wire-eid == capture-eid
int g_mismatched = 0;   // ... of which they DIVERGED
int g_wireOnly = 0;     // wire expressions with NO recorded capture-eid (informational, not a verdict input)
constexpr size_t kCap = 16384;  // probe backstop

}  // namespace

bool IsEnabled() {
    static const bool s = coop::ini_config::IsIniKeyTrue("eid_lifetime_trace");
    return s;
}

void RecordCaptureEid(void* actor, uint32_t eid) {
    if (!IsEnabled() || !actor || eid == 0) return;
    if (g_captureEid.size() >= kCap) return;
    auto [it, inserted] = g_captureEid.try_emplace(actor, eid);
    if (inserted) ++g_captured;
    else it->second = eid;  // a re-capture of the same actor refreshes (idempotent self-seed); keep the latest
}

void CheckWireEid(void* actor, uint32_t wireEid) {
    if (!IsEnabled() || !actor || wireEid == 0) return;
    if (!g_checked.insert(actor).second) return;  // one check per actor (the drain may revisit)
    auto it = g_captureEid.find(actor);
    if (it == g_captureEid.end()) { ++g_wireOnly; return; }  // expressed but never captured -> not a verdict input
    ++g_wireChecked;
    if (it->second == wireEid) {
        ++g_matched;
    } else {
        ++g_mismatched;
        if (g_mismatched <= 10) {
            UE_LOGW("eid_lifetime_trace: MISMATCH actor=%p capture-eid=%u wire-eid=%u -- the host re-minted "
                    "this native's eid between capture and expression (a bind keyed on the capture-eid would "
                    "miss the host's wire-eid)", actor, it->second, wireEid);
        }
    }
}

void EmitVerdict() {
    if (!IsEnabled()) return;
    const char* verdict =
        (g_wireChecked == 0) ? "N/A (no native was both captured AND wire-expressed this join -- retry / check the path)"
        : (g_mismatched == 0) ? "STABLE -> capture-eid == wire-eid for every comparable native; the bind model is sound (S8.2 PASS)"
                              : "DIVERGES -> some capture-eid != wire-eid; the eid re-mints between capture and expression -> mini-design (b) needs a fix BEFORE the bind";
    UE_LOGW("eid_lifetime_trace: VERDICT captured=%d wire-checked=%d matched=%d mismatched=%d wire-only=%d :: %s",
            g_captured, g_wireChecked, g_matched, g_mismatched, g_wireOnly, verdict);
    UE_LOGW("eid_lifetime_trace: (captured = host save-capture eids recorded; wire-checked = expressions that "
            "had a recorded capture-eid; matched = wire-eid==capture-eid; wire-only = expressed natives never "
            "captured. mismatched>0 means the eid is NOT stable capture->wire -> the bind would not match.)");
    // clear for the next join
    g_captureEid.clear();
    g_checked.clear();
    g_captured = g_wireChecked = g_matched = g_mismatched = g_wireOnly = 0;
}

}  // namespace coop::dev::eid_lifetime_trace
