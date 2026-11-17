// coop/net/intent_bucket.h -- a client's budget of intents on the host: a token bucket per sender slot, the bound the
// lanes that take a client's presses and verbs put on it, so a client past its budget costs the host no work, no log
// line and no answer per message; the receive edge's bound on every message holds one too (receive_budget.h). One
// thread per instance: the lanes' on the game thread, the edge's on the net thread.

#pragma once

#include <cstdint>

namespace coop::net {

// A lane's budget: `burst` intents at once, refilled at `perSecond`.
struct IntentBudget {
    float burst;
    float perSecond;
};

// One sender's bucket, full until its first intent. Reset makes it full again, for the slot's next occupant.
class IntentBucket {
public:
    // One intent at `nowMs`, a steady clock's milliseconds: true when the bucket holds a token, which it spends.
    bool Take(const IntentBudget& budget, uint64_t nowMs) { return TakeN(budget, nowMs, 1.0f); }

    // An intent that costs `cost` tokens (a request for ten props costs ten): true when the bucket holds that many,
    // which it spends; otherwise nothing is spent. A cost above the burst can never be taken.
    bool TakeN(const IntentBudget& budget, uint64_t nowMs, float cost) {
        Refill(budget, nowMs);
        if (tokens_ < cost) return false;
        tokens_ -= cost;
        return true;
    }

    // One intent paid into debt: a token is spent even when the bucket holds none, down to `maxDebt` tokens below
    // zero, which the refills then pay before any Take succeeds. False, nothing spent, past that.
    bool Charge(const IntentBudget& budget, uint64_t nowMs, float maxDebt) {
        Refill(budget, nowMs);
        if (tokens_ - 1.0f < -maxDebt) return false;
        tokens_ -= 1.0f;
        return true;
    }

    // The time, in `nowMs`'s clock, at which the bucket is full again (`nowMs` itself when it is full now). A client's
    // menu publishes it, and any thread works out how many tokens the bucket holds at a later time from it alone.
    uint64_t FullAtMs(const IntentBudget& budget, uint64_t nowMs) {
        Refill(budget, nowMs);
        if (tokens_ >= budget.burst || !(budget.perSecond > 0.0f)) return nowMs;
        return nowMs + static_cast<uint64_t>((budget.burst - tokens_) / budget.perSecond * 1000.0f) + 1;
    }

    // A taken token back: the intent it paid for ran nothing.
    void Refund(const IntentBudget& budget) {
        tokens_ += 1.0f;
        if (tokens_ > budget.burst) tokens_ = budget.burst;
    }

    void Reset() {
        tokens_ = 0.0f;
        lastMs_ = 0;
    }

private:
    void Refill(const IntentBudget& budget, uint64_t nowMs) {
        if (lastMs_ == 0) {
            tokens_ = budget.burst;
        } else if (nowMs > lastMs_) {
            tokens_ += budget.perSecond * static_cast<float>(nowMs - lastMs_) / 1000.0f;
            if (tokens_ > budget.burst) tokens_ = budget.burst;
        }
        lastMs_ = nowMs;
    }

    float    tokens_ = 0.0f;
    uint64_t lastMs_ = 0;
};

}  // namespace coop::net
