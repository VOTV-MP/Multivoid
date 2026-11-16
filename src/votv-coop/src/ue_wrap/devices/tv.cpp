// ue_wrap/devices/tv.cpp -- see ue_wrap/devices/tv.h.

#include "ue_wrap/devices/tv.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/ftext_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/world_identity.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace ue_wrap::tv {
namespace {

namespace R = ue_wrap::reflection;

// struct_tv's layout: two FStrings and a bool. The struct is the game's own and its members are
// reached by size, the FString header being the engine's.
constexpr int32_t kNameOff = 0x00;
constexpr int32_t kLinkOff = 0x10;
constexpr int32_t kIsUrlOff = 0x20;
constexpr int32_t kChannels = 16;

void* TvClass() {
    static void* cls = nullptr;
    if (!cls) cls = R::FindClass(L"prop_tv2_C");
    return cls;
}

int32_t Off(void* tv, const wchar_t* member) { return R::FindPropertyOffset(R::ClassOf(tv), member); }

void* Fn(void* obj, const wchar_t* name) { return R::FindDispatchFunctionCached(R::ClassOf(obj), name); }

struct Walk {
    void* base;
    void* world;
    std::vector<void*>* out;
};

}  // namespace

bool IsTv(void* obj) {
    void* base = TvClass();
    if (!obj || !base || !R::IsLive(obj)) return false;
    return R::IsDescendantOfAny(R::ClassOf(obj), &base, 1);
}

std::vector<void*> FindAll() {
    std::vector<void*> out;
    Walk walk{TvClass(), ue_wrap::world_identity::CurrentWorld(), &out};
    if (!walk.base || !walk.world) return out;
    ue_wrap::object_index::ForEachClass([](void* ctx, void* cls, void* /*anyInstance*/) {
        Walk* w = static_cast<Walk*>(ctx);
        if (!R::IsDescendantOfAny(cls, &w->base, 1)) return;
        ue_wrap::object_index::ForEachInstance(cls, [](void* c, void* obj, int32_t) {
            Walk* inner = static_cast<Walk*>(c);
            if (!obj || !R::IsLive(obj)) return;
            if (R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
            if (ue_wrap::world_identity::WorldOf(obj) != inner->world) return;
            inner->out->push_back(obj);
        }, w);
    }, &walk);
    return out;
}

bool Powered(void* tv) {
    const int32_t off = Off(tv, L"powered");
    return off >= 0 && *(reinterpret_cast<const uint8_t*>(tv) + off) != 0;
}

void* PlayerOf(void* tv) {
    const int32_t off = Off(tv, L"MediaPlayer");
    if (off < 0) return nullptr;
    void* player = *reinterpret_cast<void* const*>(reinterpret_cast<const uint8_t*>(tv) + off);
    return (player && R::IsLive(player)) ? player : nullptr;
}

bool TakeAnyChannel(void* tv) {
    if (PlayerOf(tv)) return true;
    void* fn = Fn(tv, L"useChannel");
    if (!fn) return false;
    // useChannel leaves the set without a player when the gamemode says the channel is busy.
    for (int32_t channel = 0; channel < kChannels; ++channel) {
        ue_wrap::ParamFrame f(fn);
        if (!f.valid() || !f.Set<int32_t>(L"Channel", channel) || !ue_wrap::Call(tv, f)) return false;
        if (PlayerOf(tv)) return true;
    }
    return false;
}

bool OpenLink(void* tv, const std::wstring& name, const std::wstring& link) {
    void* fn = Fn(tv, L"openLink");
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    const int32_t off = f.valid() ? f.ParamOffset(L"Data") : -1;
    if (off < 0 || f.FrameSize() < off + kIsUrlOff + 1) return false;
    // The graph copies the struct into its own frame before it returns, so the two headers point
    // at this call's strings.
    ue_wrap::field_io::FStringView n{const_cast<wchar_t*>(name.c_str()), static_cast<int32_t>(name.size() + 1),
                                     static_cast<int32_t>(name.size() + 1)};
    ue_wrap::field_io::FStringView l{const_cast<wchar_t*>(link.c_str()), static_cast<int32_t>(link.size() + 1),
                                     static_cast<int32_t>(link.size() + 1)};
    uint8_t* frame = static_cast<uint8_t*>(f.data());
    std::memcpy(frame + off + kNameOff, &n, sizeof(n));
    std::memcpy(frame + off + kLinkOff, &l, sizeof(l));
    frame[off + kIsUrlOff] = 1;
    f.Set<bool>(L"pausePlayer", false);
    const bool ran = ue_wrap::Call(tv, f);
    // The frame is freed as bytes; the headers must not outlive the strings they point at.
    std::memset(frame + off, 0, static_cast<size_t>(kIsUrlOff + 1));
    return ran;
}

bool ReadOpenLinkArgs(void* function, const unsigned char* locals, std::wstring* name, std::wstring* link,
                      bool* isUrl) {
    if (!function || !locals) return false;
    static void* sFn = nullptr;
    static int32_t sOff = -1;
    if (sFn != function) {
        sFn = function;
        sOff = R::FindParamOffset(function, L"Data");
    }
    if (sOff < 0) return false;
    if (name) *name = ue_wrap::field_io::ReadFStringAt(locals, sOff + kNameOff);
    if (link) *link = ue_wrap::field_io::ReadFStringAt(locals, sOff + kLinkOff);
    if (isUrl) *isUrl = locals[sOff + kIsUrlOff] != 0;
    return true;
}

bool ReadCurrent(void* tv, std::wstring* name, std::wstring* link, bool* isUrl) {
    const int32_t off = tv ? Off(tv, L"Data") : -1;
    if (off < 0) return false;
    const uint8_t* base = reinterpret_cast<const uint8_t*>(tv);
    if (name) *name = ue_wrap::field_io::ReadFStringAt(base, off + kNameOff);
    if (link) *link = ue_wrap::field_io::ReadFStringAt(base, off + kLinkOff);
    if (isUrl) *isUrl = base[off + kIsUrlOff] != 0;
    return true;
}

int IsPlaying(void* tv) {
    void* player = PlayerOf(tv);
    void* fn = player ? Fn(player, L"IsPlaying") : nullptr;
    if (!fn) return -1;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid() || !ue_wrap::Call(player, f)) return -1;
    return f.Get<bool>(L"ReturnValue") ? 1 : 0;
}

