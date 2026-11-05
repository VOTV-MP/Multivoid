// coop/world/vent_crawler_sync.cpp -- see coop/world/vent_crawler_sync.h.
//
// What the crawl plays, from ventCrawler_C's graph (0.9.0n): the box overlap closes the signal room door and writes
// its `active` false, then a vent step at the vent (footstepVent_Cue, att_default, volume 2, pitch 0.85) 2 s on,
// another 0.5 s later (pitch 0.95), and 2.5 s after that moveTL plays from its start. moveTL's stepSounds event plays
// footstepVent_Cue at the crawler (att_default_noao) at its stepVolume times multiplyVolume and its pitch; its
// breakVent event plays break_metal_1 50 cm under the vent (att_default, 0.5, 1.25), spawns eff_metalbreak 25 cm over
// it, writes the door's `active` true and plays ventBreakTL, which swings the vent from where it hung (vent_a) to that
// transform moved 15 cm up its own z and turned (pitch 4.98, roll -15.03) (vent_b), by TLerp at
// lerp(sin(a*2160 deg) + a^(1/3), 1, a^(1/3)) of its track `a`, and at its end sets the vent frozen (setPropProps).
// No lane carries that swing, so a client runs it on its copy through the same engine math.

#include "coop/world/vent_crawler_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/actors/prop_flags.h"
#include "ue_wrap/devices/door.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_audio.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/keyed_objects.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

