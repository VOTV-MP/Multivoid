// coop/world/explosion_request_client.cpp -- the client's half of the explosion request lane: see
// coop/world/explosion_request.h. A client's launcher fire bodies are watched to tell a rocket or sticky bomb born
// inside one (the player's shot) from one the world made; each such shot is taken when its body ends and sent then,
// or after the player's own hand announce of its launcher (the outbox), and the dissolved birth of an explosion its
// own player caused is sent as a request that names it, behind its shot if that still waits. Game thread.

#include "coop/world/explosion_request.h"

#include "explosion_request_internal.h"  // co-located private header (src tree, not include/)

#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_echo_suppress.h"
#include "coop/world/explosion_recipe.h"
#include "coop/world/spawn_authority.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/explosion_source.h"

#include <cstdint>
#include <cwchar>

namespace coop::explosion_request {
namespace {

namespace RC = coop::explosion_recipe;
namespace XS = ue_wrap::explosion_source;
namespace SG = ue_wrap::script_gate;
namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
using ue_wrap::FVector;

UE_FACT_FN(kRowFireRpg,    "explosion_request", L"prop_funGun_rpg_C",            L"fireBulletPrimary", Declared);
UE_FACT_FN(kRowFireSticky, "explosion_request", L"prop_funGun_stickyLauncher_C", L"fireBulletPrimary", Declared);
UE_FACT_FN(kRowFireWitch,  "explosion_request", L"car1_witch_C", L"InpActEvt_LeftMouseButton_K2Node_InputKeyEvent_0", Declared);

constexpr int kTagFire = 0x45584652;  // 'EXFR'

// The launchers are RC::Launcher's values 1..3; their arrays are indexed by one less.
constexpr int kLaunchers = 3;
const ue_wrap::facts::Function* const kFireRows[kLaunchers] = {&kRowFireRpg, &kRowFireSticky, &kRowFireWitch};

// The install runs off the pump every tick, so a watch that is refused is retried on a one-second throttle, at
// most 30 tries, then said once; never a per-tick retry.
constexpr uint64_t kResolveRetryMs  = 1000;
constexpr int      kMaxResolveTries = 30;

struct FireWatch {
    bool registered = false;
    bool live       = false;
    bool gaveUp     = false;
    int  tries      = 0;
};
FireWatch g_watch[kLaunchers];
uint64_t  g_nextTryMs = 0;
bool      g_listenerTried = false;

// Each launcher's fire body, as its watch's PRE saw it.
ue_wrap::CachedObjRef g_body[kLaunchers];

// A rocket or sticky bomb born inside a launcher's fire body, kept by its object index: an actor still inside its
// own spawn may not yet read as alive. It lives until that body's post sends it or the same launcher's next pre
// drops it (a world change or a disconnect clears it too), so it carries no serial.
struct Pending {
    void*   actor;
    int32_t index;
    int     launcher;
    uint8_t projClass;
};
constexpr size_t kPendingCap = 16;
Pending g_pending[kPendingCap];
size_t  g_pendingCount = 0;

// A shot sent, until the blast that names it: the projectile by pointer, index and slot serial.
struct SentShot {
    void*   actor;
    int32_t index;
    int32_t serial;
    uint32_t shotId;
    uint8_t projClass;
};
constexpr size_t kShotMapCap = 64;
SentShot g_shots[kShotMapCap];
size_t   g_shotCount = 0;

uint32_t g_nextShotId = 1;

// The one-shot logs of a session: a pending shot whose projectile died, one left by a body that ended without its
// post, a projectile of this client's own that exploded with no shot sent, and (per message type) a message dropped
// unsent.
enum class Msg : int { Shot, Request };
bool g_deadLogged     = false;
bool g_orphanLogged   = false;
bool g_unmappedLogged = false;
bool g_dropLogged[2]  = {false, false};
bool g_notAnnouncedLogged = false;

void EraseShot(size_t i) {
    for (size_t j = i + 1; j < g_shotCount; ++j) g_shots[j - 1] = g_shots[j];
    --g_shotCount;
}

// The object index announced a death: the object may be freed, so only its pointer and index are compared.
void OnDeleted(void*, void* obj, void*, int32_t idx) {
    for (size_t i = 0; i < g_shotCount; ++i) {
        if (g_shots[i].actor == obj && g_shots[i].index == idx) {
            EraseShot(i);
            return;
        }
    }
}

void AddShot(const SentShot& s) {
    if (g_shotCount == kShotMapCap) EraseShot(0);
    g_shots[g_shotCount++] = s;
}

// The entry of `actor`, whose slot still holds the serial it was sent under; -1 for none.
int FindShot(void* actor) {
    for (size_t i = 0; i < g_shotCount; ++i)
        if (g_shots[i].actor == actor && R::SlotSerial(g_shots[i].index) == g_shots[i].serial)
            return static_cast<int>(i);
    return -1;
}

// A message that could not leave, never retried: counted every time, said once a session per message type.
void NoteDropped(Msg m, const char* why) {
    ++detail::MutCounters().clientDropped;
    bool& logged = g_dropLogged[static_cast<int>(m)];
    if (logged) return;
    logged = true;
    UE_LOGW("[EXPLOSION] %s was dropped unsent: %s", m == Msg::Shot ? "a shot" : "a request", why);
}

const char* ProjName(uint8_t projClass) {
    return projClass == static_cast<uint8_t>(RC::ProjClass::Rocket) ? "rocket" : "sticky";
}

// The outbox, the client's messages in the order they go on the one ordered lane. A shot of a held launcher waits
// here until the player's own hand announce of that launcher has gone out, so the host has the hand before the
// shot; a request that names a waiting shot queues behind it, any other request goes at once. Head first.
struct OutItem {
    bool                               shot;
    coop::net::ProjectileFiredPayload  fired;
    coop::net::ExplosionRequestPayload request;
    RC::RequestKind                    kind;  // of `request`, so the counter slot never comes from the wire byte
    uint32_t                           lookSeq;  // a shot's: the hand lane's look count at the shot
};
constexpr size_t kOutboxCap = 16;
OutItem g_outbox[kOutboxCap];
size_t  g_outboxCount = 0;

uint32_t OutShotId(const OutItem& o) { return o.shot ? o.fired.shotId : o.request.shotId; }

bool ShotQueued(uint32_t shotId) {
    for (size_t i = 0; i < g_outboxCount; ++i)
        if (g_outbox[i].shot && g_outbox[i].fired.shotId == shotId) return true;
    return false;
}

// Every outbox entry that names `shotId` (the shot and its queued requests) goes, and the shot's map entry with
// them; returns how many outbox entries went.
size_t DropQueued(uint32_t shotId) {
    size_t kept = 0;
    size_t gone = 0;
    for (size_t i = 0; i < g_outboxCount; ++i) {
        if (OutShotId(g_outbox[i]) == shotId)
            ++gone;
        else
            g_outbox[kept++] = g_outbox[i];
    }
    g_outboxCount = kept;
    for (size_t i = 0; i < g_shotCount; ++i) {
        if (g_shots[i].shotId == shotId) {
            EraseShot(i);
            break;
        }
    }
    return gone;
}

void PopHead() {
    for (size_t j = 1; j < g_outboxCount; ++j) g_outbox[j - 1] = g_outbox[j];
    --g_outboxCount;
}

// At the cap the oldest shot goes with its requests, counted as dropped unsent; a request whose shot went is dropped.
void Enqueue(const OutItem& item) {
    if (g_outboxCount == kOutboxCap) {
        const size_t gone = DropQueued(OutShotId(g_outbox[0]));
        for (size_t i = 0; i < gone; ++i) NoteDropped(Msg::Shot, "the outbox was full");
    }
    if (!item.shot && !ShotQueued(item.request.shotId))
        return NoteDropped(Msg::Request, "its shot left the full outbox");
    g_outbox[g_outboxCount++] = item;
}

// A shot dropped because the hand lane looked at the hand after it and did not announce its launcher: it and its
// queued requests, counted.
void DropNotAnnounced(uint32_t shotId) {
    detail::MutCounters().shotNotAnnounced += DropQueued(shotId);
    if (g_notAnnouncedLogged) return;
    g_notAnnouncedLogged = true;
    UE_LOGW("[EXPLOSION] shot dropped: the hand lane looked after the shot and did not announce its launcher");
}

// One shot on the wire; false, counted, when the session took none.
bool TransmitShot(coop::net::Session& s, const coop::net::ProjectileFiredPayload& p) {
    if (!s.SendReliable(coop::net::ReliableKind::ProjectileFired, &p, sizeof(p))) {
        NoteDropped(Msg::Shot, "the session took no reliable message");
        return false;
    }
    ++detail::MutCounters().shotsSent;
    UE_LOGI("[EXPLOSION] shot sent id=%u class=%s launcher=%d", p.shotId, ProjName(p.projClass), p.launcher);
    return true;
}

// The launcher's shot, taken once its body has finished the projectile's spawn: its place is read here, while the
// projectile is live, and its shot-map entry and id are made here. The rpg's and the sticky launcher's shot goes
// out at once if the player's announce of that launcher has, else it waits in the outbox; the witch car has no hand.
void SendShot(coop::net::Session& s, const Pending& e) {
    if (!R::IsLiveByIndex(e.actor, e.index)) {
        ++detail::MutCounters().pendingDead;
        if (!g_deadLogged) {
            g_deadLogged = true;
            UE_LOGW("[EXPLOSION] shot dropped: its projectile died inside the fire body");
        }
        return;
    }
    FVector loc{};
    if (!E::TryGetActorLocation(e.actor, loc)) return NoteDropped(Msg::Shot, "the projectile's location did not read");
    coop::net::ProjectileFiredPayload p{};
    p.projClass = e.projClass;
    p.launcher  = static_cast<uint8_t>(e.launcher);
    p.shotId    = g_nextShotId;
    p.origin[0] = loc.X;
    p.origin[1] = loc.Y;
    p.origin[2] = loc.Z;
    const bool held = e.launcher != static_cast<int>(RC::Launcher::WitchCar) &&
                      !coop::hand_item::OwnAnnouncedA(RC::LauncherClassName(p.launcher));
    if (!held && !TransmitShot(s, p)) return;
    ++g_nextShotId;
    AddShot(SentShot{e.actor, e.index, R::AllocateSlotSerial(e.index), p.shotId, e.projClass});
    if (!held) return;
    OutItem item{};
    item.shot        = true;
    item.fired       = p;
    item.lookSeq     = coop::hand_item::OwnHandLookSeq();
    static bool s_said = false;   // the first held shot names the hand state it waits on
    if (!s_said) {
        s_said = true;
        void* player = coop::players::Registry::Get().Local();
        ue_wrap::engine::MainPlayerGrabState gs{};
        const bool read = player && ue_wrap::engine::ReadMainPlayerGrabState(player, gs);
        void* ha = read ? gs.holdingActor : nullptr;
        UE_LOGI("[EXPLOSION] shot held for the announce: look=%u holding=%ls live=%d keyed=%d prop=%d grabbing=%ls",
                item.lookSeq, ha ? R::ClassNameOf(ha).c_str() : L"none", ha && R::IsLive(ha) ? 1 : 0,
                ha && ue_wrap::prop::IsKeyedInteractable(ha) ? 1 : 0, ha && ue_wrap::prop::IsDescendantOfProp(ha) ? 1 : 0,
                read && gs.grabbingActor ? R::ClassNameOf(gs.grabbingActor).c_str() : L"none");
    }
    Enqueue(item);
}

// Launcher `l`'s fire body has ended: its pending births are sent, or counted dead, and dropped. The session is set
// at Install and never cleared, so it is here whenever a client fires.
void FlushLauncher(int l) {
    coop::net::Session* s = detail::SessionPtr();
    size_t kept = 0;
    for (size_t i = 0; i < g_pendingCount; ++i) {
        const Pending e = g_pending[i];
        if (e.launcher != l)
            g_pending[kept++] = e;
        else
            SendShot(*s, e);
    }
    g_pendingCount = kept;
}

// A body of launcher `l` is starting, so any pending birth of `l` belongs to a body that ended without its post. Its
// projectile's location would be read late, long after the shot, and the host would refuse it: it is dropped, counted.
void DropOrphans(int l) {
    size_t kept = 0;
    for (size_t i = 0; i < g_pendingCount; ++i) {
        const Pending e = g_pending[i];
        if (e.launcher != l) {
            g_pending[kept++] = e;
            continue;
        }
        ++detail::MutCounters().pendingOrphaned;
        if (!g_orphanLogged) {
            g_orphanLogged = true;
            UE_LOGW("[EXPLOSION] shot dropped: its fire body ended without its post");
        }
    }
    g_pendingCount = kept;
}

template <int L>
SG::Verdict OnFirePre(const SG::Call& call) {
    if (!g_body[L - 1].Is(call.function)) g_body[L - 1].Set(call.function);
    DropOrphans(L);
    return SG::Verdict::Run;
}

template <int L>
void OnFirePost(const SG::Call&) {
    if (!coop::spawn_authority::IsActiveClientSession()) return;
    FlushLauncher(L);
}

constexpr SG::PreFn  kFirePre[kLaunchers]  = {&OnFirePre<1>, &OnFirePre<2>, &OnFirePre<3>};
constexpr SG::PostFn kFirePost[kLaunchers] = {&OnFirePost<1>, &OnFirePost<2>, &OnFirePost<3>};

bool AllSettled() {
    for (const FireWatch& w : g_watch)
        if (!w.live && !w.gaveUp) return false;
    return true;
}

void Send(coop::net::Session& s, const coop::net::ExplosionRequestPayload& p, RC::RequestKind kind) {
    if (!s.SendReliable(coop::net::ReliableKind::ExplosionRequest, &p, sizeof(p)))
        return NoteDropped(Msg::Request, "the session took no reliable message");
    ++detail::MutCounters().requestsSent[detail::KindSlot(kind)];
    UE_LOGI("[EXPLOSION] request sent kind=%u class=%u shot=%u", static_cast<unsigned>(p.kind),
            static_cast<unsigned>(p.srcClass), p.shotId);
}

}  // namespace

namespace detail {

void InstallClient() {
    if (!g_listenerTried) {
        g_listenerTried = true;
        if (!OI::AddListener(OI::Listener{nullptr, nullptr, &OnDeleted}))
            UE_LOGE("[EXPLOSION] the object index took no death listener; a sent shot leaves the map only when "
                    "its blast or the cap takes it");
    }
    if (AllSettled()) return;
    const uint64_t now = coop::net::NowMs();
    if (now < g_nextTryMs) return;
    g_nextTryMs = now + kResolveRetryMs;
    bool anyRegistered = false;
    for (int i = 0; i < kLaunchers; ++i) {
        FireWatch& w = g_watch[i];
        if (w.live || w.gaveUp) continue;
        if (!w.registered) {
            if (SG::WatchName(*kFireRows[i], kTagFire, kFirePre[i], kFirePost[i])) {
                w.registered = true;
            } else if (++w.tries >= kMaxResolveTries) {
                w.gaveUp = true;
                UE_LOGE("[EXPLOSION] fire watch %ls gave up: the script gate refused it %d times",
                        RC::LauncherClassName(static_cast<uint8_t>(i + 1)), kMaxResolveTries);
            }
        }
        anyRegistered = anyRegistered || w.registered;
    }
    // WatchName returns at registration; the watch intercepts only once its name resolved.
    if (!anyRegistered) return;
    SG::ResolvePendingNames();
    for (int i = 0; i < kLaunchers; ++i) {
        FireWatch& w = g_watch[i];
        if (!w.registered || w.live || w.gaveUp) continue;
        const wchar_t* cls = RC::LauncherClassName(static_cast<uint8_t>(i + 1));
        if (SG::NameWatchLive(*kFireRows[i], kTagFire)) {
            w.live = true;
            UE_LOGI("[EXPLOSION] fire watch %ls live", cls);
        } else if (SG::NameWatchSettled(*kFireRows[i], kTagFire)) {
            w.gaveUp = true;
            UE_LOGE("[EXPLOSION] fire watch %ls gave up: it settled dead in the script gate", cls);
        }
    }
}

// The outbox's head first: a shot goes once the player's announce of its launcher has, and is dropped, with its
// queued requests, when a look of the hand lane since the shot left that launcher unannounced; a request goes when
// its shot has. The first head that must wait holds everything behind it.
void TickClient() {
    coop::net::Session* s = SessionPtr();
    if (!s) return;
    while (g_outboxCount > 0) {
        const OutItem head = g_outbox[0];
        if (!head.shot) {
            Send(*s, head.request, head.kind);
            PopHead();
            continue;
        }
        const wchar_t* cls = RC::LauncherClassName(head.fired.launcher);
        // The launcher's announce sends it. A look at the hand since the shot that left it unannounced (another item,
        // the empty hand, the launcher gone) means it never will be: dropped. No look yet -- the hand lane may not
        // have started, a client's streams wait for its load tail -- the shot waits. A look announces what it sees.
        if (!coop::hand_item::OwnAnnouncedA(cls)) {
            if (coop::hand_item::OwnHandLookSeq() == head.lookSeq) return;
            DropNotAnnounced(head.fired.shotId);
            continue;
        }
        if (TransmitShot(*s, head.fired))
            PopHead();
        else
            DropQueued(head.fired.shotId);
    }
}

void ClearClientWorld() {
    g_pendingCount = 0;
    g_shotCount    = 0;
    g_outboxCount  = 0;
}

void ClearClient() {
    ClearClientWorld();
    g_nextShotId     = 1;
    g_deadLogged     = false;
    g_orphanLogged   = false;
    g_unmappedLogged = false;
    g_dropLogged[0]  = false;
    g_dropLogged[1]  = false;
    g_notAnnouncedLogged = false;
}

}  // namespace detail

void OnClientProjectileBirth(void* actor) {
    if (!actor || !coop::spawn_authority::IsActiveClientSession()) return;
    const XS::Kind kind = XS::KindOfProjectileClass(R::ClassOf(actor));
    if (kind == XS::Kind::None) return;
    const uint8_t projClass = static_cast<uint8_t>(kind == XS::Kind::Rocket ? RC::ProjClass::Rocket
                                                                            : RC::ProjClass::Sticky);
    // A mirror's projectile is born outside every fire body, so it finds no launcher.
    int launcher = 0;
    for (int i = 0; i < kLaunchers; ++i) {
        void* body = g_body[i].Get();
        if (body && SG::IsBodyActive(body)) {
            launcher = i + 1;
            break;
        }
    }
    if (launcher == 0) return;
    if (g_pendingCount == kPendingCap) {
        for (size_t j = 1; j < g_pendingCount; ++j) g_pending[j - 1] = g_pending[j];
        --g_pendingCount;
        ++detail::MutCounters().pendingDropped;
    }
    g_pending[g_pendingCount++] = Pending{actor, R::InternalIndexOf(actor), launcher, projClass};
}

void OnClientBirth(void* sourceObject, void* /*explosionActor*/) {
    coop::net::Session* s = detail::SessionPtr();
    if (!s || !sourceObject) return;
    if (OnClientAtvBirth(sourceObject)) return;  // an ATV's own explosion has its own kind and its own judge
    const XS::Kind kind = XS::KindOfClass(R::ClassOf(sourceObject));
    coop::net::ExplosionRequestPayload p{};
    p.srcClass = RC::SrcClassOf(kind);
    bool atSource = true;  // the blast's place is the source's; a pilled burn sends none
    int  shotAt   = -1;    // the projectile's entry in the shot map, spent once its location has read
    RC::RequestKind rk{};  // set by every case that goes on; the wire byte is made from it below
    switch (kind) {
        case XS::Kind::Rocket:
        case XS::Kind::Sticky: {
            shotAt = FindShot(sourceObject);
            if (shotAt < 0) {
                // A host's projectile copied to this client is the normal case: its blast is the host's.
                if (coop::prop_echo_suppress::IsMirrorSpawn(sourceObject)) return;
                ++detail::MutCounters().unmappedProjectile;
                if (!g_unmappedLogged) {
                    g_unmappedLogged = true;
                    UE_LOGI("[EXPLOSION] a projectile exploded with no shot sent: its blast is not requested");
                }
                return;
            }
            p.shotId = g_shots[shotAt].shotId;
            rk = RC::RequestKind::Projectile;
            break;
        }
        case XS::Kind::Grenade:
        case XS::Kind::Pipebomb:
        case XS::Kind::PoopGrenade:
        case XS::Kind::SoapGrenade:
            if (!coop::hand_item::IsLocalHandViewActor(sourceObject)) return;
            rk = RC::RequestKind::Hand;
            break;
        case XS::Kind::Player: {
            if (!coop::players::Registry::Get().IsLocal(sourceObject)) return;
            // The Blueprint zeroes the level after the spawn, so it is read here, at the birth.
            float level = 0.0f;
            if (!ue_wrap::vitals::Read(ue_wrap::vitals::Field::GasolinePilled, &level))
                return NoteDropped(Msg::Request, "the pill level did not read");
            p.pillLevel = level;
            rk       = RC::RequestKind::Self;
            atSource = false;
            break;
        }
        case XS::Kind::None:
            return;
    }
    p.kind = static_cast<uint8_t>(rk);
    if (atSource) {
        FVector loc{};
        if (!E::TryGetActorLocation(sourceObject, loc))
            return NoteDropped(Msg::Request, "the source's location did not read");
        p.loc[0] = loc.X;
        p.loc[1] = loc.Y;
        p.loc[2] = loc.Z;
    }
    if (shotAt >= 0) EraseShot(static_cast<size_t>(shotAt));
    // A blast whose shot still waits for the hand announce queues behind it, so the host has the shot first.
    if (shotAt >= 0 && ShotQueued(p.shotId)) {
        OutItem item{};
        item.shot    = false;
        item.request = p;
        item.kind    = rk;
        return Enqueue(item);
    }
    Send(*s, p, rk);
}

}  // namespace coop::explosion_request
