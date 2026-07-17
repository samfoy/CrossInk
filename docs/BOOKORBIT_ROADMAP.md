# BookOrbit / CrossInk — Roadmap & Improvement Sketch

_Status as of the working end-to-end catalog: browse → detail → download (de-chunked, heap-safe) → opens cleanly. Branch: `feat/bookorbit-catalog`._

---

## Tier 0 — Finish what's in flight

### 0.1 Bring back the download progress bar (SAFE this time)  ⭐ explicitly requested
The bar was pulled because the progress callback fired `requestUpdate(true)`, which
woke the **render task on core 0** — that render allocated framebuffer memory
*concurrently* with HTTPClient's per-chunk `malloc()`, fragmenting the heap →
`-8 TOO_LESS_RAM`. Two viable re-introductions, in order of robustness:

- **(A) Manual de-chunk into a single pre-allocated buffer + synchronous render.**
  Replace `http.writeToStream()` with our own chunk-decoding loop (read hex size
  line, read N bytes into a buffer we `malloc` ONCE before the transfer, write to
  SD, repeat). Because there is zero per-chunk malloc on the HTTP side, and the
  render is driven **synchronously from the same task** between chunks (never the
  concurrent render task), there's no heap race. Throttle to whole-percent / ~1 Hz.
  This is the "correct" fix and also removes our dependency on Arduino's chunk
  decoder. ~1 evening.
- **(B) Pre-reserve the framebuffer BW-chunks before the download, render on a
  throttle, keep `writeToStream`.** Lighter change, but still relies on the render
  task not allocating — need to confirm `storeBwBuffer()` reuses reserved chunks
  rather than re-`malloc`ing. Riskier; (A) is preferred.

Acceptance: download a 3–4 MB book, live bar + `NN%` / `x / y KB` updates ~1/sec,
`result=0`, opens cleanly, no `-8`.

### 0.2 Merge `feat/bookorbit-catalog` → `main`
After 0.1 lands and one more hardware pass. Squash the long fix-chain into a clean
feature commit; refresh the release bins (drop the DEBUG suffix).

---

## Tier 1 — Dashboard depth (highest user value)

### 1.1 "Already downloaded" indicator + open-in-place
On the book list / detail, check if the destination file already exists on SD
(`Storage.exists(destPathForDetail())`). Show a ✓ and change the action from
**Download** → **Open**. Avoids re-downloading, and makes the catalog feel like a
real library view. Cheap, high-impact.

### 1.2 Search
The catalog API already accepts `?q=`. Add a search entry (reuse the on-device
text-input used for kosync creds) → `fetchBooks(q, sort, page)`. Lets you find a
book in a large library without paging. Medium.

### 1.3 Sort options
API supports `sort=title | recently_added | author | …`. Add a small sort picker
on the "All Books" list (cycle button or submenu). Low effort, big usability win
for large libraries.

### 1.4 Series awareness / "next in series"
BookOrbit knows series + index (we already parse `seriesName` / `seriesIndex`).
On detail, show "Book N of <series>" and, when the current book is finished, offer
**Download next in series**. (Concrete example from your own library: God Emperor
of Dune is next after Children of Dune.) Medium.

### 1.5 Read-status filter on the list
Filter All Books by `reading | finished | unread | abandoned`. The dashboard
already distinguishes these server-side. Pairs well with 1.3. Low–medium.

### 1.6 Cover thumbnails (detail view first)
`thumbnailUrl(bookId)` exists. Rendering images on e-ink costs RAM + decode time,
so start conservative: fetch + show a single cover **only on the detail screen**
(not the list), downscaled, reusing the heap-safe download path. Defer list
thumbnails until measured. Medium–high (memory-sensitive — respect the C3 budget).

---

## Tier 2 — Read-status & sync write-back

### 2.1 Full read-status controls
We have **Mark Finished** (`setReadStatus`). Add **Mark as Reading** and
**Abandoned** on the detail screen (API already accepts all three). Closes the loop
that the on-device stats_v5 "Mark as Finished" never pushed. Low.

### 2.2 Rating write-back
Catalog API has a rating endpoint. Add a 1–5 star setter on detail. Low–medium.

### 2.3 Auto-mark "reading" on first open of a catalog-downloaded book
When a book downloaded via the catalog is opened, PUT `status=reading` so BookOrbit
reflects it immediately (before page-stats/kosync catch up). Low.

---

## Tier 3 — Robustness & polish (reusable across CrossInk)

### 3.1 Document the C3 TLS/heap patterns as a skill
Hard-won this session and worth capturing: (a) `WiFiClientSecure::setInsecure()`
vs `esp_crt_bundle_attach` heap cost, (b) `WifiPowerSaveGuard` for throughput,
(c) never render on the concurrent task during a transfer, (d) chunked-transfer
decoding, (e) small read buffers to survive fragmentation. Update
`crossink-firmware-dev`. Low, high leverage for future work.

### 3.2 Download resume / retry
Partial-file cleanup exists on failure. Next: an HTTP `Range` resume for
interrupted large downloads, or at least a one-tap retry. Medium.

### 3.3 Error surfacing
We now classify NETWORK / UNAVAILABLE / NO_CREDENTIALS / PARSE distinctly — surface
friendlier messages (e.g. "Wi-Fi lost", "Sign in again") instead of generic
"Download failed". Low.

### 3.4 Delete a downloaded book from the device
From detail, when a file exists locally: **Remove download** (`Storage.remove`).
Manage SD space without a computer. Low.

---

## Tier 4 — Upstream / cross-cutting

### 4.1 Per-device session attribution (BookOrbit #718)
Filed upstream; `reading_sessions` has no device column. If/when accepted, surface
per-device stats on the dashboard. Depends on upstream.

### 4.2 Opt-in gating audit
Confirm every BookOrbit-catalog surface is behind credentials/opt-in so plain
KOReader-sync users see no new UI. Mostly done (menu entry gated on
`hasCredentials()`); quick review. Low.

### 4.3 Config: choose download folder
Currently root `/`. Optional setting for `/books` or a user path, with
`ensureDirectoryExists`. Low; only if you want it.

---

## Suggested next session order
1. **0.1 progress bar via manual de-chunk** (the requested item; also hardens the
   download path).
2. **1.1 already-downloaded ✓ + open-in-place** (fast, makes it feel finished).
3. **1.3 sort + 1.2 search** (library usability).
4. **3.1 skill capture** (lock in the heap lessons).
5. **0.2 merge to main** once 0.1 is hardware-verified.