namespace coop::vent_crawler_sync {
namespace {

namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;
namespace SG = ue_wrap::script_gate;
using Clock = std::chrono::steady_clock;

UE_FACT_FN(kRowBoxOverlap, "vent_crawler_sync", L"ventCrawler_C",
           L"BndEvt__ventCrawler_Box_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);
constexpr int kTag = 0x56435257;   // 'VCRW'

// The crawl's own delays, as times from the cue. ventCrawler_C's graph (bp_cpp.py ventCrawler, ExecuteUbergraph) chains
// Delay(2f) to the first vent step, Delay(0.5f) to the second, and Delay(2.5f) to moveTL->PlayFromStart(): the steps
// at 2.0 and 2.0 + 0.5 = 2.5 s, the timeline at 2.5 + 2.5 = 5.0 s. The bytecode agrees with these values.
constexpr float kStep1 = 2.0f, kStep2 = 2.5f, kTimeline = 5.0f;
constexpr float kShowLimit = 60.f;   // a crawl whose mirror never goes ends here

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watched = false, g_live = false;
std::vector<std::pair<int32_t, int32_t>> g_started;   // the host's crawlers whose crawl was sent, by index and serial
uint32_t g_sent = 0;

// HOST: the crawl a joiner is told of. Stamped with the world it began in, so a crawl of an earlier world is no state.
struct HostCrawl {
    bool began = false;
    uint32_t world = 0;
    Clock::time_point t0{};                 // when its cue was sent
    std::pair<int32_t, int32_t> crawler{-1, 0};   // the host's crawler: gone, the crawl is over
    bool haveVentStart = false;             // the hanging vent's pose was read when the crawl began
    ue_wrap::FVector ventLoc{};             // where the vent hung at the crawl's start: the swing's A
    ue_wrap::FRotator ventRot{};
} g_host;
std::atomic<unsigned> g_sounds{0};
std::atomic<unsigned> g_states{0};   // crawl states accepted from the host
uint32_t g_bursts = 0;

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

// A weak id: the slot and its serial, ALLOCATED when the slot has none (the engine's own weak-pointer rule). The
// engine hands out serials on demand, so an object no weak reference has asked for reads serial 0, and a {slot, 0} id
// never resolves: the show would lose its vent and its crawler at once.
std::pair<int32_t, int32_t> IdOf(void* obj) {
    const int32_t idx = R::InternalIndexOf(obj);
    return {idx, idx >= 0 ? R::AllocateSlotSerial(idx) : 0};
}

void* Resolve(const std::pair<int32_t, int32_t>& id) {
    return id.first >= 0 ? R::ResolveWeakObject(id.first, id.second) : nullptr;
}

// ---- HOST ----------------------------------------------------------------------------------------------------------

// The crawler's first overlap is the one its graph acts on (a DoOnce): the crawl starts there.
void OnOverlapPost(const SG::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    // A running host session is the world's host whether or not a client is seated (it stays in Handshaking until the
    // first one arrives), so a crawl begun alone is recorded for the joiner; only the live cue needs a peer.
    if (!s || s->role() != coop::net::Role::Host || !call.object || !R::IsLive(call.object)) return;
    const auto id = IdOf(call.object);
    const uint32_t world = ue_wrap::world_identity::Generation();
    if (g_host.world != world) {   // another world: its crawls start again
        g_host = HostCrawl{};
        g_started.clear();
    }
    if (std::find(g_started.begin(), g_started.end(), id) != g_started.end()) return;
    g_started.push_back(id);
    g_host.began = true;
    g_host.world = world;
    g_host.t0 = Clock::now();
    g_host.crawler = id;
    // The vent hangs where its swing starts until the crawl's break; a joiner mid-crawl is owed that pose.
    void* vent = ue_wrap::keyed_objects::Resolve(L"ventHanging");
    g_host.haveVentStart =
        vent && E::TryGetActorLocation(vent, g_host.ventLoc) && E::TryGetActorRotation(vent, g_host.ventRot);
    ue_wrap::FVector at{};
    E::TryGetActorLocation(call.object, at);
    coop::net::EventCuePayload p{};
    p.cueId = kCueCrawl;
    p.x = at.X;
    p.y = at.Y;
    p.z = at.Z;
    const bool sent = s->connected() && s->SendReliable(coop::net::ReliableKind::EventCue, &p, sizeof(p));
    if (sent) ++g_sent;
    UE_LOGI("vent_crawler: the crawl started at (%.0f, %.0f, %.0f) -- %s", at.X, at.Y, at.Z,
            sent ? "EventCue sent" : "recorded only, no peer to send the cue to");
}

// ---- CLIENT: the mirror's timeline ---------------------------------------------------------------------------------

// FTimeline's events and float tracks, and the two entry structs, resolved once from the engine's types.
struct Layout {
    bool tried = false, ok = false;
    int32_t timeline = -1, events = -1, floats = -1, length = -1;
    int32_t evSize = 0, evTime = -1, evName = -1;          // FTimelineEventEntry: Time, EventFunc's FunctionName
    int32_t trSize = 0, trCurve = -1, trName = -1;         // FTimelineFloatTrack: FloatCurve, TrackName
    void* curveFn = nullptr;                               // UCurveFloat::GetFloatValue
} g_layout;

int32_t FieldOffset(void* st, const wchar_t* name) {
    for (const auto& f : R::EnumerateStructFields(st))
        if (f.name == name) return f.offset;
    return -1;
}

bool ResolveLayout() {
    if (g_layout.tried) return g_layout.ok;
    void* tl = ue_wrap::object_index::ClassByName(L"TimelineComponent");
    void* curve = ue_wrap::object_index::ClassByName(L"CurveFloat");
    if (!tl || !curve) return false;   // asked again next tick
    g_layout.tried = true;
    Layout& L = g_layout;
    L.timeline = R::FindPropertyOffset(tl, L"TheTimeline");
    void* st = L.timeline >= 0 ? R::PropertyInnerStruct(tl, L"TheTimeline") : nullptr;
    L.events = st ? R::FindPropertyOffset(st, L"Events") : -1;
    L.floats = st ? R::FindPropertyOffset(st, L"InterpFloats") : -1;
    L.length = st ? R::FindPropertyOffset(st, L"Length") : -1;
    void* ev = R::FindObject(L"TimelineEventEntry", L"ScriptStruct");
    void* tr = R::FindObject(L"TimelineFloatTrack", L"ScriptStruct");
    L.evSize = R::StructSize(ev);
    L.evTime = ev ? FieldOffset(ev, L"Time") : -1;
    const int32_t func = ev ? FieldOffset(ev, L"EventFunc") : -1;
    L.evName = func >= 0 ? func + 8 : -1;   // FScriptDelegate: the weak object (8 bytes), then the function's name
    L.trSize = R::StructSize(tr);
    L.trCurve = tr ? FieldOffset(tr, L"FloatCurve") : -1;
    L.trName = tr ? FieldOffset(tr, L"TrackName") : -1;
    L.curveFn = R::FindFunction(curve, L"GetFloatValue");
    L.ok = L.events >= 0 && L.floats >= 0 && L.evSize > 0 && L.evTime >= 0 && L.evName >= 0 && L.trSize > 0 &&
           L.trCurve >= 0 && L.trName >= 0 && L.curveFn;
    if (!L.ok)
        UE_LOGW("vent_crawler: the timeline's layout did not resolve (events=%d floats=%d ev=%d/%d/%d tr=%d/%d/%d "
                "curve=%p) -- a client plays the two vent steps only", L.events, L.floats, L.evSize, L.evTime, L.evName,
                L.trSize, L.trCurve, L.trName, L.curveFn);
    return L.ok;
}

struct TArr {
    uint8_t* data;
    int32_t num, max;
};

TArr ArrayAt(void* obj, int32_t off) {
    TArr a{};
    std::memcpy(&a, static_cast<uint8_t*>(obj) + off, sizeof(a));
    if (!a.data || a.num < 0 || a.num > 256) a.num = 0;
    return a;
}

struct Event {
    float time;
    bool step;   // stepSounds; else breakVent
};

struct Track {
    void* curve;
    std::wstring name;   // lower case
};

// One of the mirror's timelines, by its member name: its stepSounds and breakVent events, its float tracks and length.
bool ReadTimeline(void* crawler, const wchar_t* member, std::vector<Event>& events, std::vector<Track>& tracks,
                  float* length = nullptr) {
    void* tlComp = nullptr;
    const int32_t off = crawler ? R::FindPropertyOffset(R::ClassOf(crawler), member) : -1;
    if (off >= 0) std::memcpy(&tlComp, static_cast<uint8_t*>(crawler) + off, sizeof(tlComp));
    if (!tlComp || !R::IsLive(tlComp) || !ResolveLayout()) return false;
    const Layout& L = g_layout;
    void* timeline = static_cast<uint8_t*>(tlComp) + L.timeline;
    if (length) {
        if (L.length < 0) return false;
        std::memcpy(length, static_cast<uint8_t*>(timeline) + L.length, sizeof(float));
    }
    const TArr ev = ArrayAt(timeline, L.events);
    for (int32_t i = 0; i < ev.num; ++i) {
        const uint8_t* e = ev.data + static_cast<size_t>(i) * L.evSize;
        float t = 0.f;
        std::memcpy(&t, e + L.evTime, sizeof(t));
        const std::wstring fn = Lower(R::ToString(*reinterpret_cast<const R::FName*>(e + L.evName)));
        if (fn.find(L"stepsounds") != std::wstring::npos) events.push_back({t, true});
        else if (fn.find(L"breakvent") != std::wstring::npos) events.push_back({t, false});
    }
    const TArr tr = ArrayAt(timeline, L.floats);
    for (int32_t i = 0; i < tr.num; ++i) {
        const uint8_t* e = tr.data + static_cast<size_t>(i) * L.trSize;
        void* curve = nullptr;
        std::memcpy(&curve, e + L.trCurve, sizeof(curve));
        tracks.push_back({curve, Lower(R::ToString(*reinterpret_cast<const R::FName*>(e + L.trName)))});
    }
    std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.time < b.time; });
    return true;
}

// The named track's value at `t`, or `dflt` when the timeline has no such track.
float TrackAt(const std::vector<Track>& tracks, const wchar_t* name, float t, float dflt) {
    for (const Track& tr : tracks) {
        if (tr.name != name || !tr.curve || !R::IsLive(tr.curve)) continue;
        ue_wrap::ParamFrame f(g_layout.curveFn);
        if (!f.valid() || !f.Set<float>(L"InTime", t) || !ue_wrap::Call(tr.curve, f)) return dflt;
        return f.Get<float>(L"ReturnValue");
    }
    return dflt;
}

// ---- CLIENT: the vent's swing, through the engine's own math -------------------------------------------------------

// An engine object or function found by name once, and again only when its ref has died: the lookups walk the object
// array (FindObject, the class default objects) or a class's function list, so none runs per footstep or swing frame.
// A miss is asked again at most once a second, so a missing asset or function costs no walk per frame either.
struct HeldRef {
    ue_wrap::CachedObjRef ref;
    Clock::time_point missAt{};
};
template <class Find>
void* Held(HeldRef& h, Find&& find) {
    if (void* p = h.ref.Get()) return p;
    const Clock::time_point now = Clock::now();
    if (h.missAt != Clock::time_point{} && now - h.missAt < std::chrono::seconds(1)) return nullptr;
    h.ref.Set(find());
    h.missAt = h.ref.Raw() ? Clock::time_point{} : now;
    return h.ref.Raw();
}

// A static library's class default object and one of its functions, each held.
struct LibFn {
    const wchar_t* libClass;
    const wchar_t* name;
    HeldRef lib, fn;
    void* Resolve(void*& libOut) {
        libOut = Held(lib, [&] { return R::FindClassDefaultObject(libClass); });
        return libOut ? Held(fn, [&] { return R::FindFunction(R::ClassOf(libOut), name); }) : nullptr;
    }
};
LibFn g_makeTransform{L"KismetMathLibrary", L"MakeTransform"};
LibFn g_composeTransforms{L"KismetMathLibrary", L"ComposeTransforms"};
LibFn g_tlerp{L"KismetMathLibrary", L"TLerp"};
LibFn g_spawnEmitter{L"GameplayStatics", L"SpawnEmitterAtLocation"};

template <class Set>
bool MathCall(LibFn& slot, Set&& set, ue_wrap::FTransform& out) {
    void* lib = nullptr;
    void* fn = slot.Resolve(lib);
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid() || !set(f) || !ue_wrap::Call(lib, f)) return false;
    return f.GetRaw(L"ReturnValue", &out, sizeof(out));
}

