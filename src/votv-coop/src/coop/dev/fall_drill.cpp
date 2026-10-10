// coop/dev/fall_drill.cpp -- see coop/dev/fall_drill.h.

#include "coop/dev/fall_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_drive_stream.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/remote_prop.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace coop::dev::fall_drill {
namespace {

namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;

constexpr const wchar_t* kDriveCls = L"prop_drive_C";
constexpr float    kAheadCm      = 200.f;   // in front of the host's player
constexpr float    kDropCm       = 400.f;   // above its feet's height: a fall the stream must carry
constexpr float    kTolCm        = 5.f;     // the client's copy at rest against the host's final pose
constexpr uint64_t kTrackBoundMs = 5000;    // HOST: the spawn to the drive's eid
constexpr uint64_t kRestBoundMs  = 20000;   // HOST: the eid to the drive lying still
constexpr uint64_t kEndBoundMs   = 60000;   // CLIENT: world-ready to the host's stream end
constexpr uint64_t kCheckEveryMs = 250;
constexpr int      kRestChecks   = 4;       // reads in a row, a check apart, within a centimetre

enum class Step : uint8_t { Arm, Track, Rest, End, Done };
Step     g_step = Step::Arm;
uint64_t g_stepMs = 0;
uint64_t g_nextCheckMs = 0;
uint64_t g_baseMs = 0;         // CLIENT: the session's first tick; an end before it is not the drill's
int      g_session = 1;
ue_wrap::CachedObjRef g_drive; // HOST: the dropped drive
uint32_t g_eid = 0;
float    g_fromZ = 0.f;
ue_wrap::FVector g_lastAt{};
int      g_restReads = 0;

bool Enabled() {
    static const bool on = coop::config::ResolveString(::coop::config_registry::rows::fall_drill) == "run";
    return on;
}
void Next(Step s) { g_step = s; g_stepMs = ::GetTickCount64(); }
bool Expired(uint64_t bound) { return ::GetTickCount64() - g_stepMs > bound; }
void Fail(const char* what) {
    UE_LOGW("[FALL-DRILL] FAIL in session %d: %s", g_session, what);
    g_step = Step::Done;
}
void Abandon(const char* why) {
    UE_LOGW("[FALL-DRILL] ABANDONED in session %d: %s", g_session, why);
    g_step = Step::Done;
}
float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    return std::sqrt((a.X - b.X) * (a.X - b.X) + (a.Y - b.Y) * (a.Y - b.Y) + (a.Z - b.Z) * (a.Z - b.Z));
}

void HostTick(coop::net::Session* s) {
    switch (g_step) {
    case Step::Arm: {
        if (!s->IsSlotWorldReady(1)) return;
        void* puppet = coop::puppet_drive::Puppet(1).GetActor();   // the peer's pose is arriving: its stream reads
        void* me = coop::players::Registry::Get().Local();
        ue_wrap::FVector at{};
        if (!puppet || !R::IsLive(puppet) || !me || !E::TryGetActorLocation(me, at)) return;
        void* cls = R::FindClass(kDriveCls);
        if (!cls) { Abandon("the drive's class is not loaded"); return; }
        const ue_wrap::FVector fwd = E::GetActorForwardVector(me);
        const ue_wrap::FVector from{at.X + fwd.X * kAheadCm, at.Y + fwd.Y * kAheadCm, at.Z + kDropCm};
        void* drive = E::SpawnActor(cls, from);
        if (!drive) { Abandon("the drive did not spawn"); return; }
        g_drive.Set(drive);
        g_fromZ = from.Z;
        UE_LOGI("[FALL-DRILL] host: a drive dropped from (%.0f,%.0f,%.0f)", from.X, from.Y, from.Z);
        Next(Step::Track);
        return;
    }
    case Step::Track: {
        void* drive = g_drive.Get();
        if (!drive) { Abandon("the dropped drive died"); return; }
        g_eid = static_cast<uint32_t>(coop::prop_element_tracker::GetPropElementIdForActor(drive));
        if (g_eid == 0) {
            if (Expired(kTrackBoundMs)) Fail("the dropped drive got no eid within 5 s");
            return;
        }
        g_restReads = 0;
        Next(Step::Rest);
        return;
    }
    case Step::Rest: {
        const uint64_t now = ::GetTickCount64();
        if (now < g_nextCheckMs) return;
        g_nextCheckMs = now + kCheckEveryMs;
        void* drive = g_drive.Get();
        ue_wrap::FVector at{};
        if (!drive || !E::TryGetActorLocation(drive, at)) { Abandon("the dropped drive died"); return; }
        g_restReads = Dist(at, g_lastAt) < 1.f ? g_restReads + 1 : 0;
        g_lastAt = at;
        if (g_restReads >= kRestChecks) {
            UE_LOGI("[FALL-DRILL] host: the drive eid=%u fell %.0f cm and rests at (%.1f,%.1f,%.1f)", g_eid,
                    g_fromZ - at.Z, at.X, at.Y, at.Z);
            g_step = Step::Done;
            return;
        }
        if (Expired(kRestBoundMs)) Abandon("the dropped drive never came to rest");
        return;
    }
    case Step::End:
    case Step::Done:
        return;
    }
}

