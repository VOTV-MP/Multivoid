// coop/dev/cord_drill_cast.cpp -- see coop/dev/cord_drill_detail.h: the makers and sockets as this peer holds them, and
// the reads of a socket's place that the recipe walks by.

#include "cord_drill_detail.h"  // co-located private header (src tree, not include/)

#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/cord.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_nav.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace coop::dev::cord_drill::detail {
namespace {

namespace R = ue_wrap::reflection;
namespace CD = ue_wrap::cord;
namespace E = ue_wrap::engine;

Cast g_cast;

// A scene component's world forward (USceneComponent::GetForwardVector, reflected); false when it did not read.
bool ForwardOf(void* comp, ue_wrap::FVector& out) {
    if (!comp || !R::IsLive(comp)) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(comp), L"GetForwardVector");
    const int32_t ret = fn ? R::FindParamOffset(fn, L"ReturnValue") : -1;
    const int32_t fs = fn ? R::FunctionFrameSize(fn) : 0;
    if (ret < 0 || fs < ret + static_cast<int32_t>(sizeof(ue_wrap::FVector))) return false;
    std::vector<uint8_t> f(static_cast<size_t>(fs), 0u);
    if (!R::CallFunction(comp, fn, f.data())) return false;
    std::memcpy(&out, f.data() + ret, sizeof(out));
    return true;
}

const char* LetterOf(int m) { return m == 0 ? "A" : "B"; }

void AppendLink(std::string& out, int m) {
    out += LetterOf(m);
    const int l = LinkOf(m);
    if (l == 0) out += " unplugged";
    else if (l < 0) out += " missing";
    else if (l == 9) out += " in another socket";
    else out += " in " + std::to_string(l);
}

}  // namespace

Cast& TheCast() { return g_cast; }

void ResetCast() { g_cast = Cast{}; }

void* Maker(int m) {
    void* p = g_cast.maker[m].Get();
    return p && CD::IsCordedProp(p) ? p : nullptr;
}

void* CordOfMaker(int m) {
    void* p = Maker(m);
    return p ? CD::CordOf(p) : nullptr;
}

void* Sock(int k) { return g_cast.sock[k].Get(); }

int LinkOf(int m) {
    void* cord = CordOfMaker(m);
    if (!cord) return -1;
    void* s = CD::SocketOf(cord);
    if (!s) return 0;
    for (int k = 0; k < kSocks; ++k)
        if (s == Sock(k)) return k + 1;
    return 9;
}

bool Matches(const Expect& e) { return LinkOf(0) == e.a && LinkOf(1) == e.b; }

std::string LinkText() {
    std::string out;
    AppendLink(out, 0);
    out += ", ";
    AppendLink(out, 1);
    return out;
}

void* FieldOf(void* obj, const wchar_t* name) {
    if (!obj || !R::IsLive(obj)) return nullptr;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(obj), name);
    return off < 0 ? nullptr : *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(obj) + off);
}

bool SocketGeometry(void* socket, ue_wrap::FVector& at, ue_wrap::FVector& fwd) {
    return socket && E::TryGetActorLocation(socket, at) && ForwardOf(FieldOf(socket, L"dir"), fwd);
}

bool NominalStand(void* player, const ue_wrap::FVector& at, const ue_wrap::FVector& fwd, ue_wrap::FVector& nav) {
    const ue_wrap::FVector raw{at.X + fwd.X * kStandOutCm, at.Y + fwd.Y * kStandOutCm, at.Z};
    const ue_wrap::FVector box{50.f, 50.f, 250.f};
    return player && E::ProjectToNav(player, raw, box, &nav) && std::hypot(nav.X - raw.X, nav.Y - raw.Y) <= kStandSlipCm;
}

bool PeerStand(void* player, const ue_wrap::FVector& at, const ue_wrap::FVector& fwd, const ue_wrap::FVector& nominal,
               Peer who, ue_wrap::FVector& stand, ue_wrap::FVector& nav) {
    const float sign = who == Peer::Host ? 1.f : -1.f;
    const ue_wrap::FVector raw{nominal.X - fwd.Y * kPeerSideCm * sign, nominal.Y + fwd.X * kPeerSideCm * sign, at.Z};
    const ue_wrap::FVector box{50.f, 50.f, 250.f};
    if (!player || !E::ProjectToNav(player, raw, box, &nav) || std::hypot(nav.X - raw.X, nav.Y - raw.Y) > kStandSlipCm)
        return false;
    stand = raw;
    return true;
}

bool StandOf(void* player, void* socket, Peer who, ue_wrap::FVector& stand) {
    ue_wrap::FVector at{}, fwd{}, nominal{}, nav{};
    return SocketGeometry(socket, at, fwd) && NominalStand(player, at, fwd, nominal) &&
           PeerStand(player, at, fwd, nominal, who, stand, nav);
}

}  // namespace coop::dev::cord_drill::detail
