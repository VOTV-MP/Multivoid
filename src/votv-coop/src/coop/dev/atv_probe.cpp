// coop/dev/atv_probe.cpp -- see header.

#include "coop/dev/atv_probe.h"

#include "coop/config/config.h"
#include "coop/element/object_scan_hub.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "coop/interactables/atv_sync.h"   // OwnsTick -- which SIDE of the mirror this sample is
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::atv_probe {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

bool g_enabled   = false;
bool g_sitArmed  = false;
bool g_checked   = false;
bool g_installed = false;
bool g_sitFired  = false;
void* g_sitFn    = nullptr;   // ATV_C::playerSit(player)
std::chrono::steady_clock::time_point g_firstValid{};

// The sit arm waits this long after the FIRST sample whose body is placed, not after
// boot: the ATV is in GUObjectArray with body=(0,0,0) for ~15 samples before its
// components exist, and seating into a rig that is not there yet proves nothing.
constexpr int kSitDelayMs = 25000;

// Field offsets the ue_wrap ATV surface does not expose (it has fuel/health/brake).
// Resolved by NAME -- an offset literal here would be a second copy of the version
// surface that docs/VERSION_MIGRATION.md exists to keep to two files.
int32_t g_batteryOff = -1;
int32_t g_dirtOff    = -1;
int32_t g_dirtVelOff = -1;
bool    g_offsResolved = false;

void* g_partsFn = nullptr;   // ATV_C::vehicleGetParts (8 out-params, no in-params)

// The hub hands us matches into `g_pending`; `g_atvs` is the completed swap.
std::vector<void*> g_pending;
std::vector<void*> g_atvs;

std::chrono::steady_clock::time_point g_lastSample{};
uint32_t g_sample = 0;

constexpr int kSampleMs = 500;

bool ResolveOffsets() {
    if (g_offsResolved) return true;
    void* cls = R::FindClass(L"ATV_C");
    if (!cls) return false;
    g_batteryOff = R::FindPropertyOffset(cls, L"battery");
    g_dirtOff    = R::FindPropertyOffset(cls, L"dirt");
    g_dirtVelOff = R::FindPropertyOffset(cls, L"dirtVel");
    g_partsFn    = R::FindFunction(cls, L"vehicleGetParts");
    g_sitFn      = R::FindFunction(cls, L"playerSit");
    g_offsResolved = true;
    UE_LOGI("atv_probe: resolved battery=%d dirt=%d dirtVel=%d vehicleGetParts=%p playerSit=%p",
            g_batteryOff, g_dirtOff, g_dirtVelOff, g_partsFn, g_sitFn);
    return true;
}

float ReadFloat(void* obj, int32_t off) {
    if (!obj || off < 0) return -1.f;
    return *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(obj) + off);
}

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// ---- scan-hub consumer ------------------------------------------------------------
bool HubEnsure()            { return ue_wrap::atv::EnsureResolved(); }
bool HubIsInstance(void* o) { return ue_wrap::atv::IsAtv(o); }
void HubPassBegin(void*, bool)  { g_pending.clear(); }
void HubMatch(void*, void* obj) { if (obj) g_pending.push_back(obj); }
size_t HubPassComplete(void*, bool, uint32_t) {
    g_atvs.swap(g_pending);
    g_pending.clear();
    return g_atvs.size();
}

// One ATV, one line. Everything here is a READ.
void SampleOne(void* atv, size_t idx) {
    const std::wstring key = ue_wrap::atv::GetKeyString(atv);

    // The game's own matched 4-body read. If it is unavailable we still log the
    // vitals half rather than dropping the sample -- a partial line is evidence,
    // a missing line is ambiguous between "not resolved" and "no ATV".
    ue_wrap::FVector bodyL{}, frL{}, flL{}, bkL{};
    ue_wrap::FRotator bodyR{};
    bool haveParts = false;
    if (g_partsFn) {
        ue_wrap::ParamFrame pf(g_partsFn);
        if (pf.valid() && ue_wrap::Call(atv, pf)) {
            bodyL = pf.Get<ue_wrap::FVector>(L"body_location");
            bodyR = pf.Get<ue_wrap::FRotator>(L"body_rotation");
            frL   = pf.Get<ue_wrap::FVector>(L"frontRight_location");
            flL   = pf.Get<ue_wrap::FVector>(L"frontLeft_location");
            bkL   = pf.Get<ue_wrap::FVector>(L"back_location");
            haveParts = true;
        }
    }

    const float fuel    = ue_wrap::atv::GetFuel(atv);
    const float health  = ue_wrap::atv::GetHealth(atv);
    const float battery = ReadFloat(atv, g_batteryOff);
    const float dirt    = ReadFloat(atv, g_dirtOff);
    const float dirtVel = ReadFloat(atv, g_dirtVelOff);
    const bool  driven  = ue_wrap::atv::IsDriven(atv);
    const void* occ     = ue_wrap::atv::GetOccupantPlayer(atv);
    // WHICH SIDE OF THE MIRROR is this sample from? The 2026-08-29 run could not say, so
    // docs/vehicles/ATV.md 11.1 stayed open even though the probe ran perfectly: an idle ATV is
    // never mirrored, and nothing asserted the receiver was actually mirroring during the driven
    // window. Read through atv_sync's own published set rather than recomputing the predicate --
    // an instrument that reimplements the code under test agrees with itself, not with it.
    const bool  ownsTick = coop::atv_sync::OwnsTick(atv);

    if (haveParts) {
        // |wheel - body| is ROTATION-INVARIANT, so it isolates suspension travel from
        // the body tipping/turning. A rigid frozen rig holds these three constant to
        // the bit; a live one breathes. THIS is the frozen-corpse measurement.
        const float dFR = Dist(frL, bodyL), dFL = Dist(flL, bodyL), dBK = Dist(bkL, bodyL);
        UE_LOGI("[ATVP] n=%u i=%zu key='%ls' driven=%d owns=%d occ=%p "
                "body=(%.1f,%.1f,%.1f) rot=(%.1f,%.1f,%.1f) "
                "susFR=%.3f susFL=%.3f susBK=%.3f "
                "fuel=%.3f batt=%.3f dirt=%.4f dirtVel=%.4f hp=%.2f",
                g_sample, idx, key.c_str(), driven ? 1 : 0, ownsTick ? 1 : 0, occ,
                bodyL.X, bodyL.Y, bodyL.Z, bodyR.Pitch, bodyR.Yaw, bodyR.Roll,
                dFR, dFL, dBK, fuel, battery, dirt, dirtVel, health);
    } else {
        ue_wrap::FVector loc{}; ue_wrap::FRotator rot{};
        ue_wrap::atv::GetRootTransform(atv, loc, rot);
        UE_LOGI("[ATVP] n=%u i=%zu key='%ls' driven=%d owns=%d occ=%p "
                "body=(%.1f,%.1f,%.1f) rot=(%.1f,%.1f,%.1f) NOPARTS "
                "fuel=%.3f batt=%.3f dirt=%.4f dirtVel=%.4f hp=%.2f",
                g_sample, idx, key.c_str(), driven ? 1 : 0, ownsTick ? 1 : 0, occ,
                loc.X, loc.Y, loc.Z, rot.Pitch, rot.Yaw, rot.Roll,
                fuel, battery, dirt, dirtVel, health);
    }
}

}  // namespace