double PlayTime(void* tv) {
    void* player = PlayerOf(tv);
    void* fn = player ? Fn(player, L"GetTime") : nullptr;
    if (!fn) return -1.0;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid() || !ue_wrap::Call(player, f)) return -1.0;
    return static_cast<double>(f.Get<int64_t>(L"ReturnValue")) / 1e7;  // FTimespan ticks of 100 ns
}

namespace {

// A no-argument verb of the set's player that answers a bool.
bool PlayerVerb(void* tv, const wchar_t* verb) {
    void* player = PlayerOf(tv);
    void* fn = player ? Fn(player, verb) : nullptr;
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && ue_wrap::Call(player, f);
}

void* Member(void* obj, const wchar_t* name) {
    const int32_t off = obj ? Off(obj, name) : -1;
    if (off < 0) return nullptr;
    void* m = *reinterpret_cast<void* const*>(reinterpret_cast<const uint8_t*>(obj) + off);
    return (m && R::IsLive(m)) ? m : nullptr;
}

}  // namespace

double Duration(void* tv) {
    void* player = PlayerOf(tv);
    void* fn = player ? Fn(player, L"GetDuration") : nullptr;
    if (!fn) return -1.0;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid() || !ue_wrap::Call(player, f)) return -1.0;
    return static_cast<double>(f.Get<int64_t>(L"ReturnValue")) / 1e7;
}

bool Pause(void* tv) { return PlayerVerb(tv, L"Pause"); }
bool Resume(void* tv) { return PlayerVerb(tv, L"Play"); }

bool Seek(void* tv, double seconds) {
    void* player = PlayerOf(tv);
    void* fn = player ? Fn(player, L"Seek") : nullptr;
    if (!fn || seconds < 0.0) return false;
    ue_wrap::ParamFrame f(fn);
    const int64_t ticks = static_cast<int64_t>(seconds * 1e7);  // FTimespan
    return f.valid() && f.Set<int64_t>(L"Time", ticks) && ue_wrap::Call(player, f);
}

void* RemoteSet(void* remote) {
    void* tv = Member(remote, L"radio");
    return IsTv(tv) ? tv : nullptr;
}

double RemoteSliderSeconds(void* remote) {
    void* slider = Member(remote, L"Slider_playback");
    void* fn = slider ? Fn(slider, L"GetValue") : nullptr;
    const int32_t durOff = remote ? Off(remote, L"Duration") : -1;
    if (!fn || durOff < 0) return -1.0;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid() || !ue_wrap::Call(slider, f)) return -1.0;
    const double length = static_cast<double>(*reinterpret_cast<const int64_t*>(reinterpret_cast<const uint8_t*>(remote) + durOff)) / 1e7;
    return length > 0.0 ? static_cast<double>(f.Get<float>(L"ReturnValue")) * length : -1.0;
}

bool ClearRemoteSearch(void* remote) {
    void* box = Member(remote, L"textbox_search");
    void* setText = box ? Fn(box, L"SetText") : nullptr;
    if (!setText) return false;
    ue_wrap::ParamFrame f(setText);
    const int32_t off = f.valid() ? f.ParamOffset(L"InText") : -1;
    if (off < 0 || !ue_wrap::ftext_utils::EmptyFText(static_cast<uint8_t*>(f.data()) + off) || !ue_wrap::Call(box, f)) return false;
    // The list is filtered by the box's text when it is built.
    if (void* gen = Fn(remote, L"gen")) {
        ue_wrap::ParamFrame g(gen);
        if (g.valid()) ue_wrap::Call(remote, g);
    }
    return true;
}

