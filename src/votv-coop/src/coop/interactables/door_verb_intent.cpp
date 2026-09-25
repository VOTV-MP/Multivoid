// coop/interactables/door_verb_intent.cpp -- see coop/interactables/door_verb_intent.h.

#include "coop/interactables/door_verb_intent.h"

#include "coop/element/intent_authority.h"
#include "coop/interactables/interactable_sync.h"  // the door lane's key for a door, and its resolve
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/door.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <string>

namespace coop::door_verb_intent {
namespace {

namespace D  = ue_wrap::door;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace V  = coop::net::door_verb;

// The entry verbs, watched by name: every class's function of that name fires, so the callback
// gates on the door class before anything else. The tag says which verb fired.
struct VerbWatch { const wchar_t* name; int tag; uint8_t verb; const char* what; };
constexpr VerbWatch kWatches[] = {
    { L"actionOptionIndex", 0x44565052 /*'DVPR'*/, V::kPress, "press" },
    { L"addDamage",         0x44564854 /*'DVHT'*/, V::kHit,   "hit" },
    { L"crowbarOpen",       0x44565059 /*'DVPY'*/, V::kPry,   "pry" },
};
constexpr int kWatchCount = static_cast<int>(sizeof(kWatches) / sizeof(kWatches[0]));

// The reach a sender's verb is judged against. A player's press and hit reach as far as the look-at
// trace, whose length is mainPlayer.armLength: 200 by default, replaced by the held weapon's own
// length from list_weapons, a table not read here. Twice the default is the bound: it refuses a
// door the sender cannot be at, never a reach the game itself would allow a held weapon.
constexpr float kDoorReachUU = 400.0f;

// A sender's verbs run from a bounded queue at a bounded rate: a melee swing is a few hits a
// second and a press is rarer, so the bound is only felt by a stalled or flooding client.
constexpr float    kVerbBurst     = 4.0f;
constexpr float    kVerbPerSecond = 4.0f;
constexpr size_t   kMaxPending    = 8;
constexpr uint64_t kRefusalSayMs  = 10000;

std::atomic<coop::net::Session*> g_session{nullptr};
// Each watch registers once: a refusal (a full gate table) is final, and said once. The set settles
// once every watch is live, or once no name is left to resolve and the rest are dead for good.
enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg[kWatchCount] = {};
bool g_settled = false;

uint64_t g_sent = 0, g_ran = 0, g_denied = 0, g_worldRefused = 0;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Bucket {
    float    tokens = kVerbBurst;
    uint64_t lastMs = 0;
    uint64_t nextSayMs = 0;
};
Bucket g_rate[coop::net::kMaxPeers];
std::deque<coop::net::DoorVerbIntentPayload> g_pending[coop::net::kMaxPeers];

bool TakeToken(uint8_t slot) {
    Bucket& b = g_rate[slot];
    const uint64_t now = NowMs();
    if (b.lastMs != 0 && now > b.lastMs) {
        b.tokens += kVerbPerSecond * static_cast<float>(now - b.lastMs) / 1000.0f;
        if (b.tokens > kVerbBurst) b.tokens = kVerbBurst;
    }
    b.lastMs = now;
    if (b.tokens < 1.0f) return false;
    b.tokens -= 1.0f;
    return true;
}

const char* VerbName(uint8_t verb) {
    for (const VerbWatch& w : kWatches)
        if (w.verb == verb) return w.what;
    return "?";
}

// ---- host side -------------------------------------------------------------------------------
void Execute(coop::net::Session& s, const coop::net::DoorVerbIntentPayload& p, uint8_t slot) {
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    void* door = key.empty() ? nullptr : coop::interactable_sync::ResolveDoor(key);
    if (!door) {
        ++g_denied;
        UE_LOGI("[DOOR-VERB] DENY slot=%u %s key='%ls' -- this door lane indexes no door by that key",
                static_cast<unsigned>(slot), VerbName(p.verb), key.c_str());
        return;
    }
    const coop::element::IntentSubject subject =
        coop::element::IntentTarget::ForClientIntent(s, slot, kDoorReachUU).Authorize(door);
    if (!subject) {
        ++g_denied;
        UE_LOGI("[DOOR-VERB] DENY slot=%u %s key='%ls' -- %s (%.0f uu of %.0f)",
                static_cast<unsigned>(slot), VerbName(p.verb), key.c_str(),
                coop::element::OutcomeName(subject.outcome), subject.distUU, subject.reachUU);
        return;
    }
    // The body Authorize just measured: the sender's puppet, passed as the presser and the hitter.
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
    void* body = (rp && rp->valid()) ? rp->GetActor() : nullptr;

    bool before = false, after = false;
    const bool readBefore = D::TryReadOpenIntent(door, before);
    bool dispatched = false;
    switch (p.verb) {
    case V::kPress: dispatched = D::CallPress(door, body, p.action); break;
    case V::kHit:   dispatched = D::CallHit(door, body, p.damage); break;
    case V::kPry:   dispatched = D::CallCrowbarOpen(door); break;
    default: break;  // the dispatcher refuses any other verb
    }
    const bool readAfter = D::TryReadOpenIntent(door, after);
    if (dispatched) ++g_ran; else ++g_denied;
    // The door's own body owns the outcome, so a press on an unpowered door or a hit below the pry
    // leaves the state as it was; the line says which, and the door's state verbs send any change.
    UE_LOGI("[DOOR-VERB] host ran slot %u's %s on key='%ls': dispatched=%d damage=%.1f, open %s -> %s",
            static_cast<unsigned>(slot), VerbName(p.verb), key.c_str(), dispatched ? 1 : 0, p.damage,
            readBefore ? (before ? "1" : "0") : "(unread)", readAfter ? (after ? "1" : "0") : "(unread)");
}

// ---- the client's gate -----------------------------------------------------------------------
// The parameter offsets of the function that fired, per verb; a door's verbs are one function
// each (no door subclass overrides them), so each resolves once.
struct ParamOffsets { void* fn = nullptr; int32_t first = -1; int32_t second = -1; };
ParamOffsets g_params[kWatchCount];

const ParamOffsets& OffsetsOf(const sg::Call& call, int watchIndex, uint8_t verb) {
    ParamOffsets& o = g_params[watchIndex];
    if (o.fn == call.function) return o;
    o.fn = call.function;
    o.first = o.second = -1;
    if (verb == V::kPress) {
        o.first = R::FindParamOffset(call.function, L"action");
    } else if (verb == V::kHit) {
        o.first  = R::FindParamOffset(call.function, L"actor");
        o.second = R::FindParamOffset(call.function, L"damage");
    }
    if ((verb == V::kPress && o.first < 0) || (verb == V::kHit && (o.first < 0 || o.second < 0)))
        UE_LOGW("[DOOR-VERB] the %s verb's parameters did not resolve (first=%d second=%d)",
                VerbName(verb), o.first, o.second);
    return o;
}

sg::Verdict OnVerbPre(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return sg::Verdict::Run;
    if (!call.object || !D::IsDoor(call.object)) return sg::Verdict::Run;
    int wi = -1;
    for (int i = 0; i < kWatchCount; ++i)
        if (kWatches[i].tag == call.tag) { wi = i; break; }
    if (wi < 0) return sg::Verdict::Run;
    const uint8_t verb = kWatches[wi].verb;

    // A door the lane does not index has no name the host could resolve, so its verbs stay native;
    // said, rate-limited, since to the player it is a door that moves on this screen alone.
    const std::wstring key = coop::interactable_sync::DoorKey(call.object);
    if (key.empty()) {
        static uint64_t s_nextSayMs = 0;
        const uint64_t now = NowMs();
        if (now >= s_nextSayMs) {
            s_nextSayMs = now + 3000;
            UE_LOGW("[DOOR-VERB] a %s on door %p, which the door lane does not index -- it runs here "
                    "alone (raw game key='%ls')", VerbName(verb), call.object,
                    D::GetKeyString(call.object).c_str());
        }
        return sg::Verdict::Run;
    }

    coop::net::DoorVerbIntentPayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.verb = verb;
    const ParamOffsets& o = OffsetsOf(call, wi, verb);
    if (verb == V::kPress) {
        if (o.first >= 0) p.action = *reinterpret_cast<const uint8_t*>(call.locals + o.first);
    } else if (verb == V::kHit) {
        if (o.first < 0 || o.second < 0) return sg::Verdict::Cancel;  // unreadable: said once above
        void* actor = *reinterpret_cast<void* const*>(call.locals + o.first);
        // Only this peer's own player authors a hit here. Everything else that damages a door -- a
        // creature, an explosion, the cheat menu -- is the world's, and the host's world runs it on
        // its own copy; running it here too would move this copy alone.
        if (!coop::players::Registry::Get().IsLocal(actor)) {
            ++g_worldRefused;
            return sg::Verdict::Cancel;
        }
        p.damage = *reinterpret_cast<const float*>(call.locals + o.second);
    }
    if (!s->SendReliable(coop::net::ReliableKind::DoorVerbIntent, &p, sizeof(p))) {
        UE_LOGW("[DOOR-VERB] the %s on key='%ls' was not sent (the session refused it); the door "
                "stays as the host has it", VerbName(verb), key.c_str());
        return sg::Verdict::Cancel;
    }
    ++g_sent;
    if (verb != V::kHit || g_sent <= 3 || g_sent % 20 == 0)
        UE_LOGI("[DOOR-VERB] CLIENT SENT a %s on key='%ls' (#%llu)", VerbName(verb), key.c_str(),
                static_cast<unsigned long long>(g_sent));
    return sg::Verdict::Cancel;
}

void RegisterWatches() {
    for (int i = 0; i < kWatchCount; ++i) {
        if (g_reg[i] != Reg::Pending) continue;
        if (sg::WatchName(kWatches[i].name, kWatches[i].tag, &OnVerbPre, nullptr)) {
            g_reg[i] = Reg::Registered;
            continue;
        }
        g_reg[i] = Reg::Refused;
        UE_LOGE("[DOOR-VERB] the script-body gate refused the watch on '%ls' -- for the rest of this process "
                "that door verb runs only where it is used", kWatches[i].name);
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    RegisterWatches();
}

void Tick(coop::net::Session& session) {
    sg::ResolvePendingNames();
    if (!g_settled) {
        int live = 0, pending = 0;
        for (int i = 0; i < kWatchCount; ++i) {
            if (g_reg[i] == Reg::Pending) ++pending;
            else if (g_reg[i] == Reg::Registered && sg::NameWatchLive(kWatches[i].name, kWatches[i].tag)) ++live;
        }
        if (live == kWatchCount) {
            g_settled = true;
            UE_LOGI("[DOOR-VERB] the door verb gates are live: press, hit and pry");
        } else if (pending == 0 && sg::PendingNameCount() == 0) {
            g_settled = true;
            UE_LOGE("[DOOR-VERB] %d of %d door verb gates are dead (refused, or resolved into a full table) -- "
                    "those verbs run only where they are used", kWatchCount - live, kWatchCount);
        }
    }
    if (!session.running() || session.role() != coop::net::Role::Host) return;
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        if (g_pending[slot].empty() || !TakeToken(slot)) continue;
        const coop::net::DoorVerbIntentPayload p = g_pending[slot].front();
        g_pending[slot].pop_front();
        Execute(session, p, slot);
    }
}

void OnDoorVerbIntent(coop::net::Session& session, const coop::net::DoorVerbIntentPayload& payload,
                      uint8_t senderSlot) {
    if (session.role() != coop::net::Role::Host) return;
    if (senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    auto& q = g_pending[senderSlot];
    if (q.size() >= kMaxPending) {
        Bucket& b = g_rate[senderSlot];
        const uint64_t now = NowMs();
        if (now >= b.nextSayMs) {
            b.nextSayMs = now + kRefusalSayMs;
            UE_LOGW("[DOOR-VERB] slot %u queue full (%zu) -- refusing its door verbs until it drains",
                    static_cast<unsigned>(senderSlot), q.size());
        }
        ++g_denied;
        return;
    }
    q.push_back(payload);
}

void OnPeerLeft(uint8_t slot) {
    if (slot >= coop::net::kMaxPeers) return;
    g_pending[slot].clear();
    g_rate[slot] = Bucket{};
}

void OnDisconnect() {
    if (g_sent || g_ran || g_denied || g_worldRefused)
        UE_LOGI("[DOOR-VERB] session end -- sent=%llu ran=%llu denied=%llu world-damage refused=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_ran),
                static_cast<unsigned long long>(g_denied),
                static_cast<unsigned long long>(g_worldRefused));
    for (uint8_t slot = 0; slot < coop::net::kMaxPeers; ++slot) {
        g_pending[slot].clear();
        g_rate[slot] = Bucket{};
    }
    g_sent = g_ran = g_denied = g_worldRefused = 0;
}

}  // namespace coop::door_verb_intent
