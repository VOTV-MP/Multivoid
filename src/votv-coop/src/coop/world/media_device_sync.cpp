// coop/world/media_device_sync.cpp -- see coop/world/media_device_sync.h.

#include "coop/world/media_device_sync.h"

#include "coop/interactables/atv_sync.h"  // an ATV radio's ATV, by its key
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/remote_prop.h"  // KeyToWString

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/world/media_device.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::media_device_sync {
namespace {

namespace MD = ue_wrap::media_device;
namespace R = ue_wrap::reflection;
using Clock = std::chrono::steady_clock;

constexpr auto kPoll = std::chrono::milliseconds(250);
constexpr auto kHeartbeat = std::chrono::seconds(5);  // a late copy's time, a joiner's first state
constexpr auto kPredictFor = std::chrono::seconds(3);  // this client's own edit stands until the host has answered

std::atomic<coop::net::Session*> g_session{nullptr};
Clock::time_point g_nextPoll{};

// HOST: what went out last.
struct Sent {
    MD::State state;
    Clock::time_point at{};
};
std::unordered_map<std::wstring, Sent> g_sent;
std::unordered_map<std::wstring, MD::State> g_edit;

// CLIENT: the state this copy is moved to, and what the player's interface last read.
struct Want {
    MD::State state;
    Clock::time_point at{};
    Clock::time_point predictUntil{};
    bool have = false;
    bool warned = false;
    MD::State seen;
    bool seenAny = false;
};
std::unordered_map<std::wstring, Want> g_want;

uint32_t g_published = 0, g_edits = 0, g_applied = 0, g_missing = 0;

// An ATV's radio is a child actor of the ATV with no key of its own that names it on every peer; it is named by its
// ATV's key, which does.
constexpr const wchar_t* kAtvRadioPrefix = L"atv:";

std::wstring KeyOf(void* actor) {
    if (void* parent = ue_wrap::engine::ParentActorOf(actor)) {
        if (!ue_wrap::atv::IsAtv(parent)) return {};
        const std::wstring atv = ue_wrap::atv::GetKeyString(parent);
        return atv.empty() || atv == L"None" ? std::wstring{} : kAtvRadioPrefix + atv;
    }
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(actor);
    return key == L"None" ? std::wstring{} : key;
}

// The device a key names here: a keyed radio or TV, or the radio of the ATV an `atv:` key names.
void* FindDevice(const std::wstring& key) {
    const size_t n = std::char_traits<wchar_t>::length(kAtvRadioPrefix);
    if (key.compare(0, n, kAtvRadioPrefix) != 0) return coop::prop_key_index::Find(key);
    coop::atv_sync::TableView t;
    if (!coop::atv_sync::ReadTable(key.substr(n), t) || !t.actor) return nullptr;
    void* component = nullptr;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(t.actor), L"radio");
    if (off >= 0) std::memcpy(&component, static_cast<const uint8_t*>(t.actor) + off, sizeof(component));
    const int32_t child = component && R::IsLive(component) ? R::FindPropertyOffset(R::ClassOf(component), L"ChildActor") : -1;
    void* radio = nullptr;
    if (child >= 0) std::memcpy(&radio, static_cast<const uint8_t*>(component) + child, sizeof(radio));
    return radio && R::IsLive(radio) ? radio : nullptr;
}

std::vector<void*> g_classes;
uint64_t g_classVersion = ~uint64_t{0};

void Collect(std::vector<void*>& out) {
    struct Walk {
        void* world;
        std::vector<void*>* out;
    } walk{ue_wrap::world_identity::CurrentWorld(), &out};
    if (!walk.world) return;
    const uint64_t version = ue_wrap::object_index::ClassSetVersion();
    if (version != g_classVersion) {  // a TV's family is several classes, and the walk is by exact class
        g_classVersion = version;
        g_classes.clear();
        ue_wrap::object_index::ForEachClass(
            [](void*, void* cls, void*) {
                if (MD::IsDeviceClass(cls)) g_classes.push_back(cls);
            },
            nullptr);
    }
    for (void* cls : g_classes)
        ue_wrap::object_index::ForEachInstance(
            cls,
            [](void* ctx, void* obj, int32_t) {
                auto* w = static_cast<Walk*>(ctx);
                if (ue_wrap::world_identity::WorldOf(obj) == w->world) w->out->push_back(obj);
            },
            &walk);
}

coop::net::MediaDeviceStatePayload Encode(const std::wstring& key, const MD::State& state) {
    coop::net::MediaDeviceStatePayload p{};
    for (size_t i = 0; i < key.size() && i < sizeof(p.key.data); ++i)
        p.key.data[p.key.len++] = static_cast<char>(key[i]);  // save keys are ASCII
    p.track = state.track;
    p.ticks = state.ticks;
    p.playing = state.playing ? 1 : 0;
    p.mode = state.mode;
    p.channel = state.channel;
    p.volume = state.volume;
    p.brightness = state.brightness;
    p.squeezeX = state.squeezeX;
    p.squeezeY = state.squeezeY;
    return p;
}

MD::State Decode(const coop::net::MediaDeviceStatePayload& p) {
    MD::State state;
    state.track = p.track;
    state.ticks = p.ticks;
    state.playing = p.playing != 0;
    state.mode = p.mode;
    state.channel = p.channel;
    state.volume = p.volume;
    state.brightness = p.brightness;
    state.squeezeX = p.squeezeX;
    state.squeezeY = p.squeezeY;
    return state;
}

bool SameChoice(const MD::State& a, const MD::State& b) {
    return a.track == b.track && a.playing == b.playing && a.mode == b.mode && a.channel == b.channel &&
           a.volume == b.volume && a.brightness == b.brightness && a.squeezeX == b.squeezeX && a.squeezeY == b.squeezeY;
}

void PollHost(coop::net::Session& s, const std::vector<void*>& radios, Clock::time_point now) {
    for (void* actor : radios) {
        const std::wstring key = KeyOf(actor);
        if (key.empty()) continue;
        // A client's edit that met a track still opening waits here, and what this copy reads until then is not the state.
        const auto edit = g_edit.find(key);
        if (edit != g_edit.end()) {
            if (MD::ApplyState(actor, edit->second) == MD::Apply::Waiting) continue;
            g_edit.erase(edit);
        }
        MD::State state;
        if (!MD::Read(actor, state)) continue;
        auto it = g_sent.find(key);
        if (it != g_sent.end() && SameChoice(it->second.state, state) && (!state.playing || now - it->second.at < kHeartbeat))
            continue;
        const coop::net::MediaDeviceStatePayload p = Encode(key, state);
        if (!s.SendReliable(coop::net::ReliableKind::MediaDeviceState, &p, sizeof(p))) continue;
        const bool changed = it == g_sent.end() || !SameChoice(it->second.state, state);
        g_sent[key] = {state, now};
        ++g_published;
        if (changed)
            UE_LOGI("media_device_sync: '%ls' track=%llx playing=%d mode=%u -- MediaDeviceState sent (#%u)", key.c_str(),
                    static_cast<unsigned long long>(state.track), state.playing ? 1 : 0, state.mode, g_published);
    }
}

void PollClient(coop::net::Session& s, const std::vector<void*>& radios, Clock::time_point now) {
    void* const open = MD::DeviceWithOpenInterface(coop::players::Registry::Get().Local());
    for (void* actor : radios) {
        const std::wstring key = KeyOf(actor);
        if (key.empty()) continue;
        Want& want = g_want[key];
        MD::State read;
        const bool readable = MD::Read(actor, read);
        if (actor != open) {
            want.seenAny = false;
        } else if (readable) {  // while a track opens there is nothing to read, and the last reading stands
            if (want.seenAny && !SameChoice(want.seen, read)) {
                const coop::net::MediaDeviceStatePayload p = Encode(key, read);
                if (s.SendReliableToSlot(0, coop::net::ReliableKind::MediaDeviceState, &p, sizeof(p))) ++g_edits;
                want.state = read;
                want.at = now;
                want.have = true;
                want.predictUntil = now + kPredictFor;
                UE_LOGI("media_device_sync: '%ls' changed here, track=%llx playing=%d mode=%u -- MediaDeviceState sent (#%u)", key.c_str(),
                        static_cast<unsigned long long>(read.track), read.playing ? 1 : 0, read.mode, g_edits);
            }
            want.seen = read;
            want.seenAny = true;
        }
        if (!want.have || now < want.predictUntil) continue;
        MD::State target = want.state;
        if (target.playing)
            target.ticks += std::chrono::duration_cast<std::chrono::microseconds>(now - want.at).count() * 10;
        const MD::Apply result = MD::ApplyState(actor, target);
        if (result == MD::Apply::NoSuchTrack && !want.warned) {
            want.warned = true;
            ++g_missing;
            UE_LOGI("media_device_sync: '%ls' plays a track this peer cannot open (not in its catalog, or Custom Content off) -- "
                    "silent here", key.c_str());
        }
        if (result == MD::Apply::Done) want.warned = false;
    }
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !s->connected()) return;
    const auto now = Clock::now();
    if (now < g_nextPoll) return;
    g_nextPoll = now + kPoll;
    std::vector<void*> radios;
    Collect(radios);
    if (s->role() == coop::net::Role::Host) PollHost(*s, radios, now);
    else PollClient(*s, radios, now);
}

