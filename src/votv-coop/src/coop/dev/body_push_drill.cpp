// coop/dev/body_push_drill.cpp -- see coop/dev/body_push_drill.h. The entry and what both halves share.

#include "coop/dev/body_push_drill.h"

#include "body_push_drill_detail.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/player/roster.h"
#include "coop/session/rig_verdict.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace coop::dev::body_push_drill {
namespace detail {
namespace {
bool g_over = false;
char g_lastWait[2][160] = {};   // the last wait said, per peer role (host, client)
}  // namespace

bool ReadFootprint(void* actor, Footprint& out) {
    return actor && ue_wrap::engine::GetActorBounds(actor, /*onlyColliding=*/true, out.origin, out.extent);
}

float GapCm(const ue_wrap::FVector& pawnAt, float pawnRadius, const Footprint& f) {
    const float dx = std::fmax(std::fabs(pawnAt.X - f.origin.X) - f.extent.X, 0.f);
    const float dy = std::fmax(std::fabs(pawnAt.Y - f.origin.Y) - f.extent.Y, 0.f);
    return std::hypot(dx, dy) - pawnRadius;
}

bool Over() { return g_over; }
void Rearm() {
    g_over = false;
    g_lastWait[0][0] = g_lastWait[1][0] = '\0';
}

void Wait(char who, const char* fmt, ...) {
    char what[160];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(what, sizeof(what), fmt, ap);
    va_end(ap);
    char* last = g_lastWait[who == 'H' ? 0 : 1];
    if (std::strcmp(last, what) == 0) return;
    std::snprintf(last, sizeof(g_lastWait[0]), "%s", what);
    UE_LOGI("[BODY-PUSH-DRILL] [%c] waiting for %s", who, what);
}

void Invalid(char who, const char* fmt, ...) {
    if (g_over) return;
    g_over = true;
    char why[300];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(why, sizeof(why), fmt, ap);
    va_end(ap);
    UE_LOGW("[BODY-PUSH-DRILL] [%c] INVALID %s", who, why);
    coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid, "%s", why);
}

}  // namespace detail

bool IsEnabled() {
    static const bool on = coop::config::ResolveFlag(::coop::config_registry::rows::body_push_drill);
    return on;
}

void Tick(coop::net::Session* session) {
    if (!IsEnabled() || !session || !session->connected() || detail::Over()) return;
    if (coop::roster::LocalIsHost()) detail::host::Tick(*session);
    else detail::client::Tick(*session);
}

void OnDisconnect() {
    detail::host::Reset();
    detail::client::Reset();
    detail::Rearm();
}

}  // namespace coop::dev::body_push_drill
