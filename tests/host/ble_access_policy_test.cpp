#include "BleAccessPolicy.h"
#include <cassert>
#include <iostream>
int main() {
  using namespace BleAccessPolicy;
  static_assert(authorizationError(false, false) == 0x0f, "link encryption first");
  static_assert(authorizationError(true, false) == 0x05, "bonding is link security");
  static_assert(authorizationError(true, true) == 0x83, "app auth must not trigger OS pairing");
  assert(receiptReadable(100, 2100, true, true, true, true));
  assert(!receiptReadable(100, 2100, false, true, true, true));
  assert(!receiptReadable(100, 2100, true, false, true, true));
  assert(!receiptReadable(100, 2100, true, true, false, true));
  assert(!receiptReadable(100, 2100, true, true, true, false));
  assert(!receiptReadable(100, 0, true, true, true, true));
  assert(!receiptReadable(2100, 2100, true, true, true, true));
  assert(!receiptReadable(2101, 2100, true, true, true, true));
  assert(receiptReadable(0xfffffff0, 100, true, true, true, true));
  assert(!receiptReadable(101, 100, true, true, true, true));
  std::cout << "BLE application rejection and connection-bound receipt deadline tests passed\n";
}
