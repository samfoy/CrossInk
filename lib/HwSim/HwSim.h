#pragma once
// Hardware-limitation emulation for the Linux simulator (SIMULATOR builds only).
//
// The stock simulator reports a flat 1 MB free heap and never fails an
// allocation, so the whole class of ESP32-C3 bugs we hit on real hardware (TLS
// handshake OOM, fragmented large-buffer alloc failures, e-ink refresh latency
// causing per-chunk repaint stalls) is invisible in the sim. This module models
// those limits so the simulator can catch them before a hardware flash.
//
// On real hardware (no SIMULATOR define) every function here is a no-op / passes
// through to the real ESP APIs, so production code paths are unchanged.

#include <cstddef>
#include <cstdint>

namespace hwsim {

// ---- Emulated heap model -------------------------------------------------
// Values approximate an Xteink X3 (ESP32-C3, ~320 KB DRAM, no PSRAM) at the
// point catalog/sync run: a large chunk is already consumed by the framebuffer,
// fonts, EPUB engine, and TLS session, leaving a tight, fragmented free pool.
// These mirror what real-hardware logs showed (free ~50-78 KB, largest
// contiguous block ~34-43 KB, dropping to ~5-15 KB mid-TLS-handshake).

// Total DRAM budget the emulator hands out before allocations start failing.
inline constexpr size_t kEmulatedHeapTotal = 320 * 1024;
// Baseline already-consumed bytes (framebuffer + fonts + engine + statics).
// Tuned so normal free heap is ~78 KB, matching real-hardware logs (the device
// completes small POSTs like page-stats fine; only large contiguous allocs and
// the deepest handshake troughs fail).
inline constexpr size_t kEmulatedBaselineUsed = 238 * 1024;
// Largest single contiguous allocation the fragmented heap can satisfy (real
// logs showed ~34-43 KB max block); a 16 KB+ download buffer alloc is the bug.
inline constexpr size_t kEmulatedLargestBlock = 40 * 1024;
// Extra transient bytes reserved during a TLS handshake. Real logs: free dipped
// from ~78 KB toward ~14 KB at the trough, so ~24 KB keeps small POSTs above the
// MIN_HEAP_FOR_TLS(55 KB) guard while making a concurrent big alloc fail.
inline constexpr size_t kEmulatedTlsHandshakePeak = 24 * 1024;

#ifdef SIMULATOR

// Enable/disable the emulation at runtime (default on in SIMULATOR builds).
void setEnabled(bool enabled);
bool isEnabled();

// Report emulated free heap (kEmulatedHeapTotal - baseline - tracked-live-bytes,
// minus the TLS reservation while a handshake scope is active).
uint32_t freeHeap();
// Report the emulated largest allocatable contiguous block.
uint32_t maxAllocHeap();
// Minimum free heap observed since boot (tracks the handshake trough).
uint32_t minFreeHeap();

// Account for a firmware-tracked allocation of `bytes`. Returns false when the
// emulated heap cannot satisfy it (too little free, or exceeds the largest
// contiguous block) — callers that null-check their allocations then behave
// exactly as they would on the device.
bool trackAlloc(size_t bytes);
void trackFree(size_t bytes);

// RAII scope that reserves the TLS handshake peak for its lifetime, so heap
// guards (MIN_HEAP_FOR_TLS etc.) see the realistic mid-handshake trough.
class TlsHandshakeScope {
 public:
  TlsHandshakeScope();
  ~TlsHandshakeScope();
};

// ---- Emulated e-ink refresh timing --------------------------------------
// A full X3 refresh is ~380 ms and a fast refresh ~120 ms; the sim redraws
// instantly, hiding the "repaint per chunk stalls the download" class of bug.
// displayDelay() sleeps the emulated refresh time so timing-sensitive loops
// behave like hardware.
void displayDelay(bool fastRefresh);

#else  // !SIMULATOR — everything compiles to nothing on device.

inline void setEnabled(bool) {}
inline bool isEnabled() { return false; }
inline bool trackAlloc(size_t) { return true; }
inline void trackFree(size_t) {}
inline void displayDelay(bool) {}
class TlsHandshakeScope {
 public:
  TlsHandshakeScope() = default;
  ~TlsHandshakeScope() = default;
};

#endif  // SIMULATOR

}  // namespace hwsim

// Heap-reporting seam used by the memory-sensitive network paths. On device it
// reads the real ESP API; in the simulator it reads the emulated model so the
// same guards (e.g. MIN_HEAP_FOR_TLS) actually trip.
#ifdef SIMULATOR
#define HWSIM_FREE_HEAP() (::hwsim::freeHeap())
#define HWSIM_MAX_ALLOC_HEAP() (::hwsim::maxAllocHeap())
#else
#define HWSIM_FREE_HEAP() (ESP.getFreeHeap())
#define HWSIM_MAX_ALLOC_HEAP() (ESP.getMaxAllocHeap())
#endif
