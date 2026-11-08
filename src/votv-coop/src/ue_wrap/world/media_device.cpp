// ue_wrap/world/media_device.cpp -- see ue_wrap/world/media_device.h.

#include "ue_wrap/world/media_device.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflected_offset.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"

#include <cmath>
#include <cstring>
#include <unordered_map>

namespace ue_wrap::media_device {
namespace {

namespace R = reflection;

constexpr int64_t kSeekSlackTicks = 20000000;  // 2 s: a peer's own decoder drifts less, and a seek is audible
constexpr int32_t kMaxCatalog = 10000;
constexpr float   kPictureSlack = 0.001f;

std::unordered_map<void*, Kind> g_kinds;

// Two families, and each field read off the class that declares it, so a mixed poll asks nothing twice.
class Field {
public:
    explicit Field(const wchar_t* name) : name_(name) {}
    int32_t Of(void* actor) {
        const size_t family = KindOf(actor) == Kind::Tv ? 1 : 0;
        void* const cls = R::ClassOf(actor);
        if (cls != cls_[family]) {
            cls_[family] = cls;
            off_[family] = R::FindPropertyOffset(cls, name_);
        }
        return off_[family];
    }

private:
    const wchar_t* name_;
    void* cls_[2] = {nullptr, nullptr};
    int32_t off_[2] = {-1, -1};
};

Field g_data{L"data"};
Field g_player{L"mediaPlayer"};
Field g_mode{L"playmode"};
Field g_pause{L"pausePlayer"};
Field g_channel{L"channel"};
Field g_volume{L"audioVolume"};
Field g_brightness{L"brightness"};
Field g_squeezeX{L"squeeze_x"};
Field g_squeezeY{L"squeeze_y"};
Field g_gamemode{L"gamemode"};

void* g_layoutClass[2] = {nullptr, nullptr};
int32_t g_stride = 0, g_link = -1;

void* Object(void* owner, Field& field) {
    const int32_t off = owner ? field.Of(owner) : -1;
    if (off < 0) return nullptr;
    void* value = nullptr;
    std::memcpy(&value, static_cast<const uint8_t*>(owner) + off, sizeof(value));
    return value && R::IsLive(value) ? value : nullptr;
}

template <class T>
T Value(void* actor, Field& field, T fallback) {
    const int32_t off = field.Of(actor);
    if (off < 0) return fallback;
    T value{};
    std::memcpy(&value, static_cast<const uint8_t*>(actor) + off, sizeof(T));
    return value;
}

template <class T>
void Put(void* actor, Field& field, const T& value) {
    const int32_t off = field.Of(actor);
    if (off >= 0) std::memcpy(static_cast<uint8_t*>(actor) + off, &value, sizeof(T));
}

// The struct_tv row's layout, shared by both families, read off the device's `data` and its openLink parameter.
bool Layout(void* actor) {
    void* const cls = R::ClassOf(actor);
    if (!cls) return false;
    const size_t family = KindOf(actor) == Kind::Tv ? 1 : 0;
    if (cls == g_layoutClass[family]) return g_stride > 0;
    g_layoutClass[family] = cls;
    void* const type = R::PropertyInnerStruct(cls, L"data");
    void* const open = R::FindDispatchFunctionCached(cls, L"openLink");
    g_stride = type && open ? R::FindParamSize(open, L"data") : 0;
    g_link = type ? R::FindPropertyOffsetByPrefix(type, L"link_") : -1;
    if (g_stride <= 0 || g_link < 0) {
        g_stride = 0;
        UE_LOGW("media_device: the struct_tv layout did not resolve -- radios and TVs stay on each peer");
    }
    return g_stride > 0;
}

uint64_t RowTrack(const uint8_t* row) {
    const std::wstring link = field_io::ReadFStringAt(row, g_link);
    return link.empty() ? 0 : TrackId(link);
}

// Every catalog row the device can open: the gamemode's two radio lists, or its one TV list.
template <class Fn>
bool ForEachRow(void* actor, Fn&& fn) {
    void* const gamemode = Object(actor, g_gamemode);
    if (!gamemode || !Layout(actor)) return false;
    static R::InstanceOffset sOnline{L"radioLinks"}, sOffline{L"radioLinks_offlin"}, sTv{L"tvVids"};
    R::InstanceOffset* const radio[] = {&sOffline, &sOnline};
    R::InstanceOffset* const tv[] = {&sTv, nullptr};
    for (R::InstanceOffset* list : (KindOf(actor) == Kind::Tv ? tv : radio)) {
        const int32_t off = list ? list->Of(gamemode) : -1;
        if (off < 0) continue;
        field_io::TArrayView rows{};
        std::memcpy(&rows, static_cast<const uint8_t*>(gamemode) + off, sizeof(rows));
        if (!rows.data || rows.num <= 0 || rows.num > kMaxCatalog || rows.max < rows.num) continue;
        for (int32_t i = 0; i < rows.num; ++i)
            if (fn(rows.data + i * g_stride)) return true;
    }
    return false;
}

template <class T>
bool Query(void* object, const wchar_t* name, T& value) {
    void* const fn = object ? R::FindDispatchFunctionCached(R::ClassOf(object), name) : nullptr;
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && Call(object, f) && f.GetRaw(L"ReturnValue", &value, sizeof(value));
}

bool Command(void* object, const wchar_t* name) {
    bool result = false;
    return Query(object, name, result) && result;
}

// The player's url is empty once a channel change or a close has emptied it, though `data` still names the track.
bool HasOpenMedia(void* player) {
    R::FString url{};
    const bool ok = Query(player, L"GetUrl", url);
    const bool open = ok && url.Data && url.Num > 1;
    if (url.Data) R::EngineFree(url.Data);
    return open;
}

bool Powered(void* actor) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(actor), L"powered", off, mask) || off < 0 || !mask) return false;
    return (static_cast<const uint8_t*>(actor)[off] & mask) != 0;
}

