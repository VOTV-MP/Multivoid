// coop/dev/crowbarpry_probe.cpp -- [dev] a client pries a stuck board off its wall with a crowbar
// (VOTVCOOP_CROWBARPRY_PROBE=1 on both peers).
//
// A board on a wall (prop_pryablePlank_C, a prop_wallAttachable_pryable_C) carries hidden prying crowbars
// (pryingCrowbar_C, its child actors). The crowbar's right-click use runs activatePrying, which calls the nearest one's
// `begin`: the held crowbar is destroyed and the prying crowbar shown in the board. Its `End(pryOpen, dropSelf,
// destroyed)` hides it again and spawns a new crowbar: freed from the board after the board's crowbarOpen, dropped at
// it, or put back in the hand. The rig's save holds the central facility's planks, held static on their walls.
// EVERY PEER, the host at 22 s and a client at 26 s, after a client is connected: the stuck board with the least key.
// THE CLIENT: at 28 s stands on the NavMesh before it, where a trace from eye height reaches it, a crowbar in hand;
// at 30 s aims at it; at 32 s runs the crowbar's activatePrying on the plank its aim is on; at 36 s ends the pry on
// that plank's shown prying crowbar, End(open, drop, false) as VOTVCOOP_CROWBARPRY_END says. EVERY PEER, every second
// to 50 s: the planks stuck, the prying crowbars shown, and the crowbars within 4 m of the board (`[CROWBARPRY]`).

#include "coop/dev/crowbarpry_probe.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/director/director.h"
#include "coop/dev/drill_hand.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_flags.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_nav.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace coop::dev::crowbarpry_probe {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
using ue_wrap::FVector;

uint64_t Now() { return ::GetTickCount64(); }

bool On() {
    static const bool v = coop::config::ReadEnv("VOTVCOOP_CROWBARPRY_PROBE") == "1";
    return v;
}

std::vector<void*> All(const wchar_t* cls) {
    std::vector<void*> out;
    for (void* o : R::FindObjectsByClass(cls))
        if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__")) out.push_back(o);
    return out;
}

// VOTVCOOP_CROWBARPRY_END: how the pry ends -- "open" (the default: pried open), "drop" (the crowbar dropped at the
// board) or "hand" (given up, the crowbar back in the hand).
std::string EndHow() {
    static const std::string v = [] {
        const std::string e = coop::config::ReadEnv("VOTVCOOP_CROWBARPRY_END");
        return e.empty() ? std::string("open") : e;
    }();
    return v;
}

float Dist2(const FVector& a, const FVector& b) {
    return (a.X - b.X) * (a.X - b.X) + (a.Y - b.Y) * (a.Y - b.Y) + (a.Z - b.Z) * (a.Z - b.Z);
}

int Hidden(void* a) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!a || !R::FindBoolProperty(R::ClassOf(a), L"bHidden", off, mask)) return -1;
    return (static_cast<const uint8_t*>(a)[off] & mask) ? 1 : 0;
}

// The board stuck to its wall: comp_wallAttachable's `stick` where it reads, else the prop held static (a plank of the
// central facility reads only the latter). 1 stuck, 0 not, -1 no board.
int Stuck(void* board) {
    if (!board) return -1;
    const int32_t co = R::FindPropertyOffset(R::ClassOf(board), L"comp_wallAttachable");
    if (co < 0) return ue_wrap::prop::IsStatic(board) ? 1 : 0;
    void* comp = nullptr;
    std::memcpy(&comp, static_cast<const uint8_t*>(board) + co, sizeof(comp));
    int32_t off = -1;
    uint8_t mask = 0;
    if (!comp || !R::FindBoolProperty(R::ClassOf(comp), L"stick", off, mask)) return ue_wrap::prop::IsStatic(board) ? 1 : 0;
    return (static_cast<const uint8_t*>(comp)[off] & mask) ? 1 : 0;
}

std::vector<void*> Boards() {
    std::vector<void*> out = All(L"prop_pryablePlank_C");
    for (void* o : All(L"prop_pryablePlank_edgesOnly_C")) out.push_back(o);
    return out;
}

