// coop/desk_sim_sync.h -- the signal-desk download sim as a HOST-AUTHORITATIVE output stream.
//
// THE ROOT: the download-rate formula (AanalogDScreenTest) rolls TWO UNSEEDED RNG terms per tick,
// the detector needle DL_resDetecPercent and a transient noise, and integrates the filter offsets
// from per-peer frame dt, so the OUTPUTS diverge across peers even from identical knob inputs.
// Streaming them on the occupant-authored, claim-gated DeskState cannot fix that: unclaimed means
// no stream and self-divergence, and a client occupant would author shared-world RNG. Seeding is
// impossible against an unseeded roll plus transient noise, so the host owns the simulation and
// streams the output vector (DeskSimPose, about 10 Hz, newest-wins, interpolated like the cursor)
// and the client overwrites its own local sim, whose garbage the overwrite hides.
//
// The knob INTENTS (speeds, active, dir) stay occupant-authored on DeskState, the host applies
// them and its own blueprint integrates the offset, so this vector is host-down only: one author.
// frData and poData ride the vector rather than converging natively -- they read a filter-size
// upgrade that has no sync lane of its own.

#pragma once

namespace coop::net { class Session; }

namespace coop::desk_sim_sync {

void Install(coop::net::Session* session);

// Game thread, per pump tick. HOST: read the live sim outputs and publish through
// Session::SetHostDeskSim, whose net thread fans out DeskSimPose. CLIENT: drain the host's
// vector, interpolate PER CHANNEL -- each channel keeps its own deadline, and an unchanged
// target that ARRIVES snaps cur to target exactly, where a window shared across channels and
// reopened by every packet kept the detector's bitwise 1.0 from ever landing, so the client's
// own below-1.0 gated block beeped every frame -- then WriteSimOutputs, raw every tick for
// smoothness, with the full repaint pulsing at about 3 Hz. The vector is 7 channels;
// coord_cooldown belongs to desk_input_sync.
void Tick();

void OnDisconnect();

}  // namespace coop::desk_sim_sync
