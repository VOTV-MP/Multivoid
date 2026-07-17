// coop/dev/join_window_pos_trace.cpp -- see header.

#include "coop/dev/join_window_pos_trace.h"

#include "coop/element/registry.h"     // Registry::Get (resolve the final bound actor by eid)
#include "coop/element/element.h"      // Element::GetActor / GetInternalIdx
#include "coop/config/config.h"
#include "ue_wrap/engine/engine.h"            // GetActorLocation, FVector
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"        // IsLiveByIndex

#include <cmath>
#include <unordered_map>

namespace coop::dev::join_window_pos_trace {
namespace {

namespace R = ue_wrap::reflection;

// Per-keyed-prop record. All state game-thread-only (snapshot OnSpawn + sweep re-bind + quiescence
// verdict are GT). No mutex.
struct Rec {
    uint32_t eid = 0;
    uint32_t snapshotSeq = 0;   // 0 = no snapshot expression seen
    uint32_t recreateSeq = 0;   // 0 = no loadObjects-recreate re-bind seen
    ue_wrap::FVector hostPos{};   // snapshot-carried host pos
    ue_wrap::FVector snapCurPos{}; // actor pos at snapshot time
    ue_wrap::FVector recreatePos{}; // recreate actor pos at re-bind (candidate save-pos)
    bool hostHeld = false;      // drive-skip / local-grab-skip fired at snapshot (host held at snap)
    bool sawSnapshot = false;
    bool sawRecreate = false;
};

std::unordered_map<std::wstring, Rec> g_rec;  // key -> record
uint32_t g_seq = 0;           // monotonic order counter (proves snapshot-vs-recreate ordering)
bool g_verdictPending = false;
constexpr size_t kCap = 8192;         // probe backstop against unbounded growth
constexpr float  kEpsCm = 5.0f;       // "same position" tolerance (loose; a host move is meters)

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::IsIniKeyTrue("join_window_pos_trace");
    return s;
}

void ArmForJoin() {
    if (!IsEnabled()) return;
    g_rec.clear();
    g_seq = 0;
    g_verdictPending = true;
    UE_LOGW("join_window_pos_trace: join bracket open, verdict pending -- tracing every KEYED-prop snapshot "
            "expression + loadObjects-recreate re-bind this join (read-only; F1 root discrimination)");
}

void NoteSnapshotExpression(const std::wstring& key, uint32_t eid, void* actor,
                            const ue_wrap::FVector& hostPos, bool hostHeld) {
    if (!IsEnabled() || !g_verdictPending) return;
    if (key.empty()) return;
    if (g_rec.size() >= kCap && g_rec.find(key) == g_rec.end()) return;  // backstop
    Rec& r = g_rec[key];
    r.eid = eid;
    r.hostPos = hostPos;
    r.snapCurPos = actor ? ue_wrap::engine::GetActorLocation(actor) : ue_wrap::FVector{};
    r.hostHeld = hostHeld;
    r.snapshotSeq = ++g_seq;
    r.sawSnapshot = true;
    UE_LOGI("join_window_pos_trace: A snapshot key='%ls' eid=%u seq=%u host=(%.1f,%.1f,%.1f) "
            "cur=(%.1f,%.1f,%.1f) hostHeld=%d (host-carried pos vs the actor's pos right now; hostHeld=1 "
            "means the snapshot DRIVE-SKIPPED placing it)",
            key.c_str(), eid, r.snapshotSeq, hostPos.X, hostPos.Y, hostPos.Z,
            r.snapCurPos.X, r.snapCurPos.Y, r.snapCurPos.Z, hostHeld ? 1 : 0);
}

void NoteRecreateRebind(const std::wstring& key, uint32_t eid, void* actor) {
    if (!IsEnabled() || !g_verdictPending) return;
    if (key.empty()) return;
    if (g_rec.size() >= kCap && g_rec.find(key) == g_rec.end()) return;  // backstop
    Rec& r = g_rec[key];
    r.eid = eid;  // the re-bind's eid is authoritative (it is the expressed identity's eid)
    r.recreatePos = actor ? ue_wrap::engine::GetActorLocation(actor) : ue_wrap::FVector{};
    r.recreateSeq = ++g_seq;
    r.sawRecreate = true;
    UE_LOGI("join_window_pos_trace: B recreate-rebind key='%ls' eid=%u seq=%u recreatePos=(%.1f,%.1f,%.1f) "
            "(the loadObjects-recreate the churn re-bind claimed; its pos is the candidate SAVE pos -- "
            "recreateSeq>snapshotSeq proves the recreate landed AFTER the snapshot)",
            key.c_str(), eid, r.recreateSeq, r.recreatePos.X, r.recreatePos.Y, r.recreatePos.Z);
}

void EmitVerdictAtQuiescence() {
    if (!IsEnabled() || !g_verdictPending) return;
    g_verdictPending = false;  // one verdict per join

    int nClobber = 0, nSnapshotWon = 0, nHostHeld = 0, nDead = 0, nUnresolved = 0, nNoSnapshot = 0;
    auto& reg = coop::element::Registry::Get();

    for (auto& [key, r] : g_rec) {
        // Resolve the FINAL bound actor by eid (authoritative -- the mirror row's current actor), guarded
        // by IsLiveByIndex exactly like ApplyPendingPosCorrections (a purge frees the actor while the row
        // lingers; raw IsLive on a freed ptr can misread TRUE).
        coop::element::Element* el = r.eid ? reg.Get(r.eid) : nullptr;
        void* finalActor = el ? el->GetActor() : nullptr;
        const bool finalLive = finalActor && el && R::IsLiveByIndex(finalActor, el->GetInternalIdx());
        ue_wrap::FVector finalPos{};
        if (finalLive) finalPos = ue_wrap::engine::GetActorLocation(finalActor);

        if (!r.sawSnapshot) {
            ++nNoSnapshot;
            UE_LOGW("join_window_pos_trace: VERDICT key='%ls' eid=%u :: NO-SNAPSHOT (re-bound but never "
                    "snapshot-expressed -- unexpected for a keyed prop; investigate the express path)",
                    key.c_str(), r.eid);
            continue;
        }
        if (!finalLive) {
            ++nDead;
            UE_LOGW("join_window_pos_trace: VERDICT key='%ls' eid=%u :: DEAD-AT-VERDICT (final actor not "
                    "live -- grabbed/destroyed in-window; for a rock hold-R pickup DESTROYS the world actor, "
                    "so this is the expected grab-during-window signature)", key.c_str(), r.eid);
            continue;
        }
        if (r.hostHeld) {
            ++nHostHeld;
            UE_LOGW("join_window_pos_trace: VERDICT key='%ls' eid=%u :: HOST-HELD-AT-SNAPSHOT -- the snapshot "
                    "drive-skipped placing it; final=(%.1f,%.1f,%.1f) host=(%.1f,%.1f,%.1f) d=%.1fcm. This is "
                    "root (2), NOT loadObjects-clobber; a live-pos reconcile must read the host's pos AFTER "
                    "the host releases.", key.c_str(), r.eid, finalPos.X, finalPos.Y, finalPos.Z,
                    r.hostPos.X, r.hostPos.Y, r.hostPos.Z, Dist(finalPos, r.hostPos));
            continue;
        }

        const float dHostToFinal = Dist(finalPos, r.hostPos);
        const bool orderedAfter = r.sawRecreate && r.recreateSeq > r.snapshotSeq;
        const float dSaveToFinal = r.sawRecreate ? Dist(finalPos, r.recreatePos) : -1.0f;
        const float dHostToSave  = r.sawRecreate ? Dist(r.recreatePos, r.hostPos) : -1.0f;

        if (orderedAfter && dHostToSave > kEpsCm && dHostToFinal > kEpsCm && dSaveToFinal <= kEpsCm) {
            ++nClobber;
            UE_LOGW("join_window_pos_trace: VERDICT key='%ls' eid=%u :: LOADOBJECTS-CLOBBER (F1 ROOT (1) "
                    "CONFIRMED) -- recreate seq=%u landed AFTER snapshot seq=%u; final=(%.1f,%.1f,%.1f) sits "
                    "at the SAVE pos (%.1fcm from it) and %.1fcm from the host-moved pos (%.1f,%.1f,%.1f). The "
                    "client's own loadObjects recreated at save-pos and overwrote the snapshot's host pos.",
                    key.c_str(), r.eid, r.recreateSeq, r.snapshotSeq, finalPos.X, finalPos.Y, finalPos.Z,
                    dSaveToFinal, dHostToFinal, r.hostPos.X, r.hostPos.Y, r.hostPos.Z);
            continue;
        }
        if (dHostToFinal <= kEpsCm) {
            ++nSnapshotWon;
            UE_LOGI("join_window_pos_trace: VERDICT key='%ls' eid=%u :: SNAPSHOT-WON -- final=(%.1f,%.1f,%.1f) "
                    "is at the host pos (%.1fcm); no F1 clobber for this key (either no recreate, or the "
                    "snapshot converged onto the recreate).", key.c_str(), r.eid,
                    finalPos.X, finalPos.Y, finalPos.Z, dHostToFinal);
            continue;
        }
        ++nUnresolved;
        UE_LOGW("join_window_pos_trace: VERDICT key='%ls' eid=%u :: UNRESOLVED -- final=(%.1f,%.1f,%.1f) "
                "matches neither host pos (d=%.1fcm) nor%s save/recreate pos (d=%.1fcm); sawRecreate=%d "
                "orderedAfter=%d. Investigate (a third mover, or a recreate that skipped the churn re-bind).",
                key.c_str(), r.eid, finalPos.X, finalPos.Y, finalPos.Z, dHostToFinal,
                r.sawRecreate ? "" : " (none)", dSaveToFinal, r.sawRecreate ? 1 : 0, orderedAfter ? 1 : 0);
    }

    const char* verdict =
        (nClobber > 0)      ? "F1 ROOT (1) CONFIRMED -> loadObjects clobbers the host-moved pos; the host-auth "
                              "live-pos reconcile (generalize b3) is the right fix"
        : (nHostHeld > 0 && nSnapshotWon == 0 && nUnresolved == 0)
                            ? "ROOT (2) ONLY -> every diverged key was host-HELD at snapshot; the fix must read "
                              "host pos AFTER release, not just reconcile at quiescence"
        : (nSnapshotWon > 0 && nClobber == 0 && nUnresolved == 0)
                            ? "NO F1 OBSERVED -> every key ended at the host pos (snapshot won); the reported "
                              "bug did not reproduce in this run -- re-run the exact repro (host moves the rock "
                              "mid-window)"
        : (nUnresolved > 0) ? "UNRESOLVED -> at least one key ended at neither pos; the two-root model is "
                              "incomplete, do NOT build the reconcile yet"
                            : "INCONCLUSIVE -> no diverged keyed prop observed this join (host moved nothing "
                              "in-window, or the moved prop was not keyed); re-run the repro";
    UE_LOGW("join_window_pos_trace: VERDICT keys=%zu clobber=%d snapshot-won=%d host-held=%d dead=%d "
            "unresolved=%d no-snapshot=%d :: %s",
            g_rec.size(), nClobber, nSnapshotWon, nHostHeld, nDead, nUnresolved, nNoSnapshot, verdict);
    g_rec.clear();
}

}  // namespace coop::dev::join_window_pos_trace
