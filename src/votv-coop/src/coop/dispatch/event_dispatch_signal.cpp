// The signal-pipeline reliable-kind case bodies -- the workstation chain: the
// sky-signal set and catch, dish pose, arm and calibration, desk input, state, log
// and audio, the saved-signal and comp stores, the tape caddy and daily task, the
// stationary PC. Same family contract as the sibling routers: returns true iff
// msg.kind is in this family (processed or validation-dropped), so the switch IS
// the single membership declaration (see coop/event_dispatch.h).

#include "event_dispatch.h"  // co-located private header (src tree, not include/)

#include "coop/interactables/comp_sync.h"
#include "coop/interactables/console_state_sync.h"
#include "coop/interactables/deck_play_sync.h"    // PlayDeckEvent
#include "coop/interactables/desk_input_sync.h"
#include "coop/interactables/desk_snd_fx.h"
#include "coop/interactables/dish_sync.h"
#include "coop/interactables/laptop_sync.h"      // LaptopState, LaptopBlob
#include "coop/interactables/laptop_buffer_sync.h"  // LaptopQuad
#include "coop/interactables/floppybox_sync.h"   // FloppyBoxState
#include "coop/interactables/meadow_db_sync.h"   // MeadowAppend/MeadowDelete
#include "coop/interactables/physmods_sync.h"    // PhysModsState
#include "coop/interactables/drive_sync.h"       // DriveSlotState/DrivePayload
#include "coop/interactables/drive_rack_sync.h"  // RackState
#include "coop/interactables/signal_catch_sync.h"
#include "coop/interactables/signal_sync.h"
#include "coop/interactables/tape_caddy_sync.h"  // ReelSlot
#include "coop/world/daily_task_sync.h"          // TaskNewState

#include "ue_wrap/core/log.h"

#include <cstring>

