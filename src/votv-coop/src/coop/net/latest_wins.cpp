// coop/net/latest_wins.cpp -- see coop/net/latest_wins.h.

#include "coop/net/latest_wins.h"

#include "ue_wrap/core/log.h"

namespace coop::net {

bool LatestWins::RunSelftest() {
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("latest_wins selftest FAIL: %s", what);
    };
    const IntentBudget budget{2.0f, 1.0f};
    {
        LatestWins w;
        check(!w.Owed(), "a fresh instance owes nothing");
        check(w.Offer(budget, 1000) && !w.Owed(), "a first value is applied at once");
        check(w.Offer(budget, 1000), "the burst's second too");
        check(!w.Offer(budget, 1000) && w.Owed(), "past the burst the value is owed");
        check(!w.Due(budget, 1500) && w.Owed(), "half a token pays nothing and the debt stays");
        check(w.Due(budget, 2000) && !w.Owed(), "a refilled token pays the debt");
        check(!w.Due(budget, 9000), "with nothing owed the payment pays nothing, tokens or not");
    }
    {
        LatestWins w;
        w.Offer(budget, 1000);
        w.Offer(budget, 1000);
        w.Offer(budget, 1000);
        check(!w.Offer(budget, 1500) && w.Owed(), "a newer value while one is owed and no token: still owed");
        check(w.Offer(budget, 2500) && !w.Owed(), "a newer value with a token: applied, nothing owed");
    }
    {
        LatestWins w;
        w.Offer(budget, 1000);
        w.Offer(budget, 1000);
        w.Offer(budget, 1000);
        w.Withdraw();
        check(!w.Owed() && !w.Due(budget, 9000), "a withdrawn debt is not paid");
        check(w.Offer(budget, 9000), "and the bucket it left still pays a later change");
    }
    {
        LatestWins w;
        w.Offer(budget, 1000);
        w.Offer(budget, 1000);
        w.Offer(budget, 1000);
        w = LatestWins{};
        check(!w.Owed() && w.Offer(budget, 1000), "a value-initialised instance owes nothing and holds its burst");
    }
    if (pass == total) {
        UE_LOGI("latest_wins selftest: ALL PASS (%d checks)", total);
        return true;
    }
    UE_LOGE("latest_wins selftest: %d/%d checks passed", pass, total);
    return false;
}

}  // namespace coop::net
