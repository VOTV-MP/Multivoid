// coop/dev/crowbarpry_drill.cpp -- see coop/dev/crowbarpry_drill.h.

#include "coop/dev/crowbarpry_drill.h"

#include "coop/config/config.h"
#include "coop/dev/director/director.h"
#include "coop/dev/drill_hand.h"
#include "coop/dev/prop_birth_key_probe.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace coop::dev::crowbarpry_drill {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
namespace PR = ue_wrap::prop;
namespace PK = coop::dev::prop_birth_key_probe;
using ue_wrap::FVector;
using coop::rig_verdict::Outcome;

constexpr const char* kTag = "crowbarpry";
constexpr float    kAheadCm      = 130.f;    // the board's distance in front of the client's puppet
constexpr float    kClientNearCm = 500.f;    // the client's search for its copy of the board
constexpr float    kPryerNearCm  = 150.f;    // a prying crowbar belongs to the board within this
constexpr float    kHostNearCm   = 600.f;    // a host crowbar born within this of the board is the pry's
constexpr uint64_t kBoundMs      = 90000;    // every wait that ends on no event says INVALID after this

// The wall-attachable pryables the pak has (prop_wallAttachable_pryable_C), in the order the host tries them.
constexpr const wchar_t* kBoardClasses[] = {L"prop_baseroomsign_C", L"prop_torchHolder_C"};

enum class Arm { Off, Open, Hand };

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::crowbarpry_drill);
        return v == "open" ? Arm::Open : v == "hand" ? Arm::Hand : Arm::Off;
    }();
    return a;
}

uint64_t Now() { return ::GetTickCount64(); }

float Dist2(const FVector& a, const FVector& b) {
    return (a.X - b.X) * (a.X - b.X) + (a.Y - b.Y) * (a.Y - b.Y) + (a.Z - b.Z) * (a.Z - b.Z);
}

void CollectOne(void* ctx, void* obj, int32_t) {
    if (R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) static_cast<std::vector<void*>*>(ctx)->push_back(obj);
}

// Every live instance of exactly `cls`. The index lists a birth once its event is drained.
std::vector<void*> All(const wchar_t* cls) {
    std::vector<void*> out;
    OI::DrainAll();
    if (void* c = OI::ClassByName(cls)) OI::ForEachInstance(c, &CollectOne, &out);
    return out;
}

void* NearestTo(const wchar_t* cls, const FVector& at, float radiusCm) {
    void* best = nullptr;
    float bestD = radiusCm * radiusCm;
    for (void* o : All(cls)) {
        FVector l{};
        if (!E::TryGetActorLocation(o, l)) continue;
        const float d = Dist2(l, at);
        if (d <= bestD) { bestD = d; best = o; }
    }
    return best;
}

int Hidden(void* a) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!a || !R::FindBoolProperty(R::ClassOf(a), L"bHidden", off, mask)) return -1;
    return (static_cast<const uint8_t*>(a)[off] & mask) ? 1 : 0;
}

bool KeyReads(const std::wstring& k) { return !k.empty() && k != L"None"; }

// A sign wired the game's way: comp_wallAttachable::init(prop, arrow), the call a placed sign's loadData makes, sets the
// component's `prop`, which prop_stick_sync's unstick watch reads (no prop, nothing crosses). It is the repair when the sign's
// own BeginPlay has not run it; it does not touch the static state (the host holds that, see HostWaitClient).
// False when a step failed; the log names the step (the property read, the function, the frame, the call).
bool StickBoard(void* board) {
    const int32_t co = R::FindPropertyOffset(R::ClassOf(board), L"comp_wallAttachable");
    const int32_t so = R::FindPropertyOffset(R::ClassOf(board), L"stick");
    void* comp = nullptr;
    void* arrow = nullptr;
    if (co >= 0) std::memcpy(&comp, static_cast<const uint8_t*>(board) + co, sizeof(comp));
    if (so >= 0) std::memcpy(&arrow, static_cast<const uint8_t*>(board) + so, sizeof(arrow));
    const auto stop = [&](const char* step) {
        UE_LOGW("[CROWBARPRY] StickBoard stops at: %s (board class '%ls', comp_wallAttachable offset %d -> %p, stick offset "
                "%d -> %p)", step, R::ClassNameOf(board).c_str(), co, comp, so, arrow);
        return false;
    };
    if (co < 0 || !comp) return stop(co < 0 ? "the board has no comp_wallAttachable property" : "comp_wallAttachable reads null");
    if (so < 0 || !arrow) return stop(so < 0 ? "the board has no stick property" : "stick reads null");
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(comp), L"init");
    if (!fn) return stop("init does not resolve on the component's class");
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return stop("init's frame is invalid");
    if (!f.Set<void*>(L"prop", board)) return stop("init has no param 'prop'");
    if (!f.Set<void*>(L"arrow", arrow)) return stop("init has no param 'arrow'");
    if (!ue_wrap::Call(comp, f)) return stop("the init call was refused");
    return true;
}

