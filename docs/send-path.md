# The send path

## Purpose

How fast the mod sends on each connection, who may put reliable bytes into a connection's send
buffer, and what happens to a send those rules hold back. The transport offers no bandwidth
estimate of its own, so every link a peer owns is measured and paced by the mod. This is the detail
under [architecture.md](architecture.md)'s transport section; the join's own download is in
[join.md](join.md).

## How it works

### The transport contributes no estimate

GameNetworkingSockets writes a connection's send rate once at connect, from the ping, and
thereafter only clamps it into the configured minimum and maximum. That opening estimate is an
initial window of 4,380 bytes divided by the round trip, so it falls under the transport's own
256 KB/s stock floor at about seventeen milliseconds of ping, and a link past that runs at the
configured floor for its whole life whatever it can actually carry. Where the minimum and the
maximum are written to one value -- which is how an application says it owns the rate, and what the
mod does on every connection -- the clamp takes the "bandwidth estimation disabled" branch instead
and the ping-derived guess is never consulted at all.

Nothing writes a rate globally. `coop/net/send_rate_control` is the sole source of a steering
decision, one per connection; `coop/net/connection_tuning` makes the only other write, opening each
link at the start rate, or at a drill's pin where one is set. The 1 MiB/s global floor that used to
stand under every connection is gone, on two measurements: it was the rate rather than a floor on
every link a player has -- 63,052 of 63,052 distinct field samples read it to the byte -- and a
host uplink thinner than it was overdriven into pure loss, costing 71% of a 256 KB/s link. Its
second premise, that a raised floor shields the unreliable pose stream from a reliable burst, was
wrong in the same runs. Nothing drops a queued unreliable message; what starves one is the shared
send buffer, and a reliable retransmission is gathered ahead of the lane-priority loop, so
overdrive is precisely what puts a bulk retry in front of the pose datagram. The 25 MiB/s ceiling
that stood with the floor went for a plainer reason: reaching it needed an init ping under
0.17 ms, and no measurement in the arc came near that.

### Two byte counters, and no reference that can drift

Each slot is measured on two quantities, both byte counters:

- **served** -- bytes the peer acknowledges, plus unreliable bytes handed over, per second. A queue
  cannot inflate it: no amount of buffering makes a link deliver more than it can.
- **inflight** -- unacknowledged reliable bytes divided by the rate those bytes are being
  acknowledged at. Little's law read backwards: the milliseconds of delivery standing on the wire.
  Both terms are reliable-only, which is the only way the division means anything.

A round trip is timed as well, on the high lane the pose stream rides. A busy link is probed ten
times a second and reports once; an idle one is probed once a second with the same 32-byte
datagram, so a baseline is never taken only under load. A probe sent while at most one packet of
ours was on the wire is admitted to the link's **base** round trip, the minimum over the last ten
one-minute minima (the per-minute history of RFC 6817 section 2.5). The **queue** the law reads is
the inflight time above that base: at a relay's 190-420 ms the raw inflight would read as queue
where there is none.

What the law compares inflight against is a reference a queue of ours cannot enter: a probe sent
under load is not admitted, so the base never learns a standing queue. Two laws built before this
one compared against a reference derived from their own loaded past, and both were refuted on that
property: a delay-versus-baseline rule of the LEDBAT family, and a port of ENet's peer throttle,
which judges against the previous interval's lowest smoothed round trip
(`reference/enet/protocol.c:908-911`). A standing queue moves that reference,
so "better than baseline" stops meaning anything exactly when it starts to matter. The ENet port
was measured doing it: the baseline rose from 18 ms to a median of 1,545 ms, its accelerate branch
turned true of nearly every reading, and the rate ratcheted to the ceiling for 180 of 210 seconds.
Read against the same archived run, the byte terms separate with no overlap -- healthy inflight ran
a median of 60 ms (p90 163) and the collapsed regime a median of 13,055 (p10 1,458), while the rate
the delay law applied stood a median of 442 times above the delivery printed on the same line.

### The law

One decision per sample, ten times a second, so a join can climb inside its own download:

| Branch | When | What it does |
|---|---|---|
| hold | nothing queued, or delivery not measured yet | leaves the rate alone |
| brake | queue above 400 ms (inflight above the base), or a wire holding reliable bytes while nothing at all is being acknowledged, whatever the base | drops straight to the measured delivery |
| climb | queue at or under 150 ms | 5/4 of the rate plus 8 KB/s |
| dead band | between the two | leaves the rate alone, because a controller with no dead band hunts |
| anchor | after the above, whenever there is delivery to measure | a rate may not stand above 5/4 of the largest recent delivery reading |
| envelope | last, and unconditionally | clamps the result into the floor and ceiling below, so on a link under about 26 KB/s the floor stands above what the anchor just allowed |

The thresholds are the regimes above, measured as inflight on 5-17 ms links where all of it was
queue, and read as queue (inflight above the base) so they mean the same on a slow path: the climb
stops where health stopped (p90 163 ms) and the
brake fires an order of magnitude under the collapse (p10 1,458 ms), so a threshold between them
flags 93.4% of the collapse and none of the health. The brake goes to the measurement rather than
to a fraction of the current rate, because a multiplicative back-off from a rate hundreds of times
too high takes hundreds of decisions to arrive.

The anchor is the guard that would have stopped both refuted laws on its own, and its margin above
1.0 is paid for in loss: pacing at k times what a link carries offers a share of every packet to a
bottleneck that cannot take it. At k = 2 on the drop-policed rig the goodput held at 95.3% of
nominal while the peer reported `qual=51/100`, about half of what we sent needing retransmission --
and a reliable retry outranks the pose datagram, so that is not free. 5/4 is the probe margin BBR
uses and the step the climb takes.

The anchor bounds against the largest recent delivery reading and never against the smoothed one,
and that is a measured constraint rather than a preference: while a link is climbing, delivery
equals the rate and the smoothed estimate lags it by about half, so an anchor fed by it clamps
below a rate that is already succeeding and fights its own climb. Measured, an unpoliced join went
from 5 s under no control at all to 13 s under a law wired that way, never reaching the ceiling.

Neither estimate is trusted before its own time constant has passed. The first sample of a transfer
is the worst case there is -- the save pump has already filled the buffer while the delivery
estimate is one reading old -- and without that warm-up the opening second took one spurious brake
to the floor on a link with megabytes of headroom.

A link opens at 145 KB/s, about 1% of the ceiling: measured reaching the ceiling in three seconds
where the headroom exists, and inside a thin link's capacity rather than the 17x outside it that
was measured costing 71% of a thin uplink. The exact value is the rung the refuted ladder happened
to open on, kept because two runs measured it and nothing argues for moving it. The envelope runs
from 32 KB/s -- an honest minimum, where a join is slow rather than broken -- to the save pump's
own output, which is the largest producer in the tree; a rate above that paces nothing that exists.

### Who may fill the buffer

The transport admits a message only while a connection's pending bytes plus that message fit the
send buffer, and that sum is connection-wide: a bulk stream at the brim refuses the pose and voice
datagrams beside it, and the probe that times the path they ride. So `coop/net/send_admission`
bounds the mod's own contribution to that brim -- every reliable send but the probe pair and the
join beacon stops short of a 64 KB reserve, or a quarter of the buffer where that is smaller. The
beacon is exempt for the reason the rule exists: the buffer is at the brim exactly while the world
blob streams, which is when a joiner most needs to hear from the host. It is not a guarantee
of room: the transport re-queues a NACKed segment with no admission check, so on a lossy link
pending passes the buffer size from outside while the mod adds nothing.

A send the rule holds back is never lost. It enters the per-lane backlog (`coop/net/send_backlog`),
or it is the save pump's pacing signal, which is what a full buffer already meant to it.

### The second bound, in time

The byte rule alone bounds a queue whose drain rate varies by more than an order of magnitude: read
as pending reliable bytes over the send rate, at shipped defaults, the same 4 MiB brim measured
5.6 seconds of queue on one policed joiner and 125.96 seconds on a slot losing a three-way race for
a single uplink. Bytes inside the transport's buffer can be neither
retracted nor reordered, so that is how long a cancelled transfer keeps spending a host's uplink,
and how far behind everything queued after it starts.

The send path with no queue behind it -- where a refusal is the producer's pacing signal -- is
therefore admitted only while this slot's queue is under two seconds of the link's own delivery.
Measured across a bulk transfer, the queue's median fell from 7.97 to 1.98 seconds and its maximum
from 28.12 to 3.23, at no cost in throughput (96.7% against 96.9% of nominal, with the delivery
window unchanged); three joiners contending on one machine fell from 57.81 to 2.21.

