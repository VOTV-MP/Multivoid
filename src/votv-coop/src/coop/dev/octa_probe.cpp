// coop/dev/octa_probe.cpp -- see coop/dev/octa_probe.h.

#include "coop/dev/octa_probe.h"

#include "coop/creatures/geom_octa_sync.h"
#include "coop/net/net_clock.h"

#include "ue_wrap/actors/geom_octa.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_component.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace coop::dev::octa_probe {
namespace {

namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
namespace GO = ue_wrap::geom_octa;
using ue_wrap::FVector;

constexpr uint64_t kEveryMs = 1000;
constexpr int      kMaxLines = 120;
constexpr uint8_t  kPawnChannel = 3;   // ECC_Pawn

constexpr uint64_t kBeginPlayMs = 2000;

uint64_t g_nextMs = 0;
int      g_lines = 0;
uint64_t g_octaSeenAt = 0;
bool     g_octaSaid = false, g_hostSaid = false, g_puppetSaid = false;

// One reflected UFunction of `comp` with no argument and a byte, bool or float return. -1 (or NaN) when the class does
// not carry it, said in the line as `?`.
template <class T>
bool Read(void* comp, const wchar_t* fnName, T& out) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(comp), fnName);
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid() || !ue_wrap::Call(comp, f)) return false;
    out = f.Get<T>(L"ReturnValue");
    return true;
}

bool ReadResponse(void* comp, uint8_t channel, uint8_t& out) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(comp), L"GetCollisionResponseToChannel");
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid() || !f.Set<uint8_t>(L"Channel", channel) || !ue_wrap::Call(comp, f)) return false;
    out = f.Get<uint8_t>(L"ReturnValue");
    return true;
}

void Collider(const char* who, void* comp) {
    if (!comp || !R::IsLive(comp)) {
        UE_LOGI("[OCTA-PROBE] collider %s: none (the component did not resolve)", who);
        return;
    }
    FVector at{};
    const bool haveAt = E::TryGetComponentLocation(comp, at);
    uint8_t enabled = 0, objType = 0, response = 0;
    bool overlap = false;
    float radius = 0.f;
    const bool okEnabled = Read(comp, L"GetCollisionEnabled", enabled);
    const bool okType = Read(comp, L"GetCollisionObjectType", objType);
    const bool okOverlap = Read(comp, L"GetGenerateOverlapEvents", overlap);
    const bool okResponse = ReadResponse(comp, kPawnChannel, response);
    const bool okRadius = Read(comp, L"GetScaledSphereRadius", radius) || Read(comp, L"GetScaledCapsuleRadius", radius);
    char b[5][12];
    std::snprintf(b[0], sizeof(b[0]), okEnabled ? "%d" : "?", enabled);
    std::snprintf(b[1], sizeof(b[1]), okType ? "%d" : "?", objType);
    std::snprintf(b[2], sizeof(b[2]), okOverlap ? "%d" : "?", overlap ? 1 : 0);
    std::snprintf(b[3], sizeof(b[3]), okResponse ? "%d" : "?", response);
    std::snprintf(b[4], sizeof(b[4]), okRadius ? "%.0f" : "?", radius);
    UE_LOGI("[OCTA-PROBE] collider %s class=%ls at=%s(%.0f,%.0f,%.0f) enabled=%s(0 none 1 query 2 physics 3 both) "
            "objType=%s overlapEvents=%s pawnResponse=%s(0 ignore 1 overlap 2 block) radius=%s",
            who, R::ClassNameOf(comp).c_str(), haveAt ? "" : "unread", at.X, at.Y, at.Z, b[0], b[1], b[2], b[3], b[4]);
}

float Flat(const FVector& a, const FVector& b) { return std::hypot(a.X - b.X, a.Y - b.Y); }

}  // namespace

void Tick(void* octa, void* hostPlayer, void* puppetBody) {
    const uint64_t now = coop::net::NowMs();
    // Each collider once, as its body resolves: a body's root component is its capsule (K2_GetRootComponent, as the
    // code base reads one). The Octa's two seconds after it is first seen, once its BeginPlay has run.
    if (octa && !g_octaSeenAt) g_octaSeenAt = now;
    if (!g_octaSaid && octa && now - g_octaSeenAt >= kBeginPlayMs) {
        g_octaSaid = true;
        Collider("octa.minSphere", GO::MinSphere(octa));
    }
    if (!g_hostSaid && hostPlayer) {
        g_hostSaid = true;
        Collider("host player root", E::RootComponentOf(hostPlayer));
    }
    if (!g_puppetSaid && puppetBody) {
        g_puppetSaid = true;
        Collider("client puppet root", E::RootComponentOf(puppetBody));
    }
    if (now < g_nextMs || g_lines >= kMaxLines) return;
    g_nextMs = now + kEveryMs;
    ++g_lines;
    FVector oa{}, pa{}, ha{};
    const bool haveOcta = octa && E::TryGetActorLocation(octa, oa);
    const bool havePuppet = puppetBody && E::TryGetActorLocation(puppetBody, pa);
    const bool haveHost = hostPlayer && E::TryGetActorLocation(hostPlayer, ha);
    void* target = GO::TargetActor(octa);
    const coop::geom_octa_sync::Counters c = coop::geom_octa_sync::GetCounters();
    UE_LOGI("[OCTA-PROBE] t=%llu octa=%s(%.0f,%.0f,%.0f) puppet=%s(%.0f,%.0f,%.0f) dOctaPuppet=%.0f "
            "hostPlayer=%s(%.0f,%.0f,%.0f) dOctaHost=%.0f target=%ls isAnim=%d overlaps min=%u prop=%u",
            static_cast<unsigned long long>(now), haveOcta ? "" : "unread", oa.X, oa.Y, oa.Z,
            puppetBody ? (havePuppet ? "" : "unread") : "unresolved", pa.X, pa.Y, pa.Z,
            haveOcta && havePuppet ? Flat(oa, pa) : -1.f, hostPlayer ? (haveHost ? "" : "unread") : "unresolved",
            ha.X, ha.Y, ha.Z, haveOcta && haveHost ? Flat(oa, ha) : -1.f, target ? R::ClassNameOf(target).c_str() : L"none",
            GO::IsAnim(octa) ? 1 : 0, static_cast<unsigned>(c.overlapMin), static_cast<unsigned>(c.overlapProp));
}

void Reset() {
    g_nextMs = 0;
    g_lines = 0;
    g_octaSeenAt = 0;
    g_octaSaid = g_hostSaid = g_puppetSaid = false;
}

}  // namespace coop::dev::octa_probe
