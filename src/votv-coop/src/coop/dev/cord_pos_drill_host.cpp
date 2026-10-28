// coop/dev/cord_pos_drill_host.cpp -- see coop/dev/cord_pos_drill.h: the host's half. On the tick it sees slot 1's
// peer-world-ready it spawns three coffee makers (the first at the measured coffee spot, the others at ledge spots
// by free powered sockets, or beside the first), plugs each once its cord exists, and says each one's rest in the
// drill chat once its copy lies still.

#include "cord_pos_drill_detail.h"  // co-located private header (src tree, not include/)
#include "cord_drill_detail.h"      // co-located private header: FieldOf, SocketGeometry

#include "coop/dev/drill_chat.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/devices/cord.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace coop::dev::cord_pos_drill::detail {
namespace {

namespace CD = ue_wrap::cord;
namespace CDD = coop::dev::cord_drill::detail;
namespace DC = coop::dev::drill_chat;
namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;

constexpr const wchar_t* kMakerCls = L"prop_coffeeMaker_C";

enum class Ph : uint8_t { Wait, Plug, Rest, Done };

struct Maker {
    ue_wrap::CachedObjRef actor, sock;
    std::string key, sockKey;
    RestClock clock;
    bool plugged = false, said = false;
};
struct Host {
    Ph ph = Ph::Wait;
    uint64_t phaseMs = 0;
    Maker m[kMakers];
};
Host g_h;

// The host's environment or instrument cannot stage the drill (no socket, a socket occupied, a maker that does not
// spawn): not a verdict on the product. The run ends: the client is told by the drill chat.
void Invalid(const char* why) {
    UE_LOGW("[CORDPOS-DRILL] INVALID (host): %s", why);
    DC::Say(kInvalidLine);
    g_h.ph = Ph::Done;
}

// The socket's `powered` flag: 1, 0, or -1 when the class has none.
int PoweredOf(void* socket) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!socket || !R::FindBoolProperty(R::ClassOf(socket), L"powered", off, mask)) return -1;
    return (static_cast<const uint8_t*>(socket)[off] & mask) ? 1 : 0;
}

std::string SockKeyOf(void* socket) { return CDD::Narrow(ue_wrap::prop::GetActorSaveKeyString(socket)); }

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// ---- the sockets ----

struct Cand {
    void* sock = nullptr;
    ue_wrap::FVector at{}, fwd{}, spot{};
    float dFirst = 0.f;
    bool ledge = false;
};
struct Census {
    void* first = nullptr;
    std::vector<Cand> free;   // free, powered sockets with a readable face, the first excluded
};

void Collect(void* p, void* o, int32_t) {
    auto& cs = *static_cast<Census*>(p);
    if (!R::IsLive(o) || R::NameStartsWith(R::NameOf(o), L"Default__")) return;
    if (SockKeyOf(o) == kFirstSocketKey) { cs.first = o; return; }
    if (CDD::FieldOf(o, L"cord") || CDD::FieldOf(o, L"parent") || PoweredOf(o) != 1) return;
    Cand c;
    c.sock = o;
    if (!CDD::SocketGeometry(o, c.at, c.fwd)) return;
    cs.free.push_back(c);
}

// A ledge: the surface under the spot kSpotOutCm out from the socket's face lies within kLedgeBandCm of the
// socket's height, and nothing lies within kDropCm below that surface. The spot is then kLiftCm above it.
bool LedgeSpot(void* player, Cand& c) {
    const float x = c.at.X + c.fwd.X * kSpotOutCm, y = c.at.Y + c.fwd.Y * kSpotOutCm;
    ue_wrap::trace::Hit top;
    if (!ue_wrap::trace::LineHitStatDyn(player, {x, y, c.at.Z + kLedgeBandCm}, {x, y, c.at.Z - kLedgeBandCm}, &top) ||
        !top.blocked)
        return false;
    ue_wrap::trace::Hit below;
    const float z = top.point.Z;
    if (!ue_wrap::trace::LineHitStatDyn(player, {x, y, z - 2.f}, {x, y, z - kDropCm}, &below) || below.blocked)
        return false;
    c.spot = {x, y, z + kLiftCm};
    return true;
}

struct Pick {
    void* sock[kMakers] = {};
    ue_wrap::FVector spot[kMakers]{};
    const char* how[kMakers] = {};
    int free = 0, ledges = 0;
};

