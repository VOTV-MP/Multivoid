// coop/net/send_rate_control.cpp -- see coop/net/send_rate_control.h.

#include "coop/net/send_rate_control.h"

#include "ue_wrap/core/log.h"

#include <cstdio>

namespace coop::net {

void SendRateControl::Reset(bool controlEnabled, long pinnedKbs) {
    controlEnabled_ = controlEnabled;
    pinnedKbs_ = pinnedKbs;
    // Session::Start, with no net thread yet: the reset is performed here rather than armed, so a
    // session that never connects leaves nothing behind either.
    for (int i = 0; i < kSlots; ++i) {
        Slot& s = slots_[i];
        s.queuedBytes.store(0, std::memory_order_relaxed);
        s.unrelBytes.store(0, std::memory_order_relaxed);
        s.recvBytes.store(0, std::memory_order_relaxed);
        s.resetArmed.store(false, std::memory_order_relaxed);
        s.ClearPublished();
        s.m = Measured{};
        s.m.rateBps = s.m.rateWritten = StartRateBps();
    }
    costUs_ = 0;
    costPasses_ = 0;
    costLinks_ = 0;
    costWindowMs_ = 0;
    anyBusy_ = false;
    rateWriteUs_ = 0;
    rateWriteCount_ = 0;
}

int64_t SendRateControl::StartRateBps() { return kStartRateBps; }

int64_t SendRateControl::ServedBps(int slot) {
    if (slot < 0 || slot >= kSlots) return 0;
    // Through Take_, the one door to the measured block, so an armed teardown is performed here
    // exactly as it is for every other net-thread reader rather than read around.
    const Measured& m = Take_(slot);
    // Until the EWMA is warm neither guard in the law may bind, and the same holds here: an
    // estimate built from one or two samples would price a queue against noise.
    return (m.servedSamples >= kServedWarmupSamples) ? m.servedBps : 0;
}

void SendRateControl::FreeSlot(int slot) {
    if (slot < 0 || slot >= kSlots) return;
    // Arm only, and touch neither counter. Clearing them here would race the net thread between its
    // arm test and its own read of the totals -- two separate operations, which no fence can join --
    // and the sample in that window reads a zeroed total against a live window, i.e. a hugely
    // negative delivery. The whole reset happens on the net thread, in Take_.
    slots_[slot].resetArmed.store(true, std::memory_order_release);
}

SendRateControl::Measured& SendRateControl::Take_(int slot) {
    Slot& s = slots_[slot];
    if (s.resetArmed.exchange(false, std::memory_order_acquire)) {
        s.queuedBytes.store(0, std::memory_order_relaxed);
        s.unrelBytes.store(0, std::memory_order_relaxed);
        s.recvBytes.store(0, std::memory_order_relaxed);
        s.ClearPublished();
        s.m = Measured{};
        s.m.rateBps = s.m.rateWritten = StartRateBps();
    }
    return s.m;
}

void SendRateControl::NoteReliableQueued(int slot, int bytes) {
    if (slot < 0 || slot >= kSlots || bytes <= 0) return;
    slots_[slot].queuedBytes.fetch_add(static_cast<uint64_t>(bytes), std::memory_order_relaxed);
}

void SendRateControl::NoteUnreliableQueued(int slot, int bytes) {
    if (slot < 0 || slot >= kSlots || bytes <= 0) return;
    slots_[slot].unrelBytes.fetch_add(static_cast<uint64_t>(bytes), std::memory_order_relaxed);
}

void SendRateControl::NoteReliableReceived(int slot, int bytes) {
    if (slot < 0 || slot >= kSlots || bytes <= 0) return;
    slots_[slot].recvBytes.fetch_add(static_cast<uint64_t>(bytes), std::memory_order_relaxed);
}

void SendRateControl::NoteEchoRefused(int slot) {
    if (slot < 0 || slot >= kSlots) return;
    ++Take_(slot).echoesRefused;
}

int SendRateControl::RttMinFor_(const Measured& m, uint64_t nowMs) {
    const uint64_t sec = nowMs / 1000;
    int best = -1;
    for (int i = 0; i < kRttMinBuckets; ++i) {
        // An empty bucket has stamp 0; one older than the window is stale and ignored rather than
        // cleared, so no rotation pass is needed.
        if (m.rttBucketSec[i] == 0) continue;
        if (m.rttBucketSec[i] + kRttMinBuckets <= sec) continue;
        if (best < 0 || m.rttBucketMin[i] < best) best = m.rttBucketMin[i];
    }
    return best;
}

void SendRateControl::ExpireOutstanding_(Measured& m, uint64_t nowMs) {
    for (int i = 0; i < kMaxOutstanding; ++i) {
        if (m.out[i].token == 0) continue;
        if (nowMs - m.out[i].sentMs <= kProbeLostMs) continue;
        m.out[i].token = 0;
        ++m.probesLost;
    }
}

void SendRateControl::OpenWindow_(Measured& m, uint64_t nowMs, int64_t delivered, uint64_t recv) {
    m.windowMs = nowMs;
    m.windowDelivered = delivered;
    m.windowRecv = recv;
    m.peakBps = 0;
    m.sawTraffic = false;
    // The last round trip belongs to the window it was measured in. Carried across, a link that
    // stopped answering would keep republishing one reading as though it were fresh, and the
    // queueDelay derived from it with it.
    m.rttLastMs = -1;
    m.rttMinWindow = -1;
    m.rttMaxWindow = -1;
    m.rttSumWindow = 0;
    m.rttCountWindow = 0;
    m.probesSent = 0;
    m.probesReplied = 0;
    m.probesRefused = 0;
    m.probesLost = 0;
    m.probesUnknown = 0;
    m.probesSkipped = 0;
    m.echoesRefused = 0;
    m.climbCount = 0;
    m.brakeCount = 0;
    m.anchorCount = 0;
    m.deadCount = 0;
    m.holdCount = 0;
    m.flipCount = 0;
    m.rateWrites = 0;
}

int64_t SendRateControl::ServedPeak_(const Measured& m) {
    int64_t peak = 0;
    for (int64_t v : m.servedRing) {
        if (v > peak) peak = v;
    }
    return peak;
}

SendRateControl::LawKind SendRateControl::Steer_(Measured& m) {
    // THE LAW. Every term below is a byte count or a ratio of two byte counts, and one reference: the
    // link's base round trip, the minimum over per-minute minima of probes sent while this slot had
    // at most one packet in flight (BaseHistory). That is not the refuted family
    // (docs/send-path.md): those laws compared against their own LOADED past, so a standing queue
    // became the baseline; a queue of ours cannot enter this one, because a probe sent under load
    // is not admitted. RFC 6817 section 2.5 (LEDBAT) keeps the same per-minute history; libutp
    // keeps 13 minutes, we keep 10.

    // An idle link decides nothing. A rate is a CEILING, so with nothing queued it costs nothing to
    // leave where it is -- and a rate raised on an empty link is the rate the next burst opens at,
    // which is root A rebuilt by the thing meant to end it.
    if (m.demand < kIdleDemandBytes) { ++m.holdCount; return LawKind::Hold; }
    // NOT MEASURED, in either of the two ways that happens: an estimate younger than its own time
    // constant, or one that has decayed to nothing because the link went silent while work stayed
    // queued. There is one state here, and it holds the rate where it is.
    //
    // BOTH estimates are tested, and that is the point. `servedBps` is an arithmetic-shift EWMA
    // that reaches exactly 0, and a slot with bytes queued but nothing on the wire then reads
    // `unacked == 0` -> `inflightMs == 0` -> the CLIMB branch, where every guard below is a
    // multiple of zero: the rate climbs unopposed to the ceiling and the next burst opens there.
    // The two do not reach zero together either -- the ring is 4 samples and empties after 400 ms
    // of silence, while the EWMA sheds a quarter of itself per sample and takes about 4 s from a
    // quarter-megabyte -- so testing the EWMA alone leaves a gap in which a link that has delivered
    // nothing meets an anchor whose peak is 0 and is clamped to kFloorBps, the same defect
    // mirrored. A zero from EITHER means "not measured", and holds.
    const int64_t peak = ServedPeak_(m);
    if (m.servedSamples < kServedWarmupSamples || m.servedBps <= 0 || peak <= 0) {
        ++m.holdCount;
        return LawKind::Hold;
    }

    // Little's law, read backwards: the bytes the transport has on the wire and unacknowledged,
    // divided by the rate those bytes are being acknowledged at, is the time the wire is standing
    // in front of delivery. Both terms are reliable-only, which is the only way the division means
    // anything. An empty wire is no queue; a wire holding bytes while nothing is acknowledged is
    // the worst case there is, and is braked rather than left undefined.
    const int64_t unacked = static_cast<int64_t>(m.last.sentUnackedReliable);
    m.inflightMs = unacked <= 0 ? 0
                 : m.relBps > 0 ? unacked * 1000 / m.relBps
                                : kQueueBrakeMs * 2;
    // The queue is what the in-flight bytes cost beyond the link's own base round trip. In-flight
    // time CONTAINS the round trip, so at a relay's 190-420 ms the raw number reads as queue where
    // there is none; the thresholds compare the part above the base. Before a base exists the
    // queue is the raw in-flight time, which is the law as it shipped.
    const int baseMs = m.base.Min();
    m.queueMs = baseMs >= 0 ? (m.inflightMs > baseMs ? m.inflightMs - baseMs : 0) : m.inflightMs;

    // A STALL -- bytes in flight and nothing acknowledged -- brakes whatever the base says: the
    // inflight above is a sentinel there, and a slow path's base must not cancel the brake on a
    // dead link. The decision below short-circuits on it before the queue is compared.
    const bool stalled = unacked > 0 && m.relBps <= 0;
    m.stalled = stalled;

    LawKind kind;
    if (stalled || m.queueMs > kQueueBrakeMs) {
        kind = LawKind::Brake;
        // Braking goes straight to the measurement rather than to a fraction of the current rate:
        // a multiplicative back-off from a rate that is already hundreds of times too high takes
        // hundreds of decisions to arrive, and the link has been telling us its capacity all along.
        m.rateBps = m.servedBps;
        ++m.brakeCount;
    } else if (m.queueMs <= kQueueClimbMs) {
        kind = LawKind::Climb;
        m.rateBps = m.rateBps * kClimbNum / kClimbDen + kClimbStepBps;
        ++m.climbCount;
    } else {
        kind = LawKind::Dead;
        ++m.deadCount;
    }

    // THE ANCHOR, applied unconditionally to every decision including the ones that changed nothing.
    // A rate may not stand above a small multiple of what the link is actually carrying, whatever
    // the delay term says -- and on the archived run the delay term said "accelerate" for 180 of 210
    // seconds while this bound was 442x away. It is deliberately the last word, and it needs no
    // guard of its own because the hold above tests THIS peak, not merely the EWMA.
    //
    // It reads the windowed MAXIMUM, never the EWMA -- see kServedPeakSamples. A climbing link
    // delivers exactly what it is paced at, so an average that lags the climb reports a link half
    // as capable as it just proved itself to be, and the anchor would brake a link that is not
    // failing.
    const int64_t cap = peak * kOverdriveNum / kOverdriveDen;
    if (m.rateBps > cap) {
        m.rateBps = cap;
        ++m.anchorCount;
    }
    if (m.rateBps < kFloorBps) m.rateBps = kFloorBps;
    if (m.rateBps > kCeilingBps) m.rateBps = kCeilingBps;
    return kind;
}

void SendRateControl::FormatBase_(const Measured& m, char* buf, size_t cap) {
    const int base = m.base.Min();
    if (base < 0) snprintf(buf, cap, "-");
    else          snprintf(buf, cap, "%d ms", base);
}

const char* SendRateControl::LawName_(LawKind kind) {
    switch (kind) {
        case LawKind::Hold:  return "hold";
        case LawKind::Brake: return "brake";
        case LawKind::Climb: return "climb";
        case LawKind::Dead:  return "dead";
    }
    return "?";
}

void SendRateControl::BaseHistory::Note(int rttMs) {
    if (curMin < 0 || rttMs < curMin) curMin = rttMs;
}

int SendRateControl::BaseHistory::Min() const {
    int best = curMin;
    for (int i = 0; i < count; ++i) {
        if (best < 0 || hist[i] < best) best = hist[i];
    }
    return best;
}

void SendRateControl::BaseHistory::Roll(uint64_t nowMs) {
    if (minuteStartMs == 0) {
        minuteStartMs = nowMs;
        return;
    }
    if (nowMs - minuteStartMs < kBaseMinuteMs) return;
    // A minute with no admitted reply says nothing about the base and pushes nothing, so a link kept
    // busy for ten minutes keeps the last quiet minutes rather than a history of absences.
    if (curMin >= 0) {
        hist[next] = curMin;
        next = (next + 1) % kBaseHistMinutes;
        if (count < kBaseHistMinutes) ++count;
    }
    curMin = -1;
    minuteStartMs = nowMs;
}

bool SendRateControl::RunSelftest() {
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("send_rate_control selftest FAIL: %s", what);
    };
    BaseHistory b;
    check(b.Min() == -1, "no admitted sample: the base is -1");
    b.Roll(1000);
    check(b.minuteStartMs == 1000 && b.count == 0,
          "the first Roll starts the minute and pushes nothing");
    b.Note(60);
    check(b.Min() == 60, "one sample in the minute being filled is the base");
    b.Note(80);
    check(b.Min() == 60, "a larger sample does not move the minute's minimum");
    b.Note(40);
    check(b.Min() == 40, "a smaller sample does");
    b.Roll(1000 + kBaseMinuteMs - 1);
    check(b.count == 0 && b.curMin == 40, "a minute that has not run its 60 s is not closed");
    b.Roll(1000 + kBaseMinuteMs);
    check(b.count == 1 && b.curMin == -1 && b.Min() == 40, "a closed minute pushes its minimum");
    b.Roll(1000 + 2 * kBaseMinuteMs);
    check(b.count == 1 && b.Min() == 40,
          "a minute with no sample pushes nothing and keeps the base");
    b.Note(70);
    check(b.Min() == 40, "the base is the minimum over the history and the minute being filled");
    b.Note(25);
    check(b.Min() == 25, "and a smaller current minute lowers it");