// The rule openLink tests, read without the hint its own test shows.
bool CustomContentAllowed(void* actor) {
    void* const lib = R::FindClassDefaultObject(L"lib_C");
    void* const fn = lib ? R::FindDispatchFunctionCached(R::ClassOf(lib), L"isCustom") : nullptr;
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && f.Set<void*>(L"__WorldContext", actor) && Call(lib, f) && f.Get<bool>(L"isCustom");
}

bool Seek(void* player, int64_t ticks) {
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(player), L"Seek");
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && f.Set<int64_t>(L"Time", ticks) && Call(player, f);
}

// As the interface's setChannel does: the old channel freed, the new one taken. The game's -1 is removeChannel's.
void ApplyChannel(void* actor, int32_t now, int32_t wanted) {
    void* const gamemode = Object(actor, g_gamemode);
    if (now >= 0 && gamemode) {
        if (void* fn = R::FindDispatchFunctionCached(R::ClassOf(gamemode), L"removeMediaChannel")) {
            ParamFrame f(fn);
            if (f.valid() && f.Set<int32_t>(L"index", now)) Call(gamemode, f);
        }
    }
    const wchar_t* const verb = wanted >= 0 ? L"useChannel" : L"removeChannel";
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(actor), verb);
    if (!fn) return;
    ParamFrame f(fn);
    if (!f.valid()) return;
    if (wanted >= 0 ? f.Set<int32_t>(L"channel", wanted) : f.Set<int32_t>(L"index", now)) Call(actor, f);
}

void ApplyPicture(void* actor, const State& state) {
    const auto near = [](float a, float b) { return std::fabs(a - b) < kPictureSlack; };
    if (near(Value<float>(actor, g_volume, 0.f), state.volume) && near(Value<float>(actor, g_brightness, 0.f), state.brightness) &&
        near(Value<float>(actor, g_squeezeX, 0.f), state.squeezeX) && near(Value<float>(actor, g_squeezeY, 0.f), state.squeezeY))
        return;
    Put(actor, g_volume, state.volume);
    Put(actor, g_brightness, state.brightness);
    Put(actor, g_squeezeX, state.squeezeX);
    Put(actor, g_squeezeY, state.squeezeY);
    component_calls::CallParamlessNamed(actor, L"updProperties");
}

}  // namespace

bool IsDeviceClass(void* cls) {
    if (!cls) return false;
    auto it = g_kinds.find(cls);
    if (it != g_kinds.end()) return it->second != Kind::None;
    Kind kind = Kind::None;
    for (void* c = cls; c; c = R::SuperStructOf(c)) {
        if (R::NameEquals(R::NameOf(c), L"prop_radio_atv_C")) {
            kind = Kind::None;
            break;
        }
        if (R::NameEquals(R::NameOf(c), L"prop_radio_internet_C")) kind = Kind::Radio;
        else if (R::NameEquals(R::NameOf(c), L"prop_tv2_C")) kind = Kind::Tv;
    }
    g_kinds.emplace(cls, kind);
    return kind != Kind::None;
}

Kind KindOf(void* actor) {
    if (!actor || !R::IsLive(actor)) return Kind::None;
    void* const cls = R::ClassOf(actor);
    if (!IsDeviceClass(cls)) return Kind::None;
    return g_kinds[cls];
}

uint64_t TrackId(std::wstring link) {
    for (wchar_t& c : link) {
        if (c == L'\\') c = L'/';
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + (L'a' - L'A'));
    }
    if (link.find(L"://") == std::wstring::npos || link.rfind(L"file://", 0) == 0) {
        size_t root = link.rfind(L"/radio/");
        size_t skip = 7;
        if (root == std::wstring::npos) {
            root = link.rfind(L"/tv/");
            skip = 4;
        }
        if (root != std::wstring::npos) link.erase(0, root + skip);
        else if (const size_t slash = link.find_last_of(L'/'); slash != std::wstring::npos) link.erase(0, slash + 1);
    }
    uint64_t hash = 14695981039346656037ull;  // FNV-1a
    for (wchar_t c : link)
        for (unsigned shift : {0u, 8u}) {
            hash ^= (static_cast<uint16_t>(c) >> shift) & 255u;
            hash *= 1099511628211ull;
        }
    return hash ? hash : 1;
}

