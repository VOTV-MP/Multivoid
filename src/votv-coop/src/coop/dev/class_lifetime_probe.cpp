// coop/dev/class_lifetime_probe.cpp -- [dev] class and function identity across worlds. See the header.
#include "coop/dev/class_lifetime_probe.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/world/world_singleton.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::dev::class_lifetime_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;
namespace P  = ue_wrap::profile;
namespace WI = ue_wrap::world_identity;
namespace WS = ue_wrap::world_singleton;
using Clock = std::chrono::steady_clock;

struct Ident {
    void*   ptr = nullptr;
    int32_t idx = -1;
    int32_t serial = 0;
};

struct Seen {
    Ident    id;
    uint32_t gen = 0;        // the world it was last seen in
    bool     native = false;
    bool     present = false;  // in the latest census
};

// A class a lane holds by pointer, with a function a lane hooks by pointer (null: the class only).
struct Watched {
    const wchar_t* cls;
    const wchar_t* fn;
};
const Watched kWatched[] = {
    {P::name::MainPlayerClass, L"impactDamage"},  // player_damage's impact interceptors
    {L"kerfurOmega_C", L"actionName"},           // kerfur_convert's menu interceptor
    {L"prop_kerfurOmega_C", L"spawnKerfuro"},
    {P::name::DaynightCycleClass, P::name::DaynightCycle_timerRainFn},  // weather's scheduler interceptors
    {P::name::DirectionalWindClass, L"changeWindOrigin"},
    {L"ticker_fireflySpawner_C", L"ReceiveTick"},  // firefly's observer pair
    {P::name::PropClass, P::name::PropInitFn},     // prop_lifecycle's Init observers
    {L"trashBitsPile_C", P::name::PropInitFn},
    {L"prop_garbageContainer_C", nullptr},
    {L"comp_wallAttachable_C", nullptr},
    {P::name::GamemodeClass, nullptr},
    {L"ui_menu_C", nullptr},
    {P::name::GameplayStaticsClass, L"PlaySound2D"},  // native: the control
};

const wchar_t* const kBlueprintMetas[] = {
    L"BlueprintGeneratedClass", L"WidgetBlueprintGeneratedClass", L"AnimBlueprintGeneratedClass",
};

using NameMap = std::unordered_map<std::wstring, Ident>;

std::unordered_map<std::wstring, Seen> g_classes;    // by class name
std::unordered_map<std::wstring, Seen> g_functions;  // by "class::function"
uint32_t g_judgedGen = 0;
uint32_t g_seenGen = 0;
Clock::time_point g_seenAt{};
Clock::time_point g_lastCensus{};

Ident Capture(void* obj) {
    Ident id;
    id.ptr = obj;
    id.idx = R::InternalIndexOf(obj);
    id.serial = R::AllocateSlotSerial(id.idx);
    return id;
}

bool StillLive(const Ident& id) {
    return R::IsLiveByIndex(id.ptr, id.idx) && R::SlotSerial(id.idx) == id.serial;
}

enum class Verdict : uint8_t { First, Same, SameAddressNewObject, NewAddress };

Verdict Judge(const Ident& now, const Seen* before) {
    if (!before) return Verdict::First;
    if (now.ptr == before->id.ptr && now.idx == before->id.idx && now.serial == before->id.serial)
        return Verdict::Same;
    if (now.ptr == before->id.ptr) return Verdict::SameAddressNewObject;
    return Verdict::NewAddress;
}

const char* Text(Verdict v) {
    switch (v) {
        case Verdict::First:                return "first sight";
        case Verdict::Same:                 return "the same object";
        case Verdict::SameAddressNewObject: return "SAME ADDRESS, NEW OBJECT";
        case Verdict::NewAddress:           return "a new object at a new address";
    }
    return "?";
}

struct Judged {
    Verdict v = Verdict::First;
    Seen    before;  // valid unless v is First
};
using Verdicts = std::unordered_map<std::wstring, Judged>;

struct Tally {
    int total = 0, first = 0, same = 0, sameAddress = 0, newAddress = 0, gone = 0, goneLive = 0,
        changedWithin = 0;
    std::vector<std::wstring> sameAddressNames, newAddressNames, goneNames;
};

// Every name of a list, twelve to a line, so the whole churn can be matched against the tree.
void LogNames(const char* what, const char* verdict, const std::vector<std::wstring>& names) {
    std::wstring line;
    for (size_t i = 0; i < names.size(); ++i) {
        if (!line.empty()) line += L", ";
        line += names[i];
        if ((i + 1) % 12 == 0 || i + 1 == names.size()) {
            UE_LOGI("class_life:   %s %s [%zu-%zu of %zu]: %ls", what, verdict, i / 12 * 12 + 1, i + 1,
                    names.size(), line.c_str());
            line.clear();
        }
    }
}

struct Sink {
    NameMap* out;
    int*     duplicates;
};

