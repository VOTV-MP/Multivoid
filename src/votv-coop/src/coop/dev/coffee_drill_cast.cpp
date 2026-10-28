// coop/dev/coffee_drill_cast.cpp -- see coop/dev/coffee_drill_detail.h: the devices as this peer holds them, the
// maker's flags, the props the host spawns, and the host's pick of a socket.

#include "coffee_drill_detail.h"  // co-located private header (src tree, not include/)
#include "cord_drill_detail.h"    // co-located private header: the stand points and socket reads the cord drill proved

#include "coop/dev/director/routes.h"
#include "coop/props/prop_drive_stream.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/remote_prop.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_nav.h"
#include "ue_wrap/world/economy.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace coop::dev::coffee_drill::detail {
namespace {

namespace R  = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;
namespace E  = ue_wrap::engine;
namespace CDD = coop::dev::cord_drill::detail;

Cast g_cast;

struct BoolProp {
    int32_t off = -1;
    uint8_t mask = 0;
};

// The maker's four flags as the class lays them out, asked again when another class object comes.
struct MakerLayout {
    void* cls = nullptr;
    BoolProp cup, coffee, ready, power;
    bool ok = false;
};

bool ResolveBool(void* cls, const wchar_t* name, BoolProp& out) {
    return R::FindBoolProperty(cls, name, out.off, out.mask);
}

int ReadBool(void* obj, const BoolProp& p) {
    return (reinterpret_cast<const uint8_t*>(obj)[p.off] & p.mask) ? 1 : 0;
}

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// ---- the pick ----

struct PickCtx {
    void* player;
    ue_wrap::FVector vendStand;
    float bestCm;
    Pick* out;
};

// The route from the vending machine's stand to `to`, as the director walks it: its length, and the largest height
// it strays from the vending stand. False when there is no route, it ends short of `to`, or a waypoint is off
// the vending floor or the route is too long.
bool RouteOnFloor(void* player, const ue_wrap::FVector& from, const ue_wrap::FVector& to, float& lengthCm,
                  float& maxDzCm) {
    std::vector<ue_wrap::FVector> route;
    if (!coop::director::RouteFrom(player, from, to, &route) || route.size() < 2) return false;
    const ue_wrap::FVector& end = route.back();
    if (std::hypot(end.X - to.X, end.Y - to.Y) > CDD::kStandReachCm) return false;
    lengthCm = 0.f;
    maxDzCm = 0.f;
    for (size_t i = 0; i < route.size(); ++i) {
        if (i) lengthCm += Dist(route[i - 1], route[i]);
        const float dz = std::fabs(route[i].Z - from.Z);
        if (dz > maxDzCm) maxDzCm = dz;
    }
    return maxDzCm <= kFloorCm && lengthCm <= kRouteCm;
}

void ConsiderSocket(void* p, void* o, int32_t) {
    auto& cx = *static_cast<PickCtx*>(p);
    if (!R::IsLive(o) || R::NameStartsWith(R::NameOf(o), L"Default__")) return;
    Pick& pk = *cx.out;
    if (CDD::FieldOf(o, L"cord") || CDD::FieldOf(o, L"parent")) return;   // a level socket, free
    ++pk.free;
    BoolProp powered;
    if (!ResolveBool(R::ClassOf(o), L"powered", powered) || !ReadBool(o, powered)) return;
    ++pk.powered;
    ue_wrap::FVector at{}, fwd{}, nominal{}, stand{}, nav{}, hostStand{}, hostNav{};
    if (!CDD::SocketGeometry(o, at, fwd) || !CDD::NominalStand(cx.player, at, fwd, nominal) ||
        !CDD::PeerStand(cx.player, at, fwd, nominal, CDD::Peer::Client, stand, nav) ||
        !CDD::PeerStand(cx.player, at, fwd, nominal, CDD::Peer::Host, hostStand, hostNav))
        return;
    ++pk.placed;
    // The client walks from the vending machine to its stand: both stands must be on the vending machine's floor.
    if (std::fabs(nav.Z - cx.vendStand.Z) > kFloorCm || std::fabs(hostNav.Z - cx.vendStand.Z) > kFloorCm) {
        ++pk.offFloor;
        return;
    }
    float lengthCm = 0.f, maxDzCm = 0.f;
    if (!RouteOnFloor(cx.player, cx.vendStand, nav, lengthCm, maxDzCm)) {
        ++pk.noRoute;
        return;
    }
    // The maker's spot is to the side of the stand point, as the cord drill's makers are; the line from knee height
    // there must be clear, so it does not spawn inside a wall.
    const ue_wrap::FVector knee{nominal.X, nominal.Y, nominal.Z + CDD::kKneeCm};
    const ue_wrap::FVector spot{nominal.X - fwd.Y * CDD::kSideCm, nominal.Y + fwd.X * CDD::kSideCm, at.Z};
    if (ue_wrap::trace::LineBlockedStatDyn(cx.player, knee, spot) != 0) {
        ++pk.blocked;
        return;
    }
    if (pk.sock && lengthCm >= cx.bestCm) return;
    cx.bestCm = lengthCm;
    pk.routeCm = lengthCm;
    pk.maxDzCm = maxDzCm;
    pk.clientNav = nav;
    pk.hostNav = hostNav;
    pk.sock = o;
    pk.at = at;
    pk.fwd = fwd;
    pk.stand = nominal;
    pk.makerAt = spot;
}

struct KeysCtx {
    std::vector<std::wstring>* keys;
};

void CollectKey(void* p, void* o, int32_t) {
    if (!R::IsLive(o) || R::NameStartsWith(R::NameOf(o), L"Default__")) return;
    const std::wstring k = ue_wrap::prop::GetKeyString(o);
    if (!k.empty() && k != L"None") static_cast<KeysCtx*>(p)->keys->push_back(k);
}

struct SpawnCtx {
    int32_t off;
    R::FName name;
};

void WriteName(void* ctx, void* actor) {
    const auto* c = static_cast<const SpawnCtx*>(ctx);
    if (c->off >= 0) std::memcpy(static_cast<uint8_t*>(actor) + c->off, &c->name, sizeof(c->name));
}

}  // namespace