    // The ring: one low minute, then minutes of 50. Ten pushes fill it; the eleventh overwrites the
    // low one, and only then does the base rise.
    BaseHistory r;
    uint64_t t = 5000;
    r.Roll(t);
    r.Note(5);
    t += kBaseMinuteMs;
    r.Roll(t);
    for (int i = 1; i < kBaseHistMinutes; ++i) {
        r.Note(50);
        t += kBaseMinuteMs;
        r.Roll(t);
    }
    check(r.count == kBaseHistMinutes && r.Min() == 5,
          "ten pushes fill the ring and the low minute is still in it");
    r.Note(50);
    t += kBaseMinuteMs;
    r.Roll(t);
    check(r.count == kBaseHistMinutes && r.Min() == 50,
          "the eleventh push drops the oldest minute");

    const bool ok = (pass == total);
    if (ok) UE_LOGI("send_rate_control selftest: PASS (%d/%d cases)", pass, total);
    else    UE_LOGE("send_rate_control selftest: FAIL (%d/%d cases)", pass, total);
    return ok;
}

int64_t SendRateControl::PendingRateWrite(int slot) {
    if (!controlEnabled_ || slot < 0 || slot >= kSlots) return -1;
    Measured& m = Take_(slot);
    if (m.rateBps <= 0 || m.rateBps == m.rateWritten) return -1;
    return m.rateBps;
}

