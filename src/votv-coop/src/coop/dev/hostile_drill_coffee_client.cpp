// coop/dev/hostile_drill_coffee_client.cpp -- the coffee family's sender: see coop/dev/hostile_drill_coffee.h. The
// client records the props of the arm's class it holds and says so in the lobby chat, waits for the host to say it is
// ready, finds the prop the host spawned, then sends raw CoffeeActionIntent payloads through the session's own send,
// bypassing its coffee lane, or (the record arm) sets hasCup on its copy of the maker and sends that record.

#include "hostile_drill_coffee_detail.h"  // co-located private header (src tree, not include/)

#include "coop/dev/drill_chat.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/death_revive.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_save_data.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace coop::dev::hostile_drill::coffee::detail {
namespace {

namespace DC = coop::dev::drill_chat;
namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
using Kind = coop::net::ReliableKind;
using Payload = coop::net::CoffeeActionIntentPayload;

enum class CStep : uint8_t { Run, Done };
CStep    g_step = CStep::Run;
bool     g_idleSaid = false, g_started = false, g_ready = false, g_hit = false, g_killed = false;
Clock::time_point g_since{};
const char* g_waiting = nullptr;
// The props of the arm's class this world held when the client's join ended: the host's is the one outside it.
std::vector<ue_wrap::CachedObjRef> g_start;

void Inconclusive(const Spec& sp, const std::string& why) {
    Verdict(sp.arm, "INCONCLUSIVE", why);
    g_step = CStep::Done;
}

// True once the wait named `what` has run past the failure bound; a different wait restarts the clock.
bool Late(const char* what) {
    if (!g_waiting || std::strcmp(g_waiting, what) != 0) {
        g_waiting = what;
        g_since = Clock::now();
    }
    return Clock::now() - g_since > kFailBound;
}

bool OwnJoinOver() {
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

// ---- the forged messages ----

coop::net::WireKey KeyOf(const wchar_t* text) {
    coop::net::WireKey k{};
    coop::net::WireKeyFromString(text, k);
    return k;
}

// One forged run, counting what it sent; a send that fails stops the run.
struct Out {
    coop::net::Session* s;
    uint32_t            n = 0;
    bool                ok = true;
    void Send(const Payload& p, int len = static_cast<int>(sizeof(Payload))) {
        ok = ok && s->SendReliable(Kind::CoffeeActionIntent, &p, len);
        ++n;
    }
};

// The arm's presses. The malformed ones each break one rule of the form; the others are honest in form, on the host's
// prop.
void SendArm(Out& o, const Spec& sp, const coop::net::WireKey& prop) {
    switch (sp.arm) {
    case Arm::Malformed: {
        Payload p{};
        p.key = KeyOf(L"none");
        o.Send(p, static_cast<int>(sizeof(Payload)) - 1);                 // one byte short
        Payload longKey{};
        std::memset(longKey.key.data, 'x', sizeof(longKey.key.data));
        longKey.key.len = static_cast<uint8_t>(sizeof(longKey.key.data) + 1);   // a key longer than the wire holds
        o.Send(longKey);
        break;
    }
    case Arm::WrongClass:
    case Arm::Reach:
    case Arm::DeadSender: {
        Payload p{};
        p.key = prop;
        o.Send(p);
        break;
    }
    case Arm::Flood:
        for (uint32_t i = 0; i < sp.sent; ++i) {
            Payload p{};
            p.key = prop;
            o.Send(p);
        }
        break;
    default:
        break;
    }
}

// ---- finding the host's prop ----

void RecordStart(const Spec& sp) {
    g_start.clear();
    void* c = ue_wrap::object_index::ClassByName(FixtureClass(sp.fixture));
    if (!c) return;
    ue_wrap::object_index::ForEachInstance(c, [](void*, void* obj, int32_t) {
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
        ue_wrap::CachedObjRef ref;
        ref.Set(obj);
        g_start.push_back(ref);
    }, nullptr);
}

enum class Found : uint8_t { Wait, Ok, Bad };

// The one prop of the arm's class outside the start set, with its key. Wait while it is missing or has no key yet;
// Bad when more than one stands outside the set.
Found FindProp(const Spec& sp, coop::net::WireKey& key, void*& actor) {
    struct Scan { const std::vector<ue_wrap::CachedObjRef>* start; void* hit; int n; } sc{&g_start, nullptr, 0};
    void* c = ue_wrap::object_index::ClassByName(FixtureClass(sp.fixture));
    if (!c) return Found::Wait;
    ue_wrap::object_index::ForEachInstance(c, [](void* ctx, void* obj, int32_t) {
        auto* sn = static_cast<Scan*>(ctx);
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
        for (const ue_wrap::CachedObjRef& old : *sn->start)
            if (old.Is(obj)) return;
        if (!sn->n) sn->hit = obj;
        ++sn->n;
    }, &sc);
    if (sc.n > 1) return Found::Bad;
    if (sc.n < 1) return Found::Wait;
    const std::wstring text = ue_wrap::prop::GetKeyString(sc.hit);
    if (text.empty() || text == L"None") return Found::Wait;
    coop::net::WireKeyFromString(text, key);
    actor = sc.hit;
    return Found::Ok;
}

// The host's ready line, latched on sight from the arm's first tick, whatever the client is doing: a live chat row
// retires after seconds, so a line is read when it lands. Polled a few times a second.
void Observe(const Spec& sp) {
    if (g_ready || !DC::PollDue()) return;
    g_ready = DC::ChatHasLive(ReadyLine(sp.arm));
}

// ---- the death window (Mode::Dead) ----

// A lethal hit lands and the revive answers it only once the world has settled, the revive is armed and the spawn
// protection is over (the gate coop/dev/burn_revive_drill.cpp waits on); a death before that ends the run.
bool DeathArmed(void* local) {
    if (!coop::net_pump::IsWorldSettled() || coop::death_revive::ArmReadyAfterPawnMs() < 0) return false;
    bool invinc = true;
    return E::ReadMainPlayerStartInvinc(local, invinc) && !invinc;
}

bool IsDead(void* local) {
    bool ragdoll = false, dead = false;
    return E::ReadMainPlayerRagdollState(local, ragdoll, dead) && dead;
}

// The client's own player takes the lethal hit once the host's prop stands (the host spawns it beside a living
// puppet), the hit coop/dev/burn_revive_drill.cpp deals. The death is read by AwaitOwnDeath.
void DealLethalHit(const Spec& sp, void* local) {
    coop::net::WireKey unused{};
    void* unusedActor = nullptr;
    switch (FindProp(sp, unused, unusedActor)) {
    case Found::Wait:
        if (Late("the host's prop")) Inconclusive(sp, "the host's prop never appeared, with its key");
        return;
    case Found::Bad:
        Inconclusive(sp, "more props of the arm's class stand outside the start set than the host spawned");
        return;
    case Found::Ok:
        break;
    }
    if (!DeathArmed(local)) {
        if (Late("the revive to arm")) Inconclusive(sp, "the revive never armed, so a death would end the run");
        return;
    }
    float maxHealth = 0.f;
    if (!ue_wrap::vitals::Read(ue_wrap::vitals::Field::MaxHealth, &maxHealth) || maxHealth <= 0.f) {
        Inconclusive(sp, "max health unread");
        return;
    }
    if (!E::InvokeAddPlayerDamage(local, 2.f * maxHealth, /*blood=*/true)) {
        Inconclusive(sp, "the lethal hit did not run");
        return;
    }
    UE_LOGI("[HOSTILE-DRILL] %s client dealt the lethal hit", sp.name);
    g_hit = true;
    g_waiting = nullptr;
}

// After the hit, the player's own death state is read each frame (the ragdoll state's dead flag); the press waits for
// it. True once the death reads; false while waiting, or after an INCONCLUSIVE.
bool AwaitOwnDeath(const Spec& sp, void* local) {
    if (!IsDead(local)) {
        if (Late("the player's death to read")) Inconclusive(sp, "the client's death never registered");
        return false;
    }
    UE_LOGI("[HOSTILE-DRILL] %s client reads its player dead", sp.name);
    g_killed = true;
    g_waiting = nullptr;
    return true;
}

// ---- the forged record ----

// This client's copy of the host's maker takes hasCup, and its record goes to the host as any author's would. False
// when the flag did not resolve or the record was not sent.
bool SendForgedRecord(coop::net::Session* s, void* maker, const coop::net::WireKey& key) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(maker), L"hasCup", off, mask) || off < 0) return false;
    reinterpret_cast<uint8_t*>(maker)[off] |= mask;
    return coop::prop_save_data::Publish(s, maker, coop::net::StringFromWireKey(key));
}

}  // namespace

