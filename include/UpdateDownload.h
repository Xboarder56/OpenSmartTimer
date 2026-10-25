#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ReleaseUpdate.h"

// V2 on-device firmware download.
//
// A background task fetches this model's signed record from the update site,
// verifies its signature, and downloads the image over the timer's own
// Wi-Fi. It never writes flash: it hands the image to loop() one chunk at a
// time, and WebApplication installs it through the same validators as an
// uploaded image (header, product marker, CRC32, release signature and
// SHA-256). TLS only carries the bytes; authenticity comes from the
// signature, as for a download made by the browser.
namespace UpdateDownload {

enum class Phase : uint8_t {
  Idle,         // nothing running
  Checking,     // fetching and verifying the signed record
  Ready,        // record verified; waiting for loop() to begin the install
  Downloading,  // image bytes flowing to loop()
  Downloaded,   // every byte handed over; loop() finishes the install
  Failed,       // stopped; error() says why
};

// Largest chunk handed to loop(); the installer's write buffer.
constexpr size_t kChunkBytes = 1436;

struct Progress {
  Phase phase = Phase::Idle;
  uint32_t receivedBytes = 0;
  uint32_t totalBytes = 0;
  char version[ReleaseUpdate::kVersionBytes]{};
  char error[32]{};
};

#if defined(SMART_TIMER_HARDWARE_V2) && defined(ESP_PLATFORM)
// loop() side. start() returns false when a download is already running or
// the task cannot be created. expectedVersion, when not empty, is the
// version the person approved; a different published version is refused.
bool start(const char* expectedVersion, const char* installedVersion);
Progress progress();
bool active();  // Checking through Downloaded
// Valid in Ready: the verified descriptor and image file name.
bool descriptor(ReleaseUpdate::Descriptor& out);
// loop() has begun the install; the task may download the image.
void beginImage();
// Copies the next chunk, if one is waiting. Never blocks.
bool takeChunk(uint8_t* out, size_t& length);
// loop() ended the install (or refused it): stop the task and report.
void finish(bool installed, const char* error);
#else
inline bool start(const char*, const char*) { return false; }
inline Progress progress() { return {}; }
inline bool active() { return false; }
inline bool descriptor(ReleaseUpdate::Descriptor&) { return false; }
inline void beginImage() {}
inline bool takeChunk(uint8_t*, size_t&) { return false; }
inline void finish(bool, const char*) {}
#endif

inline const char* phaseName(Phase phase) {
  switch (phase) {
    case Phase::Checking: return "checking";
    case Phase::Ready:
    case Phase::Downloading: return "downloading";
    case Phase::Downloaded: return "installing";
    case Phase::Failed: return "failed";
    case Phase::Idle:
    default: return "idle";
  }
}

}  // namespace UpdateDownload
