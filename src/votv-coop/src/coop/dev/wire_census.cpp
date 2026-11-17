// coop/dev/wire_census.cpp -- see header. NET-THREAD only; no locks needed.

#include "coop/dev/wire_census.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/player/slot_count.h"  // kMaxPeers: the per-slot table's length
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <cstdint>

namespace coop::dev::wire_census {
namespace {

constexpr int kSlots = coop::players::kMaxPeers;  // out-of-range slots are dropped
constexpr int kTypes = 256;  // full uint8_t MsgType space

int g_enabled = -1;  // -1 = the row not resolved yet (latched on first Enabled())
uint32_t g_streamCounts[kSlots][kTypes];
uint64_t g_lastFlushTick = 0;

}  // namespace

bool Enabled() {
    if (g_enabled < 0)
        g_enabled = coop::config::ResolveFlag(::coop::config_registry::rows::wire_census) ? 1 : 0;
    return g_enabled == 1;
}

void Tick() {
    const uint64_t now = GetTickCount64();
    if (g_lastFlushTick == 0) { g_lastFlushTick = now; return; }
    if (now - g_lastFlushTick < 1000) return;
    // The heartbeat comes first, whether or not a counter moved: a quiet second used to print
    // nothing, so a reader waiting for "the census passed tick T" could not tell a quiet wire from
    // a stopped census.
    UE_LOGI("wire_census: tick=%llu", static_cast<unsigned long long>(now));
    for (int s = 0; s < kSlots; ++s) {
        for (int t = 0; t < kTypes; ++t) {
            if (g_streamCounts[s][t] == 0) continue;
            UE_LOGI("wire_census: tick=%llu slot=%d STREAM type=%u n=%u",
                    static_cast<unsigned long long>(now), s, t, g_streamCounts[s][t]);
            g_streamCounts[s][t] = 0;
        }
    }
    g_lastFlushTick = now;
    // Census lines are INFO, and INFO is flushed only when a second has passed since the last
    // flush; a killed process discards the tail -- which here would be the wire window itself.
    // The explicit 1 Hz flush bounds the loss to under 1 s.
    ue_wrap::log::Flush();
}

void NoteStream(int routeSlot, unsigned msgType) {
    if (routeSlot < 0 || routeSlot >= kSlots || msgType >= kTypes) return;
    g_streamCounts[routeSlot][msgType]++;
}

void NoteReliable(int routeSlot, unsigned kind) {
    UE_LOGI("wire_census: tick=%llu slot=%d RELIABLE kind=%u",
            static_cast<unsigned long long>(GetTickCount64()), routeSlot, kind);
}

}  // namespace coop::dev::wire_census
