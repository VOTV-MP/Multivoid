// coop/interactables/woodchipper_sync.cpp -- see coop/interactables/woodchipper_sync.h.

#include "coop/interactables/woodchipper_sync.h"

#include "coop/element/object_scan_hub.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_key_index.h"
#include "coop/session/net_pump.h"
#include "coop/session/poll_timer.h"
#include "coop/world/spawn_authority.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop::woodchipper_sync {
namespace {

namespace F  = ue_wrap::prop;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
using Clock = std::chrono::steady_clock;
using Payload = coop::net::WoodchipperPayload;

UE_FACT_FN(kRowAction, "woodchipper_sync", nullptr, L"actionOptionIndex", AnyClass);
constexpr int kTagAction = 0x57434850;  // 'WCHP'

constexpr uint8_t kUseAction = 4;      // the button's branch of the chipper's actionOptionIndex
constexpr float kMaxFuel = 100.f;      // gascanFuel's own ceiling, and the tick's clamp
constexpr float kPourMin = 1.f;        // a client's copy gained this much fuel at once: a pour (burning only lowers it)
constexpr float kSendFuelStep = 0.5f;  // the host sends when the fuel moved by this
constexpr float kReachCm = 800.f;      // a pour's or a press's sender from its chipper, on the host
constexpr size_t kQueueCap = 32;
constexpr auto kResend = std::chrono::seconds(1);      // a running chipper again, as its fuel burns
constexpr auto kStateLife = std::chrono::seconds(10);  // a client's unplaced state is dropped after this

std::atomic<coop::net::Session*> g_session{nullptr};
coop::session::PollTimer g_poll{std::chrono::milliseconds(250)};
uint32_t g_world = 0;
bool g_watched = false, g_saidLive = false, g_hubRegistered = false;
int g_applying = 0;   // our own writes on a client run inside this

struct ApplyScope {
    ApplyScope() { ++g_applying; }
    ~ApplyScope() { --g_applying; }
};

// ---- the class ----

void* g_cls = nullptr;
int32_t g_offFuel = -1, g_offActive = -1;

bool EnsureResolved() {
    if (g_cls) return true;
    void* cls = R::FindClass(L"woodchipper_C");
    if (!cls) return false;
    g_offFuel = R::FindPropertyOffset(cls, L"fuel");
    g_offActive = R::FindPropertyOffset(cls, L"active");
    if (g_offFuel < 0 || g_offActive < 0) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            UE_LOGE("woodchipper: woodchipper_C has no `fuel` or no `active` (%d, %d) -- not carried", g_offFuel,
                    g_offActive);
        }
        return false;
    }
    g_cls = cls;
    return true;
}

bool IsChipper(void* obj) {
    if (!obj || !g_cls) return false;
    void* cls = R::ClassOf(obj);
    void* bases[1] = {g_cls};
    return cls && R::IsDescendantOfAny(cls, bases, 1);
}

struct State {
    float fuel = 0.f;
    bool active = false;
};

bool Read(void* c, State& out) {
    if (!c || !R::IsLive(c) || !g_cls) return false;
    const uint8_t* b = static_cast<const uint8_t*>(c);
    std::memcpy(&out.fuel, b + g_offFuel, sizeof(out.fuel));
    out.active = b[g_offActive] != 0;
    return std::isfinite(out.fuel);
}

void WriteFuel(void* c, float fuel) {
    std::memcpy(static_cast<uint8_t*>(c) + g_offFuel, &fuel, sizeof(fuel));
}

void WriteActive(void* c, bool active) {
    static_cast<uint8_t*>(c)[g_offActive] = active ? 1 : 0;
}

// The chipper's own checkActive: its intake, its particles, its spin-up and its sounds follow `active` and the fuel.
void CheckActive(void* c) {
    if (!ue_wrap::component_calls::CallParamlessNamed(c, L"checkActive"))
        UE_LOGW("woodchipper: checkActive did not run");
}

std::wstring KeyOf(void* c) {
    std::wstring key = F::GetInteractableKeyString(c);
    if (key.empty() || key == L"None" || key.size() >= sizeof(coop::net::WireKey::data)) return {};
    return key;
}

// ---- the index: every chipper of this world, by key, from the shared scan pass ----

struct Ref {
    void* actor;
    int32_t idx;
};
std::unordered_map<std::wstring, Ref> g_index;
std::vector<std::pair<std::wstring, Ref>> g_scanFound;
uint32_t g_indexGen = 0;
size_t g_lastCount = SIZE_MAX;

bool IndexCurrent() { return g_indexGen == ue_wrap::world_identity::Generation(); }

void HubPassBegin(void*, bool) { g_scanFound.clear(); }

