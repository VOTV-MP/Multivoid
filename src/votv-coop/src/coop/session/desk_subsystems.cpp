// coop/session/desk_subsystems.cpp -- see coop/session/desk_subsystems.h.

#include "coop/session/desk_subsystems.h"

#include "coop/interactables/desk_cursor_sync.h"
#include "coop/interactables/desk_input_sync.h"
#include "coop/interactables/desk_ping_sync.h"
#include "coop/interactables/deck_play_sync.h"  // deck playback lane
#include "coop/interactables/physmods_sync.h"  // desk physical-modules lane
#include "coop/interactables/drive_payload_sync.h"  // a drive's row, the host's
#include "coop/interactables/drive_sync.h"  // the drive slots
#include "coop/interactables/drive_rack_sync.h"  // rack storage
#include "coop/interactables/desk_snd_fx.h"
#include "coop/interactables/desk_sim_sync.h"
#include "coop/interactables/dish_calib_sync.h"
#include "coop/interactables/dish_hashcode_sync.h"
#include "coop/interactables/dish_sync.h"
#include "coop/interactables/download_arm_sync.h"
#include "coop/interactables/sat_console_sync.h"  // the SAT console's shared commands run on the host
#include "coop/interactables/tape_caddy_sync.h"
#include "coop/world/daily_task_sync.h"
#include "coop/dev/perf_probe.h"
#include "ue_wrap/core/walk_timer.h"  // per-sync [WALK-TIME] attribution (diagnostic)

namespace coop::subsystems::desk {

void Install(coop::net::Session& session) {
    coop::desk_cursor_sync::Install(&session);  // coords-panel live-cursor unreliable motion stream (interpolated mirror)
    coop::desk_input_sync::Install(&session);  // the claim-free field-granular desk input lane
    coop::desk_snd_fx::Install(&session);  // desk audio-effect mirror (Func-patch audio seam)
    coop::deck_play_sync::Install(&session);  // deck playback edge mirror (audio-seam Activate/Deactivate + gen guard)
    coop::physmods_sync::Install(&session);  // physMods slot ops at the desk's verbs + host-canonical array
    coop::drive_sync::Install(&session);  // the drive chain's slot lane (verb dirty-marks + the slot sweep; the slot and rack verb watches)
    coop::drive_payload_sync::Install(&session);  // a drive's row: the host authors it at prop_drive_C::upd
    coop::drive_rack_sync::Install(&session);  // rack storage lane (marks forwarded from drive_sync)
    coop::desk_ping_sync::Install(&session);  // the ping's verdict: a client's refused and rolled on the host's desk
    coop::desk_sim_sync::Install(&session);  // download-SIM host-authoritative output stream (decoded/needle/rate/frData/poData/offsets; client overwrites)
    coop::dish_sync::Install(&session);  // host-auth dish pose mirror (client sim parked)
    coop::download_arm_sync::Install(&session);  // the download machine's arm and reset, the host's, replayed through the game's verbs
    coop::dish_calib_sync::Install(&session);  // the dishes' precision: the host authors it, a client's own verbs are intents
    coop::dish_hashcode_sync::Install(&session);  // the dishes' hash codes: the host's rollover sends them, a client refuses its own
    coop::sat_console_sync::Install(&session);  // the SAT console: a client's shared commands run on a terminal the host keeps for it
    coop::tape_caddy_sync::Install(&session);  // caddy reel slots (presser edges) + host accrual corrector (client accrual NOT parked -- corrector-bounded)
    coop::daily_task_sync::Install(&session);  // saveSlot.taskNew host mirror (rollover/sell are host-only live)
}

void Tick() {
    namespace PP = coop::dev::perf_probe;
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:desk_cursor"}; coop::desk_cursor_sync::Tick(); }  // coords-panel live cursor -- holder streams viewCoordinate / mirror interpolates (50ms) + WriteCursorOnly
    { PP::Scope _s{PP::Bucket::Interactable}; coop::desk_ping_sync::Tick(); }  // the ping lane's watches until live
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:desk_sim"}; coop::desk_sim_sync::Tick(); }  // download-SIM -- host streams outputs (10Hz) / client interpolates + WriteSimOutputs
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:dish"}; coop::dish_sync::Tick(); }  // host pose sweep (4Hz) / client apply + park latch / calib diff-poll (1Hz)
    { PP::Scope _s{PP::Bucket::Interactable}; coop::download_arm_sync::Tick(); }  // the desk watches' name resolve
    { PP::Scope _s{PP::Bucket::Interactable}; coop::dish_hashcode_sync::Tick(); }  // host: the marked codes, a joiner's owed set / client: rows waiting for their dish
    { PP::Scope _s{PP::Bucket::Interactable}; coop::sat_console_sync::Tick(); }  // owed blobs, and a terminal whose world is gone
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:reel"}; coop::tape_caddy_sync::Tick(); }  // 4Hz slot sentinel poll (both peers) + host 1Hz corrector / client exact-snap apply
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:task"}; coop::daily_task_sync::Tick(); }  // host 1Hz taskNew change-hash poll (fires a few times per game-day)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:desk_input"}; coop::desk_input_sync::Tick(); }  // 250ms input-field poll -> claim-free DeskInput deltas + cooldown charge/scan classification
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:desk_snd"}; coop::desk_snd_fx::Tick(); }  // audio-seam ring flush + lazy hook install + pending loop retry
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:deck_play"}; coop::deck_play_sync::Tick(); }  // deck playback ring flush + lazy Deactivate/fin seam install + gen author
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:physmods"}; coop::physmods_sync::Tick(); }  // parked-canonical apply at desk resolve
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:drive"}; coop::drive_sync::Tick(); coop::drive_payload_sync::Tick(); }  // the slot lane's drain and sweep; the rows' enrolments
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:drive_rack"}; coop::drive_rack_sync::Tick(); }  // rack barrier drain + 1 Hz sweep
}

void OnDisconnect() {
    coop::desk_cursor_sync::OnDisconnect();
    coop::desk_ping_sync::OnDisconnect();
    coop::desk_sim_sync::OnDisconnect();
    coop::dish_sync::OnDisconnect();  // wire-residue sweep + the disher's restore (the suppression loan)
    coop::download_arm_sync::OnDisconnect();
    coop::dish_hashcode_sync::OnDisconnect();
    coop::sat_console_sync::OnDisconnect();  // the terminals it keeps, and a busy flag mirrored here
    coop::tape_caddy_sync::OnDisconnect();  // poll baselines + IsRecent stamps + the singleton cache (no suppression -- nothing to restore)
    coop::daily_task_sync::OnDisconnect();  // change-hash baseline
    coop::desk_input_sync::OnDisconnect();
    coop::desk_snd_fx::OnDisconnect();
    coop::deck_play_sync::OnDisconnect();  // gen counters + ring + self-test latch
    coop::physmods_sync::OnDisconnect();  // verb snapshots + parked canonical + deny records
    coop::drive_sync::OnDisconnect();  // slot baselines + latch/dirty state
    coop::drive_payload_sync::OnDisconnect();  // held and sent rows, notes
    coop::drive_rack_sync::OnDisconnect();  // rack baselines/shadow + deny/taken rings
}

}  // namespace coop::subsystems::desk