namespace coop::event_feed {

bool HandleSignalEvent(net::Session& /*session*/,
                       const net::Session::ReliableMessage& msg) {
    switch (msg.kind) {
    case net::ReliableKind::SkySignalState: {
        // HOST-authoritative sky-signal SET snapshot parts. The client-only apply, the
        // slot-0 trust gate and part assembly live in console_state_sync::OnSkySignalState.
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
        // The signal-catch consume replay. HOST: dedup through the recent-catch TTL, replay,
        // rebroadcast. There is deliberately NO holder check -- the desk hold releases in the
        // same second as the catch edge, so a holder-equals-sender test raced exactly as the
        // detector's claim gate did. CLIENTS replay (transport-trusted). The gates live in
        // signal_catch_sync::OnReliable.
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
        // The stationary PC power/floppy lane. HOST applies and re-fans (origin excluded);
        // the gates live in laptop_sync::OnLaptopState.
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
    case net::ReliableKind::PlayDeckEvent: {
        // A deck playback edge (presser-authored; host relays; the gen guard and gate
        // prechecks live in deck_play_sync::OnPlayDeck).
        if (msg.payloadLen < sizeof(net::PlayDeckEventPayload)) {
            UE_LOGW("event_feed: PlayDeckEvent payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::PlayDeckEventPayload));
            break;
        }
        net::PlayDeckEventPayload pd{};
        std::memcpy(&pd, msg.payload, sizeof(pd));
        const uint8_t pdslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::deck_play_sync::OnPlayDeck(pd, pdslot);
        break;
    }
    case net::ReliableKind::PhysModsState: {
        // Value-ops (peer->host) / canonical array (host->all) / deny. Role and trust gates
        // live in physmods_sync::OnPhysMods.
        if (msg.payloadLen < sizeof(net::PhysModsStatePayload)) {
            UE_LOGW("event_feed: PhysModsState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::PhysModsStatePayload));
            break;
        }
        net::PhysModsStatePayload pm{};
        std::memcpy(&pm, msg.payload, sizeof(pm));
        const uint8_t pmslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::physmods_sync::OnPhysMods(pm, pmslot);
        break;
    }
    case net::ReliableKind::DriveSlotState: {
        // An idempotent drive-slot FSM state line (any-peer announced; pre-check and
        // host-canonical logic in drive_sync).
        if (msg.payloadLen < sizeof(net::DriveSlotStatePayload)) {
            UE_LOGW("event_feed: DriveSlotState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::DriveSlotStatePayload));
            break;
        }
        net::DriveSlotStatePayload ds{};
        std::memcpy(&ds, msg.payload, sizeof(ds));
        const uint8_t dslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::drive_sync::OnDriveSlotState(ds, dslot);
        break;
    }
    case net::ReliableKind::DrivePayload: {
        // Chunked drive data_0 rows (writer-authored, host-relayed).
        if (msg.payloadLen < sizeof(net::BlobChunkPayload)) {
            UE_LOGW("event_feed: DrivePayload payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::BlobChunkPayload));
            break;
        }
        net::BlobChunkPayload dp{};
        std::memcpy(&dp, msg.payload, sizeof(dp));
        const uint8_t dpslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::drive_sync::OnDrivePayloadChunk(dp, dpslot);
        break;
    }
    case net::ReliableKind::RackState: {
        // Rack index ops (peer->host) / canonical + deny (host->peer).
        if (msg.payloadLen < sizeof(net::BlobChunkPayload)) {
            UE_LOGW("event_feed: RackState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::BlobChunkPayload));
            break;
        }
        net::BlobChunkPayload rc{};
        std::memcpy(&rc, msg.payload, sizeof(rc));
        const uint8_t rcslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::drive_rack_sync::OnRackStateChunk(rc, rcslot);
        break;
    }
    case net::ReliableKind::MeadowAppend: {
        // Chunked meadow-DB row (presser-symmetric, host-relayed).
        if (msg.payloadLen < sizeof(net::BlobChunkPayload)) {
            UE_LOGW("event_feed: MeadowAppend payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::BlobChunkPayload));
            break;
        }
        net::BlobChunkPayload mc{};
        std::memcpy(&mc, msg.payload, sizeof(mc));
        const uint8_t mslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::meadow_db_sync::OnAppendChunk(mc, mslot);
        break;
    }
    case net::ReliableKind::MeadowDelete: {
        // Content-keyed meadow-DB delete (player-symmetric, host-relayed).
        if (msg.payloadLen < sizeof(net::ContentHashPayload)) {
            UE_LOGW("event_feed: MeadowDelete payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::ContentHashPayload));
            break;
        }
        net::ContentHashPayload mh{};
        std::memcpy(&mh, msg.payload, sizeof(mh));
        const uint8_t mdslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::meadow_db_sync::OnDelete(mh, mdslot);
        break;
    }
    case net::ReliableKind::MeadowOrder: {
        // Chunked order-as-state line (client->host op, host-canonical back; NOT relayed --
        // receivers drop non-host authors themselves).
        if (msg.payloadLen < sizeof(net::BlobChunkPayload)) {
            UE_LOGW("event_feed: MeadowOrder payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::BlobChunkPayload));
            break;
        }
        net::BlobChunkPayload oc{};
        std::memcpy(&oc, msg.payload, sizeof(oc));
        const uint8_t oslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::meadow_db_sync::OnOrderChunk(oc, oslot);
        break;
    }
    case net::ReliableKind::LaptopBlob: {
        // Laptop content chunks (slot/disc; the host re-fans them byte-for-byte inside
        // laptop_sync with the origin byte).
        if (msg.payloadLen < sizeof(net::BlobChunkPayload)) {
            UE_LOGW("event_feed: LaptopBlob payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::BlobChunkPayload));
            break;
        }
        net::BlobChunkPayload lb{};
        std::memcpy(&lb, msg.payload, sizeof(lb));
        const uint8_t lbslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::laptop_sync::OnLaptopBlobChunk(lb, lbslot);
        break;
    }
    case net::ReliableKind::LaptopQuad: {
        // Quad edit-script batches (client->host) + canonicals (host->clients; receivers
        // enforce senderSlot==0).
        if (msg.payloadLen < sizeof(net::BlobChunkPayload)) {
            UE_LOGW("event_feed: LaptopQuad payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::BlobChunkPayload));
            break;
        }
        net::BlobChunkPayload lq{};
        std::memcpy(&lq, msg.payload, sizeof(lq));
        const uint8_t lqslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::laptop_buffer_sync::OnQuadChunk(lq, lqslot);
        break;
    }
    case net::ReliableKind::FloppyBoxState: {
        // Box push/pop ops (client->host) + deny/canonical (host->clients; receivers
        // enforce senderSlot==0).
        if (msg.payloadLen < sizeof(net::BlobChunkPayload)) {
            UE_LOGW("event_feed: FloppyBoxState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::BlobChunkPayload));
            break;
        }
        net::BlobChunkPayload fb{};
        std::memcpy(&fb, msg.payload, sizeof(fb));
        const uint8_t fbslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::floppybox_sync::OnBoxChunk(fb, fbslot);
        break;
    }
    case net::ReliableKind::DishArm: {
        // The download-ARM state edge (host-authored; host polarity).
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
        // The joiner's full-24 dish pose/state seed.
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
        // The symmetric calibration batch (apply + prime; host relays).
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
        // A caddy slot sentinel edge (presser-authored; host relays).
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
        // The host's saveSlot.taskNew mirror (host-authored; trust-gated in the handler).
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
        // One coords-terminal event line (producer-symmetric, host-relayed).
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
        // ADOPT-ONLY (the host->joiner connect seed): live desk INPUT rides DeskInput and the
        // simulation rides DeskSimPose. The adopt gate lives in OnDeskState.
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
        // The claim-free field-granular desk INPUT delta (presser-authored; the host relays
        // to all except the originator). The apply and echo-prime live in desk_input_sync.
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
        // The quick-scan happened on a peer -- replay its arrow visual on this mirror. The
        // beep rides the desk sound-effect lane.
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
        // A peer's desk audio effect (an organic Play/SetActive caught at the native seam)
        // -- replay on this mirror. Symmetric, relayed.
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
        // The claim owner's dish-aim stream (host-relayed). The holder gate lives in
        // OnDishAim.
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
        // Chunked saved-signal rows (producer-symmetric, host-relayed).
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
        // Content-keyed saved-signal delete (player-symmetric, host-relayed).
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
        // The decode-pane scalar stream (simulator-authoritative).
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
        // Chunked comp_data_0 (change edges + host adopt).
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
