// coop/dev/container_intent_drill_throw_into_observer.cpp -- the throwinto arm's n=5, "B throws into a box A opened". Two clients: A, the
// first, takes the one record of an unarmed basket (the taken run's preamble, so the host arms the box); B, the one with a connected peer
// of a lower slot, throws into it. A only watches after its take: it counts its mirror's records and its live drives around each of B's
// tries and answers the host's read, so the host judges both mirrors in one window. B idles through n=1..4 until the host begins n=5.

#include "container_intent_drill_throw_into.h"
#include "container_intent_drill_put.h"

#include "coop/dev/drill_chat.h"

#include "ue_wrap/core/log.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>

namespace coop::dev::container_intent_drill::detail::into {
namespace {

namespace DC = coop::dev::drill_chat;

constexpr int      kRun         = 5;
constexpr const char* kRunAfterFour = "ci-tinto host run r=4 t=";   // the host's line that ends n=4 on any try (see Ln) and begins n=5
constexpr uint64_t kObsBoundMs  = 240000;   // a wait on the host or on B: the arm line, the read, the next try or the end

enum class S : uint8_t { WaitArmed, WaitRead, WaitNext };
S        g_s = S::WaitArmed;
uint64_t g_ms = 0;
int      g_t = 1;
uint32_t g_eid = 0;
Meter    g_m;   // A's baselines at the host's arm line: the mirror's count and the live drives then

void Go(S s) {
    g_s = s;
    g_ms = ::GetTickCount64();
}
bool Over() { return ::GetTickCount64() - g_ms > kObsBoundMs; }

ObsState Failed(char* why, size_t len, const char* text) {
    std::snprintf(why, len, "%s", text);
    return ObsState::Failed;
}

}  // namespace

bool SecondClient() { return Put2IsJoiner(); }

IdleState IdleTick() {
    if (DC::SeenHas(kRunAfterFour)) return IdleState::Start;
    if (DC::SeenHas(kDoneLine)) return IdleState::Done;
    return IdleState::Working;
}

void ObsBegin(uint32_t packEid) {
    g_eid = packEid;
    g_t = 1;
    Go(S::WaitArmed);
}

ObsState ObsTick(char* why, size_t len) {
    switch (g_s) {
    case S::WaitArmed:   // B's try begins: this mirror's count and live drives are the baseline
        if (DC::SeenHas(Ln("host", "armed", kRun, g_t, false))) {
            MeterBegin(g_m, "observer", g_eid);
            Go(S::WaitRead);
        } else if (DC::SeenHas(kDoneLine)) {
            return ObsState::Done;
        } else if (Over()) {
            return Failed(why, len, "the host never armed n=5 within the observer's bound");
        }
        return ObsState::Working;
    case S::WaitRead:   // the host has read its own copy: this mirror is read in the same window
        if (DC::SeenHas(Ln("host", "read", kRun, g_t, false))) {
            const int c1 = CountOf(g_eid);
            const int stray = StrayOf(g_m, /*control=*/false);
            UE_LOGI("[CI-DRILL] observer %s n=%d try %d read: mirror %d -> %d, stray=%d", ArmName(), kRun, g_t, g_m.c0, c1, stray);
            char say[48];
            std::snprintf(say, sizeof(say), "c0=%d c1=%d stray=%d", g_m.c0, c1, stray);
            DC::Say(Ln("clientA", "late", kRun, g_t, true) + say);
            Go(S::WaitNext);
        } else if (Over()) {
            return Failed(why, len, "the host never asked for the observer's read of n=5 within its bound");
        }
        return ObsState::Working;
    case S::WaitNext:   // another try (B missed), or the end
        if (DC::SeenHas(Ln("host", "retry", kRun, g_t, false))) {
            ++g_t;
            Go(S::WaitArmed);
        } else if (DC::SeenHas(kDoneLine)) {
            return ObsState::Done;
        } else if (Over()) {
            return Failed(why, len, "the host gave no verdict on n=5 within the observer's bound");
        }
        return ObsState::Working;
    }
    return ObsState::Working;
}

void ObsReset() {
    g_s = S::WaitArmed;
    g_ms = 0;
    g_t = 1;
    g_eid = 0;
    g_m = Meter{};
}

}  // namespace coop::dev::container_intent_drill::detail::into