// ---- the host ----------------------------------------------------------------------------------------

enum class HostStep { WaitClient, Watch, Done };
HostStep g_host = HostStep::WaitClient;
ue_wrap::CachedObjRef g_plank;
FVector g_plankAt{};
std::vector<void*> g_crowbarsBefore;   // pointer identity: the crowbars the world had when the board was spawned
uint64_t g_hostMs = 0;
bool g_foundLogged = false;
bool g_sawStatic = false;   // the sign was seen static on the host before the pry: only then is "free" a fact

void HostSay(Outcome o, const char* why) {
    g_host = HostStep::Done;
    coop::rig_verdict::Say(kTag, o, "host: %s", why);
}

// The first client past its join: its puppet's place and facing.
bool ClientBody(coop::net::Session& s, FVector& at, FVector& fwd) {
    for (int slot = 1; slot < static_cast<int>(coop::players::kMaxPeers); ++slot) {
        if (!s.IsSlotWorldReady(slot) || !coop::prop_snapshot::IsBracketClosed(slot) ||
            coop::join_window_baseline::IsLateWindowOpen(slot))
            continue;
        coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
        if (!rp || !rp->valid() || !rp->HasPose() || !E::TryGetActorLocation(rp->GetActor(), at)) continue;
        fwd = E::GetActorForwardVector(rp->GetActor());
        return true;
    }
    return false;
}

void HostWaitClient(coop::net::Session& s) {
    FVector at{}, fwd{};
    if (!ClientBody(s, at, fwd)) return;
    g_plankAt = {at.X + fwd.X * kAheadCm, at.Y + fwd.Y * kAheadCm, at.Z};
    // A wall-attachable pryable: the sign, else the torch holder. (prop_pryablePlank_C is not one: its open is a plain
    // setPropProps on itself, which no lane carries.)
    void* plank = nullptr;
    for (const wchar_t* name : kBoardClasses) {
        if (void* cls = R::FindClass(name)) plank = E::SpawnActor(cls, g_plankAt);
        if (plank) break;
    }
    if (!plank) { HostSay(Outcome::Invalid, "neither prop_baseroomsign_C nor prop_torchHolder_C spawned"); return; }
    // A stuck sign is held static (comp_wallAttachable::unstick refuses one that is not, and a pry on a loose one was refused); in the open no
    // wall holds it, so the host holds it the game's way and the spawn record carries the flag to the client.
    PR::CallSetPropProps(plank, true, false, false, false);
    const bool held = StickBoard(plank);
    g_plank.Set(plank);
    g_sawStatic = false;
    g_crowbarsBefore = All(L"prop_crowbar_C");
    g_hostMs = Now();
    g_foundLogged = false;
    UE_LOGI("[CROWBARPRY] host: the board '%ls' at (%.0f,%.0f,%.0f) 130 cm before the client, static=%d (wired %d), %zu "
            "crowbar(s) in the world", PR::GetInteractableKeyString(plank).c_str(), g_plankAt.X, g_plankAt.Y, g_plankAt.Z,
            PR::IsStatic(plank) ? 1 : 0, held ? 1 : 0, g_crowbarsBefore.size());
    g_host = HostStep::Watch;
}