void ClientTick(coop::net::Session* s, const Spec& sp) {
    if (g_step == CStep::Done) return;
    Observe(sp);
    if (!OwnJoinOver()) return;
    const uint8_t self = coop::players::Registry::Get().LocalPeerId();
    if (!coop::players::Registry::Get().Local() || self == 0 || self >= coop::players::kMaxPeers) return;
    if (self != kSlot) {
        if (!g_idleSaid) {
            g_idleSaid = true;
            UE_LOGI("[HOSTILE-DRILL] %s idles: this client is slot %u, the family acts on slot %u", sp.name,
                    static_cast<unsigned>(self), static_cast<unsigned>(kSlot));
        }
        g_step = CStep::Done;
        return;
    }
    if (!g_started) {
        RecordStart(sp);
        g_started = true;
        // The host spawns the prop only after this line: one born before the set is recorded would sit in it.
        DC::Say(ClientReadyLine(sp.arm));
    }
    void* const local = coop::players::Registry::Get().Local();
    if (sp.mode == Mode::Dead && !g_hit) {
        DealLethalHit(sp, local);
        return;
    }
    if (sp.mode == Mode::Dead && !g_killed && !AwaitOwnDeath(sp, local)) return;
    // The press goes out only when this player reads dead and the host has said it saw the sender dead.
    if (!g_ready) {
        if (Late("the host's ready line"))
            Inconclusive(sp, sp.mode == Mode::Dead ? "the host never saw the sender dead" : "the host never said it was ready");
        return;
    }
    // The press must land in the death: the host judges it refused as dead, and an alive sender is another outcome.
    if (sp.mode == Mode::Dead && !IsDead(local)) {
        Inconclusive(sp, "the death window closed before the press was sent");
        return;
    }
    coop::net::WireKey prop{};
    void* actor = nullptr;
    if (sp.fixture != Fixture::None) {
        switch (FindProp(sp, prop, actor)) {
        case Found::Wait:
            if (Late("the host's prop")) Inconclusive(sp, "the host's prop never appeared, with its key");
            return;
        case Found::Bad:
            Inconclusive(sp, "more props of the arm's class stand outside the start set than the host spawned");
            return;
        case Found::Ok:
            break;
        }
    }
    bool ok = true;
    uint32_t sent = 0;
    if (sp.arm == Arm::ForgedRecord) {
        ok = SendForgedRecord(s, actor, prop);
        sent = 1;
    } else {
        Out o{s};
        SendArm(o, sp, prop);
        ok = o.ok;
        sent = o.n;
    }
    if (!ok) {
        Verdict(sp.arm, "FAIL", "client: a forged message was not sent");
        g_step = CStep::Done;
        return;
    }
    UE_LOGI("[HOSTILE-DRILL] %s sent %u messages", sp.name, static_cast<unsigned>(sent));
    g_waiting = nullptr;
    g_step = CStep::Done;
}

void ClientReset() {
    g_step = CStep::Run;
    g_idleSaid = g_started = g_ready = g_hit = g_killed = false;
    g_since = {};
    g_waiting = nullptr;
    g_start.clear();
}

}  // namespace coop::dev::hostile_drill::coffee::detail