void Install() {
    if (!g_checked) {
        g_checked = true;
        g_enabled  = ::coop::config::ResolveFlag(::coop::config_registry::rows::atv_probe);
        g_sitArmed = ::coop::config::ResolveFlag(::coop::config_registry::rows::atv_probe_sit);
        if (g_enabled) UE_LOGI("atv_probe: ENABLED (ini [dev] atv_probe=1) -- sampling every %d ms "
                               "(sit arm %s)", kSampleMs, g_sitArmed ? "ARMED" : "off");
    }
    if (!g_enabled || g_installed) return;
    g_installed = true;
    coop::element::scan_hub::Register(coop::element::scan_hub::Consumer{
        /*name*/           "atv_probe",
        /*ctx*/            nullptr,
        /*EnsureResolved*/ &HubEnsure,
        /*IsInstance*/     &HubIsInstance,
        /*OnPassBegin*/    &HubPassBegin,
        /*OnMatch*/        &HubMatch,
        /*OnPassComplete*/ &HubPassComplete,
        /*settleScans*/    15,   // ATVs are a static class: never force full passes for us
    });
    UE_LOGI("atv_probe: scan-hub consumer registered");
}

// The one write, host-side, once. Seats the local player via the game's own path so
// the ATV becomes AUTHORED and the other peer is forced to build a real mirror.
void TrySit(void* atv) {
    if (!g_sitFn) { UE_LOGW("[ATVP] SIT: playerSit unresolved -- arm cannot fire"); g_sitFired = true; return; }
    void* local = coop::players::Registry::Get().Local();
    if (!local) return;   // not in gameplay yet; retry next tick
    ue_wrap::ParamFrame pf(g_sitFn);
    if (!pf.valid()) { UE_LOGE("[ATVP] SIT: ParamFrame(playerSit) invalid"); g_sitFired = true; return; }
    pf.Set<void*>(L"player", local);
    const bool ok = ue_wrap::Call(atv, pf);
    g_sitFired = true;
    UE_LOGI("[ATVP] SIT fired: playerSit(local=%p) on atv=%p -> %s (driven now=%d)",
            local, atv, ok ? "called" : "CALL FAILED",
            ue_wrap::atv::IsDriven(atv) ? 1 : 0);
}

void Tick(bool isHost) {
    if (!g_enabled) return;
    if (!ResolveOffsets()) return;
    if (g_atvs.empty()) return;

    const auto now = std::chrono::steady_clock::now();
    if (g_lastSample.time_since_epoch().count() != 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastSample).count() < kSampleMs)
        return;
    g_lastSample = now;
    ++g_sample;

    for (size_t i = 0; i < g_atvs.size(); ++i) {
        if (g_atvs[i]) SampleOne(g_atvs[i], i);
    }

    if (!g_sitArmed || !isHost || g_sitFired || g_atvs.empty()) return;
    // Clock the delay from the first PLACED sample, not from boot.
    ue_wrap::FVector loc{}; ue_wrap::FRotator rot{};
    if (!ue_wrap::atv::GetRootTransform(g_atvs[0], loc, rot)) return;
    if (std::fabs(loc.X) < 1e-6f && std::fabs(loc.Y) < 1e-6f && std::fabs(loc.Z) < 1e-6f) return;
    if (g_firstValid.time_since_epoch().count() == 0) { g_firstValid = now; return; }
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_firstValid).count() < kSitDelayMs)
        return;
    TrySit(g_atvs[0]);
}

}  // namespace coop::dev::atv_probe
