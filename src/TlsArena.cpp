#include "TlsArena.h"

#if defined(SMART_TIMER_HARDWARE_V2) && defined(ESP_PLATFORM)

#include <string.h>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/platform.h>
#include <multi_heap.h>

namespace {

// The relay creates its task under this name (RelayRuntime.cpp).
constexpr char kOwnerTaskName[] = "relay";

uint8_t* region = nullptr;
size_t regionSize = 0;
multi_heap_handle_t arena = nullptr;
portMUX_TYPE arenaLock = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t fallbacks = 0;

bool ownerTask() {
  const char* name = pcTaskGetName(nullptr);
  return name != nullptr && strcmp(name, kOwnerTaskName) == 0;
}

bool inRegion(const void* pointer) {
  const uint8_t* p = static_cast<const uint8_t*>(pointer);
  return region != nullptr && p >= region && p < region + regionSize;
}

// Installed as mbedTLS's allocator once the arena exists. Outside the arena
// they match the framework's default (internal 8-bit RAM), and they free
// either kind, so blocks allocated before the switch are released correctly.
void* arenaCalloc(size_t count, size_t size) {
  if (ownerTask()) {
    size_t total = 0;
    if (__builtin_mul_overflow(count, size, &total)) return nullptr;
    void* pointer = multi_heap_malloc(arena, total);
    if (pointer != nullptr) {
      memset(pointer, 0, total);
      return pointer;
    }
    fallbacks = fallbacks + 1;
  }
  return heap_caps_calloc(count, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void arenaFree(void* pointer) {
  if (inRegion(pointer)) multi_heap_free(arena, pointer);
  else heap_caps_free(pointer);
}

}  // namespace

namespace TlsArena {

bool reserve(size_t bytes) {
  if (arena != nullptr) return true;
  uint8_t* block = static_cast<uint8_t*>(
      heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (block == nullptr) return false;
  multi_heap_handle_t heap = multi_heap_register(block, bytes);
  if (heap == nullptr) {
    heap_caps_free(block);
    return false;
  }
  multi_heap_set_lock(heap, &arenaLock);
  region = block;
  regionSize = bytes;
  arena = heap;
  if (mbedtls_platform_set_calloc_free(arenaCalloc, arenaFree) != 0) {
    // mbedTLS keeps its own allocator; the relay uses the general heap.
    arena = nullptr;
    region = nullptr;
    regionSize = 0;
    heap_caps_free(block);
    return false;
  }
  return true;
}

bool reserved() { return arena != nullptr; }
size_t sizeBytes() { return regionSize; }
size_t freeBytes() { return arena ? multi_heap_free_size(arena) : 0; }
size_t largestFreeBlock() {
  if (arena == nullptr) return 0;
  multi_heap_info_t info;
  multi_heap_get_info(arena, &info);
  return info.largest_free_block;
}
size_t minimumFreeBytes() {
  return arena ? multi_heap_minimum_free_size(arena) : 0;
}
uint32_t fallbackCount() { return fallbacks; }

}  // namespace TlsArena

#endif