// A world crowbar the world did not have when the board was spawned, near it: the one the pry handed back.
void* NewCrowbar() {
    for (void* c : All(L"prop_crowbar_C")) {
        if (std::find(g_crowbarsBefore.begin(), g_crowbarsBefore.end(), c) != g_crowbarsBefore.end()) continue;
        if (coop::hand_item::IsHandAxisActor(c)) continue;   // the puppet's display copy of its hand item is no world prop
        FVector l{};
        if (E::TryGetActorLocation(c, l) && Dist2(l, g_plankAt) <= kHostNearCm * kHostNearCm) return c;
    }
    return nullptr;
}

void HostWatch() {
    void* const plank = g_plank.Get();
    if (!plank) { HostSay(Outcome::Invalid, "the board is gone"); return; }
    if (PR::IsStatic(plank)) g_sawStatic = true;
    void* const crowbar = NewCrowbar();
    if (ArmOf() == Arm::Hand) {
        // A given-up pry leaves no world crowbar: one here is the birth the hand's own copy must not have made.
        if (crowbar) {
            char why[200];
            std::snprintf(why, sizeof(why), "a world crowbar '%ls' reached the host although the pry was given up",
                          PR::GetInteractableKeyString(crowbar).c_str());
            HostSay(Outcome::Fail, why);
        }
        return;
    }
    if (crowbar) {
        const bool freed = !PR::IsStatic(plank);
        // A sign never seen static on the host proves nothing by being free: the run says so rather than pass on it.
        if (!g_sawStatic) { HostSay(Outcome::Invalid, "the sign was never static on the host, so its freeing is unjudged (it needs the static hold)"); return; }
        if (!g_foundLogged) {
            g_foundLogged = true;
            UE_LOGI("[CROWBARPRY] host: the pry's crowbar '%ls' is here, the board is %s", PR::GetInteractableKeyString(crowbar).c_str(),
                    freed ? "free" : "still static");
        }
        if (freed) {
            char why[200];
            std::snprintf(why, sizeof(why), "the pry's crowbar '%ls' exists on the host and the board is free",
                          PR::GetInteractableKeyString(crowbar).c_str());
            HostSay(Outcome::Pass, why);
            return;
        }
    }
    if (Now() - g_hostMs > kBoundMs * 2)
        HostSay(Outcome::Invalid, crowbar ? "the crowbar came but the board never came free on the host"
                                          : "no crowbar and no pry reached the host (the client never pried)");
}

void HostTick(coop::net::Session& s) {
    switch (g_host) {
    case HostStep::WaitClient: HostWaitClient(s); return;
    case HostStep::Watch: HostWatch(); return;
    case HostStep::Done: return;
    }
}

// ---- the client --------------------------------------------------------------------------------------

enum class ClientStep { WaitReady, Find, Aim, Pry, End, WaitExit, Done };
ClientStep g_client = ClientStep::WaitReady;
ue_wrap::CachedObjRef g_board;
ue_wrap::CachedObjRef g_pryer;
uint64_t g_clientMs = 0;
uint64_t g_pryMark = 0;   // the probe's enqueue count just before End: the births after it are this pry's

void ClientGo(ClientStep s) {
    g_client = s;
    g_clientMs = Now();
}

void ClientSay(Outcome o, const char* fmt, const std::string& detail = std::string()) {
    g_client = ClientStep::Done;
    coop::rig_verdict::Say(kTag, o, fmt, detail.c_str());
}

bool HandHoldsCrowbar() {
    void* const h = coop::hand_item::LocalHandActor();
    return h && R::ClassNameOf(h) == L"prop_crowbar_C";
}

