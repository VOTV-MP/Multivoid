// coop/dev/death_seam_census.cpp -- see coop/dev/death_seam_census.h.
#include "coop/dev/death_seam_census.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/death_seam.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <string>

namespace coop::dev::death_seam_census {
namespace {

namespace R  = ue_wrap::reflection;
namespace DS = coop::element::death_seam;
using coop::element::ElementType;

const char* TypeName(ElementType t) {
    switch (t) {
    case ElementType::Player:     return "player";
    case ElementType::Prop:       return "prop";
    case ElementType::Npc:        return "npc";
    case ElementType::Kerfur:     return "kerfur";
    case ElementType::WorldActor: return "world-actor";
    default:                      return "unknown";
    }
}

// In the drain the actor has ended its play and may be marked for death; its slot still names it until
// the purge, so the class is read only while the slot holds it.
void OnDeath(const DS::Death& d) {
    const std::wstring cls =
        R::ObjectAt(d.actorIndex) == d.actor ? R::ClassNameOf(d.actor) : std::wstring(L"(purged)");
    UE_LOGI("[DEATH] %s eid=%u %s %s class=%ls", TypeName(d.type), static_cast<unsigned>(d.eid),
            d.mirror ? "mirror" : "local", d.streamedOut ? "streamed-out" : "destroyed", cls.c_str());
}

}  // namespace

void Tick() {
    static const bool s_on = coop::config::ResolveFlag(::coop::config_registry::rows::death_seam_census);
    static bool s_subscribed = false;
    if (!s_on || s_subscribed) return;
    if (!DS::Install()) return;
    for (ElementType t : {ElementType::Player, ElementType::Prop, ElementType::Npc, ElementType::Kerfur,
                          ElementType::WorldActor})
        DS::Subscribe(t, &OnDeath);
    s_subscribed = true;
    UE_LOGI("[DEATH] census armed on every element type");
}

}  // namespace coop::dev::death_seam_census