// The stuck board with the least key: every peer picks the same.
void* PickBoard(std::wstring& key) {
    void* best = nullptr;
    const std::vector<void*> boards = Boards();
    int stuck = 0;
    for (void* o : boards)
        if (Stuck(o) == 1) ++stuck;
    UE_LOGI("[CROWBARPRY] boards %zu, stuck %d", boards.size(), stuck);
    for (size_t i = 0; i < boards.size() && i < 3; ++i) {
        void* const o = boards[i];
        FVector at{};
        E::TryGetActorLocation(o, at);
        const int32_t co = R::FindPropertyOffset(R::ClassOf(o), L"comp_wallAttachable");
        void* comp = nullptr;
        if (co >= 0) std::memcpy(&comp, static_cast<const uint8_t*>(o) + co, sizeof(comp));
        UE_LOGI("[CROWBARPRY] a board %ls '%ls' at (%.0f,%.0f,%.0f) stick %d (comp off %d %ls) static %d frozen %d",
                R::ClassNameOf(o).c_str(), ue_wrap::prop::GetInteractableKeyString(o).c_str(), at.X, at.Y, at.Z,
                Stuck(o), co, comp ? R::ClassNameOf(comp).c_str() : L"none", ue_wrap::prop::IsStatic(o) ? 1 : 0,
                ue_wrap::prop::IsFrozen(o) ? 1 : 0);
    }
    for (void* o : boards) {
        if (Stuck(o) != 1) continue;
        const std::wstring k = ue_wrap::prop::GetInteractableKeyString(o);
        if (k.empty() || k == L"None") continue;
        if (!best || k < key) {
            best = o;
            key = k;
        }
    }
    return best;
}

// The board's prying crowbars: those within 150 uu of it.
std::vector<void*> Pryers(void* board) {
    std::vector<void*> out;
    FVector at{};
    if (!board || !E::TryGetActorLocation(board, at)) return out;
    for (void* p : All(L"pryingCrowbar_C")) {
        FVector pa{};
        if (E::TryGetActorLocation(p, pa) && Dist2(pa, at) < 150.f * 150.f) out.push_back(p);
    }
    return out;
}

void* g_board = nullptr;
void* g_pried = nullptr;  // the client's: the plank its aim was on
std::wstring g_key;
uint64_t g_startAt = 0, g_nextLog = 0;
int g_step = 0;
bool g_reset = false;

}  // namespace

