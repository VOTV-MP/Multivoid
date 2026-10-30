// coop/net/client_authorship.h -- which reliable kinds a client may author to the host, one row per kind.
//
// MTA lets a client ask the server to run an event only if the event was registered as remotely triggerable,
// and checks that once, where the event arrives (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:
// 2663-2711; the server's own events register false, :1668); a packet id only the server sends is refused by
// its packet translator's table (CPacketTranslator.cpp:62-228). Ours is the same flag per kind, kept beside the
// lane and relay tables because our kinds are the protocol's fixed enum rather than runtime registrations: the
// host's receive edge (session_receive.cpp) drops a Host kind, or a byte that names no kind, from a seated
// client before it is inboxed or relayed.
// Only the host asks the table. A Host kind reaches a client from the host alone (the host relays only Client kinds,
// session_receive.cpp asserts it), so a client needs no sender check to trust one; a handler shared with a relayed
// Client kind may test the slot for that kind (pile_look's look rides both), and a Host kind's slot may name an origin
// the host stamped (a signal catch's catcher), never an authenticity test. MTA's client also refuses a server event
// not marked remote (its CPacketHandler.cpp:5026-5040); ours does not ask: a client's one link is the host's, which
// relays some Client kinds besides its own (session_lanes.h).
#pragma once

// Every enumerator is named below, and .github/ci/reliablekind_gate.py fails the build when one is missing,
// named twice, or relayable without being Client. A new kind is Host or Client from its first commit.
//   Client -- an honest client sends it to the host (its sending path read).
//   Host   -- no client may author it: its handler refused a non-host sender by name before this table moved
//             the refusal here, its only senders are the host's, or no one sends it yet (as MTA refuses an
//             event until it is registered, its remote-trigger flag false by default: addEvent,
//             reference/mtasa-blue/Server/mods/deathmatch/logic/luadefs/CLuaFunctionDefs.Event.cpp:26,32; a
//             client sender, when built, moves its row to Client).

#include "coop/net/protocol.h"  // ReliableKind

#include <cstdint>

