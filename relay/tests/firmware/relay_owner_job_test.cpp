#include "RelayOwnerJob.h"
#include <cassert>
#include <cstring>
#include <cstdio>
using namespace opensmarttimer::relay;
int main() {
    RelayOwnerJob slot;
    RelayOwnerJob::Request mint{1, RelayOwnerJob::Kind::Mint, 60000, "Phone"};
    assert(slot.check(mint) == RelayOwnerJob::Check::New);
    slot.accept(mint);
    mint.argument[0] = 'X'; // no request-buffer lifetime dependency
    assert(!strcmp(slot.request.argument, "Phone"));
    assert(slot.check(mint) == RelayOwnerJob::Check::Conflict);
    mint.argument[0] = 'P';
    slot.state = RelayOwnerJob::State::Running;
    assert(slot.check(mint) == RelayOwnerJob::Check::Replay);
    RelayOwnerJob::Request revoke{2, RelayOwnerJob::Kind::Revoke, 0, "abcdefghijklmnopqrstuv"};
    assert(slot.check(revoke) == RelayOwnerJob::Check::Busy);
    assert(slot.request.kind == RelayOwnerJob::Kind::Mint);
    assert(!strcmp(slot.request.argument, "Phone"));
    slot.state = RelayOwnerJob::State::Done;
    assert(slot.check(mint) == RelayOwnerJob::Check::Replay);
    assert(slot.check(revoke) == RelayOwnerJob::Check::New);
    slot.accept(revoke);
    assert(slot.check(mint) == RelayOwnerJob::Check::Conflict); // never mint twice
    slot.state = RelayOwnerJob::State::Done;
    assert(slot.check(mint) == RelayOwnerJob::Check::Conflict);
    slot.request.id = UINT32_MAX;
    assert(slot.next_id() == 0); // exhausted until new boot; no wrapped IDs
    revoke.id = 0;
    assert(slot.check(revoke) == RelayOwnerJob::Check::Conflict);
    // Share-URL cache: a new link never evicts a live link's URL (found on
    // hardware: round-robin replaced a live link after another was revoked).
    struct Wire { char fp[24]; } wires[3] = {{"A"}, {"B"}, {"C"}};
    const char live_bc[][24] = {"B", "C"};
    assert(chooseShareSlot(wires, live_bc, 2) == 0);            // A revoked
    wires[0].fp[0] = '\0';
    assert(chooseShareSlot(wires, live_bc, 2) == 0);            // empty first
    strcpy(wires[0].fp, "D");
    const char live_bcd[][24] = {"B", "C", "D"};
    assert(chooseShareSlot(wires, live_bcd, 3) == 3);           // all live: skip, never evict
    const char live_bd[][24] = {"B", "D"};
    assert(chooseShareSlot(wires, live_bd, 2) == 2);            // C expired
    puts("relay owner jobs: immutable, nonblocking, exact retry, stale-ID refusal; share-URL slots never evict a live link");
}
