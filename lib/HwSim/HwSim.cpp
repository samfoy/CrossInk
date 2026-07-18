// Hardware-limitation emulation for the Linux simulator. Compiled to nothing on
// device (the header inlines no-ops when SIMULATOR is undefined).
#ifdef SIMULATOR

#include "HwSim.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

namespace hwsim {
namespace {
std::atomic<bool> g_enabled{true};
// Bytes the firmware has explicitly tracked as live (download buffers, JSON
// bodies, etc.). Signed guard avoids underflow if free/alloc ever mismatch.
std::atomic<long> g_trackedLive{0};
// Extra reservation active while a TLS handshake scope is in scope.
std::atomic<long> g_tlsReserved{0};
std::atomic<uint32_t> g_minFree{kEmulatedHeapTotal};

void observeFree(uint32_t freeNow) {
  uint32_t prev = g_minFree.load(std::memory_order_relaxed);
  while (freeNow < prev && !g_minFree.compare_exchange_weak(prev, freeNow)) {
  }
}
}  // namespace

void setEnabled(bool enabled) { g_enabled.store(enabled); }
bool isEnabled() { return g_enabled.load(); }

uint32_t freeHeap() {
  if (!g_enabled.load()) return kEmulatedHeapTotal;
  const long used = static_cast<long>(kEmulatedBaselineUsed) + g_trackedLive.load() + g_tlsReserved.load();
  const long freeBytes = static_cast<long>(kEmulatedHeapTotal) - used;
  const uint32_t clamped = freeBytes > 0 ? static_cast<uint32_t>(freeBytes) : 0;
  observeFree(clamped);
  return clamped;
}

uint32_t maxAllocHeap() {
  if (!g_enabled.load()) return kEmulatedHeapTotal;
  // The largest contiguous block is the smaller of the fragmentation ceiling and
  // whatever free heap remains.
  return std::min<uint32_t>(kEmulatedLargestBlock, freeHeap());
}

uint32_t minFreeHeap() { return g_minFree.load(); }

bool trackAlloc(size_t bytes) {
  if (!g_enabled.load()) return true;
  if (bytes > kEmulatedLargestBlock) return false;         // exceeds contiguous block
  if (static_cast<long>(bytes) > static_cast<long>(freeHeap())) return false;  // not enough free
  g_trackedLive.fetch_add(static_cast<long>(bytes));
  freeHeap();  // update the min-free observation
  return true;
}

void trackFree(size_t bytes) {
  if (!g_enabled.load()) return;
  g_trackedLive.fetch_sub(static_cast<long>(bytes));
}

TlsHandshakeScope::TlsHandshakeScope() {
  g_tlsReserved.fetch_add(static_cast<long>(kEmulatedTlsHandshakePeak));
  freeHeap();  // record the trough
}
TlsHandshakeScope::~TlsHandshakeScope() { g_tlsReserved.fetch_sub(static_cast<long>(kEmulatedTlsHandshakePeak)); }

void displayDelay(bool fastRefresh) {
  if (!g_enabled.load()) return;
  std::this_thread::sleep_for(std::chrono::milliseconds(fastRefresh ? 120 : 380));
}

}  // namespace hwsim

#endif  // SIMULATOR
