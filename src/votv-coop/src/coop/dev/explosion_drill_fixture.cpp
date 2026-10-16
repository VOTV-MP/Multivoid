// coop/dev/explosion_drill_fixture.cpp -- see coop/dev/explosion_drill_internal.h.

#include "coop/dev/explosion_drill_internal.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>

namespace coop::dev::explosion_drill::detail {

namespace E = ue_wrap::engine;

namespace {
constexpr float kRestSpeed = 5.f;   // cm/s: a body is at rest under this (the probe's rule)
constexpr int   kRestTicks = 10;    // consecutive Ticks under kRestSpeed
constexpr float kAhead = 600.f;     // the blast point, along the puppet's yaw-forward
constexpr float kClearance = 100.f; // short of whatever the line to the point hits
constexpr float kMinAhead = 300.f;  // nearer than this the fixture is blocked
}  // namespace

float Dist(const Vec& a, const Vec& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
float Len(const Vec& v) { return std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z); }
bool Past(Clock::time_point t, std::chrono::seconds bound) { return Clock::now() - t >= bound; }

void Info(const char* fmt, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    UE_LOGI("[EXPLOSION-DRILL] %s", buf);
}
void Warn(const char* fmt, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    UE_LOGW("[EXPLOSION-DRILL] %s", buf);
}

bool AtRest(const Rest& r) { return r.ticks >= kRestTicks; }
void NoteSpeed(Rest& r, float speed) { r.ticks = speed < kRestSpeed ? r.ticks + 1 : 0; }
void NoteMoved(Rest& r, const Vec& p) {
    const Clock::time_point now = Clock::now();
    if (r.have) {
        const float dt = std::chrono::duration<float>(now - r.lastAt).count();
        if (dt > 0.f) NoteSpeed(r, Dist(p, r.last) / dt);
    }
    r.have = true;
    r.last = p;
    r.lastAt = now;
}
void NotePhysics(Rest& r, void* actor) {
    Vec lin{}, ang{};
    if (E::GetActorRootPhysicsVelocity(actor, lin, ang)) NoteSpeed(r, Len(lin));
    else r.ticks = 0;
}

Fix ComputeB(void* body, void* ctx, Vec& out) {
    Vec at{};
    if (!body || !ctx || !E::TryGetActorLocation(body, at)) return Fix::Wait;
    const Vec f = E::GetActorForwardVector(body);
    const float flat = std::sqrt(f.X * f.X + f.Y * f.Y);
    if (flat < 0.001f) return Fix::Wait;
    const Vec dir{f.X / flat, f.Y / flat, 0.f};
    float ahead = kAhead;
    ue_wrap::trace::Hit hit;
    if (!ue_wrap::trace::LineHitStatDyn(ctx, at, {at.X + dir.X * kAhead, at.Y + dir.Y * kAhead, at.Z}, &hit))
        return Fix::NoTrace;
    if (hit.blocked) ahead = Dist(at, hit.point) - kClearance;
    if (ahead < kMinAhead) return Fix::Blocked;
    out = {at.X + dir.X * ahead, at.Y + dir.Y * ahead, at.Z};
    return Fix::Ok;
}

void RestLine(uint32_t eid, const Vec& at) {
    Info("rest eid=%u at=(%.1f,%.1f,%.1f)", static_cast<unsigned>(eid), at.X, at.Y, at.Z);
}

}  // namespace coop::dev::explosion_drill::detail
