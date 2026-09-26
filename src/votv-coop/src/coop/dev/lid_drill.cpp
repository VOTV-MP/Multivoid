// coop/dev/lid_drill.cpp -- see coop/dev/lid_drill.h.

#include "coop/dev/lid_drill.h"

#include "coop/config/config.h"
#include "coop/element/registry.h"
#include "coop/interactables/portable_pc_lid.h"  // LineCounts
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady

#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/portable_pc.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/world/world_instances.h"

#include <chrono>
#include <cstdint>

namespace coop::dev::lid_drill {
namespace {

namespace PPC = ue_wrap::portable_pc;
namespace R   = ue_wrap::reflection;
namespace E   = ue_wrap::engine;
namespace LID = coop::portable_pc_lid;
using Clock = std::chrono::steady_clock;

constexpr auto kStepBound = std::chrono::seconds(60);
constexpr auto kJoinBound = std::chrono::seconds(300);  // a fresh client's boot, load and join
constexpr const wchar_t* kPcClassPath = L"/Game/objects/prop_portablePc.prop_portablePc_C";
constexpr const wchar_t* kPcClass = L"prop_portablePc_C";
constexpr uint8_t kActionOpen = 0xA, kActionClose = 0xB;  // the PC's own two lid actions
constexpr int32_t kScanMax = 16;  // the PCs a client looks through for the open one

enum class HostStep : uint8_t { Ready, Spawn, Named, WaitClose, HoldOpen, JoinerClose, Done };
enum class ClientStep : uint8_t { Ready, WaitOpen, Done };

HostStep   g_host = HostStep::Ready;
ClientStep g_client = ClientStep::Ready;
Clock::time_point g_stepAt{};
int   g_readyTicks = 0;
ue_wrap::CachedObjRef g_pc;  // the host's drill PC, slot-validated each tick

bool Enabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::lid_drill);
    return s;
}

void Enter(HostStep h) { g_host = h; g_stepAt = Clock::now(); }
void Enter(ClientStep c) { g_client = c; g_stepAt = Clock::now(); }
bool StepExpired(std::chrono::seconds bound = kStepBound) { return Clock::now() - g_stepAt >= bound; }

// The PC's own use verb with one of its lid actions, as a player's selection runs it.
bool UseAction(void* pc, void* player, uint8_t action) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(pc), L"actionOptionIndex");
    if (!fn || !player) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && f.Set<void*>(L"player", player) && f.Set<uint8_t>(L"action", action) &&
           ue_wrap::Call(pc, f);
}

bool LidOf(void* pc, bool& opened) {
    return pc && PPC::IsPortablePcClass(R::ClassOf(pc)) && PPC::ReadOpened(pc, opened);
}

void* SpawnInFrontOfPlayer() {
    void* player = coop::players::Registry::Get().Local();
    void* cls = ue_wrap::asset_load::LoadObjectByPath(kPcClassPath);
    ue_wrap::FVector at{};
    if (!player || !cls || !E::TryGetActorLocation(player, at)) return nullptr;
    const ue_wrap::FVector fwd = E::GetActorForwardVector(player);
    return E::SpawnActor(cls, {at.X + fwd.X * 150.f, at.Y + fwd.Y * 150.f, at.Z + 40.f});
}

// The host's leg ends, its PC taken out of the world again: the save gets no drill PC.
void EndHostLeg() {
    if (void* pc = g_pc.Get()) E::DestroyActor(pc);
    g_pc.Reset();
    Enter(HostStep::Done);
}

// The PC the host's open reached: every other copy on this peer loaded closed.
void* OpenPc() {
    void* found[kScanMax] = {};
    const int32_t n = ue_wrap::world_instances::Find(kPcClass, found, kScanMax);
    for (int32_t i = 0; i < n; ++i) {
        bool opened = false;
        if (LidOf(found[i], opened) && opened) return found[i];
    }
    return nullptr;
}

