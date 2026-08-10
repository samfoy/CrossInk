#pragma once
#include <cstdint>
#include <string>
#include <vector>

/**
 * Buffers per-page reading events on the SD card so they can be uploaded to a
 * BookOrbit server's KOReader plugin page-stats endpoint
 * (POST <base>/plugin/page-stats).
 *
 * Plain KOSync only carries a current progress percentage; it does NOT create
 * the timed "reading sessions" that power BookOrbit's reading-streak, reading
 * time, pace and reading-DNA stats. The official BookOrbit KOReader plugin
 * additionally uploads raw page-turn events which the server clusters into
 * sessions. This store lets CrossPoint feed that same channel.
 *
 * One event is appended per qualifying forward page read (reusing the dwell
 * time the reader already measures for its own reading-pace stats). Events are
 * grouped per document hash. The buffer is flushed to disk with the same
 * debounce as the book's reading stats (never once-per-page — see CLAUDE.md),
 * and cleared after a successful upload.
 *
 * Storage: a single append-oriented binary file at getFilePath(). The whole
 * file is small (a few bytes per event, capped at MAX_EVENTS) and is rewritten
 * atomically on save, so there is no partial-write window that could corrupt
 * unrelated data.
 */

// One raw page-turn event, matching BookOrbit's PageStatEventDto.
// To stay robust against reflowable per-section pagination we encode overall
// book progress as page/totalPages: `page` is the overall progress in basis
// points (0..10000) and `totalPages` is fixed at PROGRESS_SCALE, so the server
// derives endProgress = page/totalPages*100 == the exact reading %.
struct KOReaderPageStatEvent {
  uint32_t startTime = 0;        // Unix epoch seconds when the page was opened (UTC)
  uint16_t durationSeconds = 0;  // Dwell time on the page (>=1, capped)
  uint16_t progressBp = 0;       // Overall progress in basis points (0..10000)
};

class KOReaderPageStatsStore {
 public:
  // Fixed denominator reported to the server as `totalPages`. Progress is sent
  // as basis points so page/totalPages reproduces the real percentage.
  static constexpr uint16_t PROGRESS_SCALE = 10000;

  // Cap on buffered events to bound SD/RAM use. When full, the oldest events are
  // dropped (progress is still carried by KOSync, so losing the oldest raw
  // events only loses a little session granularity, never the current position).
  static constexpr size_t MAX_EVENTS = 2000;

  // Ignore sub-threshold dwell (matches the server's 10s minimum-session floor
  // intent; individual events can be shorter but this trims obvious noise).
  static constexpr uint16_t MIN_EVENT_SECONDS = 1;

  static const char* getFilePath() { return "/.crosspoint/koreader_pagestats.bin"; }

  KOReaderPageStatsStore() = default;

  // Load any pending events for the given document hash from disk. Replaces the
  // in-memory buffer. Returns false only on an unreadable/corrupt file (treated
  // as "no pending events"). A missing file is success with zero events.
  bool load(const std::string& documentHash);

  // Append one page-turn event for the active document. No disk write here;
  // call save() (debounced) to persist. Enforces MIN_EVENT_SECONDS and the
  // MAX_EVENTS cap (dropping the oldest event on overflow).
  void addEvent(uint32_t startEpoch, uint32_t durationSeconds, float overallProgressFraction);

  // Persist the current buffer to disk (atomic rewrite). Safe to call often;
  // the caller is responsible for debouncing to avoid per-page writes.
  bool save() const;

  // Remove the on-disk buffer and clear memory (called after a successful
  // upload). Missing file is treated as success.
  bool clear();

  bool empty() const { return events_.empty(); }
  size_t size() const { return events_.size(); }
  const std::string& documentHash() const { return documentHash_; }
  const std::vector<KOReaderPageStatEvent>& events() const { return events_; }

 private:
  std::string documentHash_;
  std::vector<KOReaderPageStatEvent> events_;
};
