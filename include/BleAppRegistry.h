#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Persisted only on V2. IDs are never reused within a factory-reset epoch;
// labels are presentation, not authentication identities. No activity writes.
struct __attribute__((packed)) BleAppRegistry {
  static constexpr uint8_t kCapacity = 3;
  static constexpr size_t kLabelBytes = 24;
  struct __attribute__((packed)) App {
    uint32_t id = 0;
    uint8_t key[16]{};
    char label[kLabelBytes + 1]{};
    uint8_t peerKnown = 0;
    uint8_t peerType = 0;
    uint8_t peerAddress[6]{};
  };
  uint32_t magic = 0x4f535441;
  uint32_t version = 1;
  uint32_t nextId = 1;
  uint32_t resetPending = 0;
  App apps[kCapacity]{};
  uint32_t checksum = 0;

  int find(uint32_t id) const {
    if (!id) return -1;
    for (uint8_t i = 0; i < kCapacity; ++i) if (apps[i].id == id) return i;
    return -1;
  }
  int empty() const {
    for (uint8_t i = 0; i < kCapacity; ++i) if (!apps[i].id) return i;
    return -1;
  }
  uint8_t count() const {
    uint8_t n = 0;
    for (const auto& app : apps) if (app.id) ++n;
    return n;
  }
  static bool validLabel(const char* text) {
    if (!text || !*text) return false;
    const size_t length = strnlen(text, kLabelBytes + 1);
    if (length > kLabelBytes) return false;
    // UTF-8, no controls, overlong sequences, surrogates or truncated glyphs.
    for (size_t i = 0; i < length;) {
      uint32_t c = static_cast<uint8_t>(text[i++]);
      if (c < 0x20 || c == 0x7f) return false;
      if (c < 0x80) continue;
      unsigned extra; uint32_t minimum;
      if (c >= 0xc2 && c <= 0xdf) { extra = 1; minimum = 0x80; c &= 0x1f; }
      else if (c >= 0xe0 && c <= 0xef) { extra = 2; minimum = 0x800; c &= 0x0f; }
      else if (c >= 0xf0 && c <= 0xf4) { extra = 3; minimum = 0x10000; c &= 7; }
      else return false;
      if (i + extra > length) return false;
      while (extra--) {
        const uint8_t b = static_cast<uint8_t>(text[i++]);
        if ((b & 0xc0) != 0x80) return false;
        c = (c << 6) | (b & 0x3f);
      }
      if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return false;
    }
    return true;
  }
  bool put(int slot, const uint8_t* key, const char* label,
           uint8_t peerType, const uint8_t* peerAddress) {
    if (slot < 0 || slot >= kCapacity || !key || !validLabel(label) ||
        !nextId || nextId == UINT32_MAX) return false;
    uint8_t nonzero = 0;
    for (size_t i = 0; i < 16; ++i) nonzero |= key[i];
    if (!nonzero) return false;
    for (const auto& app : apps) if (app.id && !memcmp(app.key, key, 16)) return false;
    App app{};
    app.id = nextId++;
    memcpy(app.key, key, 16);
    memcpy(app.label, label, strlen(label));
    if (peerAddress) {
      app.peerKnown = 1; app.peerType = peerType;
      memcpy(app.peerAddress, peerAddress, 6);
    }
    apps[slot] = app;
    return true;
  }
  // Unknown peer is permitted only for the single migrated legacy approval.
  bool retainsPeer(uint8_t type, const uint8_t* address) const {
    for (const auto& app : apps)
      if (app.id && (!app.peerKnown ||
          (app.peerType == type && !memcmp(app.peerAddress, address, 6)))) return true;
    return false;
  }
  template<class Verify>
  static int authenticate(const App* candidates, uint8_t peerType,
                          const uint8_t* peerAddress, Verify verify) {
    int selected = -1;
    for (uint8_t slot = 0; slot < kCapacity; ++slot) {
      const auto& app = candidates[slot];
      // Evaluate the verifier for every slot, even empty or wrong-peer slots.
      const bool matched = verify(app.key);
      const bool peerMatches = peerAddress && (!app.peerKnown ||
          (app.peerType == peerType && !memcmp(app.peerAddress, peerAddress, 6)));
      if (matched && app.id && peerMatches) selected = slot;
    }
    return selected;
  }
  bool valid() const {
    if (magic != 0x4f535441 || version != 1 || !nextId || resetPending > 1) return false;
    for (const auto& app : apps) if (app.id) {
      if (app.id >= nextId || !validLabel(app.label) || app.peerKnown > 1 || app.peerType > 3) return false;
      uint8_t nonzero = 0;
      for (auto b : app.key) nonzero |= b;
      if (!nonzero) return false;
      for (const auto& other : apps) if (&other != &app && other.id &&
          (other.id == app.id || !memcmp(other.key, app.key, 16))) return false;
    }
    return true;
  }
};