Cast& TheCast() { return g_cast; }

void ResetCast() { g_cast = Cast{}; }

void* Vending() { return g_cast.vending.Get(); }
void* Maker() { return g_cast.maker.Get(); }
void* Sock() { return g_cast.sock.Get(); }

bool ReadFlags(void* maker, Flags& out) {
    static MakerLayout s_layout;
    void* cls = maker ? R::ClassOf(maker) : nullptr;
    if (!cls) return false;
    if (s_layout.cls != cls) {
        s_layout = MakerLayout{};
        s_layout.cls = cls;
        s_layout.ok = ResolveBool(cls, L"hasCup", s_layout.cup) && ResolveBool(cls, L"hasCoffee", s_layout.coffee) &&
                      ResolveBool(cls, L"coffeeReady", s_layout.ready) && ResolveBool(cls, L"hasPower", s_layout.power);
    }
    if (!s_layout.ok) return false;
    out.cup = ReadBool(maker, s_layout.cup);
    out.coffee = ReadBool(maker, s_layout.coffee);
    out.ready = ReadBool(maker, s_layout.ready);
    out.power = ReadBool(maker, s_layout.power);
    return true;
}

bool FlagsAre(const Flags& f, int cup, int coffee, int ready) {
    return f.cup == cup && f.coffee == coffee && f.ready == ready;
}

std::string FlagsText(const Flags& f) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "hasCup=%d hasCoffee=%d coffeeReady=%d hasPower=%d", f.cup, f.coffee, f.ready,
                  f.power);
    return buf;
}

int32_t Points() {
    int32_t p = 0;
    return ue_wrap::economy::ReadPoints(&p) ? p : -1;
}

std::vector<std::wstring> CupKeys() {
    std::vector<std::wstring> keys;
    KeysCtx cx{&keys};
    if (void* cls = OI::ClassByName(L"prop_food_coffeeCup_C")) OI::ForEachInstance(cls, &CollectKey, &cx);
    return keys;
}

std::wstring NewCupKey(const std::vector<std::wstring>& before) {
    for (const std::wstring& k : CupKeys())
        if (std::find(before.begin(), before.end(), k) == before.end()) return k;
    return {};
}

void* SpawnProp(const wchar_t* clsName, const ue_wrap::FVector& at, const wchar_t* nameRow) {
    void* cls = R::FindClass(clsName);
    if (!cls) return nullptr;
    SpawnCtx ctx{-1, R::FName{0, 0}};
    if (nameRow) {
        ctx.off = R::FindPropertyOffset(cls, L"name");
        ctx.name = ue_wrap::fname_utils::StringToFName(nameRow);
        if (ctx.off < 0 || ctx.name.ComparisonIndex == 0) return nullptr;
    }
    return E::SpawnActor(cls, at, false, nameRow ? &WriteName : nullptr, &ctx);
}

bool KeyRides(const std::wstring& k) {
    if (k.empty() || k == L"None") return false;
    for (const wchar_t c : k)
        if (c <= 32 || c >= 127) return false;
    return true;
}

void* LiveByKey(const std::wstring& key) {
    return key.empty() ? nullptr : coop::prop_key_index::Find(key);
}

bool HeldByDrive(void* actor) {
    return coop::prop_drive_stream::IsParked(actor) || coop::remote_prop::IsActorUnderAnyDrive(actor);
}

void* NearestVending(void* player) {
    struct Nearest {
        ue_wrap::FVector me;
        void* best;
        float bestCm;
    } nearest{{}, nullptr, 0.f};
    void* cls = OI::ClassByName(L"prop_coffeevending_C");
    if (!cls || !player || !E::TryGetActorLocation(player, nearest.me)) return nullptr;
    OI::ForEachInstance(cls, [](void* p, void* o, int32_t) {
        auto& n = *static_cast<Nearest*>(p);
        if (!R::IsLive(o) || R::NameStartsWith(R::NameOf(o), L"Default__")) return;
        ue_wrap::FVector at{};
        if (!E::TryGetActorLocation(o, at)) return;
        const float d = Dist(at, n.me);
        if (!n.best || d < n.bestCm) { n.best = o; n.bestCm = d; }
    }, &nearest);
    return nearest.best;
}

bool VendingStand(void* player, void* vending, ue_wrap::FVector& place, ue_wrap::FVector& stand) {
    const ue_wrap::FVector box{300.f, 300.f, kFloorCm};
    return vending && E::TryGetActorLocation(vending, place) && E::ProjectToNav(player, place, box, &stand);
}

bool ChooseSocket(void* player, const ue_wrap::FVector& vendStand, Pick& out) {
    PickCtx cx{player, vendStand, 0.f, &out};
    void* cls = OI::ClassByName(L"cordSocket_C");
    if (!cls || !player) return false;
    OI::ForEachInstance(cls, &ConsiderSocket, &cx);
    return out.sock != nullptr;
}

}  // namespace coop::dev::coffee_drill::detail