void Collect(void* ctx, void* obj, int32_t index) {
    // A class still loading has no functions linked to read yet, and a dying one is leaving: the census
    // holds neither, so the first reads as new at the next census and the second as gone.
    if (R::SlotFlags(index) & (R::slot_flags::Dying | R::slot_flags::NotYetReadable)) return;
    auto* sink = static_cast<Sink*>(ctx);
    if (!sink->out->emplace(R::ToString(R::NameOf(obj)), Capture(obj)).second) ++*sink->duplicates;
}

// Every loaded class by name, the Blueprint ones and the native ones apart. False while the index does
// not hold the meta-classes yet.
bool Census(NameMap& blueprint, NameMap& native, int& duplicates) {
    void* classMeta = OI::ClassByName(L"Class");
    if (!classMeta || !OI::ClassByName(kBlueprintMetas[0])) return false;
    Sink bpSink{&blueprint, &duplicates};
    for (const wchar_t* meta : kBlueprintMetas)
        if (void* m = OI::ClassByName(meta)) OI::ForEachInstance(m, &Collect, &bpSink);
    Sink nativeSink{&native, &duplicates};
    OI::ForEachInstance(classMeta, &Collect, &nativeSink);
    return true;
}

// The classes the last census held that this one does not: gone, with whether the old object lives on.
void CountGone(const NameMap& blueprint, const NameMap& native, Tally& tb, Tally& tn) {
    for (auto& [name, s] : g_classes) {
        if (!s.present) continue;
        if ((s.native ? native : blueprint).count(name)) continue;
        s.present = false;
        Tally& t = s.native ? tn : tb;
        t.goneNames.push_back(name);
        ++t.gone;
        if (StillLive(s.id)) ++t.goneLive;
    }
}

// Judges `cur` against the last sightings and records it as seen in `gen`. A name already seen in this
// world is only checked for a change within it.
void Absorb(const NameMap& cur, bool native, uint32_t gen, Tally& t, Verdicts& verdicts) {
    for (const auto& [name, id] : cur) {
        auto it = g_classes.find(name);
        if (it != g_classes.end() && it->second.gen == gen) {
            if (Judge(id, &it->second) != Verdict::Same) {
                ++t.changedWithin;
                UE_LOGW("class_life: %ls changed identity within world gen=%u: %p/%d/%d -> %p/%d/%d",
                        name.c_str(), gen, it->second.id.ptr, it->second.id.idx, it->second.id.serial,
                        id.ptr, id.idx, id.serial);
                it->second.id = id;
            }
            it->second.present = true;
            continue;
        }
        Judged j;
        if (it != g_classes.end()) j.before = it->second;
        j.v = Judge(id, it == g_classes.end() ? nullptr : &it->second);
        ++t.total;
        switch (j.v) {
            case Verdict::First: ++t.first; break;
            case Verdict::Same:  ++t.same;  break;
            case Verdict::SameAddressNewObject: ++t.sameAddress; t.sameAddressNames.push_back(name); break;
            case Verdict::NewAddress:           ++t.newAddress;  t.newAddressNames.push_back(name);  break;
        }
        verdicts[name] = j;
        g_classes[name] = Seen{id, gen, native, true};
    }
}

void LogTally(const char* what, uint32_t gen, const Tally& t) {
    UE_LOGI("class_life: gen=%u %s: %d judged -- first sight %d, the same object %d, same address new "
            "object %d, new address %d; gone %d (their old object still live: %d); changed within the "
            "world %d", gen, what, t.total, t.first, t.same, t.sameAddress, t.newAddress, t.gone,
            t.goneLive, t.changedWithin);
    LogNames(what, "at a new address", t.newAddressNames);
    LogNames(what, "SAME ADDRESS, NEW OBJECT", t.sameAddressNames);
    LogNames(what, "gone", t.goneNames);
}