bool ActorTransform(void* actor, ue_wrap::FTransform& out) {
    void* fn = actor ? R::FindDispatchFunctionCached(R::ClassOf(actor), L"GetTransform") : nullptr;
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && ue_wrap::Call(actor, f) && f.GetRaw(L"ReturnValue", &out, sizeof(out));
}

bool SetActorTransform(void* actor, const ue_wrap::FTransform& t) {
    void* fn = actor ? R::FindDispatchFunctionCached(R::ClassOf(actor), L"K2_SetActorTransform") : nullptr;
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && f.SetRaw(L"NewTransform", &t, sizeof(t)) && ue_wrap::Call(actor, f);
}

// vent_b: vent_a moved 15 cm up its own z and turned, as the crawler's BeginPlay composes it.
bool VentB(const ue_wrap::FTransform& a, ue_wrap::FTransform& b) {
    ue_wrap::FTransform local{};
    const ue_wrap::FVector up{0.f, 0.f, 15.f}, one{1.f, 1.f, 1.f};
    const ue_wrap::FRotator turn{4.97946f, 0.f, -15.0295f};
    return MathCall(g_makeTransform, [&](ue_wrap::ParamFrame& f) {
               return f.SetRaw(L"Location", &up, sizeof(up)) && f.SetRaw(L"Rotation", &turn, sizeof(turn)) &&
                      f.SetRaw(L"Scale", &one, sizeof(one));
           }, local) &&
           MathCall(g_composeTransforms, [&](ue_wrap::ParamFrame& f) {
               return f.SetRaw(L"A", &local, sizeof(local)) && f.SetRaw(L"B", &a, sizeof(a));
           }, b);
}