void HubMatch(void*, void* obj) {
    if (!R::IsLive(obj)) return;
    std::wstring key = KeyOf(obj);
    if (!key.empty()) g_scanFound.emplace_back(std::move(key), Ref{obj, R::InternalIndexOf(obj)});
}

size_t HubPassComplete(void*, bool isFull, uint32_t worldGen) {
    if (isFull) g_index.clear();
    for (auto& f : g_scanFound) g_index[f.first] = f.second;
    if (!isFull) {
        for (auto it = g_index.begin(); it != g_index.end();)
            it = R::IsLiveByIndex(it->second.actor, it->second.idx) ? std::next(it) : g_index.erase(it);
    }
    g_scanFound.clear();
    g_indexGen = worldGen;
    if (g_index.size() != g_lastCount) {
        g_lastCount = g_index.size();
        UE_LOGI("woodchipper: index -- %zu chipper(s)", g_lastCount);
    }
    return g_index.size();
}

void* Resolve(const std::wstring& key) {
    if (IndexCurrent()) {
        auto it = g_index.find(key);
        if (it != g_index.end() && R::IsLiveByIndex(it->second.actor, it->second.idx)) return it->second.actor;
    }
    void* c = coop::prop_key_index::Find(key);   // one the pass has not reached yet
    return IsChipper(c) ? c : nullptr;
}

coop::net::Session* Live() {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    return s && s->connected() ? s : nullptr;
}

bool ValidFuel(float f) { return std::isfinite(f) && f >= 0.f && f <= kMaxFuel; }

// ---- HOST ----

struct Sent {
    State state;
    Clock::time_point at{};
    bool valid = false, pending = false;
};
std::unordered_map<std::wstring, Sent> g_sent;

struct Intent {
    int slot = 0;
    Payload p{};
};
std::vector<Intent> g_intents;
uint32_t g_hostPours = 0, g_hostPresses = 0, g_hostRefused = 0;

bool InReach(int slot, void* chipper) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
    void* body = rp && rp->valid() ? rp->GetActor() : nullptr;
    ue_wrap::FVector at{}, by{};
    if (!body || !ue_wrap::engine::TryGetActorLocation(chipper, at) || !ue_wrap::engine::TryGetActorLocation(body, by))
        return false;
    const float dx = at.X - by.X, dy = at.Y - by.Y, dz = at.Z - by.Z;
    return dx * dx + dy * dy + dz * dz <= kReachCm * kReachCm;
}

void HostServe(coop::net::Session& s) {
    std::vector<Intent> intents;
    intents.swap(g_intents);
    for (const Intent& in : intents) {
        const std::wstring key = coop::net::StringFromWireKey(in.p.key);
        if (!s.IsSlotWorldReady(in.slot)) continue;
        void* c = Resolve(key);
        State st;
        if (!Read(c, st)) continue;
        g_sent[key].pending = true;   // the answer, whatever it is
        if (!InReach(in.slot, c)) {
            ++g_hostRefused;
            UE_LOGW("woodchipper: slot %d's %s on '%ls' is out of reach -- refused", in.slot,
                    in.p.op == coop::net::kWoodchipperOpPour ? "pour" : "press", key.c_str());
            continue;
        }
        if (in.p.op == coop::net::kWoodchipperOpPour) {
            // gascanFuel's own end: the fuel in, then checkActive.
            const float to = std::fmin(st.fuel + in.p.fuel, kMaxFuel);
            WriteFuel(c, to);
            CheckActive(c);
            ++g_hostPours;
            UE_LOGI("woodchipper: slot %d poured %.1f into '%ls' on the host: %.1f -> %.1f (#%u)", in.slot, in.p.fuel,
                    key.c_str(), st.fuel, to, g_hostPours);
        } else if (in.p.op == coop::net::kWoodchipperOpButton) {
            // The button's own end: `active` flipped, then checkActive when there is fuel.
            const bool want = in.p.active != 0;
            if (want != st.active) {
                WriteActive(c, want);
                if (st.fuel > 0.f) CheckActive(c);
            }
            ++g_hostPresses;
            UE_LOGI("woodchipper: slot %d switched '%ls' %s on the host (#%u)", in.slot, key.c_str(), want ? "on" : "off",
                    g_hostPresses);
        }
    }
}

