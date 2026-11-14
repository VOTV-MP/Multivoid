// coop/dev/atv_riders_probe_acts.cpp -- the reads of the ATV riders probe's measurements; see coop/dev/atv_riders_probe.h.
// Reflection and the engine wrappers only; nothing here writes except the re-skin of M15's own dev act.

#include "atv_riders_probe_detail.h"

#include "coop/element/mirror_managers.h"
#include "coop/interactables/atv_sync.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/player_handshake.h"

#include "ue_wrap/actors/kerfur.h"
#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_component.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::atv_riders_probe::detail {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace K = ue_wrap::kerfur;
using ue_wrap::FRotator;
using ue_wrap::FVector;

float Dist(const FVector& a, const FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The actor's attach parent is `atv`: 1 yes, 0 no, -1 the read did not run.
int ParentIsAtv(void* actor, void* atv) {
    void* parent = nullptr;
    if (!E::GetAttachParentActor(actor, parent)) return -1;
    return parent && parent == atv ? 1 : 0;
}

// The driver's body on this peer, or null.
void* DriverBody() {
    coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(kDriverSlot);
    return p && p->valid() ? p->GetActor() : nullptr;
}

const char* Flag(int v) { return v < 0 ? "?" : v ? "1" : "0"; }

}  // namespace

void* AtvOf(const std::wstring& key) {
    coop::atv_sync::TableView tv;
    return coop::atv_sync::ReadTable(key, tv) ? tv.actor : nullptr;
}

void* KerfurMirrorOf(uint32_t eid) {
    coop::element::Npc* n = coop::element::NpcMirrors().Get(eid);
    void* actor = n ? n->LiveActor() : nullptr;
    return actor && K::IsKerfurActor(actor) ? actor : nullptr;
}

void LogMirrors(const std::wstring& key) {
    void* atv = AtvOf(key);
    std::vector<coop::element::Npc*> npcs;
    coop::element::NpcMirrors().Snapshot(npcs);
    int n = 0;
    for (coop::element::Npc* npc : npcs) {
        void* k = npc ? npc->LiveActor() : nullptr;
        if (!k || !K::IsKerfurActor(k)) continue;
        ++n;
        void* occ = K::GetOccupyCar(k);
        void* parent = nullptr;
        E::GetAttachParentActor(k, parent);
        uint8_t state = 0;
        bool spooky = false;
        uint8_t face = 0;
        const bool readState = K::ReadKerfurState(k, state, spooky, face);
        UE_LOGI("atv-probe: M3 mirror eid=%u occupyCar=%p isTheAtv=%d parent=%p state=%d", npc->GetId(), occ,
                occ && occ == atv ? 1 : 0, parent, readState ? static_cast<int>(state) : -1);
    }
    UE_LOGI("atv-probe: M3 mirrors read=%d", n);
}

bool BodySeated(const std::wstring& key) {
    void* atv = AtvOf(key);
    void* body = DriverBody();
    bool on = false;
    return atv && body && ParentIsAtv(body, atv) == 1 && ue_wrap::puppet::GetOnAtv(body, on) && on;
}

bool KerfurRides(const std::wstring& key, uint32_t eid) {
    void* atv = AtvOf(key);
    void* k = KerfurMirrorOf(eid);
    return atv && k && ParentIsAtv(k, atv) == 1;
}

bool ReadRoot(const std::wstring& key, FVector& out) {
    void* atv = AtvOf(key);
    FRotator rot{};
    return atv && ue_wrap::atv::GetRootTransform(atv, out, rot);
}

Sample TakeSample(const std::wstring& key, uint32_t kerfurEid) {
    Sample s;
    void* atv = AtvOf(key);
    if (!atv) return s;
    FVector at{}, seat{};
    FRotator rot{};
    void* body = DriverBody();
    if (body && ue_wrap::atv::GetSeatTransform(atv, seat, rot) && E::TryGetActorLocation(body, at)) {
        s.bodyCm = Dist(at, seat);
        s.bodyParent = ParentIsAtv(body, atv);
    }
    void* k = kerfurEid ? KerfurMirrorOf(kerfurEid) : nullptr;
    const int32_t off = k ? R::FindPropertyOffset(R::ClassOf(atv), L"sitarrow") : -1;
    void* arrow = off < 0 ? nullptr : *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(atv) + off);
    if (k && arrow && R::IsLive(arrow) && E::TryGetComponentLocation(arrow, seat) && E::TryGetActorLocation(k, at)) {
        s.kerfurCm = Dist(at, seat);
        s.kerfurParent = ParentIsAtv(k, atv);
    }
    return s;
}

void RunSkinProbe() {
    coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(kDriverSlot);
    if (!p || !p->valid()) {
        UE_LOGI("atv-probe: M15 the driver's body is not there");
        return;
    }
    const std::string original = coop::player_handshake::SkinForSlot(kDriverSlot);
    const std::string other = original == "kerfur_omega" ? "kerfur_maid" : "kerfur_omega";
    auto read = [&] {
        bool on = false;
        return ue_wrap::puppet::GetOnAtv(p->GetActor(), on) ? (on ? 1 : 0) : -1;
    };
    const int before = read();
    p->ApplySkin(other);
    const int after = read();
    p->ApplySkin(original);
    const int back = read();
    UE_LOGI("atv-probe: M15 skin=%s before=%s after=%s back=%s", other.c_str(), Flag(before), Flag(after), Flag(back));
}

}  // namespace coop::dev::atv_riders_probe::detail