bool Read(void* actor, State& out) {
    const Kind kind = KindOf(actor);
    if (kind == Kind::None || !Layout(actor)) return false;
    const int32_t data = g_data.Of(actor), mode = g_mode.Of(actor);
    if (data < 0 || mode < 0) return false;
    State value;
    value.mode = static_cast<const uint8_t*>(actor)[mode];
    if (kind == Kind::Tv) {
        value.channel = Value<int32_t>(actor, g_channel, -1);
        value.volume = Value<float>(actor, g_volume, 0.f);
        value.brightness = Value<float>(actor, g_brightness, 0.f);
        value.squeezeX = Value<float>(actor, g_squeezeX, 0.f);
        value.squeezeY = Value<float>(actor, g_squeezeY, 0.f);
    }
    void* const player = Object(actor, g_player);
    if (!player) {
        if (kind == Kind::Radio) return false;
        out = value;  // a TV on no channel has no player and no track to read
        return true;
    }
    bool preparing = false, playing = false;
    if (!Query(player, L"IsPreparing", preparing) || preparing) return false;
    value.track = RowTrack(static_cast<const uint8_t*>(actor) + data);
    if (value.track) {
        if (!Query(player, L"IsPlaying", playing) || !Query(player, L"GetTime", value.ticks)) return false;
        value.playing = playing;
        if (value.ticks < 0) value.ticks = 0;
    }
    out = value;
    return true;
}

Apply ApplyState(void* actor, const State& state) {
    const Kind kind = KindOf(actor);
    if (kind == Kind::None || !Layout(actor)) return Apply::Waiting;
    const int32_t data = g_data.Of(actor), mode = g_mode.Of(actor), pause = g_pause.Of(actor);
    if (data < 0 || mode < 0 || pause < 0) return Apply::Waiting;
    auto* base = static_cast<uint8_t*>(actor);
    base[mode] = state.mode;
    if (kind == Kind::Tv) {
        const int32_t channel = Value<int32_t>(actor, g_channel, -1);
        if (channel != state.channel) ApplyChannel(actor, channel, state.channel);
        ApplyPicture(actor, state);
    }
    if (!state.track) return Apply::Done;
    void* const player = Object(actor, g_player);
    if (!player) return Apply::Waiting;
    bool preparing = false, playing = false;
    if (!Query(player, L"IsPreparing", preparing) || preparing) return Apply::Waiting;
    base[pause] = state.playing ? 0 : 1;  // its opened callback reads this
    if (RowTrack(base + data) != state.track || !HasOpenMedia(player)) {
        if (kind == Kind::Tv && !Powered(actor)) return Apply::Waiting;  // its openLink does nothing until its cord is in
        const uint8_t* found = nullptr;
        if (CustomContentAllowed(actor)) ForEachRow(actor, [&](const uint8_t* row) {
            if (RowTrack(row) != state.track) return false;
            found = row;
            return true;
        });
        if (!found) {
            if (Query(player, L"IsPlaying", playing) && playing) Command(player, L"Pause");
            return Apply::NoSuchTrack;
        }
        void* const open = R::FindDispatchFunctionCached(R::ClassOf(actor), L"openLink");
        if (open) {
            ParamFrame f(open);
            if (f.valid() && f.SetRaw(L"data", found, g_stride) && f.Set<bool>(L"pausePlayer", !state.playing)) Call(actor, f);
        }
        return Apply::Waiting;
    }
    int64_t time = 0;
    if (!Query(player, L"IsPlaying", playing) || !Query(player, L"GetTime", time)) return Apply::Waiting;
    if (time - state.ticks > kSeekSlackTicks || state.ticks - time > kSeekSlackTicks) Seek(player, state.ticks);
    if (playing != state.playing) Command(player, state.playing ? L"Play" : L"Pause");
    return Apply::Done;
}

uint64_t CatalogTrack(void* actor, int index) {
    uint64_t track = 0;
    int at = 0;
    ForEachRow(actor, [&](const uint8_t* row) {
        if (at++ != index) return false;
        track = RowTrack(row);
        return true;
    });
    return track;
}

void* DeviceWithOpenInterface(void* player) {
    const int32_t off = player ? reflected_offset::MainPlayer_activeInterface() : -1;
    if (off < 0) return nullptr;
    void* widget = nullptr;
    std::memcpy(&widget, static_cast<const uint8_t*>(player) + off, sizeof(widget));
    if (!widget || !R::IsLive(widget)) return nullptr;
    void* const cls = R::ClassOf(widget);
    if (!R::NameEquals(R::NameOf(cls), L"ui_radioInterface_C") && !R::NameEquals(R::NameOf(cls), L"ui_tvInterface_C")) return nullptr;
    const int32_t device = R::FindPropertyOffset(cls, L"radio");  // both interfaces name their device this
    if (device < 0) return nullptr;
    void* value = nullptr;
    std::memcpy(&value, static_cast<const uint8_t*>(widget) + device, sizeof(value));
    return IsDevice(value) ? value : nullptr;
}

}  // namespace ue_wrap::media_device