void Tick(coop::net::Session* session) {
    if (!On() || !session || !session->running() || !session->connected()) return;
    const bool host = session->role() == coop::net::Role::Host;
    if (!g_startAt) {
        g_startAt = Now();
        UE_LOGI("[CROWBARPRY] probe armed role=%s", host ? "host" : "client");
        // The client's step to the board crosses walls, which the game takes for a clip to the backrooms: the host
        // closes them for the drill and puts its setting back at 50 s.
        if (host)
            UE_LOGI("[CROWBARPRY] host: the backrooms closed (%d)",
                    static_cast<int>(coop::config::SetValue(coop::config_registry::rows::world_backrooms, "false")));
    }
    const uint64_t t = Now() - g_startAt;
    if (host && t >= 50000u && !g_reset) {
        g_reset = true;
        UE_LOGI("[CROWBARPRY] host: the backrooms setting put back (%d)",
                static_cast<int>(coop::config::ResetValue(coop::config_registry::rows::world_backrooms)));
    }
    if (t < 20000u) return;
    if (t < (host ? 22000u : 26000u)) return;
    if ((!g_board || !R::IsLive(g_board)) && g_step == 0) {
        g_board = PickBoard(g_key);
        g_step = g_board ? 1 : -1;
        UE_LOGI("[CROWBARPRY] the board '%ls' (found=%d)", g_key.c_str(), g_board ? 1 : 0);
    }
    void* const me = coop::players::Registry::Get().Local();
    FVector bat{};
    const bool haveBoard = g_board && E::TryGetActorLocation(g_board, bat);
    if (!host && me && haveBoard) {
        if (g_step == 1 && t >= 28000u) {
            g_step = 2;
            // A NavMesh point 70 to 150 uu off the board along its forward axis, either side, whose eye height (the
            // capsule's middle, 90 uu over the NavMesh, plus the camera's 60) a trace reaches the board from: a spot
            // set into a wall sends the player to the backrooms.
            const FVector f = E::GetActorForwardVector(g_board);
            FVector stand{};
            bool found = false;
            for (const float d : {110.f, 80.f, 150.f}) {
                for (const float side : {1.f, -1.f}) {
                    const FVector p{bat.X + f.X * d * side, bat.Y + f.Y * d * side, bat.Z};
                    FVector nav{};
                    if (!E::ProjectToNav(me, p, {40.f, 40.f, 250.f}, &nav)) continue;
                    const FVector eye{nav.X, nav.Y, nav.Z + 150.f};
                    ue_wrap::trace::Hit hit;
                    if (ue_wrap::trace::LineHitStatDyn(me, eye, bat, &hit) && hit.blocked && hit.actor == g_board) {
                        stand = {nav.X, nav.Y, nav.Z + 90.f};
                        found = true;
                        break;
                    }
                }
                if (found) break;
            }
            if (!found) {
                UE_LOGI("[CROWBARPRY] client: no NavMesh spot before the board reaches it");
                g_step = -1;
                return;
            }
            FVector was{};
            E::TryGetActorLocation(me, was);
            const bool moved = E::SetActorLocation(me, stand);
            FVector now{};
            E::TryGetActorLocation(me, now);
            UE_LOGI("[CROWBARPRY] client: from (%.0f,%.0f,%.0f) to (%.0f,%.0f,%.0f), now at (%.0f,%.0f,%.0f)", was.X, was.Y,
                    was.Z, stand.X, stand.Y, stand.Z, now.X, now.Y, now.Z);
            const bool took = coop::dev::drill_hand::TakeIntoHandNow(L"crowbar", L"prop_crowbar_C");
            UE_LOGI("[CROWBARPRY] client: before the board (side traced=%d moved=%d), a crowbar in the hand (ok=%d)",
                    found ? 1 : 0, moved ? 1 : 0, took ? 1 : 0);
        }
        if (g_step == 2 && t >= 30000u) {
            g_step = 3;
            if (void* c = E::GetController(me)) E::SetControlRotation(c, coop::director::LookAt(E::GetCameraLocation(), bat));
        }
        if (g_step == 2 && t >= 29000u && t < 29100u) {
            FVector now{};
            E::TryGetActorLocation(me, now);
            UE_LOGI("[CROWBARPRY] client: a second on, at (%.0f,%.0f,%.0f)", now.X, now.Y, now.Z);
        }
        if (g_step == 3 && t >= 31000u && t < 31100u) {
            const FVector cam = E::GetCameraLocation();
            void* const looked = E::ReadMainPlayerHitActor(me);
            UE_LOGI("[CROWBARPRY] client: the camera at (%.0f,%.0f,%.0f), %.0f uu from the board, aim on %ls",
                    cam.X, cam.Y, cam.Z, std::sqrt(Dist2(cam, bat)),
                    looked ? R::ClassNameOf(looked).c_str() : L"nothing");
        }
        if (g_step == 3 && t >= 32000u) {
            g_step = 4;
            void* const hand = coop::hand_item::LocalHandActor();
            void* const looked = E::ReadMainPlayerHitActor(me);
            // The plank the aim is on, the one a player would pry: the picked one sits in a stack of them.
            if (looked && R::ClassNameOf(looked).find(L"prop_pryablePlank") == 0) g_pried = looked;
            // activatePrying reads the player's lookAtActor: set to the board in this frame, the aim aside.
            const int32_t lo = R::FindPropertyOffset(R::ClassOf(me), L"lookAtActor");
            void* const target = g_pried ? g_pried : g_board;
            if (lo >= 0) std::memcpy(static_cast<uint8_t*>(me) + lo, &target, sizeof(target));
            void* const fn = hand ? R::FindDispatchFunctionCached(R::ClassOf(hand), L"activatePrying") : nullptr;
            const int32_t po = fn ? R::FindParamOffset(fn, L"player") : -1;
            const int32_t ao = fn ? R::FindParamOffset(fn, L"atLocation") : -1;
            bool ran = false;
            if (fn && po >= 0 && ao >= 0) {
                std::vector<uint8_t> frame(static_cast<size_t>(R::FunctionFrameSize(fn)), 0u);
                std::memcpy(frame.data() + po, &me, sizeof(me));
                std::memcpy(frame.data() + ao, &bat, sizeof(bat));
                ran = R::CallFunction(hand, fn, frame.data());
            }
            UE_LOGI("[CROWBARPRY] client: the crowbar's activatePrying (hand %ls, aim on %ls '%ls', ran=%d)",
                    hand ? R::ClassNameOf(hand).c_str() : L"none", looked ? R::ClassNameOf(looked).c_str() : L"nothing",
                    looked ? ue_wrap::prop::GetInteractableKeyString(looked).c_str() : L"", ran ? 1 : 0);
        }
        if (g_step == 5 && t >= 38000u) {
            g_step = 6;
            const int32_t ho = R::FindPropertyOffset(R::ClassOf(me), L"holding_actor");
            void* held = nullptr;
            if (ho >= 0) std::memcpy(&held, static_cast<const uint8_t*>(me) + ho, sizeof(held));
            FVector ha{};
            if (held) E::TryGetActorLocation(held, ha);
            int carried = 0;
            ue_wrap::inventory::LivePersonalStore store;
            if (ue_wrap::inventory::ReadLivePersonalStore(store))
                for (const auto& r : store.records)
                    if (r.className == L"prop_crowbar_C") ++carried;
            UE_LOGI("[CROWBARPRY] client: after the end, crowbars in the inventory %d", carried);
            UE_LOGI("[CROWBARPRY] client: after the end, holding_actor %ls '%ls' at (%.0f,%.0f,%.0f), hand actor %ls",
                    held ? R::ClassNameOf(held).c_str() : L"none",
                    held ? ue_wrap::prop::GetInteractableKeyString(held).c_str() : L"", ha.X, ha.Y, ha.Z,
                    coop::hand_item::LocalHandActor() ? R::ClassNameOf(coop::hand_item::LocalHandActor()).c_str() : L"none");
        }
        if (g_step == 4 && t >= 36000u) {
            g_step = 5;
            int ended = 0;
            for (void* p : Pryers(g_pried ? g_pried : g_board)) {
                if (Hidden(p) != 0) continue;
                void* const fn = R::FindDispatchFunctionCached(R::ClassOf(p), L"End");
                const int32_t o1 = fn ? R::FindParamOffset(fn, L"pryOpen") : -1;
                if (!fn || o1 < 0) continue;
                // lastPlayer, as the game's own callers set it first (the use's push, the action menu's give-up).
                const int32_t lp = R::FindPropertyOffset(R::ClassOf(p), L"lastPlayer");
                if (lp >= 0) std::memcpy(static_cast<uint8_t*>(p) + lp, &me, sizeof(me));
                std::vector<uint8_t> frame(static_cast<size_t>(R::FunctionFrameSize(fn)), 0u);
                const int32_t o2 = R::FindParamOffset(fn, L"dropSelf");
                frame[static_cast<size_t>(o1)] = EndHow() == "open" ? 1 : 0;
                if (o2 >= 0) frame[static_cast<size_t>(o2)] = EndHow() == "drop" ? 1 : 0;
                if (R::CallFunction(p, fn, frame.data())) ++ended;
            }
            UE_LOGI("[CROWBARPRY] client: the pry ended '%hs' (%d prying crowbar(s) ended)", EndHow().c_str(), ended);
        }
    }
    if (t < 50000u && Now() >= g_nextLog) {
        g_nextLog = Now() + 1000;
        int stuck = 0, shown = 0, pryers = 0;
        const std::vector<void*> boards = Boards();
        for (void* o : boards)
            if (Stuck(o) == 1) ++stuck;
        for (void* p : All(L"pryingCrowbar_C")) {
            ++pryers;
            if (Hidden(p) == 0) ++shown;
        }
        int crowbars = 0;
        std::wstring keys;
        for (void* c : All(L"prop_crowbar_C")) {
            FVector ca{};
            if (!haveBoard || !E::TryGetActorLocation(c, ca) || Dist2(ca, bat) >= 400.f * 400.f) continue;
            ++crowbars;
            keys += L" '" + ue_wrap::prop::GetInteractableKeyString(c) + L"'";
        }
        UE_LOGI("[CROWBARPRY] %s t=%llu: planks stuck %d of %zu, prying crowbars shown %d of %d, crowbars within 4 m %d%ls",
                host ? "host" : "client", t / 1000ull, stuck, boards.size(), shown, pryers, crowbars, keys.c_str());
    }
}

}  // namespace coop::dev::crowbarpry_probe
