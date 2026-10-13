// coop/net/apply_pause_selftest.cpp -- the un-gated arithmetic selftest of coop/net/apply_pause.h, run
// once per session start: no honest run raises a pause, so the set's idempotence and its word edges are
// proved here on pinned kinds.

#include "coop/net/apply_pause.h"

#include "ue_wrap/core/log.h"

#include <cstdint>

namespace coop::net::apply_pause {

bool RunSelftest() {
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("apply_pause selftest FAIL: %s", what);
    };
    auto kind = [](unsigned k) { return static_cast<ReliableKind>(static_cast<std::uint8_t>(k)); };

    // 1. One kind: a raise counts once however often it is repeated, and so does its lower.
    {
        ApplyPauseSet s;
        check(!s.Any() && s.Raises() == 0, "a new set holds no pause");
        check(s.Set(ReliableKind::SavedSignalAppend, true), "a first raise changes the set");
        check(!s.Set(ReliableKind::SavedSignalAppend, true), "a second raise does not");
        check(s.Has(ReliableKind::SavedSignalAppend) && s.Any() && s.Raises() == 1,
              "the kind is held, and counted once");
        check(s.Set(ReliableKind::SavedSignalAppend, false), "a lower changes the set");
        check(!s.Set(ReliableKind::SavedSignalAppend, false), "a second lower does not");
        check(!s.Any() && s.Raises() == 1, "nothing is held and the count stays");
    }

    // 2. A lower of a pause never raised changes nothing and cannot cancel another kind's raise.
    {
        ApplyPauseSet s;
        check(!s.Set(ReliableKind::EmailAppend, false), "lowering a pause never raised changes nothing");
        s.Set(ReliableKind::SavedSignalAppend, true);
        s.Set(ReliableKind::EmailAppend, false);
        check(s.Has(ReliableKind::SavedSignalAppend) && s.Any(), "another kind's raise still holds");
    }

    // 3. Kinds at each word's edge stay apart.
    {
        ApplyPauseSet s;
        const unsigned edges[] = {0, 63, 64, 127, 128, 191, 192, 255};
        for (unsigned k : edges) s.Set(kind(k), true);
        bool held = true;
        for (unsigned k : edges) held = held && s.Has(kind(k));
        check(held && s.Raises() == 8, "every edge kind is held, each counted once");
        check(!s.Has(kind(1)) && !s.Has(kind(62)) && !s.Has(kind(65)) && !s.Has(kind(254)),
              "their neighbours are not");
        for (unsigned k : edges) s.Set(kind(k), false);
        check(!s.Any(), "lowering them all empties the set");
    }

    // 4. Reset lowers every pause and keeps the count a witness reads.
    {
        ApplyPauseSet s;
        s.Set(kind(5), true);
        s.Set(kind(200), true);
        s.Reset();
        check(!s.Any() && !s.Has(kind(5)) && !s.Has(kind(200)), "Reset lowers every pause");
        check(s.Raises() == 2, "and keeps the raise count");
        check(s.Set(kind(5), true) && s.Raises() == 3, "a raise after Reset counts again");
    }

    if (pass == total) {
        UE_LOGI("apply_pause selftest: ALL PASS (%d checks)", total);
        return true;
    }
    UE_LOGE("apply_pause selftest: %d/%d checks passed", pass, total);
    return false;
}

}  // namespace coop::net::apply_pause
