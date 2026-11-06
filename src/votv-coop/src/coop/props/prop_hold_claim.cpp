// coop/props/prop_hold_claim.cpp -- see coop/props/prop_hold_claim.h.

#include "coop/props/prop_hold_claim.h"

#include "coop/comms/peer_action_feed.h"  // the line that names the holder
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/local_streams.h"    // CurrentHoldGen, LastHeldActor: this peer's own hold
#include "coop/player/players_registry.h"
#include "coop/props/prop_hold.h"         // HostHolds: the host player's own hand
#include "coop/props/prop_sound.h"        // PlayDenyClick
#include "coop/props/remote_prop.h"       // DrivingSlotOf, RefuseHold
#include "coop/props/trash_mirror.h"      // WeMade
#include "remote_prop_drive.h"            // impl-private (src-local): g_holds, HoldDroveProp

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/swinger.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <array>
#include <atomic>
#include <chrono>
#include <string>

namespace coop::prop_hold_claim {
namespace {

namespace R = ue_wrap::reflection;
using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};

// The newest hold of this peer's own the host refused.
uint16_t g_ownRefusedGen = 0;
bool     g_haveOwnRefused = false;

// The hold a client's wait was said for, per slot, so a stream of poses logs one line.
std::array<uint16_t, coop::net::kMaxPeers> g_waitSaidGen{};

// A player who keeps pressing on a held prop hears every click and reads one line per interval.
constexpr auto kNoticeInterval = std::chrono::seconds(2);
Clock::time_point g_nextNotice{};

uint32_t g_localLetGo = 0, g_hostRefused = 0, g_ownRefused = 0, g_otherRefused = 0;

// Whether a refusal's key (ASCII on the wire) is the key string of the prop the refused hold names.
bool KeyIs(const coop::net::WireKey& k, const std::wstring& w) {
    if (k.len != w.size()) return false;
    for (uint8_t i = 0; i < k.len; ++i)
        if (static_cast<wchar_t>(static_cast<unsigned char>(k.data[i])) != w[i]) return false;
    return true;
}

void Notice(int holderSlot, void* localPlayer) {
    if (localPlayer) coop::prop_sound::PlayDenyClick(localPlayer);
    if (holderSlot < 0 || holderSlot >= static_cast<int>(coop::net::kMaxPeers)) return;
    const Clock::time_point now = Clock::now();
    if (now < g_nextNotice) return;
    g_nextNotice = now + kNoticeInterval;
    coop::peer_action_feed::AnnounceDirect(static_cast<uint8_t>(holderSlot),
                                           coop::peer_action_feed::Action::UsingDevice, L"it");
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

bool IsArbitrated(void* actor) {
    if (!actor || !R::IsLive(actor) || !ue_wrap::prop::IsDescendantOfProp(actor)) return false;
    if (ue_wrap::prop::IsGarbageClump(actor) || coop::trash_mirror::WeMade(actor)) return false;
    if (ue_wrap::swinger::EnsureResolved() && ue_wrap::swinger::IsSwinger(actor)) return false;
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(actor);
    return !key.empty() && key != L"None";
}

bool AdmitLocalGrab(void* localPlayer, void* actor) {
    UE_ASSERT_GAME_THREAD("prop_hold_claim::AdmitLocalGrab");
    if (!g_session.load(std::memory_order_acquire) || !IsArbitrated(actor)) return true;
    const int holder = coop::remote_prop::DrivingSlotOf(actor);
    if (holder < 0) return true;
    ue_wrap::engine::ReleaseMainPlayerGrabIfHolding(localPlayer, actor);
    Notice(holder, localPlayer);
    ++g_localLetGo;
    UE_LOGI("prop_hold_claim: this player's grab of %p let go -- slot %d holds it (#%u)", actor, holder,
            g_localLetGo);
    return false;
}

bool AdmitRemoteHold(int slot, uint16_t holdGen, const coop::net::WireKey& key, void* actor) {
    UE_ASSERT_GAME_THREAD("prop_hold_claim::AdmitRemoteHold");
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || holdGen == 0 || slot < 0 || slot >= static_cast<int>(coop::net::kMaxPeers)) return true;
    if (!IsArbitrated(actor)) return true;

    if (s->role() == coop::net::Role::Host) {
        const int holder = coop::prop_hold::HostHolds(actor) ? 0 : coop::remote_prop::DrivingSlotOf(actor, slot);
        if (holder < 0) return true;
        coop::remote_prop::RefuseHold(slot, holdGen);
        coop::net::PropHoldRefusedPayload p{};
        p.key = key;
        p.holdGen = holdGen;
        p.slot = static_cast<uint8_t>(slot);
        p.holder = static_cast<uint8_t>(holder);
        const bool sent = s->SendReliable(coop::net::ReliableKind::PropHoldRefused, &p, sizeof(p));
        ++g_hostRefused;
        UE_LOGI("prop_hold_claim: HOST refused slot %d hold %u of %p -- slot %d holds it (sent=%d, #%u)", slot,
                static_cast<unsigned>(holdGen), actor, holder, sent ? 1 : 0, g_hostRefused);
        return false;
    }

    void* const local = coop::players::Registry::Get().Local();
    const bool mine = local && ue_wrap::engine::IsMainPlayerGrabbing(local, actor);
    const int other = coop::remote_prop::DrivingSlotOf(actor, slot);
    if (!mine && other < 0) return true;
    if (g_waitSaidGen[slot] != holdGen) {
        g_waitSaidGen[slot] = holdGen;
        UE_LOGI("prop_hold_claim: slot %d hold %u of %p waits -- %s holds it until the host says which stays", slot,
                static_cast<unsigned>(holdGen), actor, mine ? "this peer's player" : "another slot");
    }
    return false;
}

void OnRefused(const coop::net::PropHoldRefusedPayload& p) {
    UE_ASSERT_GAME_THREAD("prop_hold_claim::OnRefused");
    const uint8_t self = coop::players::Registry::Get().LocalPeerId();
    if (p.slot != self) {
        // A drive of that very hold must be on the refusal's prop; a hold no drive started yet has no copy to compare.
        const coop::remote_prop::HoldGate& g = coop::remote_prop::g_holds[p.slot];
        if (g.driveGen == p.holdGen && !coop::remote_prop::HoldDroveProp(p.slot, p.holdGen, p.key, 0)) {
            UE_LOGW("prop_hold_claim: refusal of slot %u hold %u names another prop than that hold's drive -- dropped",
                    static_cast<unsigned>(p.slot), static_cast<unsigned>(p.holdGen));
            return;
        }
        ++g_otherRefused;
        coop::remote_prop::RefuseHold(p.slot, p.holdGen);
        return;
    }
    void* const local = coop::players::Registry::Get().Local();
    void* const held = coop::local_streams::LastHeldActor();
    const bool current = coop::local_streams::CurrentHoldGen() == p.holdGen && held;
    if (current && !KeyIs(p.key, ue_wrap::prop::GetInteractableKeyString(held))) {
        UE_LOGW("prop_hold_claim: refusal of this peer's hold %u names another prop than the one held -- dropped",
                static_cast<unsigned>(p.holdGen));
        return;
    }
    if (!g_haveOwnRefused || static_cast<int16_t>(p.holdGen - g_ownRefusedGen) > 0) {
        g_ownRefusedGen = p.holdGen;
        g_haveOwnRefused = true;
    }
    ++g_ownRefused;
    if (current) {
        ue_wrap::engine::ReleaseMainPlayerGrabIfHolding(local, held);
        Notice(p.holder, local);
    }
    UE_LOGI("prop_hold_claim: the host refused this peer's hold %u -- slot %u holds the prop; %s",
            static_cast<unsigned>(p.holdGen), static_cast<unsigned>(p.holder),
            current ? "let go, no release sent" : "the hold had already ended");
}

bool IsOwnHoldRefused(uint16_t holdGen) {
    UE_ASSERT_GAME_THREAD("prop_hold_claim::IsOwnHoldRefused");
    if (holdGen == 0 || !g_haveOwnRefused) return false;
    // A newer hold of this peer has started: the record of the refused one retires, so a wrapped counter
    // cannot meet it again.
    if (static_cast<int16_t>(holdGen - g_ownRefusedGen) > 0) {
        g_haveOwnRefused = false;
        return false;
    }
    return g_ownRefusedGen == holdGen;
}

void OnSlotLeft(int slot) {
    UE_ASSERT_GAME_THREAD("prop_hold_claim::OnSlotLeft");
    if (slot >= 0 && slot < static_cast<int>(coop::net::kMaxPeers)) g_waitSaidGen[slot] = 0;
}

void OnDisconnect() {
    UE_ASSERT_GAME_THREAD("prop_hold_claim::OnDisconnect");
    g_session.store(nullptr, std::memory_order_release);
    if (g_localLetGo || g_hostRefused || g_ownRefused || g_otherRefused)
        UE_LOGI("prop_hold_claim: session end -- let go here=%u refused as host=%u own refused=%u other refused=%u",
                g_localLetGo, g_hostRefused, g_ownRefused, g_otherRefused);
    g_ownRefusedGen = 0;
    g_haveOwnRefused = false;
    g_waitSaidGen.fill(0);
    g_nextNotice = {};
    g_localLetGo = g_hostRefused = g_ownRefused = g_otherRefused = 0;
}

}  // namespace coop::prop_hold_claim
