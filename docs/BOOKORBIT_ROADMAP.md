# BookOrbit / CrossInk — Roadmap & Improvement Sketch

_Status: catalog works end-to-end (browse → detail → download → opens cleanly).
**Tier 1 COMPLETE** (built + compiles on default & simulator; hardware test pending).
Branch: `feat/bookorbit-catalog`._

---

## ✅ Tier 1 — Dashboard depth (DONE, pending hardware test)

Landing screen is now a **browse menu**: Continue Reading / All Books / Recently
Added / Currently Reading / Unread / Finished / Search — each maps to a catalog
query. The working LIST screen was left untouched (no risky button overloading on
the tight X3 keypad).

- **1.1 Already-downloaded ✓ + Open-in-place** — list rows and detail show a ✓
  when the EPUB already exists on SD; detail action becomes **Open** not Download.
- **1.2 Search** — reuses `KeyboardEntryActivity` → `?q=`.
- **1.3 Sort** — Title / Recently Added browse rows (API also supports author/series).
- **1.4 Series** — detail shows "Next in Series: <title>" (resolved from
  `relatedSections`); **Up = download next in series** (opens if already on device).
- **1.5 Read-status filters** — Currently Reading / Unread / Finished browse rows.
- **1.6 Cover thumbnail** — JPEG fetched on the detail screen, converted to a small
  1-bit BMP (`jpegFileTo1BitBmpStreamWithSize`, 120×180) and drawn top-right; text
  reflows left. Best-effort: any failure renders text-only, never crashes.

**Commits:** `c1a8e68` (search/sort/filters/series/on-device) + `55185de` (thumbnail).
**Hardware test checklist:** browse each mode; search; open a series book and use
Next-in-Series; confirm ✓ shows for downloaded books; **watch free heap on the
detail screen when a cover loads** (the ~50 KB JPEG + decode is the memory-
sensitive bit — fall back to no-cover or a smaller box if it's tight).

---

## ⭐ Clippings / Annotations sync — Phase 1 DONE (upload), verified

**Phase 1 (one-way upload) is built and verified end-to-end against the live
server** (HTTP 201, `upserted:1`; idempotent re-POST `upserted:0`, no duplicate
DB row). On every reader→BookOrbit sync (gated behind the existing
`shouldUploadReadingStats()` opt-in), CrossInk uploads the current book's
highlights/notes via `POST /plugin/annotations`.

- `KOReaderSyncClient::uploadAnnotations()` — chunked (20) POST, same TLS/auth/
  404-self-heal as page-stats; `pluginVersion crossink-an-1`.
- `ClippingStore::readForBook()` — reads one book's clippings without disturbing
  the loaded singleton.
- `KOReaderSyncActivity::uploadAnnotations()` — maps clippings→annotations,
  uploads alongside page-stats.

**Mapping (CrossInk has no KOReader DOM xpointers):** `pos0` = synthetic stable
`/crossink/<spine>/<page>/<word>`, `posFormat=xpointer`, `drawer=lighten`.
`datetime` derived deterministically from an FNV-1a hash of `pos0` so the server
dedup key `md5(datetime|pos0)` is stable across re-syncs. **Tradeoff:** displayed
date isn't the real highlight time (clipping timestamps are millis-uptime, not
wall clock). Text + page + chapter sync correctly.

**Hardware test:** enable Track Reading Stats, highlight some text, trigger a
sync, confirm highlights appear in BookOrbit; sync again → no duplicates.

### Phase 2 — bidirectional exchange (follow-up, not built)
- `POST /annotations/exchange` + `/exchange-ack` with a per-book sync cursor on
  SD; merge server annotations into the local `ClippingStore`.
- **Prereq worth doing first:** fix `Clipping::timestamp` to store a real UTC
  epoch at capture (currently `millis()/1000`). That gives real highlight
  datetimes (better than the synthetic ones) and simplifies the exchange key.
- Handle the pos0 round-trip: server annotations created in KOReader proper carry
  real xpointers CrossInk can't resolve to a page — decide whether to show them
  read-only or map by page/chapter.

### Phase 3 — UI
- Per-book annotation count on the catalog detail screen; "N highlights synced".

---

## Tier 2 — Sync write-back (small, high-value)
- **2.1** Mark **Reading** / **Abandoned** on detail (API accepts all three; we
  already have Mark Finished).
- **2.2** Rating write-back (1–5 stars) — catalog rating endpoint.
- **2.3** Auto-mark **reading** on first open of a catalog-downloaded book.

## Tier 3 — Robustness & polish
- **3.1** Capture the C3 TLS/heap lessons as a skill (setInsecure vs CA bundle,
  WifiPowerSaveGuard, no concurrent render during transfer, chunked decode,
  small read buffers, JPEG→1-bit-BMP cover path). Update `crossink-firmware-dev`.
- **3.2** Download resume (HTTP Range) / one-tap retry.
- **3.3** Friendlier error messages per error class (Wi-Fi lost / sign in again).
- **3.4** Delete a downloaded book from the device (detail → Remove download).

## Tier 4 — Upstream / cross-cutting
- **4.1** Per-device session attribution (BookOrbit #718, filed).
- **4.2** Opt-in gating audit (mostly done; quick review).
- **4.3** Optional download-folder setting (currently root `/`).

---

## Suggested next-session order
1. **Merge `feat/bookorbit-catalog` → main** once Tier 1 is hardware-verified
   (currently 8+ commits deep; squash into a clean feature history).
2. **Progress bar during download** — the safe manual-de-chunk approach (see git
   history; was pulled to fix the `-8` OOM, needs the non-concurrent-render redo).
3. **Clippings/annotations sync — Phase 1 (upload)** — the next feature.
4. **3.1 skill capture** — lock in the hard-won heap/TLS lessons.

