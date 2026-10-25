#pragma once

#include <stddef.h>
#include <stdint.h>

// A private heap for the relay's TLS connection (V2).
//
// Each relay connection allocates about 30 KB of mbedTLS state, including a
// 16 KB receive buffer, and frees it when the connection ends. Bluetooth,
// HTTP and live pages allocate around it, so after long use the general heap
// can have plenty free but no block large enough, and sharing waits offline
// with "low_memory". reserve() takes one block while memory is still
// unfragmented, keeps it for the boot and installs an mbedTLS allocator
// (mbedtls_platform_set_calloc_free) that serves the relay task from this
// block and everything else, or anything that does not fit, from the general
// heap exactly as before.
namespace TlsArena {

#if defined(SMART_TIMER_HARDWARE_V2) && defined(ESP_PLATFORM)
// Reserve the block once; later calls only report whether it exists.
bool reserve(size_t bytes);
bool reserved();
size_t sizeBytes();
size_t freeBytes();
size_t largestFreeBlock();
size_t minimumFreeBytes();
// Relay-task allocations that did not fit and used the general heap.
uint32_t fallbackCount();
#else
inline bool reserve(size_t) { return false; }
inline bool reserved() { return false; }
inline size_t sizeBytes() { return 0; }
inline size_t freeBytes() { return 0; }
inline size_t largestFreeBlock() { return 0; }
inline size_t minimumFreeBytes() { return 0; }
inline uint32_t fallbackCount() { return 0; }
#endif

}  // namespace TlsArena
