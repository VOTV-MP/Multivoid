// coop/net/intent_queue_selftest.cpp -- the un-gated arithmetic selftest of coop/net/intent_queue.h, run once per
// session start: no honest run fills a queue or spends its budget, so the helper is proved here on pinned values.

#include "coop/net/intent_queue.h"

#include "ue_wrap/core/log.h"

namespace coop::net {

namespace {
struct Entry {
    int key;
    int val;
};
}  // namespace

bool RunIntentQueueSelftest() {
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("intent_queue selftest FAIL: %s", what);
    };
    const IntentBudget oneThenNone{1.0f, 0.0f};
    auto at1000 = [] { return uint64_t{1000}; };
    auto done = [](Entry&) { return Served::Done; };
    auto wait = [](Entry&) { return Served::Wait; };
    {
        IntentQueue<Entry> q;
        check(q.empty() && q.size() == 0, "a fresh queue is empty");
        check(q.Push(Entry{1, 10}, 2) && q.Push(Entry{2, 20}, 2), "pushes up to the cap are taken");
        check(!q.Push(Entry{3, 30}, 2) && q.size() == 2, "the push past the cap is refused and queues nothing");
    }
    {
        IntentQueue<Entry> q;
        q.Push(Entry{1, 10}, 4);
        q.Push(Entry{2, 20}, 4);
        q.Serve(oneThenNone, at1000, done);
        check(q.size() == 1, "a served Done leaves the queue");
        q.Serve(oneThenNone, at1000, done);
        check(q.size() == 1, "with no token a serve does nothing");
    }
    {
        IntentQueue<Entry> q;
        q.Push(Entry{1, 10}, 4);
        int seen = 0;
        q.Serve(oneThenNone, at1000, [&](Entry& e) { seen = e.val; return Served::Wait; });
        check(seen == 10 && q.size() == 1, "a Wait keeps the head");
        q.Serve(oneThenNone, at1000, done);
        check(q.empty(), "the Wait gave its token back: the same instant serves the head");
        q.Serve(oneThenNone, at1000, wait);
        check(q.empty(), "an empty queue serves nothing");
        int clockReads = 0;
        q.Serve(oneThenNone, [&] { ++clockReads; return uint64_t{1000}; }, done);
        check(clockReads == 0, "an empty queue does not read the clock");
    }
    {
        IntentQueue<Entry> q;
        q.Push(Entry{1, 10}, 4);
        q.Serve(oneThenNone, at1000, done);
        q = IntentQueue<Entry>{};
        q.Push(Entry{1, 10}, 4);
        q.Serve(oneThenNone, at1000, done);
        check(q.empty(), "a value-initialised instance is empty and its first Take succeeds");
    }
    if (pass == total) {
        UE_LOGI("intent_queue selftest: ALL PASS (%d checks)", total);
        return true;
    }
    UE_LOGE("intent_queue selftest: %d/%d checks passed", pass, total);
    return false;
}

}  // namespace coop::net
