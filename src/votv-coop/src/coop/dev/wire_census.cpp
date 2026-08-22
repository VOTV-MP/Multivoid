// coop/dev/wire_census.cpp -- see header. NET-THREAD only; no locks needed.

#include "coop/dev/wire_census.h"

#include "coop/config/config.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <cstdint>

namespace coop::dev::wire_census {
namespace {

constexpr int kSlots = 8;    // >= kMaxPeers; out-of-range slots are dropped
constexpr int kTypes = 256;  // full uint8_t MsgType space

int g_enabled = -1;  // -1 = env not read yet (latched on first Enabled())
uint32_t g_streamCounts[kSlots][kTypes];
uint64_t g_lastFlushTick = 0;

}  // namespace

bool Enabled() {
    if (g_enabled < 0)
        g_enabled = (coop::config::ReadEnv("VOTVCOOP_WIRE_CENSUS") == "1") ? 1 : 0;
    return g_enabled == 1;
}

void Tick() {
    const uint64_t now = GetTickCount64();
    if (g_lastFlushTick == 0) { g_lastFlushTick = now; return; }
    if (now - g_lastFlushTick < 1000) return;
    for (int s = 0; s < kSlots; ++s) {
        for (int t = 0; t < kTypes; ++t) {
            if (g_streamCounts[s][t] == 0) continue;
            UE_LOGI("wire_census: tick=%llu slot=%d STREAM type=%u n=%u",
                    static_cast<unsigned long long>(now), s, t, g_streamCounts[s][t]);
            g_streamCounts[s][t] = 0;
        }
    }
    g_lastFlushTick = now;
    // Census lines are INFO and INFO rides the ~4 KB CRT buffer (flushed only on
    // WARN/ERROR); a killed process discards that tail -- which here would be the
    // wire window itself. 1 Hz explicit flush bounds the loss to <1 s.
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
