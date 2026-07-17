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

## ⭐ NEXT FEATURE — Clippings / Annotations sync

**Goal:** two-way sync of highlights + notes between the X3 and BookOrbit, so
annotations made on-device show up in BookOrbit (and vice-versa), like the KOReader
plugin's annotation exchange.

**Server API is ready** (confirmed in `koreader-plugin.controller.ts`):
- `POST …/plugin/annotations` — bulk upload (`AnnotationsUploadDto`).
- `POST …/plugin/annotations/exchange` — **bidirectional** exchange
  (`AnnotationExchangeDto`): client sends a per-book key/state, server returns
  applied additions/updates/deletions.
- `POST …/plugin/annotations/exchange-ack` — client acks what it applied
  (`AnnotationExchangeAckDto`), so the server can advance its sync cursor.
- Annotation shape (`koreader-exchange.dto.ts`): `pos0`/`pos1` (KOReader xpointer
  positions), `pageno`, `text`/`note`, `datetimeUpdated`, keyed by book (document
  hash, same hash CrossInk already computes for kosync/page-stats).

**CrossInk side — what exists vs. needs building:**
- ✅ Document-hash + kosync header auth (reused from page-stats/catalog).
- ✅ `ClippingsManager` (`src/clippings/ClippingsManager.cpp`) already manages
  on-device highlights/notes — **this is the local store to bridge.**
- ❓ Need to map CrossInk's clipping model ↔ KOReader `pos0/pos1/pageno` xpointers.
  This is the crux: CrossInk stores highlights against its own EPUB position model;
  the exchange protocol expects KOReader-style xpointers. Investigate whether a
  faithful round-trip is possible or whether we sync at page/%-granularity first.

**Suggested phased build:**
1. **Phase 1 — upload only (one-way):** on a manual "Sync Annotations" action (or
   piggyback the existing kosync sync), read `ClippingsManager` highlights for
   books that have a BookOrbit match (by hash), map to the upload DTO, `POST
   /annotations`. Low risk, immediately useful (highlights land in BookOrbit).
   Heap: batch/paginate the upload like page-stats to stay within the C3 budget.
2. **Phase 2 — bidirectional exchange:** implement `exchange` + `exchange-ack`
   with a per-book sync cursor stored on SD; merge server annotations into the
   local store. Handle the pos0/pos1 mapping properly here.
3. **Phase 3 — UI:** surface "N highlights synced" and a per-book annotation count
   on the catalog detail screen; optional conflict/merge messaging.

**Open questions to resolve first (do these before coding):**
- Exact `AnnotationsUploadDto` / `ExchangeBookDto` field list (dump the DTOs).
- How `ClippingsManager` represents a highlight on disk, and whether it retains
  enough position info to produce/consume `pos0/pos1`.
- Whether to gate behind the same opt-in as page-stats (`uploadReadingStats`) or a
  new `syncAnnotations` toggle.

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