bool ForcePowered(void* tv, bool on) {
    const int32_t off = Off(tv, L"powered");
    if (off < 0) return false;
    *(reinterpret_cast<uint8_t*>(tv) + off) = on ? 1 : 0;
    if (void* fn = Fn(tv, L"upd")) {
        ue_wrap::ParamFrame f(fn);
        if (f.valid()) ue_wrap::Call(tv, f);
    }
    return true;
}

std::string ScreenState(void* tv) {
    if (!IsTv(tv)) return "no set";
    const uint8_t* base = reinterpret_cast<const uint8_t*>(tv);
    const auto flag = [&](const wchar_t* name) {
        const int32_t off = Off(tv, name);
        return off < 0 ? -1 : (base[off] != 0 ? 1 : 0);
    };
    const auto num = [&](const wchar_t* name) {
        const int32_t off = Off(tv, name);
        return off < 0 ? -99 : *reinterpret_cast<const int32_t*>(base + off);
    };
    // A no-argument or one-argument call that answers an object, an int or a float.
    const auto askObj = [](void* obj, const wchar_t* fnName, const wchar_t* arg, int32_t value) -> void* {
        void* fn = obj ? Fn(obj, fnName) : nullptr;
        if (!fn) return nullptr;
        ue_wrap::ParamFrame f(fn);
        if (!f.valid() || (arg && !f.Set<int32_t>(arg, value)) || !ue_wrap::Call(obj, f)) return nullptr;
        return f.Get<void*>(L"ReturnValue");
    };
    const auto askInt = [](void* obj, const wchar_t* fnName, const wchar_t* arg, uint8_t value) -> int32_t {
        void* fn = obj ? Fn(obj, fnName) : nullptr;
        if (!fn) return -99;
        ue_wrap::ParamFrame f(fn);
        if (!f.valid() || (arg && !f.Set<uint8_t>(arg, value)) || !ue_wrap::Call(obj, f)) return -99;
        return f.Get<int32_t>(L"ReturnValue");
    };
    void* dynmat = Member(tv, L"dynmat");
    void* texture = Member(tv, L"MediaTexture");
    void* player = PlayerOf(tv);
    void* mesh = Member(tv, L"StaticMesh");
    const int32_t index = num(L"screenMaterialIndex");
    void* worn = askObj(mesh, L"GetMaterial", L"ElementIndex", index);
    const auto scalar = [&](const wchar_t* name) {
        void* fn = dynmat ? Fn(dynmat, L"K2_GetScalarParameterValue") : nullptr;
        if (!fn) return -99.f;
        ue_wrap::ParamFrame f(fn);
        if (!f.valid() || !f.Set<R::FName>(L"ParameterName", ue_wrap::fname_utils::StringToFName(name)) ||
            !ue_wrap::Call(dynmat, f))
            return -99.f;
        return f.Get<float>(L"ReturnValue");
    };
    void* shown = nullptr;
    if (void* fn = dynmat ? Fn(dynmat, L"K2_GetTextureParameterValue") : nullptr) {
        ue_wrap::ParamFrame f(fn);
        if (f.valid() && f.Set<R::FName>(L"ParameterName", ue_wrap::fname_utils::StringToFName(L"tex")) &&
            ue_wrap::Call(dynmat, f))
            shown = f.Get<void*>(L"ReturnValue");
    }
    const std::wstring wornName = worn ? R::ToString(R::NameOf(worn)) : std::wstring(L"none");
    char line[640];
    std::snprintf(line, sizeof(line),
                  "powered=%d picture=%d security=%d channel=%d player=%d dynmat=%d mesh-wears-dynmat=%d (index %d, worn '%ls') "
                  "static=%.2f active=%.2f disconnected=%.2f brightness=%.2f tex-is-channel-texture=%d texture=%dx%d "
                  "texture-player-is-set's=%d video-tracks=%d selected=%d",
                  flag(L"powered"), flag(L"picture"), flag(L"isSecurity"), num(L"channel"), player ? 1 : 0, dynmat ? 1 : 0,
                  (dynmat && worn == dynmat) ? 1 : 0, index, wornName.c_str(), scalar(L"static"),
                  scalar(L"active"), scalar(L"disconnected"), scalar(L"brightness"),
                  (texture && shown == texture) ? 1 : 0, askInt(texture, L"GetWidth", nullptr, 0),
                  askInt(texture, L"GetHeight", nullptr, 0),
                  (player && askObj(texture, L"GetMediaPlayer", nullptr, 0) == player) ? 1 : 0,
                  askInt(player, L"GetNumTracks", L"TrackType", 6), askInt(player, L"GetSelectedTrack", L"TrackType", 6));
    return line;
}

}  // namespace ue_wrap::tv
