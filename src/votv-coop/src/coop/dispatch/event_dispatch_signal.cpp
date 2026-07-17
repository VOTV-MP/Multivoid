// coop/event_dispatch_signal.cpp -- the SIGNAL-PIPELINE reliable-kind case
// bodies (the workstation chain: sky-signal set/catch, dish pose/arm/calib,
// desk input/state/log/audio, saved-signal + comp stores, tape caddy + daily
// task, the stationary PC), extracted VERBATIM from event_dispatch_state.cpp
// (2026-07-18 soft-cap extraction: the state router hit 791/800 LOC; every
// remaining signal-chain lane L6/L8/L5/L9 lands its case HERE). Same family
// contract as the siblings: returns true iff msg.kind is in this family
// (processed or validation-dropped) -- the switch IS the single membership
// declaration (see coop/event_dispatch.h).

#include "event_dispatch.h"  // co-located private header (src tree, not include/)

#include "coop/interactables/comp_sync.h"
#include "coop/interactables/console_state_sync.h"
#include "coop/interactables/desk_input_sync.h"
#include "coop/interactables/desk_snd_fx.h"
#include "coop/interactables/dish_sync.h"
#include "coop/interactables/laptop_sync.h"      // v116: LaptopState
#include "coop/interactables/signal_catch_sync.h"
#include "coop/interactables/signal_sync.h"
#include "coop/interactables/tape_caddy_sync.h"  // v114 (L7): ReelSlot
#include "coop/world/daily_task_sync.h"          // v114 (L7): TaskNewState

#include "ue_wrap/core/log.h"

#include <cstring>

