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
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/keyed_objects.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <functional>
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

// The crawl's own delays: the two vent steps, then the timeline.
constexpr float kStep1 = 2.0f, kStep2 = 2.5f, kTimeline = 5.0f;
constexpr float kShowLimit = 60.f;   // a crawl whose mirror never goes ends here

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watched = false, g_live = false;
std::vector<std::pair<int32_t, int32_t>> g_started;   // the host's crawlers whose crawl was sent, by index and serial
uint32_t g_sent = 0;
std::atomic<unsigned> g_sounds{0};
uint32_t g_bursts = 0;

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::pair<int32_t, int32_t> IdOf(void* obj) {
    const int32_t idx = R::InternalIndexOf(obj);
    return {idx, idx >= 0 ? R::SlotSerial(idx) : 0};
}

void* Resolve(const std::pair<int32_t, int32_t>& id) {
    return id.first >= 0 ? R::ResolveWeakObject(id.first, id.second) : nullptr;
}

// ---- HOST ----------------------------------------------------------------------------------------------------------

// The crawler's first overlap is the one its graph acts on (a DoOnce): the crawl starts there.
void OnOverlapPost(const SG::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host || !call.object || !R::IsLive(call.object)) return;
    const auto id = IdOf(call.object);
    if (std::find(g_started.begin(), g_started.end(), id) != g_started.end()) return;
    g_started.push_back(id);
    ue_wrap::FVector at{};
    E::TryGetActorLocation(call.object, at);
    coop::net::EventCuePayload p{};
    p.cueId = kCueCrawl;
    p.x = at.X;
    p.y = at.Y;
    p.z = at.Z;
    if (s->SendReliable(coop::net::ReliableKind::EventCue, &p, sizeof(p))) ++g_sent;
    UE_LOGI("vent_crawler: the crawl started at (%.0f, %.0f, %.0f) -- EventCue sent (#%u)", at.X, at.Y, at.Z, g_sent);
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

bool MathCall(const wchar_t* name, const std::function<bool(ue_wrap::ParamFrame&)>& set, ue_wrap::FTransform& out) {
    void* lib = R::FindClassDefaultObject(L"KismetMathLibrary");
    void* fn = lib ? R::FindFunction(R::ClassOf(lib), name) : nullptr;
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
    return MathCall(L"MakeTransform", [&](ue_wrap::ParamFrame& f) {
               return f.SetRaw(L"Location", &up, sizeof(up)) && f.SetRaw(L"Rotation", &turn, sizeof(turn)) &&
                      f.SetRaw(L"Scale", &one, sizeof(one));
           }, local) &&
           MathCall(L"ComposeTransforms", [&](ue_wrap::ParamFrame& f) {
               return f.SetRaw(L"A", &local, sizeof(local)) && f.SetRaw(L"B", &a, sizeof(a));
           }, b);
}

// ventBreakTL's update at its track value `a`.
bool VentAt(const ue_wrap::FTransform& va, const ue_wrap::FTransform& vb, float a, ue_wrap::FTransform& out) {
    const float root = std::pow((std::max)(a, 0.f), 0.3333333f);
    const float s = std::sin(a * 360.f * 6.f * 3.14159265f / 180.f) + root;
    const float alpha = s + (1.f - s) * root;
    return MathCall(L"TLerp", [&](ue_wrap::ParamFrame& f) {
        return f.SetRaw(L"A", &va, sizeof(va)) && f.SetRaw(L"B", &vb, sizeof(vb)) && f.Set<float>(L"Alpha", alpha) &&
               f.Set<uint8_t>(L"InterpMode", 0);
    }, out);
}

// ---- CLIENT: the show ----------------------------------------------------------------------------------------------

void* Asset(const wchar_t* name, const wchar_t* cls, const wchar_t* cls2 = nullptr) {
    void* a = R::FindObject(name, cls);
    return a || !cls2 ? a : R::FindObject(name, cls2);
}