void OnState(int senderSlot, const coop::net::MediaDeviceStatePayload& p) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    const std::wstring key = coop::remote_prop::KeyToWString(p.key);
    if (s->role() == coop::net::Role::Client) {
        Want& want = g_want[key];
        want.state = Decode(p);
        want.at = Clock::now();
        want.have = true;
        ++g_applied;
        return;
    }
    void* const actor = FindDevice(key);
    if (!actor || !MD::IsDevice(actor)) {
        UE_LOGI("media_device_sync: slot %d's edit of '%ls' names no radio or TV here -- dropped", senderSlot, key.c_str());
        return;
    }
    const MD::Apply result = MD::ApplyState(actor, Decode(p));
    if (result == MD::Apply::Waiting) g_edit[key] = Decode(p);
    else g_edit.erase(key);
    ++g_applied;
    UE_LOGI("media_device_sync: slot %d set '%ls' track=%llx playing=%u mode=%u (%hs)", senderSlot, key.c_str(),
            static_cast<unsigned long long>(p.track), p.playing, p.mode,
            result == MD::Apply::NoSuchTrack ? "the host cannot open it"
                                             : (result == MD::Apply::Waiting ? "opening" : "applied"));
}

void QueueConnectBroadcastForSlot(int /*slot*/) { g_sent.clear(); }

void OnDisconnect() {
    if (g_published || g_edits || g_applied || g_missing)
        UE_LOGI("media_device_sync: session end -- published=%u edits=%u applied=%u missing=%u", g_published, g_edits, g_applied,
                g_missing);
    g_sent.clear();
    g_edit.clear();
    g_want.clear();
    g_nextPoll = {};
    g_published = g_edits = g_applied = g_missing = 0;
}

}  // namespace coop::media_device_sync