void HostPoll(coop::net::Session& s, Clock::time_point now) {
    if (!IndexCurrent()) return;
    for (const auto& [key, ref] : g_index) {
        if (!R::IsLiveByIndex(ref.actor, ref.idx)) continue;
        State st;
        if (!Read(ref.actor, st)) continue;
        Sent& sent = g_sent[key];
        const bool due = sent.pending || !sent.valid || st.active != sent.state.active ||
                         std::fabs(st.fuel - sent.state.fuel) >= kSendFuelStep || (st.active && now - sent.at >= kResend);
        if (!due) continue;
        Payload p{};
        coop::net::WireKeyFromString(key, p.key);
        p.fuel = std::fmin(std::fmax(st.fuel, 0.f), kMaxFuel);
        p.active = st.active ? 1 : 0;
        p.op = coop::net::kWoodchipperOpState;
        if (!s.SendReliable(coop::net::ReliableKind::WoodchipperState, &p, sizeof(p))) continue;
        if (!sent.valid || st.active != sent.state.active)
            UE_LOGI("woodchipper: '%ls' is %s with %.1f fuel", key.c_str(), st.active ? "on" : "off", p.fuel);
        sent.state = st;
        sent.at = now;
        sent.valid = true;
        sent.pending = false;
    }
    for (auto it = g_sent.begin(); it != g_sent.end();)
        it = g_index.count(it->first) ? std::next(it) : g_sent.erase(it);
}

// ---- CLIENT ----

struct Shown {
    State state;
    Clock::time_point at{};
};
std::unordered_map<std::wstring, Shown> g_shown;           // the host's state, until it is written
std::unordered_map<std::wstring, float> g_seen;            // each copy's fuel at the last poll or write
std::vector<Payload> g_outbox;
// A chipper this player just poured into or pressed: the host's states sent before it heard of it would undo it on the
// copy, so they wait out this hold, and the newest is written after.
std::unordered_map<std::wstring, Clock::time_point> g_hold;
constexpr auto kHold = std::chrono::milliseconds(1500);
uint32_t g_clientPours = 0, g_clientPresses = 0, g_clientDropped = 0;

void Queue(const Payload& p) {
    if (g_outbox.size() >= kQueueCap) {
        ++g_clientDropped;
        return;
    }
    g_outbox.push_back(p);
    g_hold[coop::net::StringFromWireKey(p.key)] = Clock::now() + kHold;
}

// The local player's press on a chipper's button, read across the body: `active` before and after.
void* g_pressChipper = nullptr;
bool g_pressBefore = false;

sg::Verdict OnActionPre(const sg::Call& c) {
    g_pressChipper = nullptr;
    if (!coop::spawn_authority::IsActiveClientSession() || g_applying || !EnsureResolved() || !IsChipper(c.object))
        return sg::Verdict::Run;
    const int32_t playerOff = R::FindParamOffset(c.function, L"Player");
    const int32_t actionOff = R::FindParamOffset(c.function, L"Action");
    if (playerOff < 0 || actionOff < 0 || !c.locals || c.locals[actionOff] != kUseAction) return sg::Verdict::Run;
    void* player = *reinterpret_cast<void* const*>(c.locals + playerOff);
    if (!coop::players::Registry::Get().IsLocal(player)) return sg::Verdict::Run;
    State st;
    if (!Read(c.object, st)) return sg::Verdict::Run;
    g_pressChipper = c.object;
    g_pressBefore = st.active;
    return sg::Verdict::Run;
}

void OnActionPost(const sg::Call& c) {
    if (!g_pressChipper || c.object != g_pressChipper) return;
    g_pressChipper = nullptr;
    State st;
    if (!Read(c.object, st) || st.active == g_pressBefore) return;   // the press missed the button
    const std::wstring key = KeyOf(c.object);
    if (key.empty()) return;
    Payload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.op = coop::net::kWoodchipperOpButton;
    p.active = st.active ? 1 : 0;
    Queue(p);
    ++g_clientPresses;
    UE_LOGI("woodchipper: this player switched '%ls' %s -- the host switches it", key.c_str(), st.active ? "on" : "off");
}

// A pour on a copy: its fuel rose past the last reading, which its own burning cannot do. Read before the host's states
// are written, so a state arriving in the same poll does not hide it.
void DetectPours() {
    if (!IndexCurrent()) return;
    for (const auto& [key, ref] : g_index) {
        State st;
        if (!R::IsLiveByIndex(ref.actor, ref.idx) || !Read(ref.actor, st)) continue;
        auto seen = g_seen.find(key);
        if (seen != g_seen.end() && st.fuel > seen->second + kPourMin) {
            Payload p{};
            coop::net::WireKeyFromString(key, p.key);
            p.op = coop::net::kWoodchipperOpPour;
            p.fuel = std::fmin(st.fuel - seen->second, kMaxFuel);
            Queue(p);
            ++g_clientPours;
            UE_LOGI("woodchipper: this player poured %.1f into '%ls' -- the host fuels it", p.fuel, key.c_str());
        }
        g_seen[key] = st.fuel;
    }
}

