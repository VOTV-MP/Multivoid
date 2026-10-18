// coop/net/intent_queue.h -- one client's intents of one kind, queued on the host in arrival order and served one
// head per tick at the lane's budget: a client past its budget costs the host no work beyond a queue of `cap`
// entries, and what the lane cannot yet do (a target not ready, a line still being said) waits at the head without
// spending the budget. Past `cap` the push is refused, and the lane counts it and answers as it does today. One
// instance per slot and kind, inside the lane's PerSlotState, so the queue and its bucket clear with the occupant;
// there is no Clear. One thread per instance, as IntentBucket.

#pragma once

#include "coop/net/intent_bucket.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>

namespace coop::net {

// What a served head did: Done spends its token and leaves the queue, Wait gives the token back and stays.
enum class Served : uint8_t { Done, Wait };

// Source drops a client's excess past a token budget: when a client's pending command ticks pass
// sv_maxusrcmdprocessticks the server runs a copy of the last command and clamps the rest to the budget (Source SDK
// 2013, player.cpp:3392-3414); the command past the allowance is dropped unrun, with a throttled warning
// (player_command.cpp:318-335). MTA counts a player's client-triggered events per interval and raises a script event
// past a threshold, running each event meanwhile (CGame.cpp:2663-2711, 4927-4976). Ours queues up to `cap` and
// refuses past it, because the transport releases an honest client's held output at once (an honest burst of 54
// fresh-birth props in 2 s was measured), and a drop would lose honest intents. A sender sitting at the cap is
// refused at no cost to the host and escalates nowhere; the receive edge's budget (receive_budget.h) is what ends a
// flooding link.
template <class T>
class IntentQueue {
public:
    // Queues `v` behind the others: false when `cap` entries are already queued (nothing is queued then).
    bool Push(T&& v, size_t cap) {
        if (q_.size() >= cap) return false;
        q_.push_back(std::move(v));
        return true;
    }

    // At most one head, at `now()`, a steady clock's milliseconds read only once the queue holds something: nothing
    // when empty; else one token is taken, and with it `fn(head)` answers Done (the head leaves) or Wait (the token
    // returns, the head stays for the next call).
    template <class NowFn, class Fn>
    void Serve(const IntentBudget& b, NowFn&& now, Fn&& fn) {
        if (q_.empty()) return;
        if (!bucket_.Take(b, now())) return;
        if (fn(q_.front()) == Served::Done) {
            q_.pop_front();
        } else {
            bucket_.Refund(b);
        }
    }

    size_t size() const { return q_.size(); }
    bool   empty() const { return q_.empty(); }

private:
    std::deque<T> q_;
    IntentBucket  bucket_;
};

// The queue's arithmetic: the cap's refusal, no token serving nothing, Done and Wait, and a value-initialised
// instance. Logs ALL PASS or each failure.
bool RunIntentQueueSelftest();

}  // namespace coop::net
