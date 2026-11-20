// coop/dev/miner_drill_rig.cpp -- the mining rig drill's reads of the rig and the player, its spawn and the game's own use of a
// held item on a rig; see coop/dev/miner_drill.h.

#include "miner_drill_detail.h"

#include "coop/dev/drill_ground.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/miner.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/hit_result.h"
#include "ue_wrap/world/economy.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace coop::dev::miner_drill::detail {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace M  = ue_wrap::miner;
namespace OI = ue_wrap::object_index;
namespace DG = coop::dev::drill_ground;
namespace EL = coop::element;

namespace {

constexpr const wchar_t* kRigCls  = L"prop_miner_C";
constexpr const wchar_t* kRigPath = L"/Game/objects/prop_miner.prop_miner_C";
constexpr int kQuietTicks = 30;   // a spawned target is at rest after this many quiet ticks in a row

int g_quiet = 0;

void* PtrOf(void* o, const wchar_t* name) {
    const int32_t off = o ? R::FindPropertyOffset(R::ClassOf(o), name) : -1;
    return off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const char*>(o) + off) : nullptr;
}

void* ResolveClass() {
    if (void* c = OI::ClassByName(kRigCls)) return c;
    return ue_wrap::asset_load::LoadObjectByPath(kRigPath);
}

}  // namespace

int EidOf(void* actor) {
    const EL::ElementId eid = EL::Registry::Get().EidForActor(actor);
    return eid == EL::kInvalidId ? 0 : static_cast<int>(eid);
}

int32_t Balance() {
    int32_t v = -1;
    return ue_wrap::economy::ReadPoints(&v) ? v : -1;
}

bool ReadHeld(void* player, Held& h) {
    const int32_t n = player ? R::FindPropertyOffset(R::ClassOf(player), L"holding_name") : -1;
    const int32_t a = player ? R::FindPropertyOffset(R::ClassOf(player), L"holding_actor") : -1;
    if (n < 0 || a < 0) return false;
    h.name  = *reinterpret_cast<const R::FName*>(static_cast<const char*>(player) + n);
    h.text  = R::ToString(h.name);
    h.actor = *reinterpret_cast<void* const*>(static_cast<const char*>(player) + a);
    return true;
}

// The game's use of the item `h` in `player`'s hand on `rig`, with a blocking hit on the rig's own box: an unset hit does
// nothing, and the use with a blocking one runs `upgrade` with the hold's name and destroys the held actor.
bool CallUse(void* rig, void* player, const Held& h) {
    ue_wrap::ParamFrame f(R::FindFunction(R::ClassOf(rig), L"playerUsedOn"));
    if (!f.valid()) return false;
    void* const comp = PtrOf(rig, L"stats");
    ue_wrap::FVector at{};
    E::TryGetActorLocation(rig, at);
    f.Set<void*>(L"player", player);
    f.Set<void*>(L"lookAtComponent", comp);
    f.Set<void*>(L"holdObject", h.actor);
    f.Set<R::FName>(L"holdPropName", h.name);
    if (!ue_wrap::hit_result::Write(f, L"hit", rig, comp, at)) return false;
    return ue_wrap::Call(rig, f);
}

void* SpawnRig(void* me, float fwdCm, float rightCm) {
    void* const c = ResolveClass();
    ue_wrap::FVector at{};
    if (!c || !E::TryGetActorLocation(me, at)) return nullptr;
    const ue_wrap::FVector f = E::GetActorForwardVector(me);
    return E::SpawnActor(c, {at.X + f.X * fwdCm - f.Y * rightCm, at.Y + f.Y * fwdCm + f.X * rightCm, at.Z});
}

std::vector<uint64_t> GpusNear(void* body) {
    struct Walk {
        ue_wrap::FVector at;
        std::vector<uint64_t>* out;
    };
    std::vector<uint64_t> out;
    void* const cls = OI::ClassByName(L"prop_gpu_C");
    Walk w{{}, &out};
    if (!cls || !body || !E::TryGetActorLocation(body, w.at)) return out;
    OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
        Walk* walk = static_cast<Walk*>(ctx);
        ue_wrap::FVector at{};
        if (!obj || !E::TryGetActorLocation(obj, at) || DG::Dist(walk->at, at) > kNearCm) return;
        const int32_t idx = R::InternalIndexOf(obj);
        walk->out->push_back((static_cast<uint64_t>(static_cast<uint32_t>(idx)) << 32) |
                             static_cast<uint32_t>(R::AllocateSlotSerial(idx)));
    }, &w);
    return out;
}

int NewGpusNear(void* body, const std::vector<uint64_t>& before) {
    int n = 0;
    for (const uint64_t key : GpusNear(body)) {
        bool known = false;
        for (const uint64_t b : before) known = known || b == key;
        if (!known) ++n;
    }
    return n;
}

void* LiveProp(uint32_t eid) {
    return eid > 0 ? EL::LiveActorOfType(static_cast<EL::ElementId>(eid), EL::ElementType::Prop) : nullptr;
}

bool RigQuiet(void* rig) {
    if (rig && DG::Quiet(rig)) return ++g_quiet >= kQuietTicks;
    g_quiet = 0;
    return false;
}

bool ClientMayStart() {
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

bool HostMayStart(coop::net::Session& s) {
    return s.IsSlotWorldReady(1) && coop::prop_snapshot::IsBracketClosed(1) &&
           !coop::join_window_baseline::IsLateWindowOpen(1);
}

float PuppetDistance(void* rig) {
    coop::RemotePlayer& rp = coop::puppet_drive::Puppet(1);
    ue_wrap::FVector body{}, at{};
    if (!rig || !rp.valid() || !rp.actor() || !E::TryGetActorLocation(rp.actor(), body) || !E::TryGetActorLocation(rig, at))
        return -1.f;
    return DG::Dist(body, at);
}

int Numbers(const std::string& text, long* out, int max) {
    const char* p = text.c_str();
    int n = 0;
    while (n < max) {
        char* end = nullptr;
        const long v = std::strtol(p, &end, 10);
        if (end == p) break;
        out[n++] = v;
        p = end;
    }
    return n;
}

std::string Narrow(const std::wstring& w) {
    std::string out;
    for (const wchar_t c : w) out.push_back(static_cast<char>(c));
    return out;
}

std::wstring Widen(const std::string& s) {
    std::wstring out;
    for (const char c : s) out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    return out;
}

std::string RigText(void* rig) {
    M::Fields f;
    if (!M::Read(rig, f)) return "(unread)";
    char t[160];
    std::snprintf(t, sizeof(t), "gpus=%d slots=%d fans=%d points=%d powered=%d heat=%.3f", f.gpus, f.slots, f.fans, f.points,
                  f.powered ? 1 : 0, static_cast<double>(f.overheat));
    return t;
}

}  // namespace coop::dev::miner_drill::detail