void ClientFind(void* me) {
    FVector mine{};
    if (!E::TryGetActorLocation(me, mine)) return;
    void* board = nullptr;
    for (const wchar_t* name : kBoardClasses)
        if (void* b = NearestTo(name, mine, kClientNearCm)) { board = b; break; }
    if (!board || !KeyReads(PR::GetInteractableKeyString(board))) {
        if (Now() - g_clientMs > kBoundMs) ClientSay(Outcome::Invalid, "client: no board bound within %s of the client", "500 cm");
        return;
    }
    g_board.Set(board);
    // The host stuck its copy; this peer's copy is wired the same way, since the pry's unstick runs on it first.
    const bool wasStatic = PR::IsStatic(board);
    const bool wired = StickBoard(board);
    const bool took = coop::dev::drill_hand::TakeIntoHandNow(L"crowbar", L"prop_crowbar_C");
    UE_LOGI("[CROWBARPRY] client: the board '%ls' bound (static on arrival %d, wired %d), a crowbar in the hand (ok=%d, holds=%d)",
            PR::GetInteractableKeyString(board).c_str(), wasStatic ? 1 : 0, wired ? 1 : 0, took ? 1 : 0, HandHoldsCrowbar() ? 1 : 0);
    if (!took || !HandHoldsCrowbar()) { ClientSay(Outcome::Invalid, "client: %s", "the crowbar did not come into the hand"); return; }
    FVector at{};
    E::TryGetActorLocation(board, at);
    if (void* c = E::GetController(me)) E::SetControlRotation(c, coop::director::LookAt(E::GetCameraLocation(), at));
    ClientGo(ClientStep::Aim);
}

// The game's own activatePrying: the crowbar in the hand calls the board's nearest prying crowbar's begin. The player's
// lookAtActor is written to the board first, as the use key's trace would have.
void ClientPry(void* me) {
    void* const board = g_board.Get();
    void* const hand = coop::hand_item::LocalHandActor();
    FVector at{};
    if (!board || !hand || !E::TryGetActorLocation(board, at)) { ClientSay(Outcome::Invalid, "client: %s", "the board or the hand is gone"); return; }
    const int32_t lo = R::FindPropertyOffset(R::ClassOf(me), L"lookAtActor");
    if (lo >= 0) std::memcpy(static_cast<uint8_t*>(me) + lo, &board, sizeof(board));
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(hand), L"activatePrying");
    const int32_t po = fn ? R::FindParamOffset(fn, L"player") : -1;
    const int32_t ao = fn ? R::FindParamOffset(fn, L"atLocation") : -1;
    bool ran = false;
    if (fn && po >= 0 && ao >= 0) {
        std::vector<uint8_t> frame(static_cast<size_t>(R::FunctionFrameSize(fn)), 0u);
        std::memcpy(frame.data() + po, &me, sizeof(me));
        std::memcpy(frame.data() + ao, &at, sizeof(at));
        ran = R::CallFunction(hand, fn, frame.data());
    }
    void* shown = nullptr;
    for (void* p : All(L"pryingCrowbar_C")) {
        FVector pa{};
        if (E::TryGetActorLocation(p, pa) && Dist2(pa, at) < kPryerNearCm * kPryerNearCm && Hidden(p) == 0) shown = p;
    }
    UE_LOGI("[CROWBARPRY] client: activatePrying ran=%d, a prying crowbar shown=%d", ran ? 1 : 0, shown ? 1 : 0);
    if (!ran || !shown) {
        ClientSay(Outcome::Invalid, "client: %s", "activatePrying left no prying crowbar shown (the game refused the pry here)");
        return;
    }
    g_pryer.Set(shown);
    ClientGo(ClientStep::End);
}

// The end the game's own callers run: opened (the board's crowbarOpen, then a crowbar spawned free) or given up (the
// crowbar spawned and put back into the hand). lastPlayer is set first, as the use's push and the action menu's give-up do.
void ClientEnd(void* me) {
    void* const p = g_pryer.Get();
    void* const fn = p ? R::FindDispatchFunctionCached(R::ClassOf(p), L"End") : nullptr;
    const int32_t o1 = fn ? R::FindParamOffset(fn, L"pryOpen") : -1;
    if (!fn || o1 < 0) { ClientSay(Outcome::Invalid, "client: %s", "the prying crowbar's End did not resolve"); return; }
    const int32_t lp = R::FindPropertyOffset(R::ClassOf(p), L"lastPlayer");
    if (lp >= 0) std::memcpy(static_cast<uint8_t*>(p) + lp, &me, sizeof(me));
    std::vector<uint8_t> frame(static_cast<size_t>(R::FunctionFrameSize(fn)), 0u);
    frame[static_cast<size_t>(o1)] = ArmOf() == Arm::Open ? 1 : 0;
    g_pryMark = PK::EnqueueMark();
    const bool ran = R::CallFunction(p, fn, frame.data());
    void* const board = g_board.Get();
    UE_LOGI("[CROWBARPRY] client: End(pryOpen=%d) ran=%d, the board static=%d", ArmOf() == Arm::Open ? 1 : 0, ran ? 1 : 0,
            board && PR::IsStatic(board) ? 1 : 0);
    if (!ran) { ClientSay(Outcome::Invalid, "client: %s", "End did not run"); return; }
    ClientGo(ClientStep::WaitExit);
}