void SendRateControl::NoteRateWritten(int slot, int64_t bps, uint64_t us) {
    if (slot < 0 || slot >= kSlots || bps <= 0) return;
    Measured& m = Take_(slot);
    m.rateWritten = bps;
    ++m.rateWrites;
    rateWriteUs_ += us;
    ++rateWriteCount_;
}

SendRateControl::Published SendRateControl::PublishedFor(int slot) const {
    Published out;
    if (slot < 0 || slot >= kSlots) return out;
    const Slot& s = slots_[slot];
    // The stamp first: every value store sequenced before its release is visible after this load.
    out.sampleMs = s.pubSampleMs.load(std::memory_order_acquire);
    out.deliveredBytes = s.pubDelivered.load(std::memory_order_relaxed);
    out.queueBytes = s.pubQueue.load(std::memory_order_relaxed);
    return out;
}

void SendRateControl::Sample(int slot, const LinkSample& in, uint64_t nowMs) {
    if (slot < 0 || slot >= kSlots) return;
    Measured& m = Take_(slot);
    Slot& s = slots_[slot];
    // Before the law, so the queue it computes reads the base as of this very sample.
    m.base.Roll(nowMs);

    const int64_t queued = static_cast<int64_t>(s.queuedBytes.load(std::memory_order_relaxed));
    const int64_t unrel = static_cast<int64_t>(s.unrelBytes.load(std::memory_order_relaxed));
    const uint64_t recv = s.recvBytes.load(std::memory_order_relaxed);
    // Delivery: our count of the bytes GNS took, less what it still holds. GNS keeps both pending
    // totals in its OWN units -- it grows each message by a 1 to 6 byte reliable-stream header before
    // counting it -- so this reads low by that header times the messages currently in flight. The
    // term is zero at quiescence and does not accumulate, but it moves with the in-flight message
    // COUNT, so a burst of small reliables perturbs a one-second reading by a few tens of KB where a
    // chunked blob perturbs it by a few hundred bytes. Bytes still in our own backlog were never
    // handed to GNS and appear in neither term; the admission exchange runs before a slot exists and
    // is not counted at all, a constant that differentiating removes.
    const int64_t delivered = queued - in.pendingReliable - in.sentUnackedReliable;
    m.demand = static_cast<int64_t>(in.pendingReliable) +
               static_cast<int64_t>(in.pendingUnreliable) +
               static_cast<int64_t>(in.backlogBytes);
    m.last = in;
    // Published before any return below, so the game thread's view is never older than this pass.
    // The delivered count is clamped monotone: the header bias makes a sample read low, and the read
    // order (the status before `queued`, which a send between them raises) can make one read high,
    // after which the clamp holds it flat until the true count passes; a reader comparing two must
    // never see delivery go backwards, and a plateau only delays a renewal. The stamp is `nowMs` as
    // the CALLER handed it, taken before its status read (Session's sampler; keep that order), so a
    // sample stamped after an event was read after it. Values first (relaxed), the stamp last (release).
    {
        const int64_t last = s.pubDelivered.load(std::memory_order_relaxed);
        s.pubDelivered.store(delivered > last ? delivered : last, std::memory_order_relaxed);
        s.pubQueue.store(static_cast<int64_t>(in.pendingReliable) +
                             static_cast<int64_t>(in.sentUnackedReliable) +
                             static_cast<int64_t>(in.backlogBytes),
                         std::memory_order_relaxed);
        s.pubSampleMs.store(static_cast<int64_t>(nowMs), std::memory_order_release);
    }
    // Busy in EITHER direction. A link with queued work of its own is busy, and so is one merely
    // delivering to us at megabytes a second: the receive counter is the second, independent
    // measurement a sender's goodput estimate gets checked against, and a downloading joiner has
    // nothing queued of its own at all.
    m.busy = m.windowMs != 0 &&
             (m.demand >= kIdleDemandBytes ||
              recv - m.windowRecv >= static_cast<uint64_t>(kIdleDemandBytes));
    if (m.busy) {
        m.sawTraffic = true;
        anyBusy_ = true;
    }

    // The two smoothed rates the law reads, and the peak, all differentiated over the SAME sample
    // interval. The law runs here, on the 10 Hz cadence, because a join has to climb inside its own
    // download -- a decision per reported second would spend the whole of a short transfer ramping.
    if (m.havePrev && nowMs > m.prevSampleMs) {
        const int64_t dtMs = static_cast<int64_t>(nowMs - m.prevSampleMs);
        const int64_t relBps = (delivered - m.prevDelivered) * 1000 / dtMs;
        // Unreliable is counted as OFFERED, not delivered: nothing acknowledges it. That is exact
        // for what this term is for -- knowing what the connection-wide rate is being spent on --
        // and it is the reason the anchor does not clamp an ordinary play session, whose traffic is
        // pose and voice and whose acknowledged delivery is therefore near zero.
        const int64_t unrelBps = (unrel - m.prevUnrel) * 1000 / dtMs;
        if (relBps > m.peakBps) m.peakBps = relBps;
        const int64_t servedSample = (relBps > 0 ? relBps : 0) + (unrelBps > 0 ? unrelBps : 0);
        if (m.servedSamples == 0) {
            // Seed from the first sample rather than letting the EWMA crawl up from zero, which
            // would hold a healthy link low for the length of the time constant.
            m.relBps = relBps > 0 ? relBps : 0;
            m.servedBps = servedSample;
        } else {
            m.relBps += ((relBps > 0 ? relBps : 0) - m.relBps) >> kServedEwmaShift;
            m.servedBps += (servedSample - m.servedBps) >> kServedEwmaShift;
        }
        // Only a sample that carried traffic counts toward warmth: a connection that sat quiet
        // before its transfer would otherwise arrive at the first chunk already "warm" on an
        // estimate of zero, which is the same cold-start brake by another route.
        if (servedSample > 0 && m.servedSamples < kServedWarmupSamples) ++m.servedSamples;
        // The anchor's window, kept raw. A quiet sample is recorded as the zero it is, so the peak
        // expires within the window rather than being propped up by a link that has stopped.
        m.servedRing[m.servedRingIdx] = servedSample;
        m.servedRingIdx = (m.servedRingIdx + 1) % kServedPeakSamples;
        const LawKind kind = Steer_(m);
        // A decision that differs from the previous one is a line of its own, with the quantities the
        // law saw, when it is an edge into or out of Brake or Hold: the per-second counts cannot say
        // WHEN the law changed its mind there. A link hovering on the climb threshold changes between
        // Climb and Dead at nearly every sample, so those changes are only counted (`flips=` on the
        // per-second line), and a link that stays in one kind prints nothing.
        if (kind != m.lastKind) {
            const bool edge = kind == LawKind::Brake || kind == LawKind::Hold ||
                              m.lastKind == LawKind::Brake || m.lastKind == LawKind::Hold;
            if (edge) {
                char base[24];
                FormatBase_(m, base, sizeof(base));
                UE_LOGI("send_rate[slot %d]: law %s -> %s at %llu ms inflight=%lld ms queue=%lld ms "
                        "base=%s rel=%lld B/s rate=%lld B/s",
                        slot, LawName_(m.lastKind), LawName_(kind),
                        static_cast<unsigned long long>(nowMs),
                        static_cast<long long>(m.inflightMs), static_cast<long long>(m.queueMs),
                        base, static_cast<long long>(m.relBps), static_cast<long long>(m.rateBps));
            } else {
                ++m.flipCount;
            }
            m.lastKind = kind;
        }
    }
    m.prevDelivered = delivered;
    m.prevUnrel = unrel;
    m.prevSampleMs = nowMs;
    m.havePrev = true;

    ExpireOutstanding_(m, nowMs);

    if (m.windowMs == 0 || nowMs < m.windowMs) {
        OpenWindow_(m, nowMs, delivered, recv);
        return;
    }
    const uint64_t dt = nowMs - m.windowMs;
    if (dt < 1000) return;

    const int64_t goodput = (delivered - m.windowDelivered) * 1000 / static_cast<int64_t>(dt);
    const int64_t recvBps =
        static_cast<int64_t>(recv - m.windowRecv) * 1000 / static_cast<int64_t>(dt);
    const int rttMin = RttMinFor_(m, nowMs);
    const int rttAvg = m.rttCountWindow > 0
                           ? static_cast<int>(m.rttSumWindow / m.rttCountWindow)
                           : -1;
    // How much of the last round trip is queue: the whole point of timing our own probe, since
    // GNS's min-filtered ping cannot show it.
    const int queueDelay = (m.rttLastMs >= 0 && rttMin >= 0) ? m.rttLastMs - rttMin : -1;

    // `over` is the number this arc exists to bound: the rate the transport is pacing at, over the
    // delivery the same line is reporting. The archived run that refuted the previous law ran a
    // median of 442 here while every other field on its line looked survivable, so it is printed
    // first among the law's fields and a field log can be read for it alone.
    const int64_t overPct = goodput > 0 ? m.last.gnsRateBps * 100LL / goodput : -1;

    // Quiet while the link is idle, plus one closing line on the falling edge so the last state of
    // a finished transfer is in the log.
    if (m.sawTraffic || m.wasBusy) {
        char base[24];
        FormatBase_(m, base, sizeof(base));
        UE_LOGI("send_rate[slot %d]: goodput=%lld B/s (peak %lld B/s) recv=%lld B/s "
                "rate=%lld B/s gnsRate=%d B/s over=%lld%% served=%lld B/s inflight=%lld ms "
                "base=%s queue=%lld ms stall=%d law climb=%d brake=%d anchor=%d dead=%d hold=%d "
                "flips=%d writes=%d "
                "rtt=%d ms win=%d/%d/%d (min/avg/max) rttMin=%d queueDelay=%d "
                "ping=%d probes sent=%d echo=%d refused=%d lost=%d stray=%d skipped=%d "
                "echoRefused=%d pendRel=%d unacked=%d pendUnrel=%d backlog=%zu "
                "queued=%lld unrel=%lld delivered=%lld recvd=%llu%s",
                slot, static_cast<long long>(goodput), static_cast<long long>(m.peakBps),
                static_cast<long long>(recvBps),
                static_cast<long long>(m.rateBps), m.last.gnsRateBps,
                static_cast<long long>(overPct), static_cast<long long>(m.servedBps),
                static_cast<long long>(m.inflightMs), base, static_cast<long long>(m.queueMs),
                m.stalled ? 1 : 0, m.climbCount, m.brakeCount, m.anchorCount, m.deadCount,
                m.holdCount, m.flipCount, m.rateWrites,
                m.rttLastMs,
                m.rttMinWindow, rttAvg, m.rttMaxWindow, rttMin, queueDelay, m.last.gnsPingMs,
                m.probesSent, m.probesReplied, m.probesRefused, m.probesLost, m.probesUnknown,
                m.probesSkipped, m.echoesRefused,
                m.last.pendingReliable, m.last.sentUnackedReliable, m.last.pendingUnreliable,
                m.last.backlogBytes, static_cast<long long>(queued),
                static_cast<long long>(unrel),
                static_cast<long long>(delivered),
                static_cast<unsigned long long>(recv),
                m.sawTraffic ? "" : " (idle)");
    }
    m.wasBusy = m.sawTraffic;
    OpenWindow_(m, nowMs, delivered, recv);
}

