#include "RelaySetupApproval.h"
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace opensmarttimer::relay;

int main() {
    RelaySetupApproval approval;
    RelaySetupApproval::Request first{}, out{};
    strcpy(first.relay_origin, "https://api.example.com");
    strcpy(first.partner_origin, "https://relay.example.com");
    strcpy(first.enrollment_code, "single-use-code");
    first.pin[0] = 42;
    assert(!approval.approve(100, true, true, &out)); // no pre-arming
    assert(approval.stage(first, 100) == RelaySetupApproval::Stage::Pending);
    const auto id = approval.id();
    assert(id && approval.remaining(100) == 60000);
    assert(approval.stage(first, 1000) == RelaySetupApproval::Stage::Replay);
    assert(approval.remaining(1000) == 59100 && approval.id() == id);
    // Changes to any approved field cannot replace a request in flight.
    for (int field = 0; field < 4; ++field) {
        auto changed = first;
        if (field == 0) strcpy(changed.relay_origin, "https://evil.example.com");
        if (field == 1) strcpy(changed.partner_origin, "https://evil.example.com");
        if (field == 2) strcpy(changed.enrollment_code, "another-code");
        if (field == 3) changed.pin[0]++;
        assert(approval.stage(changed, 2000) == RelaySetupApproval::Stage::Busy);
        assert(approval.id() == id && !strcmp(approval.request().partner_origin, first.partner_origin));
    }
    assert(!approval.cancel(id + 1));
    assert(approval.approve(2500, true, true, &out));
    assert(!strcmp(out.enrollment_code, first.enrollment_code) && out.pin[0] == 42);
    assert(!approval.pending(2500) && approval.request().enrollment_code[0] == 0);
    assert(!approval.approve(2501, true, true, &out)); // one use
    assert(approval.stage(first, 3000) == RelaySetupApproval::Stage::Pending);
    assert(approval.id() != id && !approval.cancel(id)); // stale cancel
    assert(!approval.approve(3001, false, true, &out)); // timer closed/emergency
    assert(!approval.pending(3001));
    approval.stage(first, 4000);
    assert(!approval.approve(4001, true, false, &out)); // OTA/network/BLE transition
    assert(!approval.pending(4001));
    approval.stage(first, 5000);
    assert(!approval.pending(65000));
    assert(!approval.approve(65000, true, true, &out)); // exact deadline
    assert(approval.request().enrollment_code[0] == 0);
    // Millisecond rollover preserves the bounded deadline.
    approval.stage(first, UINT32_MAX - 100);
    assert(approval.pending(10) && approval.remaining(10) == 59889);
    assert(!approval.pending(59899));
    approval.expire(59899);
    assert(approval.request().enrollment_code[0] == 0);
    std::puts("immutable physical setup approval, expiry and one-use tests passed");
}
