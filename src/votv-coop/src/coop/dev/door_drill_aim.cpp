// coop/dev/door_drill_aim.cpp -- the door drill's AIMED legs; see coop/dev/door_drill_internal.h.

#include "coop/dev/door_drill_internal.h"

#include "coop/interactables/door_verb_intent.h"  // SentCount: an aimed press went to the host
#include "coop/player/players_registry.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/door.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

namespace coop::dev::door_drill::detail {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace D  = ue_wrap::door;
namespace GT = ue_wrap::game_thread;

// The camera turned at a part of the door through a fan of headings around it, each held until the
// player's own interaction trace answers (it runs every frame), until the trace strikes that part of
// that door: what the trace takes is what a player's E is sent to. `other` names an actor it took
// instead, the first one met.
struct Aim { bool took = false; std::wstring other; };
Aim AimAtPart(void* door, const wchar_t* part) {
    static constexpr float kYaw[] = {0.f, -5.f, 5.f, -10.f, 10.f, -16.f, 16.f};
    static constexpr float kPitch[] = {0.f, 8.f, -8.f, 16.f, -16.f};
    Aim a;
    for (float dp : kPitch) {
        for (float dy : kYaw) {
            const bool aimed = GT::RunAndWait([door, part, dp, dy](std::atomic<int>& done) {
                void* p = coop::players::Registry::Get().Local();
                void* c = p ? E::GetController(p) : nullptr;
                void* comp = D::PartOf(door, part);
                if (!c || !comp) { done.store(2); return; }
                const ue_wrap::FVector cam = E::GetCameraLocation();
                const ue_wrap::FVector at = E::GetComponentLocation(comp);
                const float dx = at.X - cam.X, dyv = at.Y - cam.Y, dz = at.Z - cam.Z;
                const float yaw = std::atan2(dyv, dx) * 57.29578f;
                const float pitch = std::atan2(dz, std::sqrt(dx * dx + dyv * dyv)) * 57.29578f;
                E::SetControlRotation(c, ue_wrap::FRotator{pitch + dp, yaw + dy, 0.f});
                done.store(1);
            }) == 1;
            if (!aimed) return a;
            for (int waited = 0; waited <= 300; waited += 50) {
                ::Sleep(50);
                auto other = std::make_shared<std::wstring>();
                const int hit = GT::RunAndWait([door, part, other](std::atomic<int>& done) {
                    void* p = coop::players::Registry::Get().Local();
                    void* actor = p ? E::ReadMainPlayerHitActor(p) : nullptr;
                    void* comp = p ? E::ReadMainPlayerHitComponent(p) : nullptr;
                    if (actor == door && comp && comp == D::PartOf(door, part)) { done.store(1); return; }
                    if (actor && actor != door) *other = R::ClassNameOf(actor);
                    done.store(2);
                });
                if (hit == 1) { a.took = true; return a; }
                if (!other->empty() && a.other.empty()) a.other = *other;
            }
        }
    }
    return a;
}

// The door verbs this client has sent the host, read on the game thread.
uint64_t SentVerbs() {
    auto n = std::make_shared<uint64_t>(0);
    GT::RunAndWait([n](std::atomic<int>& done) {
        *n = coop::door_verb_intent::SentCount();
        done.store(1);
    });
    return *n;
}

}  // namespace

// AIMED (a client, the door shut): the player's own E, aimed through the game's trace at a leaf and
// then at the frame. The press goes through the player's use handler, which sends the selected action
// to the actor the trace hit: it reached the door's entry verb when that verb, refused on this copy,
// went to the host as a press. Whether the door moves is the door's own say (it will not close on a
// player in its sensor), so its open is reported beside the verdict, not judged.
void AimedLegs(void* door, const std::wstring& name) {
    for (const wchar_t* part : {L"door_L", L"frame"}) {
        const int before = ReadOpenIntent(door);
        const Aim a = AimAtPart(door, part);
        if (!a.took) {
            UE_LOGW("[DOOR-DRILL] client AIMED door=%ls part=%ls: no aim of the fan put the trace on it (it took %ls) "
                    "-- INCONCLUSIVE", name.c_str(), part, a.other.empty() ? L"nothing else" : a.other.c_str());
            continue;
        }
        const uint64_t sent0 = SentVerbs();
        const bool pressed = GT::RunAndWait([](std::atomic<int>& done) {
            void* p = coop::players::Registry::Get().Local();
            done.store(p && E::CallMainPlayerUseSelectedAction(p) ? 1 : 2);
        }) == 1;
        const bool went = SentVerbs() > sent0;
        WaitForOpen(door, before == 1 ? 0 : 1, 3000);
        UE_LOGI("[DOOR-DRILL] client AIMED door=%ls part=%ls: the trace took the door through it; E through "
                "useSelectedAction dispatched=%d, %s; the door's open %d -> %d on this copy", name.c_str(), part,
                pressed ? 1 : 0, went ? "the press went to the host -- PASS" : "no press went to the host -- FAIL",
                before, ReadOpenIntent(door));
    }
}

}  // namespace coop::dev::door_drill::detail