// ventBreakTL's update at its track value `a`.
bool VentAt(const ue_wrap::FTransform& va, const ue_wrap::FTransform& vb, float a, ue_wrap::FTransform& out) {
    const float root = std::pow((std::max)(a, 0.f), 0.3333333f);
    const float s = std::sin(a * 360.f * 6.f * 3.14159265f / 180.f) + root;
    const float alpha = s + (1.f - s) * root;
    return MathCall(g_tlerp, [&](ue_wrap::ParamFrame& f) {
        return f.SetRaw(L"A", &va, sizeof(va)) && f.SetRaw(L"B", &vb, sizeof(vb)) && f.Set<float>(L"Alpha", alpha) &&
               f.Set<uint8_t>(L"InterpMode", 0);
    }, out);
}

// ---- CLIENT: the show ----------------------------------------------------------------------------------------------

// A game asset by name and class (`cls2` when the first finds none: a sound is a cue or a bare wave), held.
struct Asset {
    const wchar_t* name;
    const wchar_t* cls;
    const wchar_t* cls2;
    HeldRef ref;
    void* Get() {
        return Held(ref, [&] {
            void* a = R::FindObject(name, cls);
            return a || !cls2 ? a : R::FindObject(name, cls2);
        });
    }
};
Asset g_stepCue{L"footstepVent_Cue", L"SoundCue", L"SoundWave"};
Asset g_breakCue{L"break_metal_1", L"SoundCue", L"SoundWave"};
Asset g_attDefault{L"att_default", L"SoundAttenuation", nullptr};
Asset g_attNoAo{L"att_default_noao", L"SoundAttenuation", nullptr};
Asset g_breakFx{L"eff_metalbreak", L"ParticleSystem", nullptr};

void PlayAt(Asset& sound, Asset& att, const ue_wrap::FVector& at, float volume, float pitch) {
    void* local = coop::players::Registry::Get().Local();
    void* cue = sound.Get();
    if (!local || !cue) {
        UE_LOGW("vent_crawler: %ls did not resolve -- not played", sound.name);
        return;
    }
    E::PlaySoundAtLocation(local, cue, at, att.Get(), volume, pitch);
    g_sounds.fetch_add(1, std::memory_order_relaxed);
}

