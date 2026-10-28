#include "prop_record_refresh_stubs.h"
#include PROP_RECORD_SOURCE

#include <cstdio>

namespace pr = coop::props::prop_record_refresh;
namespace sg = ue_wrap::script_gate;

int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
        if (!ok) ++failures;
    };
    coop::net::Session session;
    fixture::Actor box, other;
    other.key = L"other";
    pr::Install(&session);
    const sg::Call call{&box, 0x44424F58, false};

    box.record = 1;
    pr::OnUpdPost(call);
    box.record = 2;
    pr::OnUpdPost(call);
    check(fixture::records == std::vector<int>({1, 2}),
          "two rapid distinct drive-box mutations both reach the publisher");

    fixture::records.clear();
    box.record = 3;
    pr::OnUpdPost({&box, 0x5242584F, false});
    box.record = 4;
    pr::OnUpdPost({&box, 0x5242584F, false});
    check(fixture::records == std::vector<int>({3, 4}),
          "two rapid reel-case mutations preserve the final state");

    fixture::records.clear();
    fixture::publishOk = false;
    pr::OnUpdPost({&other, 0x44424F58, false});
    fixture::publishOk = true;
    pr::OnUpdPost({&other, 0x44424F58, false});
    check(fixture::records.size() == 2,
          "a refused publication does not suppress the next update");

    fixture::records.clear();
    pr::OnUpdPost({&box, 0x44424F58, true});
    pr::OnUpdPost({&box, 0x5242584F, true});
    check(fixture::records.empty(), "mirrored records never echo to the sender");

    session.side = coop::net::Role::Client;
    fixture::quiesced = false;
    pr::OnUpdPost(call);
    check(fixture::records.empty(), "joining-client load churn is not published");
    fixture::quiesced = true;
    pr::OnUpdPost(call);
    check(fixture::records == std::vector<int>({4}), "ready-client mutation publishes");

    fixture::records.clear();
    box.key.clear();
    pr::OnUpdPost(call);
    box.key = L"None";
    pr::OnUpdPost(call);
    box.key = L"box";
    check(fixture::records.empty(), "unfinished identity is not published");
    session.live = false;
    pr::OnUpdPost(call);
    session.live = true;
    pr::OnDisconnect();
    pr::OnUpdPost(call);
    check(fixture::records.empty(), "disconnected session is not used");

    pr::Install(&session);
    pr::OnUpdPost(call);
    pr::OnUpdPost({&other, 0x44424F58, false});
    check(fixture::records.size() == 2, "reconnect and separate actors publish");
    pr::OnDisconnect();
    return failures ? 1 : 0;
}