bool SendRateControl::NextProbe(int slot, uint64_t nowMs, LinkProbePayload& out) {
    if (slot < 0 || slot >= kSlots) return false;
    Measured& m = Take_(slot);
    // A busy link is probed ten times a second, an idle one once -- and the idle case is what gives
    // rttMin an UNLOADED reading to be the minimum of. Probing only under load would make every
    // sample a loaded one, and a minimum over loaded samples absorbs the very queue the baseline
    // exists to expose: the defect that disqualifies the transport's own min-filtered ping.
    const uint64_t interval = m.busy ? kProbeIntervalMs : kIdleProbeIntervalMs;
    if (m.lastProbeMs != 0 && nowMs - m.lastProbeMs < interval) return false;
    ExpireOutstanding_(m, nowMs);
    int free = -1;
    for (int i = 0; i < kMaxOutstanding; ++i) {
        if (m.out[i].token == 0) { free = i; break; }
    }
    // Every slot in flight and none answered: a further probe would ask nothing these have not
    // already asked. Counted, because on a link whose round trip outruns the ring's span the skip is
    // why the readings thin out.
    if (free < 0) {
        ++m.probesSkipped;
        return false;
    }
    do { ++m.probeCounter; } while (m.probeCounter == 0);
    m.out[free].token = m.probeCounter;
    m.out[free].sentMs = nowMs;
    // `m.last` is this very pass's status read: Sample ran just before, in the same net-thread
    // iteration (session.cpp), so it is the wire as this probe leaves.
    m.out[free].admitted = m.last.sentUnackedReliable <= kBaseAdmitUnackedBytes;
    m.lastProbeMs = nowMs;
    ++m.probesSent;
    out.token = m.probeCounter;
    return true;
}

