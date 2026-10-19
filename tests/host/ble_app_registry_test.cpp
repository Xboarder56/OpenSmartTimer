#include "BleAppRegistry.h"
#include <cassert>
#include <iostream>

int main() {
  BleAppRegistry registry;
  uint8_t keys[4][16]{{1}, {2}, {3}, {4}};
  uint8_t phone[6]{1, 2, 3, 4, 5, 6}, tablet[6]{6, 5, 4, 3, 2, 1};
  assert(registry.valid() && registry.count() == 0);
  assert(registry.put(0, keys[0], "Phone", 0, phone));
  assert(registry.put(1, keys[1], "中文应用", 0, phone));
  assert(registry.put(2, keys[2], "Tablet", 1, tablet));
  assert(registry.valid() && registry.count() == 3 && registry.empty() == -1);
  auto authenticate = [&](uint8_t wanted, uint8_t type, const uint8_t* address) {
    unsigned evaluated = 0;
    const int selected = BleAppRegistry::authenticate(registry.apps, type, address,
      [&](const uint8_t* key) { ++evaluated; return key[0] == wanted; });
    assert(evaluated == 3);
    return selected;
  };
  assert(authenticate(1, 0, phone) == 0);
  assert(authenticate(2, 0, phone) == 1);
  assert(authenticate(3, 1, tablet) == 2);
  assert(authenticate(4, 0, phone) == -1);
  assert(authenticate(1, 1, tablet) == -1);
  assert(authenticate(1, 0, nullptr) == -1);
  assert(!registry.put(registry.empty(), keys[3], "Fourth", 0, phone));
  assert(!registry.put(0, keys[1], "Duplicate key", 0, phone));
  const uint32_t removedId = registry.apps[0].id;
  registry.apps[0] = {};
  assert(authenticate(1, 0, phone) == -1);
  assert(registry.retainsPeer(0, phone)); // second app shares the phone bond
  assert(registry.find(removedId) == -1);
  assert(registry.put(0, keys[3], "Replacement", 0, phone));
  assert(registry.apps[0].id != removedId && registry.valid());
  assert(!BleAppRegistry::validLabel(""));
  assert(!BleAppRegistry::validLabel("0123456789012345678901234"));
  assert(!BleAppRegistry::validLabel("bad\nlabel"));
  assert(!BleAppRegistry::validLabel("\xc0\x80"));
  assert(!BleAppRegistry::validLabel("\xed\xa0\x80"));
  assert(!BleAppRegistry::validLabel("\xe4\xb8"));
  assert(BleAppRegistry::validLabel("手机 📱"));
  registry.nextId = UINT32_MAX;
  assert(!registry.put(0, keys[0], "Overflow", 0, phone));
  std::cout << "BLE three-app registry, UTF-8, shared-bond and ID tests passed\n";
}