The divisor is the delivery the mod measures, never the rate the transport is paced at. With the
law on, the transport returns the mod's own last written rate through its equal-bounds branch,
which on a policed link stood 1.29 times above what that link carried -- a budget built on it
admitted 2.57 seconds while its name said two, and the metric checking it divided by the same
inflated figure.

The backlog path keeps the byte rule alone, and deliberately -- not because moving bytes into our
own queue would be wrong, since reordering them by lane is exactly what that backlog is for, but
because its producers are one-shot bursts against a sustained stream. The whole join burst measures
about 740 KB, while the world blob is 17 MB offered at 13.1 MB/s, and only the blob was ever
measured holding a buffer for two minutes.

MTA's precedent for the time bound is `CLatentSendQueue::DoPulse`
(`reference/mtasa-blue/Shared/mods/deathmatch/logic/CLatentSendQueue.cpp:46-69`), where a bulk
transfer hands the transport a rate times an elapsed time per pulse and keeps the remainder in its
own buffer. Deliberate divergence: MTA has to integrate because RakNet publishes no queue depth,
while GameNetworkingSockets does, so the mod bounds the depth directly and has nothing to drift.

## Who owns what

| State | Owner |
|---|---|
| The rate a connection is paced at | the peer that sends on it. Nothing is negotiated, and a peer never paces another peer's link |
| The delivery and round trip of a link | the sending end, which measures both; the receiving end counts the same reliable bytes independently, so the estimate is checkable against a second measurement |
| Admission into a send buffer | the sender, per slot, re-anchored on the transport's own pending total at each link sample |
| The save pump's pacing | the send path's refusals, which the pump reads as backpressure rather than as an error |

## Wire messages

One pair, both on the high lane: a link probe carrying a token, four bytes of it, and its echo.
The round trip is timed against the sender's own record of when that token went out, and the wire
carries no time at all, so a wrong or hostile echo has nothing to invent one from. A responder that
cannot
put an echo on the wire -- the send buffer at the brim -- says so, because to the prober a refused
echo and a lost probe otherwise look identical.

## Late join

A joining peer's link opens at the start rate and is measured from zero. A slot's byte counters are
armed for reset at teardown and cleared by the net thread at its next touch of the slot, so the
next occupant of a slot inherits no byte debt from the last one; the transport's own pending
counters restart with the connection in the same way.

## Known limits

| Limit | Evidence |
|---|---|
| A rate is a ceiling, not a reservation: two peers sharing one uplink are paced by what each measures, and neither knows about the other | `[V]` `coop/net/send_rate_control`, measured on three contending joiners |
| The time bound prices bytes against the delivery measured at the moment they are committed, and cannot retract them; a rate that collapses inside a second leaves a tail | `[V]` `coop/net/send_admission` |
| The occupancy compared is an estimate. It reads high when a drain went unseen, which is the safe direction, and low by a bounded amount -- the stream header the transport adds after its own check, and the skew between two loads -- until the next anchor corrects both | `[V]` `coop/net/send_admission` |
| A link that genuinely is slow is still a long download; the mod can only decline to make it worse | `[V]` `coop/net/send_rate_control` |
| The round trip enters the law only as the base that inflight is read above. It rides the high lane, which the scheduler serves ahead of the bulk queue a transfer stands in, so a probe under-reports what that transfer is waiting behind; that is why only probes sent with the wire nearly empty count toward the base | `[V]` `coop/net/send_rate_control` |

## Code map

| File | What it owns |
|---|---|
| `coop/net/send_rate_control` | the measurement of each link, the law above, and the rate the session writes to a connection |
| `coop/net/send_admission` | both bounds on entering a send buffer: the byte reserve, and the time budget on the unqueued path |
| `coop/net/send_backlog` | where a reliable send the byte rule refuses waits, by lane |
| `coop/net/connection_tuning` | the connect-time write: lanes, buffer size, and the rate a link opens at |
| `coop/net/session_start` | transport init, and the drill's global link policer |

Knobs, all off or default in a shipped build: `net.ratecontrol` turns the law off and leaves every
link at the transport's own stock rate, which is a control arm rather than a fallback;
`net.sendrate_kbs` pins a fixed rate; `net.sendbuf_kb` sets the buffer the bounds above are
measured against; `net.fakelink_kbs` simulates a thin uplink with the transport's own packet
policer; and the developer row `bulk_queue_cap_ms` sets the time budget, where 0 disables it.
Their shapes are in [testing.md](testing.md).