void SendRateControl::NoteProbeRefused(int slot, uint32_t token) {
    if (slot < 0 || slot >= kSlots || token == 0) return;
    Measured& m = Take_(slot);
    for (int i = 0; i < kMaxOutstanding; ++i) {
        if (m.out[i].token != token) continue;
        m.out[i].token = 0;
        // It never went out, so it is not a probe this window sent; counted as refused instead.
        if (m.probesSent > 0) --m.probesSent;
        ++m.probesRefused;
        return;
    }
}

void SendRateControl::NoteProbeReply(int slot, const LinkProbePayload& reply, uint64_t nowMs) {
    if (slot < 0 || slot >= kSlots) return;
    Measured& m = Take_(slot);
    for (int i = 0; i < kMaxOutstanding; ++i) {
        if (m.out[i].token == 0 || m.out[i].token != reply.token) continue;
        // Both stamps come from one monotonic clock on one thread, so the difference is a duration
        // by construction and needs no guard; a guard here would also drop the probe out of every
        // counter and quietly stop the ledger from adding up.
        const uint64_t sentMs = m.out[i].sentMs;
        const bool admitted = m.out[i].admitted;
        m.out[i].token = 0;
        // `protocol.c:872` floors the sample at 1 ms before anything reads it; on loopback ours
        // genuinely measures 0, and an unfloored 0 would make the accelerate test trivially true.
        const int raw = static_cast<int>(nowMs - sentMs);
        const int rtt = raw < 1 ? 1 : raw;
        if (admitted) m.base.Note(rtt);
        m.rttLastMs = rtt;
        ++m.probesReplied;
        m.rttSumWindow += rtt;
        ++m.rttCountWindow;
        if (m.rttMinWindow < 0 || rtt < m.rttMinWindow) m.rttMinWindow = rtt;
        if (rtt > m.rttMaxWindow) m.rttMaxWindow = rtt;
        const uint64_t sec = nowMs / 1000;
        const int b = static_cast<int>(sec % kRttMinBuckets);
        if (m.rttBucketSec[b] != sec) {
            m.rttBucketSec[b] = sec;
            m.rttBucketMin[b] = rtt;
        } else if (rtt < m.rttBucketMin[b]) {
            m.rttBucketMin[b] = rtt;
        }
        // The law is NOT run here: a reply is recorded and reported, and the admitted ones (the
        // wire nearly empty as the probe left) fold into the base the law reads on its own 10 Hz
        // sample, so a reading moves no rate at the moment it arrives. It is the only true round
        // trip we can see -- the transport's own ping read 0 in every sample of both archived
        // runs -- and the cross-check that says whether the byte-derived `inflight` is telling
        // the truth about a real link.
        return;
    }
    // An echo for a token we do not have outstanding: a reply that came back after its probe was
    // retired as lost, or a peer echoing something we never minted. Counted, never measured.
    ++m.probesUnknown;
}

void SendRateControl::NoteSampleCost(uint64_t us, int links, uint64_t nowMs) {
    costUs_ += us;
    ++costPasses_;
    costLinks_ += links;
    if (costWindowMs_ == 0 || nowMs < costWindowMs_) {
        costWindowMs_ = nowMs;
        return;
    }
    if (nowMs - costWindowMs_ < 1000) return;
    if (anyBusy_ && costLinks_ > 0)
        // The rate WRITE is reported beside the read because only the read had ever been measured:
        // a config write per decision at 10 Hz per slot is a claim of its own.
        UE_LOGI("send_rate: %d pass(es) over %d link sample(s) in %llu us during the last second "
                "(%llu us per link sampled); %d rate write(s) in %llu us",
                costPasses_, costLinks_,
                static_cast<unsigned long long>(costUs_),
                static_cast<unsigned long long>(costUs_ / static_cast<uint64_t>(costLinks_)),
                rateWriteCount_, static_cast<unsigned long long>(rateWriteUs_));
    costUs_ = 0;
    costPasses_ = 0;
    costLinks_ = 0;
    costWindowMs_ = nowMs;
    anyBusy_ = false;
    rateWriteUs_ = 0;
    rateWriteCount_ = 0;
}

}  // namespace coop::net
