// coop/dev/pr64_drill_coin.cpp -- see coop/dev/pr64_drill.h: the coin arm's fixtures and mint. The host spawns a coin gun
// and a sellable prop, then mints through the gun's own sell verb, the call the host's arbiter makes; the new coins are
// counted from the object index.

#include "pr64_drill_detail.h"

#include "coffee_drill_detail.h"  // co-located private header: SpawnProp

#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace coop::dev::pr64_drill::detail {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
namespace CD = coop::dev::coffee_drill::detail;
using ue_wrap::FVector;

const wchar_t* const kGunPath   = L"/Game/objects/prop_coingun.prop_coingun_C";
const wchar_t* const kPropClass = L"prop_C";   // the coffee drill's cup: a prop_C whose name row is "cup"
const wchar_t* const kPropName  = L"cup";
const wchar_t* const kCoinClass = L"baocoin_C";

struct NearCtx {
    FVector at;
    std::vector<void*>* out;
};

void CollectNear(void* p, void* o, int32_t) {
    auto& c = *static_cast<NearCtx*>(p);
    FVector l{};
    if (!R::IsLive(o) || R::NameStartsWith(R::NameOf(o), L"Default__") || !E::TryGetActorLocation(o, l)) return;
    const float dx = l.X - c.at.X, dy = l.Y - c.at.Y, dz = l.Z - c.at.Z;
    if (dx * dx + dy * dy + dz * dz <= kNearGunCm * kNearGunCm) c.out->push_back(o);
}

std::vector<void*> CoinsNear(const FVector& at) {
    std::vector<void*> v;
    NearCtx cx{at, &v};
    if (void* cls = OI::ClassByName(kCoinClass)) OI::ForEachInstance(cls, &CollectNear, &cx);
    return v;
}

// The gun's own sell, as the host's arbiter calls it: Index is the price, comp the sold prop's StaticMesh.
bool Sell(void* gun, void* prop) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(prop), L"StaticMesh");
    void* mesh = off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(prop) + off) : nullptr;
    void* fn = R::FindFunction(R::ClassOf(gun), L"sell");
    ue_wrap::ParamFrame f(fn);
    if (mesh && fn && f.valid() && f.Set<int32_t>(L"Index", kPrice) && f.Set<void*>(L"comp", mesh) &&
        ue_wrap::Call(gun, f))
        return true;
    Fail("the sell call did not dispatch (StaticMesh %p, sell %p)", mesh, fn);
    return false;
}

}  // namespace

bool SpawnAndMint(const FVector& at, std::vector<ue_wrap::CachedObjRef>& coins) {
    void* gunCls = ue_wrap::asset_load::LoadObjectByPath(kGunPath);
    void* gun = gunCls ? E::SpawnActor(gunCls, {at.X, at.Y, at.Z + kRiseCm}) : nullptr;
    void* prop = CD::SpawnProp(kPropClass, {at.X + kPropSideCm, at.Y, at.Z + kRiseCm}, kPropName);
    if (!gun || !prop) {
        Fail("the fixtures did not spawn (gun class %p, gun %p, prop %p)", gunCls, gun, prop);
        return false;
    }
    FVector gunAt{};
    E::TryGetActorLocation(gun, gunAt);
    // The index lists a birth once its event is drained, so each count follows a drain that has caught up.
    const size_t drained0 = OI::DrainAll();
    const std::vector<void*> before = CoinsNear(gunAt);
    if (!Sell(gun, prop)) return false;
    const size_t drained1 = OI::DrainAll();
    if (drained0 == SIZE_MAX || drained1 == SIZE_MAX) {
        Fail("the object index could not drain (before the sell %zu, after it %zu events applied)", drained0, drained1);
        return false;
    }
    const std::vector<void*> after = CoinsNear(gunAt);
    for (void* c : after) {
        bool old = false;
        for (void* b : before) old = old || b == c;
        if (old) continue;
        coins.emplace_back();
        coins.back().Set(c);
    }
    UE_LOGI("[PR64-DRILL] host minted through sell(Index=%d): baocoin_C near the gun %zu -> %zu, %zu new "
            "(index drained %zu then %zu events)", kPrice, before.size(), after.size(), coins.size(), drained0, drained1);
    if (coins.empty()) Fail("sell minted no coin near the gun (%zu before, %zu after)", before.size(), after.size());
    return !coins.empty();
}

}  // namespace coop::dev::pr64_drill::detail
