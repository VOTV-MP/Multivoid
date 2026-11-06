// coop/props/prop_fire_sync.cpp -- see coop/props/prop_fire_sync.h.

#include "coop/props/prop_fire_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_key_index.h"
#include "coop/session/net_pump.h"
#include "coop/session/poll_timer.h"
#include "coop/world/salt_rocket_ignite.h"
#include "coop/world/spawn_authority.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_fire.h"
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
#include <vector>

namespace coop::prop_fire_sync {
namespace {

namespace F = ue_wrap::prop;
namespace R = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
using Clock = std::chrono::steady_clock;
using Payload = coop::net::PropFirePayload;

UE_FACT_FN(kRowIgnite, "prop_fire", nullptr, L"ignite", AnyClass);
constexpr int kTagIgnite = 0x50464947;  // 'PFIG'

constexpr auto kResend = std::chrono::seconds(2);       // a burning prop's flame again, as its fuel runs down
constexpr auto kStateLife = std::chrono::seconds(10);   // a client's unplaced flame is dropped after this
constexpr float kMaxFuel = 60.f;
constexpr float kReachCm = 800.f;                       // a light's sender from its prop, on the host
constexpr size_t kRequestCap = 32;

std::atomic<coop::net::Session*> g_session{nullptr};
coop::session::PollTimer g_poll{std::chrono::milliseconds(250)};
uint32_t g_world = 0;
bool g_registered = false;
int g_applying = 0;   // our own ignite and extinguish on a client run inside this

struct ApplyScope {
    ApplyScope() { ++g_applying; }
    ~ApplyScope() { --g_applying; }
};

// HOST: what each burning prop last told the clients.
struct Sent {
    F::FireState state;
    Clock::time_point at{};
    bool pending = false;
};
std::unordered_map<std::wstring, Sent> g_sent;

struct Request {
    int slot = 0;
    Payload p{};
};
std::vector<Request> g_requests;

// CLIENT: the host's flame per prop, until it is shown; the lights waiting to be sent.
struct Shown {
    F::FireState state;
    Clock::time_point at{};
    bool applied = false;
};
std::unordered_map<std::wstring, Shown> g_shown;
std::vector<Payload> g_outbox;

uint32_t g_hostLit = 0, g_clientSent = 0, g_clientDropped = 0;

coop::net::Session* Live() {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    return s && s->connected() ? s : nullptr;
}

// The key a carried prop answers to, or empty: a keyed prop of this world, not a rocket, not in a hand.
std::wstring KeyOf(void* actor) {
    if (!actor || !R::IsLive(actor) || !F::IsDescendantOfProp(actor)) return {};
    if (coop::salt_rocket_ignite::IsSaltRocket(actor) || coop::hand_item::IsHandAxisActor(actor)) return {};
    std::wstring key = F::GetInteractableKeyString(actor);
    if (key.empty() || key == L"None" || key.size() >= sizeof(coop::net::WireKey::data)) return {};
    return key;
}

bool Same(const F::FireState& a, const F::FireState& b) {
    return a.burning == b.burning && std::fabs(a.fuelSpeed - b.fuelSpeed) < 0.01f && b.fuel <= a.fuel + 1.f;
}

bool ValidState(const Payload& p) {
    return p.burning <= 1 && std::isfinite(p.fuel) && std::isfinite(p.fuelSpeed) && p.fuel >= 0.f && p.fuel <= kMaxFuel &&
           p.fuelSpeed >= 0.f && p.fuelSpeed <= 100.f;
}

// CLIENT: a prop's ignite, refused on the copy. The player's own light goes to the host.
sg::Verdict OnIgnitePre(const sg::Call& c) {
    if (!coop::spawn_authority::IsActiveClientSession() || g_applying) return sg::Verdict::Run;
    const std::wstring key = KeyOf(c.object);
    if (key.empty()) return sg::Verdict::Run;
    if (!coop::salt_rocket_ignite::LocalLightRunning()) return sg::Verdict::Cancel;   // the host rolls a spread
    const int32_t off = c.function ? R::FindParamOffset(c.function, L"fuel") : -1;
    float fuel = 0.f;
    if (off < 0 || !c.locals) return sg::Verdict::Cancel;
    std::memcpy(&fuel, c.locals + off, sizeof(fuel));
    if (!std::isfinite(fuel) || fuel <= 0.f) return sg::Verdict::Cancel;
    if (g_outbox.size() >= kRequestCap) {
        ++g_clientDropped;
        return sg::Verdict::Cancel;
    }
    Payload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.fuel = fuel > kMaxFuel ? kMaxFuel : fuel;
    p.burning = 1;
    g_outbox.push_back(p);
    UE_LOGI("prop_fire: this player lit '%ls' (fuel %.1f) -- the host lights it", key.c_str(), p.fuel);
    return sg::Verdict::Cancel;
}

void HostServe(coop::net::Session& s) {
    std::vector<Request> requests;
    requests.swap(g_requests);
    for (const Request& r : requests) {
        const std::wstring key = coop::net::StringFromWireKey(r.p.key);
        if (!s.IsSlotWorldReady(r.slot)) continue;
        void* prop = coop::prop_key_index::Find(key);
        if (KeyOf(prop) != key) continue;
        coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(r.slot));
        void* body = rp && rp->valid() ? rp->GetActor() : nullptr;
        ue_wrap::FVector at{}, by{};
        if (!body || !ue_wrap::engine::TryGetActorLocation(prop, at) || !ue_wrap::engine::TryGetActorLocation(body, by)) continue;
        const float dx = at.X - by.X, dy = at.Y - by.Y, dz = at.Z - by.Z;
        if (dx * dx + dy * dy + dz * dz > kReachCm * kReachCm) {
            UE_LOGW("prop_fire: slot %d lit '%ls' out of reach -- refused", r.slot, key.c_str());
            continue;
        }
        if (F::CallIgnite(prop, r.p.fuel)) {
            ++g_hostLit;
            UE_LOGI("prop_fire: slot %d lit '%ls' (fuel %.1f) on the host (#%u)", r.slot, key.c_str(), r.p.fuel, g_hostLit);
            g_sent[key].pending = true;
        }
    }
}

void HostPoll(coop::net::Session& s, Clock::time_point now) {
    std::vector<void*> burning;
    F::ListBurningProps(burning);
    std::unordered_map<std::wstring, void*> lit;
    for (void* a : burning) {
        std::wstring key = KeyOf(a);
        if (!key.empty()) lit[key] = a;
    }
    for (const auto& [key, actor] : lit) {
        F::FireState state;
        if (!F::ReadPropFire(actor, state) || !state.burning) continue;
        Sent& sent = g_sent[key];
        const bool due = sent.pending || !sent.state.burning || !Same(sent.state, state) || now - sent.at >= kResend;
        if (!due) continue;
        Payload p{};
        coop::net::WireKeyFromString(key, p.key);
        p.burning = 1;
        p.fuel = state.fuel > kMaxFuel ? kMaxFuel : state.fuel;
        p.fuelSpeed = state.fuelSpeed;
        if (!s.SendReliable(coop::net::ReliableKind::PropFireState, &p, sizeof(p))) continue;
        if (!sent.state.burning) UE_LOGI("prop_fire: '%ls' burns (fuel %.1f, speed %.2f)", key.c_str(), p.fuel, p.fuelSpeed);
        sent.state = state;
        sent.at = now;
        sent.pending = false;
    }
    for (auto it = g_sent.begin(); it != g_sent.end();) {
        if (lit.count(it->first)) {
            ++it;
            continue;
        }
        if (it->second.state.burning) {
            Payload p{};
            coop::net::WireKeyFromString(it->first, p.key);
            if (!s.SendReliable(coop::net::ReliableKind::PropFireState, &p, sizeof(p))) {
                ++it;
                continue;
            }
            UE_LOGI("prop_fire: '%ls' went out", it->first.c_str());
        }
        it = g_sent.erase(it);
    }
}

void ClientPoll(coop::net::Session& s, Clock::time_point now) {
    if (!coop::net_pump::HasAnnouncedWorldReady()) return;
    for (auto it = g_outbox.begin(); it != g_outbox.end();) {
        if (!s.SendReliable(coop::net::ReliableKind::PropFireRequest, &*it, sizeof(*it))) break;
        ++g_clientSent;
        it = g_outbox.erase(it);
    }
    for (auto it = g_shown.begin(); it != g_shown.end();) {
        Shown& sh = it->second;
        if (sh.applied) {
            it = sh.state.burning ? std::next(it) : g_shown.erase(it);
            continue;
        }
        void* prop = coop::prop_key_index::Find(it->first);
        if (KeyOf(prop) != it->first) {
            it = now - sh.at > kStateLife ? g_shown.erase(it) : std::next(it);
            continue;
        }
        bool ok = false;
        {
            ApplyScope applying;
            ok = F::ApplyPropFire(prop, sh.state);
        }
        if (ok) {
            sh.applied = true;
            UE_LOGI("prop_fire: the host's '%ls' %s here", it->first.c_str(), sh.state.burning ? "burns" : "went out");
        }
        ++it;
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_registered) return;
    g_registered = true;
    if (!sg::WatchName(kRowIgnite, kTagIgnite, &OnIgnitePre, nullptr))
        UE_LOGE("prop_fire: the script-body gate refused the watch on ignite -- a client's prop fire stays its own");
}

void Tick() {
    coop::net::Session* s = Live();
    if (!s) return;
    const Clock::time_point now = Clock::now();
    if (!g_poll.Due(now)) return;
    const uint32_t world = ue_wrap::world_identity::Generation();
    if (world != g_world) {
        g_world = world;
        g_sent.clear();
        g_shown.clear();
        g_outbox.clear();
        g_requests.clear();
    }
    if (s->role() == coop::net::Role::Host) {
        HostServe(*s);
        HostPoll(*s, now);
    } else if (s->role() == coop::net::Role::Client) {
        ClientPoll(*s, now);
    }
}

void OnState(const coop::net::PropFirePayload& p) {
    if (!ValidState(p)) return;
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    if (key.empty()) return;
    Shown& sh = g_shown[key];
    sh.state.burning = p.burning != 0;
    sh.state.fuel = p.fuel;
    sh.state.fuelSpeed = p.fuelSpeed;
    sh.at = Clock::now();
    sh.applied = false;
}

void OnRequest(int senderSlot, const coop::net::PropFirePayload& p) {
    if (senderSlot <= 0 || senderSlot >= static_cast<int>(coop::net::kMaxPeers)) return;
    if (!std::isfinite(p.fuel) || p.fuel <= 0.f || p.fuel > kMaxFuel || p.burning != 1) return;
    if (g_requests.size() >= kRequestCap) return;
    g_requests.push_back({senderSlot, p});
}

void QueueConnectBroadcastForSlot(int /*slot*/) {
    for (auto& entry : g_sent) entry.second.pending = true;
}

void OnDisconnect() {
    if (g_hostLit || g_clientSent || g_clientDropped)
        UE_LOGI("prop_fire: session end -- lit for clients=%u lights sent=%u dropped=%u", g_hostLit, g_clientSent,
                g_clientDropped);
    g_hostLit = g_clientSent = g_clientDropped = 0;
    g_sent.clear();
    g_shown.clear();
    g_outbox.clear();
    g_requests.clear();
    g_session.store(nullptr);
}

}  // namespace coop::prop_fire_sync