void Burst(const ue_wrap::FVector& at) {
    void* local = coop::players::Registry::Get().Local();
    void* tmpl = g_breakFx.Get();
    void* statics = nullptr;
    void* fn = g_spawnEmitter.Resolve(statics);
    if (!local || !tmpl || !fn) {
        UE_LOGW("vent_crawler: eff_metalbreak did not resolve -- no burst");
        return;
    }
    const ue_wrap::FRotator rot{};
    const ue_wrap::FVector scale{1.f, 1.f, 1.f};
    ue_wrap::ParamFrame f(fn);
    f.Set<void*>(L"WorldContextObject", local);
    f.Set<void*>(L"EmitterTemplate", tmpl);
    f.SetRaw(L"Location", &at, sizeof(at));
    f.SetRaw(L"Rotation", &rot, sizeof(rot));
    f.SetRaw(L"Scale", &scale, sizeof(scale));
    f.Set<bool>(L"bAutoDestroy", true);
    f.Set<uint8_t>(L"PoolingMethod", 0);
    f.Set<bool>(L"bAutoActivateSystem", true);
    if (ue_wrap::Call(statics, f)) ++g_bursts;
}

void DoorPower(bool on) {
    void* door = ue_wrap::keyed_objects::Resolve(L"basedoor_signalroom");
    if (door) ue_wrap::door::WriteActive(door, on);
}

struct Show {
    bool on = false;
    uint32_t world = 0;   // the world generation the show began in: its doors and vent are that world's
    Clock::time_point t0{};
    std::pair<int32_t, int32_t> crawler{-1, 0};   // the mirror; index -1 until one is found
    ue_wrap::FVector near{};                      // where the mirror is looked for
    Clock::time_point lastLook{};
    float catchUp = 0.f;   // the timeline's seconds already gone when the show began: its events are state, not sound
    bool step1 = false, step2 = false, timelineRead = false, powerOff = false;
    float last = -1.f;   // the timeline's time already played
    std::vector<Event> events;
    std::vector<Track> tracks;
    std::pair<int32_t, int32_t> vent{-1, 0};   // the hanging vent, found once when the show began; index -1 if not there
    bool haveVent = false, swinging = false;
    ue_wrap::FTransform ventA{}, ventB{};   // where the vent hung, and where the swing ends
    float swingAt = 0.f, swingLen = 0.f;    // the swing's start, show time, and its length
    std::vector<Track> swingTracks;
} g_show;

void EndShow(const char* why) {
    // The crawl's own end has the power on; in another world the door looked up is not the one this show took it from.
    if (g_show.powerOff && g_show.world == ue_wrap::world_identity::Generation()) DoorPower(true);
    UE_LOGI("vent_crawler: the crawl's show ended (%s) -- %u sounds, %u bursts", why, g_sounds.load(), g_bursts);
    g_show = Show{};
}

// The live crawler mirror nearest `at`; the class's default object is not one.
void* MirrorNear(const ue_wrap::FVector& at) {
    struct Ctx {
        ue_wrap::FVector at;
        void* best;
        float d2;
    } ctx{at, nullptr, 1e18f};
    void* cls = ue_wrap::object_index::ClassByName(L"ventCrawler_C");
    if (!cls) return nullptr;
    ue_wrap::object_index::ForEachInstance(cls, [](void* p, void* obj, int32_t) {
        auto* c = static_cast<Ctx*>(p);
        ue_wrap::FVector o{};
        if (!R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__") || !E::TryGetActorLocation(obj, o)) return;
        const float d2 = (o.X - c->at.X) * (o.X - c->at.X) + (o.Y - c->at.Y) * (o.Y - c->at.Y) +
                         (o.Z - c->at.Z) * (o.Z - c->at.Z);
        if (d2 < c->d2) {
            c->d2 = d2;
            c->best = obj;
        }
    }, &ctx);
    return ctx.best;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!g_watched) g_watched = SG::WatchName(kRowBoxOverlap, kTag, nullptr, &OnOverlapPost);
    if (g_watched && !g_live) {
        SG::ResolvePendingNames();
        if (SG::NameWatchLive(kRowBoxOverlap, kTag)) {
            g_live = true;
            UE_LOGI("vent_crawler: a crawl's start is seen at the crawler's box overlap (a script-gate watch)");
        }
    }
}

bool OnCue(const coop::net::EventCuePayload& p) {
    if (p.cueId != kCueCrawl) return false;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Client) return true;
    if (g_show.on) EndShow("a new crawl");
    void* crawler = MirrorNear({p.x, p.y, p.z});
    g_show.on = true;
    g_show.world = ue_wrap::world_identity::Generation();
    g_show.t0 = Clock::now();
    g_show.near = {p.x, p.y, p.z};
    g_show.crawler = crawler ? IdOf(crawler) : std::pair<int32_t, int32_t>{-1, 0};
    DoorPower(false);
    g_show.powerOff = true;
    void* vent = ue_wrap::keyed_objects::Resolve(L"ventHanging");
    g_show.vent = vent ? IdOf(vent) : std::pair<int32_t, int32_t>{-1, 0};
    g_show.haveVent = vent && ActorTransform(vent, g_show.ventA) && VentB(g_show.ventA, g_show.ventB);
    UE_LOGI("vent_crawler: the host's crawl started at (%.0f, %.0f, %.0f) -- mirror %p", p.x, p.y, p.z, crawler);
    return true;
}

