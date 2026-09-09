// coop/dev/drive_selftest.h -- [dev] drive_selftest: the rack-lane end-to-end
// circles.
//
// Standalone INSTRUMENT for the extraction's digest-equality measurement: uses
// ONLY public APIs (ue_wrap drive_chain, signal_wire, blob_chunks Fnv64,
// element Registry) so the extraction commit cannot touch it. Host circle:
// organic WriteRackRow+CallRackGen -> canonical -> client adopt. Client
// circle: local write outside any guard -> SweepRacks derives ops -> host
// apply -> canonical back. Digest = Fnv64 over the serialized rack rows read
// directly via ReadRack -- no eid/seq/timestamps in the digest by
// construction; inject content = fixed constants.
//
// No-op unless [dev] drive_selftest=1.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::drive_selftest {

void Install(coop::net::Session* session);
void Tick();

}  // namespace coop::dev::drive_selftest