void HostTick(coop::net::Session& s) {
    switch (g_host) {
    case HostStep::Ready: {
        bool anyReady = false;
        for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers) && !anyReady; ++slot)
            anyReady = s.IsSlotWorldReady(slot);
        if (!anyReady || ++g_readyTicks < 2) return;
        UE_LOGI("[lid_drill] host ARMED: a client's world is ready");
        Enter(HostStep::Spawn);
        return;
    }
    case HostStep::Spawn:
        g_pc.Set(SpawnInFrontOfPlayer());
        if (!g_pc.Get()) {
            UE_LOGW("[lid_drill] ABANDONED: the portable PC could not be loaded or spawned");
            Enter(HostStep::Done);
            return;
        }
        UE_LOGI("[lid_drill] host spawned a portable PC");
        Enter(HostStep::Named);
        return;
    case HostStep::Named: {
        // Opened once the element lane names the PC, so the line carries its eid; the client's copy may
        // still be on its way, and the line waits there for it.
        void* pc = g_pc.Get();
        const auto eid = pc ? coop::element::Registry::Get().EidForActor(pc) : coop::element::kInvalidId;
        bool opened = true;
        if (eid == coop::element::kInvalidId || !LidOf(pc, opened)) {
            if (StepExpired()) {
                UE_LOGW("[lid_drill] ABANDONED: the spawned PC was not named by the element lane within 60 s");
                EndHostLeg();
            }
            return;
        }
        if (opened || !UseAction(pc, coop::players::Registry::Get().Local(), kActionOpen) ||
            !LidOf(pc, opened) || !opened) {
            UE_LOGW("[lid_drill] ABANDONED: the PC's action 10 did not open its lid");
            EndHostLeg();
            return;
        }
        UE_LOGI("[lid_drill] host opened the lid (eid=%u)", static_cast<unsigned>(eid));
        Enter(HostStep::WaitClose);
        return;
    }
    case HostStep::WaitClose: {
        void* pc = g_pc.Get();
        bool opened = true;
        if (!LidOf(pc, opened)) {
            UE_LOGW("[lid_drill] FAIL: the host's portable PC went away");
            EndHostLeg();
            return;
        }
        if (opened) {
            if (StepExpired()) {
                UE_LOGW("[lid_drill] FAIL: the client's close did not reach the host's copy within 60 s");
                EndHostLeg();
            }
            return;
        }
        const LID::Counts c = LID::LineCounts();
        if (c.sent != 1 || c.applied != 1) {
            UE_LOGW("[lid_drill] FAIL: the host's lane sent %llu and applied %llu lid lines, where 1 and 1 were "
                    "due", static_cast<unsigned long long>(c.sent), static_cast<unsigned long long>(c.applied));
            EndHostLeg();
            return;
        }
        // Open again and held for the next joiner, whose copy loads closed: only its world-ready row can
        // open it there.
        if (!UseAction(pc, coop::players::Registry::Get().Local(), kActionOpen) || !LidOf(pc, opened) || !opened) {
            UE_LOGW("[lid_drill] ABANDONED: the PC's action 10 did not open its lid again");
            EndHostLeg();
            return;
        }
        UE_LOGI("[lid_drill] host held the lid open for the next joiner: the client's close reached this copy, "
                "this lane sent its open and applied the close");
        Enter(HostStep::HoldOpen);
        return;
    }
    case HostStep::HoldOpen: {
        // The session ends when the client leaves (OnDisconnect carries the PC into the next one); until
        // then the lid stays as the host left it.
        void* pc = g_pc.Get();
        bool opened = false;
        if (!LidOf(pc, opened) || !opened) {
            UE_LOGW("[lid_drill] FAIL: the held lid closed, or its PC went, before the client left");
            EndHostLeg();
        } else if (StepExpired(kJoinBound)) {
            UE_LOGW("[lid_drill] FAIL: the client did not leave for a joiner within 300 s (run with --rejoin)");
            EndHostLeg();
        }
        return;
    }
    case HostStep::JoinerClose: {
        // A new session: the lane's counts began again at the last one's end.
        void* pc = g_pc.Get();
        bool opened = true;
        if (!LidOf(pc, opened)) {
            UE_LOGW("[lid_drill] FAIL: the host's portable PC went away");
            EndHostLeg();
            return;
        }
        if (opened) {
            if (StepExpired(kJoinBound)) {
                UE_LOGW("[lid_drill] FAIL: the joiner did not close the held lid within 300 s");
                EndHostLeg();
            }
            return;
        }
        const LID::Counts c = LID::LineCounts();
        if (c.sent == 0 && c.applied == 1)
            UE_LOGI("[lid_drill] host DONE: the joiner found the held lid open and its close reached this copy; "
                    "this session's lane sent nothing and applied the one close -- PASS");
        else
            UE_LOGW("[lid_drill] FAIL: this session's lane sent %llu and applied %llu lid lines, where 0 and 1 "
                    "were due", static_cast<unsigned long long>(c.sent), static_cast<unsigned long long>(c.applied));
        EndHostLeg();
        return;
    }
    case HostStep::Done:
        return;
    }
}

void ClientTick() {
    switch (g_client) {
    case ClientStep::Ready:
        if (!coop::net_pump::HasAnnouncedWorldReady()) return;
        UE_LOGI("[lid_drill] client ARMED: waiting for the host's portable PC to open");
        Enter(ClientStep::WaitOpen);
        return;
    case ClientStep::WaitOpen: {
        void* pc = OpenPc();
        if (!pc) {
            if (StepExpired()) {
                UE_LOGW("[lid_drill] FAIL: no portable PC on this client opened within 60 s");
                Enter(ClientStep::Done);
            }
            return;
        }
        bool opened = true;
        if (!UseAction(pc, coop::players::Registry::Get().Local(), kActionClose) || !LidOf(pc, opened) ||
            opened) {
            UE_LOGW("[lid_drill] ABANDONED: the PC's action 11 did not close its lid on this copy");
            Enter(ClientStep::Done);
            return;
        }
        const LID::Counts c = LID::LineCounts();
        if (c.sent == 1 && c.applied == 1)
            UE_LOGI("[lid_drill] client DONE: the host's open reached this copy and its close went; this lane "
                    "applied the one open and sent the one close -- PASS");
        else
            UE_LOGW("[lid_drill] FAIL: the client's lane sent %llu and applied %llu lid lines, where 1 and 1 were "
                    "due", static_cast<unsigned long long>(c.sent), static_cast<unsigned long long>(c.applied));
        Enter(ClientStep::Done);
        return;
    }
    case ClientStep::Done:
        return;
    }
}

}  // namespace

void Tick(coop::net::Session* s) {
    if (!Enabled() || !s || !s->connected()) return;
    if (s->role() == coop::net::Role::Host) HostTick(*s);
    else ClientTick();
}

void OnDisconnect() {
    g_client = ClientStep::Ready;
    g_readyTicks = 0;
    if (g_host == HostStep::HoldOpen) {
        // The client left, and the held PC stays for the one who joins next.
        Enter(HostStep::JoinerClose);
        UE_LOGI("[lid_drill] host: the session ended with the lid held open -- waiting for a joiner");
        return;
    }
    // Any other end takes the drill's PC out: left open, the next session's client would find two PCs.
    // The session ends before the world does (a quit tears the coop state down, then travels).
    if (void* pc = g_pc.Get()) E::DestroyActor(pc);
    g_host = HostStep::Ready;
    g_pc.Reset();
}

}  // namespace coop::dev::lid_drill