namespace {

// The receive boundary of the host's vent pose: finite, inside the world, and a turn in degrees.
bool VentPoseSane(const coop::net::VentCrawlStatePayload& p) {
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(p.ventLoc[i]) || std::fabs(p.ventLoc[i]) > coop::net::kMaxCoord) return false;
        if (!std::isfinite(p.ventRot[i]) || std::fabs(p.ventRot[i]) > 360.f) return false;
    }
    return true;
}

// The pose the host sent, as a transform with the scale `vent` has here.
bool HostVentPose(void* vent, const coop::net::VentCrawlStatePayload& p, ue_wrap::FTransform& to) {
    ue_wrap::FTransform cur{};
    if (!ActorTransform(vent, cur)) return false;
    const ue_wrap::FVector loc{p.ventLoc[0], p.ventLoc[1], p.ventLoc[2]};
    const ue_wrap::FRotator rot{p.ventRot[0], p.ventRot[1], p.ventRot[2]};
    const ue_wrap::FVector scale{cur.SX, cur.SY, cur.SZ};
    return MathCall(g_makeTransform, [&](ue_wrap::ParamFrame& f) {
        return f.SetRaw(L"Location", &loc, sizeof(loc)) && f.SetRaw(L"Rotation", &rot, sizeof(rot)) &&
               f.SetRaw(L"Scale", &scale, sizeof(scale));
    }, to);
}

// Puts the vent at the pose the host sent, keeping the scale it has here.
bool PlaceVentAt(void* vent, const coop::net::VentCrawlStatePayload& p) {
    ue_wrap::FTransform to{};
    return HostVentPose(vent, p, to) && SetActorTransform(vent, to);
}

}  // namespace

void QueueConnectBroadcastForSlot(int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host || !g_host.began || g_host.world != ue_wrap::world_identity::Generation())
        return;
    coop::net::VentCrawlStatePayload p{};
    // The crawler is destroyed at the crawl's own end; its weak id no longer resolves then.
    void* live = Resolve(g_host.crawler);
    p.phase = live && R::IsLive(live) ? 1 : 2;
    // Divergence from MTA, which sends a joiner state only (CGame.cpp:1393 InitialDataStream): the crawl is a Blueprint
    // timeline on a mirror whose BeginPlay is refused, so no property carries it, and the joiner resumes its show at the
    // seconds since the cue (a relative time, no clock sync).
    const float elapsed = std::chrono::duration<float>(Clock::now() - g_host.t0).count();
    p.elapsed = (std::min)((std::max)(elapsed, 0.f), kShowLimit);
    // The door's power is a field the crawl writes from its graph, so it is read, not remembered.
    bool on = false;
    void* door = ue_wrap::keyed_objects::Resolve(L"basedoor_signalroom");
    if (!door || !ue_wrap::door::TryReadActive(door, on)) {
        UE_LOGW("vent_crawler: the signal room door's power did not read -- no crawl state for slot %d", slot);
        return;
    }
    p.doorActive = on ? 1 : 0;
    // A crawl under way sends the pose the vent hung at when it began (the swing's A, which the joiner's B follows
    // from); a crawl that is over left the vent at its end pose, which only the host's own vent holds exactly.
    if (p.phase == 1) {
        if (g_host.haveVentStart) {
            p.ventLoc[0] = g_host.ventLoc.X;
            p.ventLoc[1] = g_host.ventLoc.Y;
            p.ventLoc[2] = g_host.ventLoc.Z;
            p.ventRot[0] = g_host.ventRot.Pitch;
            p.ventRot[1] = g_host.ventRot.Yaw;
            p.ventRot[2] = g_host.ventRot.Roll;
            p.hasVent = 1;
        }
    } else {
        void* vent = ue_wrap::keyed_objects::Resolve(L"ventHanging");
        ue_wrap::FVector at{};
        ue_wrap::FRotator rot{};
        if (vent && E::TryGetActorLocation(vent, at) && E::TryGetActorRotation(vent, rot)) {
            p.ventLoc[0] = at.X;
            p.ventLoc[1] = at.Y;
            p.ventLoc[2] = at.Z;
            p.ventRot[0] = rot.Pitch;
            p.ventRot[1] = rot.Yaw;
            p.ventRot[2] = rot.Roll;
            p.hasVent = 1;
        }
    }
    if (s->SendReliableToSlot(slot, coop::net::ReliableKind::VentCrawlState, &p, sizeof(p)))
        UE_LOGI("vent_crawler: the crawl's state to slot %d -- phase %u, %.1f s in, door power %u, vent pose %s", slot,
                static_cast<unsigned>(p.phase), p.elapsed, static_cast<unsigned>(p.doorActive),
                p.hasVent ? "sent" : "not read");
}

