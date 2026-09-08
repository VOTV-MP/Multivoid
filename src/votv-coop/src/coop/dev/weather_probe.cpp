// coop/dev/weather_probe.cpp -- see coop/dev/weather_probe.h. The bodies were moved out of
// weather_sync.cpp unchanged, log-line prefixes included.

#include "coop/dev/weather_probe.h"

#include "coop/config/config.h"
#include "coop/net/session.h"
#include "coop/world/weather_rain.h"
#include "coop/world/weather_sync.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/world/directionalwind.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"

#include <cmath>
#include <cstdint>

namespace coop::dev::weather_probe {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;

// [probe] eff_rain particle active-state read (UActorComponent::IsActive).
// Used by the gated weather probe to settle the load-bearing rain unknown:
// is the eff_rain UParticleSystemComponent ACTIVE on the HOST (rendering rain)
// but INACTIVE on the CLIENT after ApplyFromHost? If host=1/client=0 the fix is
// a direct Activate(); if both=1 the miss is the per-tick rainStrength parameter
// drive, not Activate. Reflection UFunction -- no offset guess. Game thread.
void* g_isActiveFn = nullptr;
bool ReadComponentIsActive(void* comp, bool* outOk) {
    if (outOk) *outOk = false;
    if (!comp || !R::IsLive(comp)) return false;
    if (!g_isActiveFn) {
        if (void* c = R::FindClass(L"ActorComponent"))
            g_isActiveFn = R::FindFunction(c, L"IsActive");
    }
    if (!g_isActiveFn) return false;
    ue_wrap::ParamFrame f(g_isActiveFn);
    if (!ue_wrap::Call(comp, f)) return false;
    if (outOk) *outOk = true;
    return f.Get<bool>(L"ReturnValue");
}

}  // namespace

void Tick(coop::net::Session* session) {
    // [probe] weather diagnostic (ini weather_probe=1): on BOTH peers, log the cycle's rain and
    // fog observable state plus eff_rain's IsActive at about 1 Hz. Watch one rain-and-fog episode
    // and compare the two logs: it settles whether the rain particle is active on the host but not
    // the client (an Activate question rather than a parameter one), and which fog the thick fog is
    // (enable_fog or enable_superfog). Gated off by default; cheap when off, one bool load.
    static const bool sProbe = ::coop::config::ResolveFlag(::coop::config_registry::rows::weather_probe);
    if (sProbe) {
        static uint32_t sN = 0;
        if ((sN++ % 125) == 0) {
            void* cycle = coop::weather_rain::Cycle();
            if (cycle && R::IsLive(cycle)) {
                auto* b = reinterpret_cast<uint8_t*>(cycle);
                const bool  isRaining = *reinterpret_cast<bool*>(b + P::off::AdaynightCycle_isRaining);
                const float rainStr   = *reinterpret_cast<float*>(b + P::off::AdaynightCycle_rainStrength);
                const bool  enFog     = *reinterpret_cast<bool*>(b + P::off::AdaynightCycle_enable_fog);
                const bool  enSuper   = *reinterpret_cast<bool*>(b + P::off::AdaynightCycle_enable_superfog);
                void*       effRain   = *reinterpret_cast<void**>(b + P::off::AdaynightCycle_eff_rain);
                // Thick-fog signals: the rolling-fog ACTOR (fogEventObject, present only during the
                // thick fog), the visible height-fog DENSITY (finalFogDensity), and permanentFog.
                // These are what distinguish a foggy peer from a clear one; the rain and enable
                // flags do not.
                void*       rollFog   = *reinterpret_cast<void**>(b + P::off::AdaynightCycle_fogEventObject);
                const float finalFog  = *reinterpret_cast<float*>(b + P::off::AdaynightCycle_finalFogDensity);
                const bool  permFog   = *reinterpret_cast<bool*>(b + P::off::AdaynightCycle_permanentFog);
                bool ok = false;
                const bool active = ReadComponentIsActive(effRain, &ok);
                auto* s = session;
                const char* role = (s && s->role() == coop::net::Role::Host) ? "HOST" : "CLIENT";
                UE_LOGI("[probe weather] role=%s rollFogActor=%p finalFogDensity=%.4f permanentFog=%d "
                        "enable_fog=%d enable_superfog=%d | isRaining=%d rainStrength=%.2f eff_rain=%p IsActive=%d(ok=%d)",
                        role, rollFog, finalFog, permFog ? 1 : 0, enFog ? 1 : 0, enSuper ? 1 : 0,
                        isRaining ? 1 : 0, rainStr, effRain, active ? 1 : 0, ok ? 1 : 0);
                // [probe wind] for the case of a client with wind and dust while the host is calm,
                // with the sync itself statically sound: log the whole visible-wind input chain on
                // both peers -- windTarget (the synced gust input), the four persistent floats, and
                // the roll interceptor counters. ReceiveTick derives intensity and windOffset from
                // windTarget every tick, so target parity is state parity: a target mismatch here
                // means the sync write is not landing, and fired == suppressed == 0 on the client
                // means the roll dispatch bypasses ProcessEvent. The eff_wind dust rate, speed and
                // lifetime are all scaled by intensity, so there is no separate dust state.
                {
                    ue_wrap::directionalwind::WindState w{};
                    ue_wrap::FVector tgt{};
                    const bool wOk = ue_wrap::directionalwind::Read(w);
                    const bool tOk = ue_wrap::directionalwind::ReadTarget(tgt);
                    UE_LOGI("[probe wind] role=%s ok=%d/%d target=(%.1f,%.1f,%.1f) |target|=%.1f "
                            "speedBg=%.2f strBg=%.2f speedRain=%.2f strRain=%.2f "
                            "roll fired=%u suppressed=%u",
                            role, wOk ? 1 : 0, tOk ? 1 : 0, tgt.X, tgt.Y, tgt.Z,
                            std::sqrt(tgt.X * tgt.X + tgt.Y * tgt.Y + tgt.Z * tgt.Z),
                            w.speedBg, w.strengthBg, w.speedRain, w.strengthRain,
                            coop::weather_sync::WindRollFired(),
                            coop::weather_sync::WindRollSuppressed());
                }
            }
        }
    }
}

}  // namespace coop::dev::weather_probe
