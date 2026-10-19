#include <cassert>
#include <cstring>
#include <iostream>

#include "ResetTrace.h"

namespace {
void at(uint32_t ms) { hostMillis() = ms; }
}  // namespace

int main() {
  using namespace ResetTrace;
  const Stall& worst = stall();
  begin();
  at(1000);
  mark(kInputs);  // time spent in setup() is not a loop pause
  assert(worst.longestMs == 0 && worst.overSecondCount == 0);

  at(1010); mark(kTimer);
  at(1500); request("/api/v1/status");
  at(3700); mark(kWebAfter);  // the request handler ran for 2.2 s
  at(3800); mark(kLoopEnd);
  at(5300); mark(kInputs);    // 1.5 s outside loop()
  assert(worst.longestMs == 2200 && worst.atMs == 3700);
  assert(worst.phase == kWebRequest && std::strcmp(worst.uri, "/api/v1/status") == 0);
  assert(worst.overSecondCount == 2);
  assert(std::strcmp(phaseName(worst.phase), "web_request") == 0);

  at(9000); mark(kTimer);     // a longer pause outside any request
  assert(worst.longestMs == 3700 && worst.phase == kInputs && worst.uri[0] == '\0');
  assert(worst.overSecondCount == 3);

  at(0xFFFFFF00UL); mark(kTimer);   // 4294958296 ms gap is the longest yet
  at(0x00000100UL); mark(kInputs);  // across the millis() wrap: 512 ms
  assert(worst.overSecondCount == 4);
  assert(current().atMs == 0x100 && current().phase == kInputs);
  std::cout << "Reset trace and loop stall tests passed\n";
}
