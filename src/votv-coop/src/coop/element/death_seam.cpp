// coop/element/death_seam.cpp -- see coop/element/death_seam.h.

#include "coop/element/death_seam.h"

#include "coop/element/registry.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/actor_end_play.h"

#include <utility>
#include <vector>

namespace coop::element::death_seam {
namespace {

namespace R  = ue_wrap::reflection;
namespace EP = ue_wrap::actor_end_play;

// One table per element type, indexed by the enum's value; the types fit in eight.
constexpr int kTypes = 8;
Handler g_handlers[kTypes][kMaxHandlersPerType] = {};
int     g_handlerCount[kTypes] = {};

struct ClassSub {
    R::FName     name;
    ClassHandler handler;
};
ClassSub g_classSubs[kMaxClassHandlers] = {};
int      g_classSubCount = 0;

bool g_anySubscribed = false;

// The ends recorded inside EndPlay and not yet handed out. A stream-out of a large level ends many
// actors in one call; past the cap an end is dropped, and the drain that follows says how many.
constexpr size_t kQueueCap = 8192;
struct ClassEnd {
    ActorEnd     end;
    ClassHandler handler;
};
std::vector<Death>    g_queue, g_draining;
std::vector<ClassEnd> g_classQueue, g_classDraining;
size_t g_droppedInBatch = 0;

int TypeIndex(ElementType t) { return static_cast<int>(t) & (kTypes - 1); }

bool SameName(const R::FName& a, const R::FName& b) {
    return a.ComparisonIndex == b.ComparisonIndex && a.Number == b.Number;
}

// The class subscriptions `actor` answers to: its class chain's names against each subscription, one
// record per subscription however many classes of the chain it names.
void RecordByClass(void* actor, bool streamedOut) {
    R::FName chain[16];
    int depth = 0;
    for (void* cls = R::ClassOf(actor); cls && depth < 16; cls = R::SuperStructOf(cls)) chain[depth++] = R::NameOf(cls);
    const int32_t index = R::InternalIndexOf(actor);
    for (int i = 0; i < g_classSubCount; ++i) {
        for (int d = 0; d < depth; ++d) {
            if (!SameName(chain[d], g_classSubs[i].name)) continue;
            if (g_classQueue.size() >= kQueueCap) {
                ++g_droppedInBatch;
                return;
            }
            g_classQueue.push_back(ClassEnd{ActorEnd{actor, index, streamedOut}, g_classSubs[i].handler});
            break;
        }
    }
}

// Inside the engine's EndPlay: record, nothing else. A level transition or the quit returns at once --
// the transition calls this for every actor of the world.
void OnEndPlay(void* actor, EP::Reason reason) {
    if (!g_anySubscribed) return;
    if (reason != EP::Reason::Destroyed && reason != EP::Reason::RemovedFromWorld) return;
    const bool streamedOut = reason == EP::Reason::RemovedFromWorld;
    if (g_classSubCount > 0) RecordByClass(actor, streamedOut);
    Registry& reg = Registry::Get();
    const ElementId eid = reg.EidForActor(actor);
    if (eid == kInvalidId) return;
    const Element* e = reg.Get(eid);
    if (!e || g_handlerCount[TypeIndex(e->GetType())] == 0) return;
    if (g_queue.size() >= kQueueCap) {
        ++g_droppedInBatch;
        return;
    }
    g_queue.push_back(Death{eid, e->GetType(), e->IsMirror(), streamedOut, actor, R::InternalIndexOf(actor)});
}

}  // namespace

bool Subscribe(ElementType type, Handler handler) {
    if (!handler) return false;
    const int t = TypeIndex(type);
    for (int i = 0; i < g_handlerCount[t]; ++i)
        if (g_handlers[t][i] == handler) return true;
    if (g_handlerCount[t] >= kMaxHandlersPerType) {
        UE_LOGW("death_seam: the handler table of element type %d is full", t);
        return false;
    }
    g_handlers[t][g_handlerCount[t]++] = handler;
    g_anySubscribed = true;
    return true;
}

bool SubscribeClass(const wchar_t* className, ClassHandler handler) {
    if (!className || !handler) return false;
    const R::FName name = ue_wrap::fname_utils::StringToFName(className);
    if (name.ComparisonIndex == 0 && name.Number == 0) return false;  // the engine cannot name it yet
    for (int i = 0; i < g_classSubCount; ++i)
        if (SameName(g_classSubs[i].name, name) && g_classSubs[i].handler == handler) return true;
    if (g_classSubCount >= kMaxClassHandlers) {
        UE_LOGW("death_seam: the class handler table is full -- '%ls' is not heard", className);
        return false;
    }
    g_classSubs[g_classSubCount++] = ClassSub{name, handler};
    g_anySubscribed = true;
    return true;
}

bool Install() {
    static bool s_installed = false;
    if (s_installed) return true;
    if (!EP::IsInstalled()) return false;
    s_installed = EP::AddSink(&OnEndPlay);
    if (s_installed) UE_LOGI("death_seam: listening on the engine's end of play");
    return s_installed;
}

void Drain() {
    if (g_droppedInBatch > 0) {
        UE_LOGW("death_seam: %zu ends of play past the queue's %zu in one batch were dropped -- their lanes "
                "did not hear them", g_droppedInBatch, kQueueCap);
        g_droppedInBatch = 0;
    }
    // Swapped out first: a handler that destroys an actor ends its play into the fresh queue, for the
    // next drain, never into the one being read.
    if (!g_queue.empty()) {
        std::swap(g_queue, g_draining);
        for (const Death& d : g_draining) {
            const int t = TypeIndex(d.type);
            for (int i = 0; i < g_handlerCount[t]; ++i) g_handlers[t][i](d);
        }
        g_draining.clear();
    }
    if (!g_classQueue.empty()) {
        std::swap(g_classQueue, g_classDraining);
        for (const ClassEnd& c : g_classDraining) c.handler(c.end);
        g_classDraining.clear();
    }
}

}  // namespace coop::element::death_seam
