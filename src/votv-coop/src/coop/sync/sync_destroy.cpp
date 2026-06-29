// coop/sync/sync_destroy.cpp -- RetireMirror, the type-dispatched mirror destroy funnel.

#include "coop/sync/sync_destroy.h"

#include "coop/element/element_deleter.h"
#include "coop/element/mirror_manager.h"
#include "coop/element/npc.h"
#include "coop/element/prop.h"
#include "coop/element/registry.h"
#include "coop/element/world_actor.h"

namespace coop::sync {

void RetireMirror(coop::element::ElementId eid) {
    using namespace coop::element;
    Element* el = Registry::Get().Get(eid);
    if (!el) return;  // already gone / never bound
    ElementDeleter& del = ElementDeleter::Get();
    switch (el->GetType()) {
        case ElementType::Prop:
            del.Enqueue(MirrorManager<Prop>::Instance().Take(eid));
            break;
        case ElementType::Npc:
            del.Enqueue(MirrorManager<Npc>::Instance().Take(eid));
            break;
        case ElementType::WorldActor:
            del.Enqueue(MirrorManager<WorldActor>::Instance().Take(eid));
            break;
        default:
            // Player/Kerfur/Unknown are not streamed-mirror kinds retired via this
            // funnel (Player has its own lifecycle; Kerfur is a host-only authority
            // record whose RENDERED form is a Prop/Npc retired above by its per-form eid).
            break;
    }
}

}  // namespace coop::sync
