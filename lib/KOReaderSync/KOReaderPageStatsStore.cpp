#include "KOReaderPageStatsStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

// Binary layout v1:
//   [0]      version (= 1)
//   [1]      hashLen (must be 32 for a KOReader partial-MD5 hash)
//   [2..33]  documentHash (hashLen bytes, ASCII hex, NOT null-terminated)
//   [34-35]  eventCount   uint16_t LE
//   then eventCount records of 8 bytes each:
//     [0-3]  startTime        uint32_t LE (unix epoch seconds, UTC)
//     [4-5]  durationSeconds  uint16_t LE
//     [6-7]  progressBp       uint16_t LE (0..PROGRESS_SCALE)
namespace {
constexpr uint8_t PAGESTATS_FILE_VERSION = 1;
constexpr size_t HASH_LEN = 32;
constexpr size_t HEADER_BYTES = 2 + HASH_LEN + 2;  // version + hashLen + hash + count
constexpr size_t EVENT_BYTES = 8;

uint16_t readLe16(const uint8_t* data, size_t offset) {
  return static_cast<uint16_t>(data[offset]) | (static_cast<uint16_t>(data[offset + 1]) << 8);
}

uint32_t readLe32(const uint8_t* data, size_t offset) {
  return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
         (static_cast<uint32_t>(data[offset + 2]) << 16) | (static_cast<uint32_t>(data[offset + 3]) << 24);
}

void writeLe16(uint8_t* data, size_t offset, uint16_t value) {
  data[offset] = value & 0xFF;
  data[offset + 1] = (value >> 8) & 0xFF;
}

void writeLe32(uint8_t* data, size_t offset, uint32_t value) {
  data[offset] = value & 0xFF;
  data[offset + 1] = (value >> 8) & 0xFF;
  data[offset + 2] = (value >> 16) & 0xFF;
  data[offset + 3] = (value >> 24) & 0xFF;
}
}  // namespace

bool KOReaderPageStatsStore::load(const std::string& documentHash) {
  documentHash_ = documentHash;
  events_.clear();

  if (documentHash.size() != HASH_LEN) {
    // Filename-mode hashes are still 32 hex chars (MD5), so a non-32 hash means
    // no usable document identity; keep an empty buffer.
    LOG_DBG("KOStats", "Skipping page-stats load: hash length %u != %u", (unsigned)documentHash.size(),
            (unsigned)HASH_LEN);
    return true;
  }

  FsFile f;
  if (!Storage.openFileForRead("KOStats", getFilePath(), f)) {
    return true;  // no pending file == no pending events
  }

  uint8_t header[HEADER_BYTES] = {};
  const int n = f.read(header, HEADER_BYTES);
  if (n != static_cast<int>(HEADER_BYTES) || header[0] != PAGESTATS_FILE_VERSION || header[1] != HASH_LEN) {
    f.close();
    LOG_DBG("KOStats", "Discarding unreadable/mismatched page-stats file");
    return true;
  }

  // If the buffered events belong to a different book, drop them: only one
  // document's events are buffered at a time (matches CrossInk's one-open-book
  // reading model). The current book's fresh events will be appended.
  if (std::memcmp(header + 2, documentHash.data(), HASH_LEN) != 0) {
    f.close();
    LOG_DBG("KOStats", "Buffered page-stats are for a different book; starting fresh");
    return true;
  }

  const uint16_t count = readLe16(header, 2 + HASH_LEN);
  events_.reserve(count <= MAX_EVENTS ? count : MAX_EVENTS);
  uint8_t rec[EVENT_BYTES];
  for (uint16_t i = 0; i < count; ++i) {
    const int rn = f.read(rec, EVENT_BYTES);
    if (rn != static_cast<int>(EVENT_BYTES)) {
      LOG_DBG("KOStats", "Truncated page-stats file at event %u/%u", (unsigned)i, (unsigned)count);
      break;
    }
    if (events_.size() >= MAX_EVENTS) {
      break;
    }
    KOReaderPageStatEvent ev;
    ev.startTime = readLe32(rec, 0);
    ev.durationSeconds = readLe16(rec, 4);
    ev.progressBp = readLe16(rec, 6);
    events_.push_back(ev);
  }
  f.close();
  LOG_DBG("KOStats", "Loaded %u pending page-stat events", (unsigned)events_.size());
  return true;
}

void KOReaderPageStatsStore::addEvent(uint32_t startEpoch, uint32_t durationSeconds, float overallProgressFraction) {
  if (durationSeconds < MIN_EVENT_SECONDS) {
    return;
  }
  if (startEpoch == 0) {
    // No valid RTC time -> the server requires startTime >= 1 and would reject
    // the whole batch. Skip silently; KOSync still carries the progress %.
    return;
  }

  KOReaderPageStatEvent ev;
  ev.startTime = startEpoch;
  ev.durationSeconds = durationSeconds > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(durationSeconds);

  float frac = overallProgressFraction;
  if (frac < 0.0f) frac = 0.0f;
  if (frac > 1.0f) frac = 1.0f;
  uint32_t bp = static_cast<uint32_t>(frac * PROGRESS_SCALE + 0.5f);
  if (bp > PROGRESS_SCALE) bp = PROGRESS_SCALE;
  ev.progressBp = static_cast<uint16_t>(bp);

  // Bound memory/flash: drop the oldest event on overflow. Erasing the front of
  // a small vector is cheap relative to the once-per-page cadence and keeps the
  // most recent (most relevant) session data.
  if (events_.size() >= MAX_EVENTS) {
    events_.erase(events_.begin());
  }
  events_.push_back(ev);
}

bool KOReaderPageStatsStore::save() const {
  if (documentHash_.size() != HASH_LEN) {
    return false;
  }

  FsFile f;
  if (!Storage.openFileForWrite("KOStats", getFilePath(), f)) {
    LOG_ERR("KOStats", "Could not write page-stats buffer");
    return false;
  }

  uint8_t header[HEADER_BYTES] = {};
  header[0] = PAGESTATS_FILE_VERSION;
  header[1] = static_cast<uint8_t>(HASH_LEN);
  std::memcpy(header + 2, documentHash_.data(), HASH_LEN);
  const size_t count = events_.size() > MAX_EVENTS ? MAX_EVENTS : events_.size();
  writeLe16(header, 2 + HASH_LEN, static_cast<uint16_t>(count));
  f.write(header, HEADER_BYTES);

  uint8_t rec[EVENT_BYTES];
  for (size_t i = 0; i < count; ++i) {
    const KOReaderPageStatEvent& ev = events_[i];
    writeLe32(rec, 0, ev.startTime);
    writeLe16(rec, 4, ev.durationSeconds);
    writeLe16(rec, 6, ev.progressBp);
    f.write(rec, EVENT_BYTES);
  }
  f.close();
  return true;
}

bool KOReaderPageStatsStore::clear() {
  events_.clear();
  if (Storage.exists(getFilePath()) && !Storage.remove(getFilePath())) {
    LOG_ERR("KOStats", "Could not delete page-stats buffer");
    return false;
  }
  return true;
}