void OnState(const coop::net::VentCrawlStatePayload& p) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Client) return;
    if (!std::isfinite(p.elapsed) || p.elapsed < 0.f || p.elapsed > kShowLimit || p.phase < 1 || p.phase > 2 ||
        p.doorActive > 1 || p.hasVent > 1 || (p.hasVent && !VentPoseSane(p))) {
        UE_LOGW("vent_crawler: a malformed crawl state (elapsed %f, phase %u, door %u, vent %u) -- dropping", p.elapsed,
                static_cast<unsigned>(p.phase), static_cast<unsigned>(p.doorActive), static_cast<unsigned>(p.hasVent));
        return;
    }
    g_states.fetch_add(1, std::memory_order_relaxed);
    if (g_show.on) EndShow("a crawl state");
    void* vent = ue_wrap::keyed_objects::Resolve(L"ventHanging");
    if (p.phase == 2) {
        DoorPower(p.doorActive != 0);
        // A host that could not read its vent sends none: the vent stays where this peer has it. The blueprint's class
        // defaults are no end pose (measured: the drill's rejoin check failed 2 of 2 runs with them).
        const char* ventNow = "left (the host sent no pose)";
        if (vent && p.hasVent) {
            ventNow = "left";
            if (PlaceVentAt(vent, p)) {
                ue_wrap::prop::CallSetPropProps(vent, false, true, false);
                ventNow = "at the host's pose, frozen";
            }
        }
        UE_LOGI("vent_crawler: the host's crawl is over -- vent %s, door power %u", ventNow,
                static_cast<unsigned>(p.doorActive));
        return;
    }
    g_show.on = true;
    g_show.world = ue_wrap::world_identity::Generation();
    g_show.t0 = Clock::now() -std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(p.elapsed));
    g_show.near = {};   // the crawler is the only one there is
    g_show.catchUp = (std::max)(p.elapsed - kTimeline, 0.f);
    g_show.step1 = p.elapsed >= kStep1;
    g_show.step2 = p.elapsed >= kStep2;
    DoorPower(p.doorActive != 0);
    g_show.powerOff = p.doorActive == 0;
    g_show.vent = vent ? IdOf(vent) : std::pair<int32_t, int32_t>{-1, 0};
    // The swing's A is where the host's vent hung when the crawl began, B follows from it (as OnCue's own pair does).
    g_show.haveVent = vent && p.hasVent && HostVentPose(vent, p, g_show.ventA) && VentB(g_show.ventA, g_show.ventB);
    if (!p.hasVent) UE_LOGW("vent_crawler: the host could not read its vent at the crawl's start -- no swing here");
    void* crawler = MirrorNear(g_show.near);
    g_show.crawler = crawler ? IdOf(crawler) : std::pair<int32_t, int32_t>{-1, 0};
    UE_LOGI("vent_crawler: joined %.1f s into the host's crawl, door power %u -- mirror %p", p.elapsed,
            static_cast<unsigned>(p.doorActive), crawler);
}

