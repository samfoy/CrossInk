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

## ✅ Tier 0 — Finish what's in flight

### 0.1 Download progress bar — DONE (safe re-introduction)
Live bar + `NN%` + `x / y KB`. The bar was pulled because the progress callback
fired `requestUpdate(true)`, waking the concurrent render task that raced
HTTPClient's malloc → `-8 TOO_LESS_RAM`. Now the callback uses
`requestUpdateAndWait()`, which **blocks the download task while the render task
draws** — serialized, never concurrent. Throttled to whole-percent changes,
min ~700 ms apart, so a multi-MB book isn't dominated by ~380 ms e-ink refreshes.
**Hardware test:** download a 3-4 MB book, watch the bar advance, confirm no `-8`.

### 0.2 Simulator hardware emulation — DONE (`lib/HwSim`)
The stock sim reported a flat 1 MB heap and never failed an alloc, hiding every
C3 OOM we hit on hardware. `HwSim` now models the real limits (no-ops on device):
- **Emulated heap:** ~84 KB free idle, **40 KB largest contiguous block**,
  dropping to ~59 KB during a TLS handshake (`TlsHandshakeScope`). Tuned to real
  logs.
- **`HWSIM_FREE_HEAP()` seam** in the sync client's TLS guards so
  `MIN_HEAP_FOR_TLS` actually trips in-sim; `doJsonPost*` wrap a handshake scope.
  `trackAlloc()` fails >40 KB allocs (reproduces the fragmentation OOM).
- **e-ink refresh latency** (~120 ms fast / ~380 ms full) via
  `hwsim::displayDelay` in `GfxRenderer::displayBuffer`, so per-chunk-repaint
  stalls surface in-sim.
- Verified: standalone test → free=84K, block=40K, TLS-trough=59K; a 50 KB alloc
  FAILs, a 16 KB alloc OKs. **Next:** route the download buffer + JSON parse
  allocs through `trackAlloc` so a regression that reintroduces a big alloc fails
  the sim build's runtime path, not just hardware.

### 0.3 Merge `feat/bookorbit-catalog` → `main`
After a hardware pass on the catalog + progress bar + annotation sync. Squash the
long fix-chain into clean feature commits; drop the DEBUG suffix on release bins.

---

## ⭐ Clippings / Annotations sync — Phase 1 (upload) + Phase 2 (bidirectional) DONE

**Phase 1 (upload) and Phase 2 (pull) are both built and verified end-to-end
against the live server.** Gated behind the existing `shouldUploadReadingStats()`
opt-in; both run on every reader→BookOrbit sync.

**Upload (Phase 1):** `POST /plugin/annotations` — highlights land server-side
(HTTP 201, idempotent). `pos0` = synthetic `/crossink/<spine>/<page>/<word>`,
`datetime` deterministically hashed from `pos0` so re-syncs don't duplicate.

**Bidirectional pull (Phase 2):** verified full cycle — exchange pulls server
annotations (201), ack advances the per-device cursor (`acked:1`), re-exchange
returns 0.
- `KOReaderSyncClient::exchangeAnnotations()` / `ackAnnotations()`.
- `KOReaderSyncActivity::downloadAnnotations()` merges new server highlights into
  the on-device `My Clippings.txt` (page + text + chapter + note). **Limitation:**
  CrossInk can't re-anchor the server's KOReader DOM xpointer, so pulled
  highlights are a **readable record, not a tappable in-book highlight.** Applied
  serverIds tracked in `/.crosspoint/annot_synced/<hash>.txt` to avoid dupes.

**Hardware test:** enable Track Reading Stats; highlight on-device → sync →
appears in BookOrbit; make a highlight in BookOrbit web → sync → appears in the
device's clippings; sync twice → no duplicates either direction.

### Phase 2b / future refinements
- **Fix `Clipping::timestamp`** to a real UTC epoch at capture (currently
  `millis()/1000`). Enables real highlight datetimes (drop the synthetic scheme)
  and lets us send device `keys[]` with `keysComplete=true` for proper two-way
  reconcile + deletion detection.
- **Push local→server deletions** and edits (currently pull-only for adds; we
  send empty `changes[]`).
- Decide whether to attempt page-based re-anchoring of pulled highlights so they
  render in-book (hard: server xpointer ≠ CrossInk position model).

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

