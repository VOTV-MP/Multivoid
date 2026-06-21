// coop/dev/perf_probe.h -- MEASURE-FIRST frame-cost probe for the 15-FPS audit.
//
// The whole game runs at ~15 FPS on BOTH host and client. That points at a
// SHARED hot path our DLL adds. The 2026-06-04 whole-project audit FALSIFIED the
// "per-dispatch detour substrate (mutex + 73-observer walk) = 50 ms" hypothesis
// using the codebase's own ~100k PE/sec figure (=> ~0.1 ms/frame, not 50 ms), and
// pointed instead at a HOT per-tick caller / observer-callback BODY doing an
// uncached reflection Find*/CountObjectsByClass (a ~1M-entry GUObjectArray walk +
// a wstring alloc per entry) -- one such call per frame is the whole 50 ms budget.
// So we MEASURE before touching code. This probe answers, once per second:
//   * how many ProcessEvent dispatches/frame (game-thread vs worker split),
//   * how many ns our DETOUR body spends per dispatch (sampled, engine excluded),
//   * how long EACH net_pump subsystem Tick takes (ms/frame),
//   * the single worst observer/interceptor cb BODY (ms + its UFunction name)
//     -- this is what catches an observer secretly walking GUObjectArray.
//
// Dev-only (RULE 3), ini-gated `perf_probe=1` (+ `perf_probe_selftime=1` to arm
// the 1/256-sampled detour self-timer). OFF (shipping default) the only steady-
// state cost is ONE relaxed atomic-bool load per dispatch. Nothing here ships.
//
// Output (logged ~1 Hz from net_pump::Tick via Sample()):
//   [perf] PE=<n>/s (GT=<g> wk=<w>) frames=<f>/s => PE/frame=<n/f> GT/frame=<g/f>
//   [perf] detour self avg=<ns>/dispatch (<m> samp) => ~<x.x> ms/frame
//   [perf] obs/intc body total=<x.x> ms/frame worst='<Fn>' <y.y> ms | post=<p> pre=<q> intc=<r>
//   [perf] net_pump::Tick=<x.x> ms/frame | interactable=.. weather=.. remoteProp=.. reaper=.. ...

#pragma once

namespace coop::dev::perf_probe {

// Named subsystem buckets timed inside net_pump::Tick (and the harness post).
// Keep in sync with the kBucketNames table in perf_probe.cpp.
enum class Bucket {
    NetPumpTick,    // the whole net_pump::Tick body
    Reaper,         // the prop-element reaper + world-change reseed block
    LocalSend,      // ReadLocalPose + held-prop + ragdoll stream
    InstallObs,     // InstallObservers fan-out
    Interactable,   // interactable_sync::Tick (door/light/container poll)
    WeatherConnect, // weather_sync::TickConnect
    ItemConnect,    // item_activate::TickConnect
    TrashWatch,     // host_spawn_watcher::TickWatchedProps (ambient-prop despawn) + kerfur/stick drains etc.
    Balance,        // balance_sync::Tick
    SnapshotDrain,  // prop_snapshot::DrainChunk
    RemoteProp,     // remote_prop::Tick
    Puppets,        // per-slot pose drive + RemotePlayer::Tick loops
    EventFeed,      // event_feed::Update
    Nameplate,      // nameplate::Update (harness post)
    Roster,         // roster::Refresh (harness post)
    Count
};

// True iff [perf_probe]=1 in votv-coop.ini (read once). Init() consults this.
bool Enabled();

// Cached "is the probe armed" flag (set by Init). Callers gate QPC on this so the
// timing is free when the probe is off.
bool Armed();

// QPC value now (raw ticks). Used by Scope; exposed for manual bracketing.
unsigned long long NowTicks();

// Accumulate a raw-QPC-tick duration into a subsystem bucket. No-op when off.
void AddTicks(Bucket b, unsigned long long ticks);

// RAII subsystem timer: times its lifetime into bucket `b` when armed; ~free off.
// Define one at the top of the scope to measure, e.g. `perf_probe::Scope s{Bucket::Interactable};`.
class Scope {
public:
    explicit Scope(Bucket b);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    Bucket b_;
    bool on_;
    unsigned long long t0_;
};

// Idempotent. When the ini flag is on, arms the detour's dispatch counter (and the
// sampled self-timer if perf_probe_selftime=1) via game_thread. Safe to call every
// net-pump tick. Game thread.
void Init();

// Count one presented frame. Called from the ImGui Present detour BEFORE the
// AnyOpen() gate so it counts every rendered frame, overlay shown or not.
void NoteFrame();

// Called once per net-pump tick (~125 Hz). Self-throttles to ~1 Hz: snapshots the
// detour counters + frame counter + subsystem buckets, computes per-second rates +
// per-frame costs, and logs. No-op when the probe is disabled. Game thread.
void Sample();

}  // namespace coop::dev::perf_probe
