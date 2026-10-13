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
    bool Take(const IntentBudget& budget, uint64_t nowMs) {
        if (lastMs_ == 0) {
            tokens_ = budget.burst;
        } else if (nowMs > lastMs_) {
            tokens_ += budget.perSecond * static_cast<float>(nowMs - lastMs_) / 1000.0f;
            if (tokens_ > budget.burst) tokens_ = budget.burst;
        }
        lastMs_ = nowMs;
        if (tokens_ < 1.0f) return false;
        tokens_ -= 1.0f;
        return true;
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
    float    tokens_ = 0.0f;
    uint64_t lastMs_ = 0;
};

}  // namespace coop::net