void ClientPoll(coop::net::Session& s, Clock::time_point now) {
    if (!coop::net_pump::HasAnnouncedWorldReady()) return;
    DetectPours();
    for (auto it = g_outbox.begin(); it != g_outbox.end();) {
        if (!s.SendReliable(coop::net::ReliableKind::WoodchipperIntent, &*it, sizeof(*it))) break;
        it = g_outbox.erase(it);
    }
    for (auto it = g_hold.begin(); it != g_hold.end();) it = now >= it->second ? g_hold.erase(it) : std::next(it);
    // The host's chippers on the copies.
    for (auto it = g_shown.begin(); it != g_shown.end();) {
        if (g_hold.count(it->first)) {
            ++it;
            continue;
        }
        void* c = Resolve(it->first);
        State cur;
        if (!Read(c, cur)) {
            it = now - it->second.at > kStateLife ? g_shown.erase(it) : std::next(it);
            continue;
        }
        const State& want = it->second.state;
        {
            ApplyScope applying;
            WriteFuel(c, want.fuel);
            if (want.active != cur.active) {
                WriteActive(c, want.active);
                CheckActive(c);
            }
        }
        g_seen[it->first] = want.fuel;
        if (want.active != cur.active)
            UE_LOGI("woodchipper: the host's '%ls' is %s here (%.1f fuel)", it->first.c_str(), want.active ? "on" : "off",
                    want.fuel);
        it = g_shown.erase(it);
    }
}

void RegisterOnce() {
    if (!g_watched) {
        g_watched = sg::WatchName(kRowAction, kTagAction, &OnActionPre, &OnActionPost);
        if (!g_watched) UE_LOGE("woodchipper: the script-body gate refused the watch on actionOptionIndex -- a client's "
                                "press stays its own");
    }
    if (!g_hubRegistered) {
        g_hubRegistered = true;
        coop::element::scan_hub::Register(coop::element::scan_hub::Consumer{
            "woodchipper", nullptr, &EnsureResolved, &IsChipper, &HubPassBegin, &HubMatch, &HubPassComplete,
            /*settleScans*/ 15});
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    RegisterOnce();
}

void Tick() {
    RegisterOnce();
    if (!g_saidLive && g_watched) {
        sg::ResolvePendingNames();
        if (sg::NameWatchLive(kRowAction, kTagAction)) {
            g_saidLive = true;
            UE_LOGI("woodchipper: a chipper's button press is watched");
        }
    }
    coop::net::Session* s = Live();
    if (!s || !EnsureResolved()) return;
    const Clock::time_point now = Clock::now();
    if (!g_poll.Due(now)) return;
    const uint32_t world = ue_wrap::world_identity::Generation();
    if (world != g_world) {
        g_world = world;
        g_sent.clear();
        g_shown.clear();
        g_seen.clear();
        g_outbox.clear();
        g_hold.clear();
        g_intents.clear();
    }
    if (s->role() == coop::net::Role::Host) {
        HostServe(*s);
        HostPoll(*s, now);
    } else if (s->role() == coop::net::Role::Client) {
        ClientPoll(*s, now);
    }
}

void OnState(const coop::net::WoodchipperPayload& p) {
    if (p.op != coop::net::kWoodchipperOpState || p.active > 1 || !ValidFuel(p.fuel)) return;
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    if (key.empty()) return;
    Shown& sh = g_shown[key];
    sh.state.fuel = p.fuel;
    sh.state.active = p.active != 0;
    sh.at = Clock::now();
}

void OnIntent(int senderSlot, const coop::net::WoodchipperPayload& p) {
    if (senderSlot <= 0 || senderSlot >= static_cast<int>(coop::net::kMaxPeers)) return;
    const bool pour = p.op == coop::net::kWoodchipperOpPour && ValidFuel(p.fuel) && p.fuel > 0.f;
    const bool press = p.op == coop::net::kWoodchipperOpButton && p.active <= 1;
    if ((!pour && !press) || g_intents.size() >= kQueueCap) return;
    g_intents.push_back({senderSlot, p});
}

void QueueConnectBroadcastForSlot(int /*slot*/) {
    for (auto& entry : g_sent) entry.second.pending = true;
}

void OnDisconnect() {
    if (g_hostPours || g_hostPresses || g_hostRefused || g_clientPours || g_clientPresses || g_clientDropped)
        UE_LOGI("woodchipper: session end -- host pours=%u presses=%u refused=%u; client pours=%u presses=%u dropped=%u",
                g_hostPours, g_hostPresses, g_hostRefused, g_clientPours, g_clientPresses, g_clientDropped);
    g_hostPours = g_hostPresses = g_hostRefused = g_clientPours = g_clientPresses = g_clientDropped = 0;
    g_sent.clear();
    g_shown.clear();
    g_seen.clear();
    g_outbox.clear();
    g_hold.clear();
    g_intents.clear();
    g_pressChipper = nullptr;
    g_session.store(nullptr);
}

}  // namespace coop::woodchipper_sync
