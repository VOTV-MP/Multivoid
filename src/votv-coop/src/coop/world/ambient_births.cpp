// coop/world/ambient_births.cpp -- see coop/world/ambient_births.h.

#include "coop/world/ambient_births.h"

#include "coop/element/death_seam.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"

#include <cstddef>

namespace coop::ambient_births {
namespace {

namespace DS = coop::element::death_seam;
namespace OI = ue_wrap::object_index;
namespace R = ue_wrap::reflection;

constexpr size_t kClasses = 4;
constexpr const wchar_t* kNames[kClasses] = {L"walkingTree_C", L"susDirtHole_C", L"beehiveBranch_C", L"prop_waspnest_bee_C"};

// A level load brings the hexahive's field of holes in one go; a queue this size holds every ambient actor of a world.
constexpr size_t kCap = 4096;

bool g_active = false;
bool g_listening = false;
bool g_fullSaid = false;

// The four class objects, by name. A pointer is dropped when the index unlists its class (the listing id moves). A null
// one is asked again when the index's class set has moved since the last ask, and once per pump tick besides: a class
// answers null while it loads and its finishing moves no number (object_index.h:55-58). Never per birth. The residual:
// a birth in the very tick its class finished loading is missed; the class loads with the spawner that hard-references it.
void*    g_cls[kClasses] = {};
uint64_t g_listingId[kClasses] = {};
uint64_t g_unlistSeen = 0;
uint64_t g_setAsked = ~uint64_t{0};
uint64_t g_tick = 0;  // the pump tick, counted by Install
uint64_t g_tickAsked = ~uint64_t{0};

std::vector<Event> g_raw;      // what the listener heard, in order
std::vector<Event> g_ready;    // what Settle passed on, in order
std::vector<Event> g_loading;  // births whose object is still loading: handed over when it reads

void SayFull(const char* which) {
    if (g_fullSaid) return;
    g_fullSaid = true;
    UE_LOGW("ambient_births: the %s queue is full (%zu) -- events are dropped until a lane drains it (the first this run)", which,
            kCap);
}

bool Recognise(void* cls, Class& out) {
    const uint64_t v = OI::ClassUnlistVersion();
    if (v != g_unlistSeen) {
        g_unlistSeen = v;
        for (size_t i = 0; i < kClasses; ++i) {
            if (!g_cls[i] || OI::ClassListingId(g_cls[i]) == g_listingId[i]) continue;
            g_cls[i] = nullptr;
            g_setAsked = ~uint64_t{0};  // the dropped class is asked again now
        }
    }
    const uint64_t set = OI::ClassSetVersion();
    const bool ask = set != g_setAsked || g_tick != g_tickAsked;
    g_setAsked = set;
    g_tickAsked = g_tick;
    for (size_t i = 0; i < kClasses; ++i) {
        if (!g_cls[i] && ask) {
            g_cls[i] = OI::ClassByName(kNames[i]);
            g_listingId[i] = g_cls[i] ? OI::ClassListingId(g_cls[i]) : 0;
        }
        if (g_cls[i] && g_cls[i] == cls) {
            out = static_cast<Class>(i);
            return true;
        }
    }
    return false;
}

void Push(const Event& e) {
    if (g_raw.size() >= kCap) {
        SayFull("listener");
        return;
    }
    g_raw.push_back(e);
}

// The engine's announcements: a class compare and a push, no engine call.
void OnCreated(void*, void* obj, void* cls, int32_t index) {
    Class c;
    if (g_active && Recognise(cls, c)) Push(Event{obj, index, Kind::Birth, c});
}

// An actor's end of play, from the death seam's drain: the engine ends it at EndPlay, well before the object is freed,
// and the free is the moment the object index announces, at the next collection. One handler per class, as the seam's
// handlers carry no context. The object may be marked for death: only its pointer and slot are kept.
template <Class C>
void OnEnd(const DS::ActorEnd& end) {
    if (g_active) Push(Event{end.actor, end.actorIndex, Kind::Death, C});
}

// The four class subscriptions, asked again each pump until the seam installs and every class name resolves; then
// never again (the seam's table has no unsubscribe).
void SubscribeEnds() {
    static bool s_all = false;
    static bool s_one[kClasses] = {};
    if (s_all || !DS::Install()) return;
    const DS::ClassHandler handlers[kClasses] = {&OnEnd<Class::Tree>, &OnEnd<Class::Hole>, &OnEnd<Class::Branch>,
                                                 &OnEnd<Class::Nest>};
    bool all = true;
    for (size_t i = 0; i < kClasses; ++i) {
        if (!s_one[i]) s_one[i] = DS::SubscribeClass(kNames[i], handlers[i]);
        all = all && s_one[i];
    }
    s_all = all;
}

void Ready(const Event& e) {
    if (g_ready.size() >= kCap) {
        SayFull("ready");
        return;
    }
    g_ready.push_back(e);
}

// A birth is handed over once its object reads; a death by its slot alone.
void Settle() {
    if (g_raw.empty() && g_loading.empty()) return;
    size_t keep = 0;
    for (size_t i = 0; i < g_loading.size(); ++i) {
        const Event e = g_loading[i];
        if (R::ObjectAt(e.index) != e.obj || (R::SlotFlags(e.index) & R::slot_flags::Unreachable)) continue;
        if (R::SlotFlags(e.index) & R::slot_flags::NotYetReadable) {
            g_loading[keep++] = e;
            continue;
        }
        if (R::IsLiveByIndex(e.obj, e.index) && !R::NameStartsWith(R::NameOf(e.obj), L"Default__")) Ready(e);
    }
    g_loading.resize(keep);

    for (const Event& e : g_raw) {
        if (e.kind == Kind::Death) {
            for (size_t i = 0; i < g_loading.size(); ++i) {
                if (g_loading[i].obj == e.obj && g_loading[i].index == e.index) {
                    g_loading.erase(g_loading.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
            }
            Ready(e);
            continue;
        }
        if (!R::IsLiveByIndex(e.obj, e.index)) continue;
        if (R::SlotFlags(e.index) & R::slot_flags::NotYetReadable) {
            g_loading.push_back(e);
            continue;
        }
        if (!R::NameStartsWith(R::NameOf(e.obj), L"Default__")) Ready(e);
    }
    g_raw.clear();
}

void SeedOne(void* ctx, void* obj, int32_t index) {
    Push(Event{obj, index, Kind::Birth, *static_cast<Class*>(ctx)});
}

}  // namespace

// The session pump calls this every tick until the session ends: the end subscriptions are asked until they take, the
// listener is added once (the list has no remove) and a session already recording returns at once.
void Install() {
    ++g_tick;
    SubscribeEnds();
    if (g_active) return;
    if (!g_listening) {
        g_raw.reserve(kCap);
        g_ready.reserve(kCap);
        g_listening = OI::AddListener(OI::Listener{nullptr, &OnCreated, nullptr});
        if (!g_listening) {
            static bool said = false;
            if (!said) UE_LOGE("ambient_births: the object index's listener list is full -- no ambient actor is announced");
            said = true;
            return;
        }
    }
    g_raw.clear();
    g_ready.clear();
    g_loading.clear();
    g_fullSaid = false;
    g_setAsked = ~uint64_t{0};  // the seed below asks each class by name once
    g_active = true;
    for (size_t i = 0; i < kClasses; ++i) {
        Class c = static_cast<Class>(i);
        Class seen;
        void* cls = OI::ClassByName(kNames[i]);
        if (cls && Recognise(cls, seen)) OI::ForEachInstance(cls, &SeedOne, &c);
    }
    UE_LOGI("ambient_births: recording; %zu ambient actors stand", g_raw.size());
}

void Drain(Mask mask, std::vector<Event>& out) {
    if (!g_active) return;
    Settle();
    size_t keep = 0;
    for (size_t i = 0; i < g_ready.size(); ++i) {
        const Event& e = g_ready[i];
        if (mask & Bit(e.cls)) out.push_back(e);
        else g_ready[keep++] = e;
    }
    g_ready.resize(keep);
}

void OnDisconnect() {
    g_active = false;
    g_raw.clear();
    g_ready.clear();
    g_loading.clear();
}

}  // namespace coop::ambient_births