namespace coop::net {

enum class ClientAuthorship : uint8_t { Client, Host, NotAKind };

constexpr ClientAuthorship AuthorshipOf(ReliableKind k) {
    switch (k) {
    case ReliableKind::Join:
    case ReliableKind::PropRelease:
    case ReliableKind::PropSpawn:
    case ReliableKind::PropDestroy:
    case ReliableKind::LightState:
    case ReliableKind::ContainerState:
    case ReliableKind::ItemActivate:
    case ReliableKind::WindowCleanState:
    case ReliableKind::GrimeState:
    case ReliableKind::GarageDoorState:
    case ReliableKind::ApplianceState:
    case ReliableKind::PowerControlState:
    case ReliableKind::AtvState:
    case ReliableKind::OrderRequest:
    case ReliableKind::FireflySpawn:
    case ReliableKind::SaveTransferRequest:
    case ReliableKind::ClientWorldReady:
    case ReliableKind::TrashPileState:
    case ReliableKind::InventoryPickup:
    case ReliableKind::ChatMessage:
    case ReliableKind::LockerDoorState:
    case ReliableKind::DeviceClaim:
    case ReliableKind::DishAimState:
    case ReliableKind::EmailDelete:
    case ReliableKind::SavedSignalAppend:
    case ReliableKind::SavedSignalDelete:
    case ReliableKind::VoiceState:
    case ReliableKind::KerfurConvertRequest:
    case ReliableKind::PropStickState:
    case ReliableKind::DeskLogLine:
    case ReliableKind::SleepState:
    case ReliableKind::PlayerInventoryBlob:
    case ReliableKind::KerfurCommand:
    case ReliableKind::GrabIntent:
    case ReliableKind::ThrowIntent:
    case ReliableKind::SkinChange:
    case ReliableKind::NameplateChange:
    case ReliableKind::AlarmState:
    case ReliableKind::NickColorChange:
    case ReliableKind::HandItem:
    case ReliableKind::PropDropIntent:
    case ReliableKind::RoachConsumed:
    case ReliableKind::OwnerEntitySpawn:
    case ReliableKind::OwnerEntityPose:
    case ReliableKind::OwnerEntityDestroy:
    case ReliableKind::DeskInput:
    case ReliableKind::DeskScanEvent:
    case ReliableKind::ReelSlot:
    case ReliableKind::ReelEjectIntent:
    case ReliableKind::DeskSndFx:
    case ReliableKind::LaptopState:
    case ReliableKind::PlayDeckEvent:
    case ReliableKind::PhysModsState:
    case ReliableKind::DriveSlotState:
    case ReliableKind::DrivePayload:
    case ReliableKind::RackState:
    case ReliableKind::MeadowAppend:
    case ReliableKind::MeadowDelete:
    case ReliableKind::MeadowOrder:
    case ReliableKind::LaptopQuad:
    case ReliableKind::FloppyBoxState:
    case ReliableKind::ContainerContents:
    case ReliableKind::CoinGunSell:
    case ReliableKind::CoinCollect:
    case ReliableKind::AuthHello:
    case ReliableKind::AuthProof:
    case ReliableKind::PropSaveDataIntent:
    case ReliableKind::FloppySlotState:
    case ReliableKind::HookState:
    case ReliableKind::HookDestroy:
    case ReliableKind::HookAnchorCommit:
    case ReliableKind::BroomStroke:
    case ReliableKind::LinkProbe:
    case ReliableKind::LinkProbeReply:
    case ReliableKind::PackTrashIntent:
    case ReliableKind::DroneFlyIntent:
    case ReliableKind::UpgradeIntent:
    case ReliableKind::WindowStroke:
    case ReliableKind::DoorVerbIntent:
    case ReliableKind::SleepingbagIntent:
    case ReliableKind::KeypadIntent:
    case ReliableKind::OvenRepairState:
    case ReliableKind::KerfusIntent:
    case ReliableKind::CordLinkState:
    case ReliableKind::ServerUpgradeState:
    case ReliableKind::SatConsole:
    case ReliableKind::DishCalibIntent:
    case ReliableKind::PowerGridState:
    case ReliableKind::DeskVerb:
    case ReliableKind::EraserPressIntent:
    case ReliableKind::FuseTowerState:
    case ReliableKind::DeskPingVerdict:
    case ReliableKind::ServerRepair:
    case ReliableKind::CommandRequest:
    case ReliableKind::StatOrderReply:
    case ReliableKind::StatQueryReply:
    case ReliableKind::PointSackRedeem:
    case ReliableKind::CoffeeActionIntent:
    case ReliableKind::RemoverRequest:
    case ReliableKind::HitIntent:
    case ReliableKind::ProjectileFired:
    case ReliableKind::ExplosionRequest:
    case ReliableKind::AtvExplodeRequest:
    case ReliableKind::PropIgniteIntent:
    case ReliableKind::AtvSeat:
    case ReliableKind::AtvTakeover:
    case ReliableKind::PropActRequest:
    case ReliableKind::PropActCommit:
    case ReliableKind::ContainerActRequest:
    case ReliableKind::ContainerActCommit:
    case ReliableKind::CameraToggleIntent:
    case ReliableKind::LaptopTabReport:
    case ReliableKind::PropDriveEnd:
    case ReliableKind::PropSyncClaim:
    case ReliableKind::DevParityReport:
    case ReliableKind::DevContainerParity:
        return ClientAuthorship::Client;
    // The Host row matches MTA's protected element data, which a client may not write: opt-in there
    // (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:2743-2755), the only mode here.
    case ReliableKind::EntitySpawn:
    case ReliableKind::EntityDestroy:
    case ReliableKind::RestoreVitals:
    case ReliableKind::TeleportClient:
    case ReliableKind::DoorState:
    case ReliableKind::WeatherState:
    case ReliableKind::RedSky:
    case ReliableKind::BadSun:
    case ReliableKind::CreatureEventActive:
    case ReliableKind::LightningStrike:
    case ReliableKind::ExplosionSpawn:
    case ReliableKind::AssignPeerSlot:
    case ReliableKind::RosterRow:
    case ReliableKind::PlayerHit:
    case ReliableKind::BalanceSync:
    case ReliableKind::KeypadState:
    case ReliableKind::SnapshotBegin:
    case ReliableKind::SnapshotComplete:
    case ReliableKind::SkyState:
    case ReliableKind::DroneState:
    case ReliableKind::PropConvert:
    case ReliableKind::SaveTransferBegin:
    case ReliableKind::SaveTransferChunk:
    case ReliableKind::TurbineState:
    case ReliableKind::SkySignalState:
    case ReliableKind::EmailAppend:
    case ReliableKind::CompState:
    case ReliableKind::CompData:
    case ReliableKind::WispGrab:
    case ReliableKind::WispTear:
    case ReliableKind::AtvSpawn:
    case ReliableKind::AtvAnswer:
    case ReliableKind::AtvDestroy:
    case ReliableKind::KerfurConvert:
    case ReliableKind::WorldActorSpawn:
    case ReliableKind::WorldActorDestroy:
    case ReliableKind::PropSnapPos:
    case ReliableKind::EventFire:
    case ReliableKind::PyramidGather:
    case ReliableKind::EventSnapshot:
    case ReliableKind::ServerState:
    case ReliableKind::RoachState:
    case ReliableKind::ChatLine:
    case ReliableKind::AuthChallenge:
    case ReliableKind::LightGroupState:
    case ReliableKind::GrabRefused:
    case ReliableKind::JoinPhaseNote:
    case ReliableKind::UpgradeLevels:
    case ReliableKind::KerfusState:
    case ReliableKind::DishHashcodes:
    case ReliableKind::ServerSetting:
    case ReliableKind::CommandReply:
    case ReliableKind::PermissionGrants:
    case ReliableKind::CoinGunResult:
    case ReliableKind::OrderRefused:
    case ReliableKind::OrderQueue:
    case ReliableKind::StatOrder:
    case ReliableKind::StatQuery:
    case ReliableKind::PropSaveData:
    case ReliableKind::EventCue:
    case ReliableKind::ChatSpeaker:
    case ReliableKind::PileResyncRequest:
    case ReliableKind::SkySignalCatch:
    case ReliableKind::DeskState:
    case ReliableKind::DishArm:
    case ReliableKind::DishSnapshot:
    case ReliableKind::DishCalib:
    case ReliableKind::TaskNewState:
    case ReliableKind::HookAnchored:
    case ReliableKind::RemoverResult:
    case ReliableKind::PropActReply:
    case ReliableKind::ContainerActReply:
    case ReliableKind::CameraActiveState:
    case ReliableKind::CameraLightState:
    case ReliableKind::LaptopTabState:
    case ReliableKind::AtvSetComplete:
    case ReliableKind::PropSyncAnswer:
    case ReliableKind::SwingerRestPose:
    case ReliableKind::GeomOctaEvent:
    case ReliableKind::DevLoadFiller:
        return ClientAuthorship::Host;
    }
    return ClientAuthorship::NotAKind;  // a byte value that names no kind: refused like Host
}

}  // namespace coop::net
