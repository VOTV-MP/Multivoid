// coop/prop_snapshot.cpp -- Phase 5S0 save snapshot bootstrap.
//
// Extracted from harness/harness.cpp (2026-05-25 modular refactor).
// See coop/prop_snapshot.h for the public interface.

#include "coop/prop_snapshot.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/prop_lifecycle.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/types.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::prop_snapshot {
namespace {

namespace R = ue_wrap::reflection;

coop::net::Session* g_session_ptr = nullptr;

// Enumeration state: vector of live Aprop_C* collected by Trigger(); index
// of the next candidate DrainChunk() will process. Game-thread access only;
// no lock needed.
std::vector<void*> g_snapshotCandidates;
size_t g_snapshotCandidateIdx = 0;

// Per-tick drain size -- 100 candidates per frame keeps the per-tick cost
// (~5-15 ms for GetActorLocation/Rotation x100) well under the 16.6 ms
// frame budget. Total ~2000 props @ 100/tick = 20-30 ticks (~330-500 ms
// wall-clock spread across frames, no single-frame stall).
constexpr size_t kSnapshotChunkSize = 100;

}  // namespace

void SetSession(coop::net::Session* session) {
    g_session_ptr = session;
}

void Trigger() {
    if (!g_session_ptr) return;
    if (g_session_ptr->role() != coop::net::Role::Host) {
        UE_LOGI("snapshot: not host -- skipping (client receives, doesn't broadcast)");
        return;
    }
    if (!g_session_ptr->connected()) {
        UE_LOGW("snapshot: not connected -- skipping");
        return;
    }
    g_snapshotCandidates.clear();
    g_snapshotCandidateIdx = 0;
    int skippedCDO = 0, skippedDying = 0;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        if (!ue_wrap::prop::IsDescendantOfProp(obj)) continue;
        const std::wstring nm = R::ToString(R::NameOf(obj));
        if (nm.rfind(L"Default__", 0) == 0) { ++skippedCDO; continue; }
        if (!R::IsLive(obj)) { ++skippedDying; continue; }
        g_snapshotCandidates.push_back(obj);
    }
    UE_LOGI("snapshot: enumerated %zu Aprop_C live candidates (skipped %d CDOs, %d dying); will drain %zu per tick",
            g_snapshotCandidates.size(), skippedCDO, skippedDying, kSnapshotChunkSize);
}

void DrainChunk() {
    if (g_snapshotCandidateIdx >= g_snapshotCandidates.size()) return;
    // (std::min) parenthesized to defeat windows.h's `min` macro.
    const size_t limit = (std::min)(g_snapshotCandidateIdx + kSnapshotChunkSize,
                                    g_snapshotCandidates.size());
    int sent = 0;
    for (; g_snapshotCandidateIdx < limit; ++g_snapshotCandidateIdx) {
        void* obj = g_snapshotCandidates[g_snapshotCandidateIdx];
        // Re-validate liveness: an actor that was live during Phase-1
        // enumeration might have been GC'd in the intervening ticks.
        if (!obj || !R::IsLive(obj)) continue;
        coop::net::PropSpawnPayload p{};
        const std::wstring cls = R::ClassNameOf(obj);
        // Audit C-1 (2026-05-24): same wire-suppress allowlist as the
        // Init POST observer. Intermediate-variant classes (mushroom7_C)
        // never cross the wire -- host-authoritative.
        if (coop::prop_lifecycle::IsWireSuppressedPropClass(cls)) {
            UE_LOGI("snapshot: skipping intermediate-variant '%ls' actor %p (wire-suppressed)",
                    cls.c_str(), obj);
            continue;
        }
        p.className.len = 0;
        for (size_t j = 0; j < cls.size() && j < 63; ++j) {
            p.className.data[p.className.len++] = static_cast<char>(cls[j]);
        }
        const std::wstring keyStr = ue_wrap::prop::GetKeyString(obj);
        if (keyStr.empty()) continue;
        p.key.len = 0;
        for (size_t j = 0; j < keyStr.size() && j < 31; ++j) {
            p.key.data[p.key.len++] = static_cast<char>(keyStr[j]);
        }
        const auto loc = ue_wrap::engine::GetActorLocation(obj);
        const auto rot = ue_wrap::engine::GetActorRotation(obj);
        p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
        p.rotPitch = ue_wrap::NormalizeAxis(rot.Pitch);
        p.rotYaw   = ue_wrap::NormalizeAxis(rot.Yaw);
        p.rotRoll  = ue_wrap::NormalizeAxis(rot.Roll);
        p.scaleX = 1.f; p.scaleY = 1.f; p.scaleZ = 1.f;
        p.physFlags = coop::net::propspawn_flags::kSimulatePhysics;
        if (ue_wrap::prop::IsHeavy(obj))  p.physFlags |= coop::net::propspawn_flags::kIsHeavy;
        if (ue_wrap::prop::IsFrozen(obj)) p.physFlags |= coop::net::propspawn_flags::kFrozen;
        p.initLinVelX = p.initLinVelY = p.initLinVelZ = 0.f;
        p.initAngVelX = p.initAngVelY = p.initAngVelZ = 0.f;
        coop::prop_lifecycle::EnqueuePropSpawnForRetry(p);
        ++sent;
    }
    if (g_snapshotCandidateIdx >= g_snapshotCandidates.size()) {
        UE_LOGI("snapshot: enumeration drain complete (%zu candidates processed); freeing candidate list",
                g_snapshotCandidates.size());
        g_snapshotCandidates.clear();
        g_snapshotCandidates.shrink_to_fit();
        g_snapshotCandidateIdx = 0;
    } else {
        UE_LOGI("snapshot: drained chunk -- this tick=%d, processed %zu/%zu",
                sent, g_snapshotCandidateIdx, g_snapshotCandidates.size());
    }
}

size_t OnDisconnect() {
    size_t pending = 0;
    if (!g_snapshotCandidates.empty()) {
        pending = g_snapshotCandidates.size() - g_snapshotCandidateIdx;
        g_snapshotCandidates.clear();
        g_snapshotCandidates.shrink_to_fit();
        g_snapshotCandidateIdx = 0;
    }
    return pending;
}

}  // namespace coop::prop_snapshot