namespace coop::event_feed {

bool HandleSignalEvent(net::Session& /*session*/,
                       const net::Session::ReliableMessage& msg) {
    switch (msg.kind) {
    case net::ReliableKind::SkySignalState: {
        // v64 (2026-06-12): HOST-authoritative sky-signal SET snapshot parts. The
        // client-only apply + the slot-0 trust gate + part assembly live in
        // console_state_sync::OnSkySignalState.
        if (msg.payloadLen < sizeof(net::SkySignalStatePayload)) {
            UE_LOGW("event_feed: SkySignalState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::SkySignalStatePayload));
            break;
        }
        net::SkySignalStatePayload sp{};
        std::memcpy(&sp, msg.payload, sizeof(sp));
        const uint8_t sslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::console_state_sync::OnSkySignalState(sp, sslot);
        break;
    }
    case net::ReliableKind::SkySignalCatch: {
        // v70: the signal-catch consume replay. HOST validates the claim holder,
        // replays + rebroadcasts; CLIENTS replay (transport-trusted). The gates
        // live in signal_catch_sync::OnReliable.
        if (msg.payloadLen < sizeof(net::SkySignalCatchPayload)) {
            UE_LOGW("event_feed: SkySignalCatch payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::SkySignalCatchPayload));
            break;
        }
        net::SkySignalCatchPayload cp{};
        std::memcpy(&cp, msg.payload, sizeof(cp));
        const uint8_t cslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::signal_catch_sync::OnReliable(cp, cslot);
        break;
    }
    case net::ReliableKind::LaptopState: {
        // v116: the stationary PC power/floppy lane. HOST applies + re-fans
        // (origin excluded); the gates live in laptop_sync::OnLaptopState.
        if (msg.payloadLen < sizeof(net::LaptopStatePayload)) {
            UE_LOGW("event_feed: LaptopState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::LaptopStatePayload));
            break;
        }
        net::LaptopStatePayload lp{};
        std::memcpy(&lp, msg.payload, sizeof(lp));
        const uint8_t lslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::laptop_sync::OnLaptopState(lp, lslot);
        break;
    }
    case net::ReliableKind::DishArm: {
        // v113 (L4): the download-ARM state edge (host-authored; host polarity).
        if (msg.payloadLen < sizeof(net::DishArmPayload)) {
            UE_LOGW("event_feed: DishArm payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::DishArmPayload));
            break;
        }
        net::DishArmPayload ap{};
        std::memcpy(&ap, msg.payload, sizeof(ap));
        const uint8_t aslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::dish_sync::OnDishArm(ap, aslot);
        break;
    }
    case net::ReliableKind::DishSnapshot: {
        // v113 (L4): the joiner's full-24 dish pose/state seed.
        if (msg.payloadLen < sizeof(net::DishSnapshotPayload)) {
            UE_LOGW("event_feed: DishSnapshot payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::DishSnapshotPayload));
            break;
        }
        net::DishSnapshotPayload sp2{};
        std::memcpy(&sp2, msg.payload, sizeof(sp2));
        const uint8_t s2slot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::dish_sync::OnDishSnapshot(sp2, s2slot);
        break;
    }
    case net::ReliableKind::DishCalib: {
        // v113 (L4): the symmetric calibration batch (apply + prime; host relays).
        if (msg.payloadLen < sizeof(net::DishCalibPayload)) {
            UE_LOGW("event_feed: DishCalib payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::DishCalibPayload));
            break;
        }
        net::DishCalibPayload cp2{};
        std::memcpy(&cp2, msg.payload, sizeof(cp2));
        const uint8_t c2slot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::dish_sync::OnDishCalib(cp2, c2slot);
        break;
    }
    case net::ReliableKind::ReelSlot: {
        // v114 (L7): a caddy slot sentinel edge (presser-authored; host relays).
        if (msg.payloadLen < sizeof(net::ReelSlotPayload)) {
            UE_LOGW("event_feed: ReelSlot payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::ReelSlotPayload));
            break;
        }
        net::ReelSlotPayload rsp{};
        std::memcpy(&rsp, msg.payload, sizeof(rsp));
        const uint8_t rslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::tape_caddy_sync::OnReelSlot(rsp, rslot);
        break;
    }
    case net::ReliableKind::TaskNewState: {
        // v114 (L7): the host's saveSlot.taskNew mirror (host-authored; trust-gated in the handler).
        if (msg.payloadLen < sizeof(net::TaskNewStatePayload)) {
            UE_LOGW("event_feed: TaskNewState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::TaskNewStatePayload));
            break;
        }
        net::TaskNewStatePayload tnp{};
        std::memcpy(&tnp, msg.payload, sizeof(tnp));
        const uint8_t tslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::daily_task_sync::OnTaskNewState(tnp, tslot);
        break;
    }
    case net::ReliableKind::DeskLogLine: {
        // v70: one coords-terminal event line (producer-symmetric, host-relayed).
        if (msg.payloadLen < sizeof(net::DeskLogLinePayload)) {
            UE_LOGW("event_feed: DeskLogLine payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::DeskLogLinePayload));
            break;
        }
        net::DeskLogLinePayload lp{};
        std::memcpy(&lp, msg.payload, sizeof(lp));
        const uint8_t lslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::console_state_sync::OnDeskLogLine(lp, lslot);
        break;
    }
    case net::ReliableKind::DeskState: {
        // v112: ADOPT-ONLY (the host->joiner connect seed). The live claimed/
        // unclaimed lanes retired to DeskInput; the adopt gate lives in
        // OnDeskState.
        if (msg.payloadLen < sizeof(net::DeskStatePayload)) {
            UE_LOGW("event_feed: DeskState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::DeskStatePayload));
            break;
        }
        net::DeskStatePayload dp{};
        std::memcpy(&dp, msg.payload, sizeof(dp));
        const uint8_t dslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::console_state_sync::OnDeskState(dp, dslot);
        break;
    }
    case net::ReliableKind::DeskInput: {
        // v112: the claim-free field-granular desk INPUT delta (presser-
        // authored; host relays to all except the originator). The apply +
        // echo-prime live in desk_input_sync.
        if (msg.payloadLen < sizeof(net::DeskInputPayload)) {
            UE_LOGW("event_feed: DeskInput payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::DeskInputPayload));
            break;
        }
        net::DeskInputPayload ip{};
        std::memcpy(&ip, msg.payload, sizeof(ip));
        const uint8_t islot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::desk_input_sync::OnDeskInput(ip, islot);
        break;
    }
    case net::ReliableKind::DeskScanEvent: {
        // v112: the SHIFT quick-scan happened on a peer -- replay the
        // accepted-branch effects (spawnDirs + beep) on this mirror.
        if (msg.payloadLen < sizeof(net::DeskScanEventPayload)) {
            UE_LOGW("event_feed: DeskScanEvent payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::DeskScanEventPayload));
            break;
        }
        net::DeskScanEventPayload sp2{};
        std::memcpy(&sp2, msg.payload, sizeof(sp2));
        const uint8_t sslot2 =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::desk_input_sync::OnDeskScan(sp2, sslot2);
        break;
    }
    case net::ReliableKind::DeskSndFx: {
        // v115: a peer's desk audio effect (organic Play/SetActive caught at the
        // native seam) -- replay on this mirror. Symmetric, relayed.
        if (msg.payloadLen < sizeof(net::DeskSndFxPayload)) {
            UE_LOGW("event_feed: DeskSndFx payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::DeskSndFxPayload));
            break;
        }
        net::DeskSndFxPayload fx{};
        std::memcpy(&fx, msg.payload, sizeof(fx));
        const uint8_t fxslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::desk_snd_fx::OnDeskSndFx(fx, fxslot);
        break;
    }
    case net::ReliableKind::DishAimState: {
        // v64: the claim owner's dish-aim stream (host-relayed). The holder gate
        // lives in OnDishAim.
        if (msg.payloadLen < sizeof(net::DishAimStatePayload)) {
            UE_LOGW("event_feed: DishAimState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::DishAimStatePayload));
            break;
        }
        net::DishAimStatePayload ap{};
        std::memcpy(&ap, msg.payload, sizeof(ap));
        const uint8_t aslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::console_state_sync::OnDishAim(ap, aslot);
        break;
    }
    case net::ReliableKind::SavedSignalAppend: {
        // v65: chunked saved-signal rows (producer-symmetric, host-relayed).
        if (msg.payloadLen < sizeof(net::BlobChunkPayload)) {
            UE_LOGW("event_feed: SavedSignalAppend payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::BlobChunkPayload));
            break;
        }
        net::BlobChunkPayload cp{};
        std::memcpy(&cp, msg.payload, sizeof(cp));
        const uint8_t slot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::signal_sync::OnAppendChunk(cp, slot);
        break;
    }
    case net::ReliableKind::SavedSignalDelete: {
        // v65: content-keyed saved-signal delete (player-symmetric, host-relayed).
        if (msg.payloadLen < sizeof(net::ContentHashPayload)) {
            UE_LOGW("event_feed: SavedSignalDelete payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::ContentHashPayload));
            break;
        }
        net::ContentHashPayload hp{};
        std::memcpy(&hp, msg.payload, sizeof(hp));
        const uint8_t slot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::signal_sync::OnDelete(hp, slot);
        break;
    }
    case net::ReliableKind::CompState: {
        // v65: the decode-pane scalar stream (simulator-authoritative).
        if (msg.payloadLen < sizeof(net::CompStatePayload)) {
            UE_LOGW("event_feed: CompState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::CompStatePayload));
            break;
        }
        net::CompStatePayload sp{};
        std::memcpy(&sp, msg.payload, sizeof(sp));
        const uint8_t slot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::comp_sync::OnState(sp, slot);
        break;
    }
    case net::ReliableKind::CompData: {
        // v65: chunked comp_data_0 (change edges + host adopt).
        if (msg.payloadLen < sizeof(net::BlobChunkPayload)) {
            UE_LOGW("event_feed: CompData payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::BlobChunkPayload));
            break;
        }
        net::BlobChunkPayload cp{};
        std::memcpy(&cp, msg.payload, sizeof(cp));
        const uint8_t slot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::comp_sync::OnDataChunk(cp, slot);
        break;
    }
    default:
        return false;  // not a signal-family kind -> event_feed tries the next family
    }
    return true;  // a signal-family kind was matched (processed or validation-dropped)
}

}  // namespace coop::event_feed