void ClientTick() {
    // The baseline is the session's first tick, before this world loads: the host drops on its own view of the
    // slot's world-ready, which can come before this peer's own, so the end may land before the arm.
    if (g_baseMs == 0) g_baseMs = ::GetTickCount64();
    switch (g_step) {
    case Step::Arm:
        if (!coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
            return;
        Next(Step::End);
        return;
    case Step::End: {
        const uint64_t now = ::GetTickCount64();
        if (now < g_nextCheckMs) return;
        g_nextCheckMs = now + kCheckEveryMs;
        // The newest end since the baseline whose prop is a drive here: the host's dropped drive.
        coop::prop_drive_stream::AppliedEnd ends[8];
        const size_t n = coop::prop_drive_stream::RecentEnds(ends, 8);
        void* actor = nullptr;
        uint32_t eid = 0;
        ue_wrap::FVector pose{};
        for (size_t k = 0; k < n && !actor; ++k) {
            if (ends[k].atMs <= g_baseMs) continue;
            void* a = coop::remote_prop::ResolveLiveActorByEid(ends[k].eid);
            if (a && R::ClassNameOf(a) == kDriveCls) { actor = a; eid = ends[k].eid; pose = ends[k].pose; }
        }
        ue_wrap::FVector here{};
        if (!actor || !E::TryGetActorLocation(actor, here)) {
            if (Expired(kEndBoundMs)) Fail("no stream end of a fallen drive came from the host within 60 s");
            return;
        }
        const float off = Dist(here, pose);
        if (off > kTolCm) {
            char why[160];
            std::snprintf(why, sizeof(why), "this client's drive eid=%u rests %.1f cm from the host's final pose", eid, off);
            Fail(why);
            return;
        }
        UE_LOGI("[FALL-DRILL] client DONE in session %d: the host's fallen drive eid=%u rests here at (%.1f,%.1f,%.1f), "
                "%.1f cm from the host's final pose -- PASS", g_session, eid, here.X, here.Y, here.Z, off);
        g_step = Step::Done;
        return;
    }
    case Step::Track:
    case Step::Rest:
    case Step::Done:
        return;
    }
}

}  // namespace

void Tick(coop::net::Session* s) {
    if (!Enabled() || !s) return;
    if (s->role() == coop::net::Role::Host) HostTick(s);
    else if (s->connected()) ClientTick();
}

void OnDisconnect() {
    if (!Enabled()) return;
    ++g_session;
    g_step = Step::Arm;
    g_stepMs = g_nextCheckMs = g_baseMs = 0;
    g_drive.Reset();
    g_eid = 0;
    g_fromZ = 0.f;
    g_lastAt = {};
    g_restReads = 0;
}

}  // namespace coop::dev::fall_drill