void Tick() {
    if (!g_show.on) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Client) return;
    if (g_show.world != ue_wrap::world_identity::Generation()) {   // the show's doors and vent are gone with its world
        EndShow("world changed");
        return;
    }
    const Clock::time_point tickAt = Clock::now();
    const float t = std::chrono::duration<float>(tickAt - g_show.t0).count();
    // A mirror or a vent not there when the show began (a joiner's, still spawning) is asked for once a second.
    if ((g_show.crawler.first < 0 || g_show.vent.first < 0) && tickAt - g_show.lastLook >= std::chrono::seconds(1)) {
        g_show.lastLook = tickAt;
        if (g_show.crawler.first < 0)
            if (void* found = MirrorNear(g_show.near)) g_show.crawler = IdOf(found);
        if (g_show.vent.first < 0)
            if (void* v = ue_wrap::keyed_objects::Resolve(L"ventHanging")) g_show.vent = IdOf(v);
    }
    void* crawler = Resolve(g_show.crawler);
    void* vent = Resolve(g_show.vent);   // a weak-id check, no name lookup per frame
    if (g_show.swinging && vent) {
        const float st = t - g_show.swingAt;
        ue_wrap::FTransform now{};
        if (st >= g_show.swingLen) {
            g_show.swinging = false;
            SetActorTransform(vent, g_show.ventB);
            ue_wrap::prop::CallSetPropProps(vent, false, true, false);
            UE_LOGI("vent_crawler: the vent's swing ended -- frozen where the host's is");
        } else if (VentAt(g_show.ventA, g_show.ventB, TrackAt(g_show.swingTracks, L"a", st, st / g_show.swingLen), now)) {
            SetActorTransform(vent, now);
        }
    }
    if (g_show.swinging) return;   // the swing outlives the mirror
    // The mirror's end is a weak-id check each frame, not the object listener's destroy event: the schedule it ends is
    // a clock poll anyway (the mirror's BeginPlay is refused, so nothing announces the times), and one resolve is the cost.
    if ((g_show.crawler.first >= 0 && !crawler && t > kTimeline) || t > kShowLimit) {
        EndShow(crawler ? "out of time" : "the mirror went");
        return;
    }
    ue_wrap::FVector ventAt{}, at{};
    const bool haveVent = vent && E::TryGetActorLocation(vent, ventAt);
    if (!g_show.step1 && t >= kStep1) {
        g_show.step1 = true;
        if (haveVent) PlayAt(g_stepCue, g_attDefault, ventAt, 2.f, 0.85f);
    }
    if (!g_show.step2 && t >= kStep2) {
        g_show.step2 = true;
        if (haveVent) PlayAt(g_stepCue, g_attDefault, ventAt, 2.f, 0.95f);
    }
    if (t < kTimeline || !crawler) return;
    if (!g_show.timelineRead) {
        g_show.timelineRead = true;
        if (ReadTimeline(crawler, L"moveTL", g_show.events, g_show.tracks)) {
            std::wstring names;
            for (const Track& tr : g_show.tracks) names += (names.empty() ? L"" : L", ") + tr.name;
            UE_LOGI("vent_crawler: the mirror's timeline has %zu events, tracks %ls", g_show.events.size(), names.c_str());
        }
    }
    const float tl = t - kTimeline;
    for (const Event& e : g_show.events) {
        if (e.time <= g_show.last || e.time > tl) continue;
        const bool gone = e.time <= g_show.catchUp;   // before the show began: its state counts, its sound does not
        if (e.step) {
            if (gone || !E::TryGetActorLocation(crawler, at)) continue;
            const float volume = TrackAt(g_show.tracks, L"stepvolume", e.time, 1.f) *
                                 TrackAt(g_show.tracks, L"multiplyvolume", e.time, 1.f);
            PlayAt(g_stepCue, g_attNoAo, at, volume, TrackAt(g_show.tracks, L"pitch", e.time, 1.f));
        } else if (haveVent) {
            if (!gone) {
                PlayAt(g_breakCue, g_attDefault, {ventAt.X, ventAt.Y, ventAt.Z - 50.f}, 0.5f, 1.25f);
                Burst({ventAt.X, ventAt.Y, ventAt.Z + 25.f});
            }
            DoorPower(true);
            g_show.powerOff = false;
            std::vector<Event> none;
            if (g_show.haveVent &&
                ReadTimeline(crawler, L"ventBreakTL", none, g_show.swingTracks, &g_show.swingLen) && g_show.swingLen > 0.f) {
                g_show.swinging = true;
                g_show.swingAt = kTimeline + e.time;
            }
            UE_LOGI("vent_crawler: the vent broke at %.2f s into the crawl", e.time);
        }
    }
    g_show.last = tl;
}

unsigned SoundsPlayed() { return g_sounds.load(std::memory_order_relaxed); }

unsigned StatesHeard() { return g_states.load(std::memory_order_relaxed); }

void OnDisconnect() {
    if (g_show.on) EndShow("disconnect");
    if (g_sent || g_sounds.load() || g_bursts)
        UE_LOGI("vent_crawler: session end -- crawls sent=%u sounds=%u bursts=%u", g_sent, g_sounds.load(), g_bursts);
    // g_host and g_started are NOT cleared: they are the host's world, not the session's. A host whose last client
    // left tears its session state down here and lives on, and the next joiner of that world is owed the crawl
    // (measured: life 2 got no state, the record wiped when life 1 left). A crawler whose box fires again after that
    // is no new crawl: were g_started cleared, the repeat overlap would send a second cue and reset t0. The world
    // generation (OnOverlapPost) is what retires both.
    g_sent = g_bursts = 0;
    g_sounds.store(0);
    g_states.store(0);
}

}  // namespace coop::vent_crawler_sync