// The end of the crowbar's birth, as the client's own drain says it: the event that decides the arm.
void ClientWaitExit() {
    const std::string exit = PK::ExitSince(L"prop_crowbar_C", g_pryMark);
    if (exit.empty()) {
        if (Now() - g_clientMs > kBoundMs) ClientSay(Outcome::Invalid, "client: %s", "the crowbar's birth never reached the drain");
        return;
    }
    const bool sent = exit.rfind("authored-", 0) == 0;
    UE_LOGI("[CROWBARPRY] client: the pry's crowbar left the drain as '%s' (sent=%d), the hand holds a crowbar=%d",
            exit.c_str(), sent ? 1 : 0, HandHoldsCrowbar() ? 1 : 0);
    if (ArmOf() == Arm::Open) {
        // The host judges the arrival. The client says FAIL only on the two exits a LOST crowbar makes (a birth no one
        // authored, a key that never read); any other exit says nothing about authorship, so it is INVALID.
        if (sent) { g_client = ClientStep::Done; return; }
        if (exit == "not-a-place-nor-whitelisted-birth" || exit == "key-wait-expired")
            ClientSay(Outcome::Fail, "client: the pry's crowbar was not sent (its drain ended '%s')", exit);
        else
            ClientSay(Outcome::Invalid, "client: the pry's crowbar ended '%s', which says nothing of its authorship", exit);
        return;
    }
    if (sent) { ClientSay(Outcome::Fail, "client: a world crowbar was sent although the pry was given up (%s)", exit); return; }
    if (exit != "hand-axis-drop" && exit != "died-before-drain") {
        ClientSay(Outcome::Invalid, "client: the given-up crowbar's birth ended '%s', neither sent nor into the hand", exit);
        return;
    }
    if (!HandHoldsCrowbar()) { ClientSay(Outcome::Fail, "client: the pry was given up but the hand holds no crowbar (%s)", exit); return; }
    ClientSay(Outcome::Pass, "client: the given-up crowbar went back into the hand (%s) and nothing was sent", exit);
}

void ClientTick() {
    void* const me = coop::players::Registry::Get().Local();
    switch (g_client) {
    case ClientStep::WaitReady:
        if (!me || !coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
            return;
        if (!PK::IsEnabled()) { ClientSay(Outcome::Invalid, "client: %s", "prop_birth_key_probe=1 is not set on the client"); return; }
        ClientGo(ClientStep::Find);
        return;
    case ClientStep::Find: if (me) ClientFind(me); return;
    case ClientStep::Aim:  ClientGo(ClientStep::Pry); return;   // a frame for the control rotation to take
    case ClientStep::Pry:  if (me) ClientPry(me); return;
    case ClientStep::End:  if (me) ClientEnd(me); return;
    case ClientStep::WaitExit: ClientWaitExit(); return;
    case ClientStep::Done: return;
    }
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (ArmOf() == Arm::Off || !session || !session->running() || !session->connected()) return;
    if (coop::roster::LocalIsHost()) HostTick(*session);
    else ClientTick();
}

void OnDisconnect() {
    // The host's board goes with the client's life; the next life's client must not find a freed one.
    if (void* const plank = g_plank.Get()) E::DestroyActor(plank);
    g_plank.Reset();
    g_pryer.Reset();
    g_board.Reset();
    g_crowbarsBefore.clear();
    g_host = HostStep::WaitClient;
    g_client = ClientStep::WaitReady;
    g_foundLogged = false;
}

}  // namespace coop::dev::crowbarpry_drill
