// coop/dev/join_leftovers_drill.cpp -- see coop/dev/join_leftovers_drill.h.

#include "coop/dev/join_leftovers_drill.h"

#include "coop/net/net_clock.h"
#include "coop/world/world_actor_sync.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

namespace coop::dev::join_leftovers_drill {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

struct Spec {
    const char*    name;
    const char*    event;
    const wchar_t* product;
    bool           hold;   // the host stops the product's tick, so it outlasts its flight
};

constexpr Spec kSpecs[] = {
    {"jlBorg",    "borgRozital", L"rozitBorg_C",  true},
    {"jlSalt",    "salt",        L"saltpile_C",   false},
    {"jlMorning", "morningGay",  L"morningUfo_C", true},
};

const Spec* SpecOf(const std::string& n) {
    for (const Spec& s : kSpecs)
        if (n == s.name) return &s;
    return nullptr;
}

constexpr uint64_t kHoldAfterMs = 12000, kSayEveryMs = 2000, kSayUntilMs = 150000;
uint64_t g_clientSeenMs = 0, g_lastSayHost = 0, g_lastSayClient = 0;
bool     g_held = false;

size_t CountLive(const wchar_t* cls) {
    size_t n = 0;
    for (void* obj : R::FindObjectsByClass(cls))
        if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) ++n;
    return n;
}

// What this peer holds of the class now: the count, and for each its place and whether the world-actor lane made it a copy.
void Say(const char* who, const Spec& s, uint64_t sinceMs) {
    const unsigned t = static_cast<unsigned>(sinceMs / 1000);
    size_t n = 0;
    for (void* obj : R::FindObjectsByClass(s.product)) {
        if (!obj || !R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
        ++n;
        ue_wrap::FVector at{};
        E::TryGetActorLocation(obj, at);
        UE_LOGI("[JL-DRILL] [%s] %s t=%us: %ls #%zu at (%.0f, %.0f, %.0f), mirror %d", who, s.name, t, s.product, n, at.X, at.Y, at.Z,
                coop::world_actor_sync::IsMirroredActor(obj) ? 1 : 0);
    }
    UE_LOGI("[JL-DRILL] [%s] %s t=%us: %zu %ls live", who, s.name, t, n, s.product);
}

bool SayDue(uint64_t sinceMs, uint64_t& last) {
    if (sinceMs > kSayUntilMs) return false;
    const uint64_t bucket = sinceMs / kSayEveryMs + 1;
    if (bucket <= last) return false;
    last = bucket;
    return true;
}

}  // namespace

bool IsLeftoversDrill(const std::string& n) { return SpecOf(n) != nullptr; }

const char* EventOf(const std::string& n) {
    const Spec* s = SpecOf(n);
    return s ? s->event : nullptr;
}

bool AlreadyHeld(const std::string& n) {
    const Spec* s = SpecOf(n);
    return s && CountLive(s->product) > 0;
}

void ClientStep(const std::string& name) {
    const Spec* s = SpecOf(name);
    if (!s) return;
    const uint64_t now = coop::net::NowMs();
    if (!g_clientSeenMs) g_clientSeenMs = now;
    const uint64_t since = now - g_clientSeenMs;
    if (SayDue(since, g_lastSayClient)) Say("C", *s, since);
}

void HostStep(const std::string& name, uint64_t since) {
    const Spec* s = SpecOf(name);
    if (!s) return;
    if (SayDue(since, g_lastSayHost)) Say("H", *s, since);
    if (s->hold && !g_held && since >= kHoldAfterMs) {
        for (void* obj : R::FindObjectsByClass(s->product)) {
            if (!obj || !R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
            g_held = true;
            E::SetActorTickEnabled(obj, false);
            UE_LOGI("[JL-DRILL] [H] %s: the host's %ls held (its tick stopped) so it outlasts its flight", name.c_str(), s->product);
            break;
        }
    }
}

void OnDisconnect() {
    g_clientSeenMs = g_lastSayHost = g_lastSayClient = 0;
    g_held = false;
}

}  // namespace coop::dev::join_leftovers_drill