void LogWatched(const Watched& w, uint32_t gen, const Verdicts& verdicts, const NameMap& blueprint,
                const NameMap& native) {
    const Ident* now = nullptr;
    if (auto b = blueprint.find(w.cls); b != blueprint.end()) now = &b->second;
    else if (auto n = native.find(w.cls); n != native.end()) now = &n->second;
    if (!now) {
        auto seen = g_classes.find(w.cls);
        if (seen == g_classes.end()) {
            UE_LOGI("class_life: watched %ls: not loaded in gen %u, never seen", w.cls, gen);
        } else {
            UE_LOGI("class_life: watched %ls: not loaded in gen %u; last seen in gen %u as %p/%d/%d, "
                    "that object %s", w.cls, gen, seen->second.gen, seen->second.id.ptr,
                    seen->second.id.idx, seen->second.id.serial,
                    StillLive(seen->second.id) ? "STILL LIVE" : "dead");
        }
    } else {
        const char* byName = OI::ClassByName(w.cls) == now->ptr ? "agrees" : "DISAGREES";
        auto j = verdicts.find(w.cls);
        if (j != verdicts.end() && j->second.v != Verdict::First) {
            const Seen& b = j->second.before;
            UE_LOGI("class_life: watched %ls: %p/%d/%d, was %p/%d/%d in gen %u -> %s; the old object "
                    "%s; ClassByName %s", w.cls, now->ptr, now->idx, now->serial, b.id.ptr, b.id.idx,
                    b.id.serial, b.gen, Text(j->second.v), StillLive(b.id) ? "still live" : "dead",
                    byName);
        } else {
            UE_LOGI("class_life: watched %ls: %p/%d/%d, %s; ClassByName %s", w.cls, now->ptr, now->idx,
                    now->serial, j != verdicts.end() ? "first sight" : "unchanged in this world",
                    byName);
        }
    }
    if (!w.fn) return;
    const std::wstring key = std::wstring(w.cls) + L"::" + w.fn;
    void* fn = now ? R::FindFunction(now->ptr, w.fn) : nullptr;
    auto fs = g_functions.find(key);
    if (!fn) {
        if (fs == g_functions.end()) {
            UE_LOGI("class_life: watched fn %ls: not found in gen %u, never seen", key.c_str(), gen);
        } else {
            UE_LOGI("class_life: watched fn %ls: not found in gen %u; last seen in gen %u as %p/%d/%d, "
                    "that object %s", key.c_str(), gen, fs->second.gen, fs->second.id.ptr,
                    fs->second.id.idx, fs->second.id.serial,
                    StillLive(fs->second.id) ? "STILL LIVE" : "dead");
        }
        return;
    }
    const Ident id = Capture(fn);
    if (fs == g_functions.end()) {
        UE_LOGI("class_life: watched fn %ls: %p/%d/%d, first sight", key.c_str(), id.ptr, id.idx, id.serial);
    } else {
        UE_LOGI("class_life: watched fn %ls: %p/%d/%d, was %p/%d/%d in gen %u -> %s; the old object %s",
                key.c_str(), id.ptr, id.idx, id.serial, fs->second.id.ptr, fs->second.id.idx,
                fs->second.id.serial, fs->second.gen, Text(Judge(id, &fs->second)),
                StillLive(fs->second.id) ? "still live" : "dead");
    }
    g_functions[key] = Seen{id, gen, false, true};
}

// One census: judged against the last sightings, logged, and every watched row the census touched.
// `worldStart` logs every watched row and the census even when nothing changed.
bool CensusAndLog(uint32_t gen, bool worldStart, WI::WorldKind kind, long long waitedMs) {
    const auto t0 = Clock::now();
    NameMap blueprint, native;
    int duplicates = 0;
    if (!Census(blueprint, native, duplicates)) return false;
    Tally tb, tn;
    CountGone(blueprint, native, tb, tn);
    Verdicts verdicts;
    Absorb(blueprint, false, gen, tb, verdicts);
    Absorb(native, true, gen, tn, verdicts);
    const long long costUs =
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
    const bool changed = tb.total || tn.total || tb.gone || tn.gone || tb.changedWithin || tn.changedWithin;
    if (!worldStart && !changed) return true;
    UE_LOGI("class_life: %s gen=%u kind=%d (%lld ms after the world appeared): %zu blueprint and %zu "
            "native classes loaded, %d duplicate names, census %lld us",
            worldStart ? "WORLD" : "within the world", gen, static_cast<int>(kind), waitedMs,
            blueprint.size(), native.size(), duplicates, costUs);
    LogTally("blueprint", gen, tb);
    LogTally("native", gen, tn);
    for (const Watched& w : kWatched)
        if (worldStart || verdicts.count(w.cls)) LogWatched(w, gen, verdicts, blueprint, native);
    return true;
}

}  // namespace

void Tick() {
    static const bool s_on = coop::config::ResolveFlag(::coop::config_registry::rows::class_lifetime_probe);
    if (!s_on) return;
    if (!OI::IsSeeded()) return;
    const uint32_t gen = WI::Generation();
    const auto now = Clock::now();
    const WI::WorldKind kind = WI::CurrentWorldKind();
    if (gen != g_judgedGen) {
        if (gen != g_seenGen) { g_seenGen = gen; g_seenAt = now; }
        // As world_singleton_parity: a world is judged once its kind is known, a gameplay world's
        // gamemode is up, and the index holds the world's load.
        if (kind == WI::WorldKind::Unknown) return;
        if (kind == WI::WorldKind::Gameplay && !WS::Gamemode()) return;
        if (OI::Backlog() != 0) return;
        const long long waitedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_seenAt).count();
        if (!CensusAndLog(gen, true, kind, waitedMs)) return;
        g_judgedGen = gen;
        g_lastCensus = now;
        return;
    }
    if (now - g_lastCensus < std::chrono::seconds(30) || OI::Backlog() != 0) return;
    g_lastCensus = now;
    CensusAndLog(gen, false, kind,
                 std::chrono::duration_cast<std::chrono::milliseconds>(now - g_seenAt).count());
}

}  // namespace coop::dev::class_lifetime_probe