void PlayAt(const wchar_t* sound, const wchar_t* att, const ue_wrap::FVector& at, float volume, float pitch) {
    void* local = coop::players::Registry::Get().Local();
    void* cue = Asset(sound, L"SoundCue", L"SoundWave");
    if (!local || !cue) {
        UE_LOGW("vent_crawler: %ls did not resolve -- not played", sound);
        return;
    }
    E::PlaySoundAtLocation(local, cue, at, Asset(att, L"SoundAttenuation"), volume, pitch);
    g_sounds.fetch_add(1, std::memory_order_relaxed);
}

void Burst(const ue_wrap::FVector& at) {
    void* local = coop::players::Registry::Get().Local();
    void* tmpl = Asset(L"eff_metalbreak", L"ParticleSystem");
    void* statics = R::FindClassDefaultObject(L"GameplayStatics");
    void* fn = statics ? R::FindFunction(R::ClassOf(statics), L"SpawnEmitterAtLocation") : nullptr;
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
    Clock::time_point t0{};
    std::pair<int32_t, int32_t> crawler{-1, 0};
    bool step1 = false, step2 = false, timelineRead = false, powerOff = false;
    float last = -1.f;   // the timeline's time already played
    std::vector<Event> events;
    std::vector<Track> tracks;
    bool haveVent = false, swinging = false;
    ue_wrap::FTransform ventA{}, ventB{};   // where the vent hung, and where the swing ends
    float swingAt = 0.f, swingLen = 0.f;    // the swing's start, show time, and its length
    std::vector<Track> swingTracks;
} g_show;

void EndShow(const char* why) {
    if (g_show.powerOff) DoorPower(true);   // the crawl's own end has it on
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
    g_show.t0 = Clock::now();
    g_show.crawler = crawler ? IdOf(crawler) : std::pair<int32_t, int32_t>{-1, 0};
    DoorPower(false);
    g_show.powerOff = true;
    void* vent = ue_wrap::keyed_objects::Resolve(L"ventHanging");
    g_show.haveVent = vent && ActorTransform(vent, g_show.ventA) && VentB(g_show.ventA, g_show.ventB);
    UE_LOGI("vent_crawler: the host's crawl started at (%.0f, %.0f, %.0f) -- mirror %p", p.x, p.y, p.z, crawler);
    return true;
}

void Tick() {
    if (!g_show.on) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Client) return;
    const float t = std::chrono::duration<float>(Clock::now() - g_show.t0).count();
    void* crawler = Resolve(g_show.crawler);
    void* vent = ue_wrap::keyed_objects::Resolve(L"ventHanging");
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
    if ((!crawler && t > kTimeline) || t > kShowLimit) {
        EndShow(crawler ? "out of time" : "the mirror went");
        return;
    }
    ue_wrap::FVector ventAt{}, at{};
    const bool haveVent = vent && E::TryGetActorLocation(vent, ventAt);
    if (!g_show.step1 && t >= kStep1) {
        g_show.step1 = true;
        if (haveVent) PlayAt(L"footstepVent_Cue", L"att_default", ventAt, 2.f, 0.85f);
    }
    if (!g_show.step2 && t >= kStep2) {
        g_show.step2 = true;
        if (haveVent) PlayAt(L"footstepVent_Cue", L"att_default", ventAt, 2.f, 0.95f);
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
        if (e.step) {
            if (!E::TryGetActorLocation(crawler, at)) continue;
            const float volume = TrackAt(g_show.tracks, L"stepvolume", e.time, 1.f) *
                                 TrackAt(g_show.tracks, L"multiplyvolume", e.time, 1.f);
            PlayAt(L"footstepVent_Cue", L"att_default_noao", at, volume, TrackAt(g_show.tracks, L"pitch", e.time, 1.f));
        } else if (haveVent) {
            PlayAt(L"break_metal_1", L"att_default", {ventAt.X, ventAt.Y, ventAt.Z - 50.f}, 0.5f, 1.25f);
            Burst({ventAt.X, ventAt.Y, ventAt.Z + 25.f});
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

void OnDisconnect() {
    if (g_show.on) EndShow("disconnect");
    if (g_sent || g_sounds.load() || g_bursts)
        UE_LOGI("vent_crawler: session end -- crawls sent=%u sounds=%u bursts=%u", g_sent, g_sounds.load(), g_bursts);
    g_started.clear();
    g_sent = g_bursts = 0;
    g_sounds.store(0);
}

}  // namespace coop::vent_crawler_sync
