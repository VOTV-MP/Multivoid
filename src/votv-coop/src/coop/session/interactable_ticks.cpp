// coop/session/interactable_ticks.cpp -- see coop/session/interactable_ticks.h.

#include "coop/session/interactable_ticks.h"

#include "coop/dev/perf_probe.h"
#include "coop/interactables/comp_sync.h"
#include "coop/interactables/deck_play_sync.h"
#include "coop/interactables/desk_input_sync.h"
#include "coop/interactables/desk_sim_sync.h"
#include "coop/interactables/desk_snd_fx.h"
#include "coop/interactables/dish_hashcode_sync.h"
#include "coop/interactables/dish_sync.h"
#include "coop/interactables/download_arm_sync.h"
#include "coop/interactables/drive_payload_sync.h"
#include "coop/interactables/drive_rack_sync.h"
#include "coop/interactables/drive_sync.h"
#include "coop/interactables/meadow_db_sync.h"
#include "coop/interactables/physmods_sync.h"
#include "coop/interactables/sat_console_sync.h"
#include "coop/interactables/signal_sync.h"
#include "coop/interactables/tape_caddy_sync.h"
#include "coop/interactables/window_stroke_sync.h"
#include "coop/interactables/window_sync.h"
#include "coop/items/coingun_sync.h"
#include "coop/items/order_sync.h"
#include "coop/voice/voice_chat.h"
#include "coop/world/daily_task_sync.h"
#include "coop/world/email_sync.h"
#include "ue_wrap/core/walk_timer.h"

namespace coop::interactable_ticks {

void Tick() {
    namespace PP = coop::dev::perf_probe;
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
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:email"}; coop::email_sync::Tick(); }  // email shadow poll (1 Hz; appends -> chunked broadcast, shrinks -> content-keyed deletes)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:signal"}; coop::signal_sync::Tick(); }  // saved-signals shadow poll (same shape on gamemode.savedSignals_0)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:meadow"}; coop::meadow_db_sync::Tick(); }  // meadow-DB: the writers' watches until settled, the pen's drain once the database is back, then retries (1 Hz) while a line, a tombstone or an owed order waits
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:comp"}; coop::comp_sync::Tick(); }  // the refiner: the host's stream and comp_data edges, the mirror's world edges
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:voice"}; coop::voice_chat::Tick(); }  // voice frame pump (mic drain -> send; inbox -> jitter; positions; state edges)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:order"}; coop::order_sync::Tick(); }  // drone economy: host commits assembled client orders; the queue mirror's retry
    // The barrier for coingun_sync: the client's own coins are captured inside the gun's verb
    // bracket (reads only; an engine call mid-bytecode corrupts) and destroyed here, one pump tick
    // later, where a dispatch is safe.
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:coingun"}; coop::coingun_sync::Tick(); }
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:window"}; coop::window_sync::Tick(); }  // base-window clean: deferred-apply retry (wipes go at the verb)
    { PP::Scope _s{PP::Bucket::Interactable}; ue_wrap::ScopedWalkTimer _w{"sync:window_stroke"}; coop::window_stroke_sync::Tick(); }  // bay-window dabs: send observed, replay received
}

}  // namespace coop::interactable_ticks