// The first maker at the measured spot; then up to two ledge sockets nearest the first; a shortfall is made up with
// spots beside the first (kAlongWallCm along the wall, plus then minus), plugged into the nearest unused free
// powered socket within kReachCm of the spot.
bool Choose(void* player, Pick& out) {
    Census cs;
    void* cls = OI::ClassByName(L"cordSocket_C");
    if (cls) OI::ForEachInstance(cls, &Collect, &cs);
    if (!cs.first) return false;
    ue_wrap::FVector firstAt{}, firstFwd{};
    if (CD::CordIn(cs.first) || !CDD::SocketGeometry(cs.first, firstAt, firstFwd)) return false;
    const ue_wrap::FVector firstSpot{kFirstSpotX, kFirstSpotY, kFirstSpotZ};
    out.sock[0] = cs.first;
    out.spot[0] = firstSpot;
    out.how[0] = "measured";
    out.free = static_cast<int>(cs.free.size());
    std::vector<Cand> ledges;
    for (Cand& c : cs.free) {
        c.dFirst = Dist(c.at, firstAt);
        if (LedgeSpot(player, c)) { c.ledge = true; ledges.push_back(c); }
    }
    out.ledges = static_cast<int>(ledges.size());
    std::sort(ledges.begin(), ledges.end(), [](const Cand& a, const Cand& b) { return a.dFirst < b.dFirst; });
    int n = 1;
    std::vector<void*> used;
    for (const Cand& c : ledges) {
        if (n >= kMakers) break;
        out.sock[n] = c.sock;
        out.spot[n] = c.spot;
        out.how[n] = "ledge";
        used.push_back(c.sock);
        ++n;
    }
    const float len = std::hypot(firstFwd.X, firstFwd.Y);
    if (n < kMakers && len < 0.01f) return false;
    int side = 0;
    while (n < kMakers) {
        const float sign = side++ == 0 ? 1.f : -1.f;
        const ue_wrap::FVector spot{firstSpot.X - firstFwd.Y / len * kAlongWallCm * sign,
                                    firstSpot.Y + firstFwd.X / len * kAlongWallCm * sign, firstSpot.Z};
        const Cand* best = nullptr;
        for (const Cand& c : cs.free) {
            if (std::find(used.begin(), used.end(), c.sock) != used.end()) continue;
            const float d = Dist(c.at, spot);
            if (d <= kReachCm && (!best || d < Dist(best->at, spot))) best = &c;
        }
        if (!best) return false;
        used.push_back(best->sock);
        out.sock[n] = best->sock;
        out.spot[n] = spot;
        out.how[n] = "beside";
        ++n;
    }
    return true;
}

// ---- the phases ----

void HostWait(coop::net::Session* s, void* player) {
    if (!s->IsSlotWorldReady(1)) return;
    Pick pick;
    const bool found = Choose(player, pick);
    UE_LOGI("[CORDPOS-DRILL] host setup: free powered sockets=%d ledge=%d casting=%s", pick.free, pick.ledges,
            found ? "found" : "none");
    if (!found) return Invalid("no free first socket, or no sockets within reach of the spots beside it");
    void* cls = R::FindClass(kMakerCls);
    if (!cls) return Invalid("the coffee maker class does not resolve");
    for (int i = 0; i < kMakers; ++i) {
        void* maker = E::SpawnActor(cls, pick.spot[i]);
        Maker& m = g_h.m[i];
        m.key = KeyOf(maker);
        if (!maker || m.key == "?") return Invalid("a maker did not spawn with a key");
        m.actor.Set(maker);
        m.sock.Set(pick.sock[i]);
        m.sockKey = SockKeyOf(pick.sock[i]);
        UE_LOGI("[CORDPOS-DRILL] host maker %d key=%s socket=%s spot=%s (%.0f,%.0f,%.0f)", i + 1, m.key.c_str(),
                m.sockKey.c_str(), pick.how[i], pick.spot[i].X, pick.spot[i].Y, pick.spot[i].Z);
    }
    g_h.ph = Ph::Plug;
    g_h.phaseMs = NowMs();
}

// Each maker is plugged once its cord exists.
void HostPlug() {
    bool all = true;
    for (Maker& m : g_h.m) {
        if (m.plugged) continue;
        void* maker = m.actor.Get();
        void* sock = m.sock.Get();
        if (!maker || !sock) return Invalid("a maker or its socket is gone");
        void* cord = CD::IsCordedProp(maker) ? CD::CordOf(maker) : nullptr;
        if (!cord) { all = false; continue; }
        if (!CD::Connect(cord, sock) || CD::SocketOf(cord) != sock) return Invalid("a scripted connect did not plug");
        m.plugged = true;
        UE_LOGI("[CORDPOS-DRILL] host plugged key=%s into %s", m.key.c_str(), m.sockKey.c_str());
    }
    if (all) {
        g_h.ph = Ph::Rest;
        g_h.phaseMs = NowMs();
    } else if (NowMs() - g_h.phaseMs > kFailMs) {
        Invalid("a maker's cord never appeared");
    }
}

// A maker's copy lying still is said once, with its place: z first (the client's hidden-window bound), then x and y.
void HostRest() {
    const uint64_t now = NowMs();
    bool all = true;
    for (Maker& m : g_h.m) {
        if (m.said) continue;
        void* maker = m.actor.Get();
        if (!maker) return Invalid("a maker is gone before it rested");
        ue_wrap::FVector at{};
        if (m.clock.Update(maker, now, kHostQuietMs) && E::TryGetActorLocation(maker, at)) {
            char line[160];
            std::snprintf(line, sizeof(line), "%s%.1f %.1f %.1f", RestPrefix(m.key).c_str(), at.Z, at.X, at.Y);
            DC::Say(line);
            UE_LOGI("[CORDPOS-DRILL] host rest key=%s at (%.1f,%.1f,%.1f)", m.key.c_str(), at.X, at.Y, at.Z);
            m.said = true;
        } else {
            all = false;
        }
    }
    if (all) {
        UE_LOGI("[CORDPOS-DRILL] host staged %d makers", kMakers);
        g_h.ph = Ph::Done;
    } else if (now - g_h.phaseMs > kFailMs) {
        Invalid("a maker did not come to rest");
    }
}

}  // namespace

void HostTick(coop::net::Session* s) {
    void* player = coop::players::Registry::Get().Local();
    if (!player) return;
    switch (g_h.ph) {
    case Ph::Wait: return HostWait(s, player);
    case Ph::Plug: return HostPlug();
    case Ph::Rest: return HostRest();
    case Ph::Done: return;
    }
}

void HostReset() { g_h = Host{}; }

}  // namespace coop::dev::cord_pos_drill::detail
