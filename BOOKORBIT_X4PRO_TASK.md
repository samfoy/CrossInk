# Task: Port BookOrbit integration to the Xteink X4 Pro (ESP32-S3) + add SmartScopes/TBR queue

## Mission

Bring Sam's complete BookOrbit integration to the **Xteink X4 Pro** (ESP32-S3), and add the
one genuinely missing feature: a **to-be-read queue driven by BookOrbit SmartScopes**.

This is a **PORT of verified working code plus two bounded additions** — NOT a greenfield build.
The C3 implementation is hardware-verified and must be preserved in behavior. Do not redesign it.

## Repository / branch state (already set up — do not re-create)

- Working repo: `/home/sam/workspace/crossink-x4pro`
- Working branch: `feat/bookorbit-x4pro`, based on `upstream/feat-touch-ui` (commit `4ac38165`)
- `upstream` = `crosspoint-reader/crosspoint-reader` (has the `[env:x4pro]` target)
- `crossink`  = local remote pointing at `/home/sam/workspace/CrossInk` (Sam's fork, the source of the features)
- The feature source branch is **`crossink/feat/bookorbit-catalog`** (31 commits, +2727 lines).
- Submodules are already initialized (`freeink-sdk` @ `7d2396ef`).

**Build command (the ONLY correct one for this device):**
```bash
export PATH="$HOME/.local/bin:$PATH"
pio run -e x4pro
```
First build downloads the xtensa toolchain (~10 min). A warm rebuild is ~4 min. `pio run -e default`
is the **C3** target — do not use it to validate this work.

## What to port (source: `crossink/feat/bookorbit-catalog`)

Read each file from the feature branch with `git show crossink/feat/bookorbit-catalog:<path>`.

### Clean adds — these files do NOT exist upstream, so copy them over essentially wholesale
- `lib/KOReaderSync/KOReaderPageStatsStore.{h,cpp}` — SD-backed page-turn event buffer
- `src/network/KOReaderCatalogClient.{h,cpp}` — BookOrbit catalog HTTP client (~550 lines)
- `src/activities/browser/BookOrbitCatalogActivity.{h,cpp}` — the browse/search UI (~675 lines)

### Real merges — upstream has diverged, so integrate by hand, do not clobber
- `lib/KOReaderSync/KOReaderSyncClient.{h,cpp}` — **934 lines in Sam's fork vs 298 upstream.**
  Sam's additions are `uploadPageStats()`, `uploadAnnotations()`, `exchangeAnnotations()`,
  `ackAnnotations()`, `doJsonPost()`, `doJsonPostWithResponse()`. Start from UPSTREAM's file and
  add Sam's methods onto it. Do not delete upstream's newer changes.
- `src/activities/reader/KOReaderSyncActivity.{h,cpp}` — upstream migrated this to the FreeInkApp
  framework (`378eca48 Migrate KOReaderSync UI to FreeInkApp framework`). Start from UPSTREAM's
  version and re-add Sam's `uploadPageStats()` / `uploadAnnotations()` / `downloadAnnotations()`
  call sites. **Both must run BEFORE `wifiOff()`.**
- `src/activities/reader/EpubReaderActivity.cpp` — re-add the `capturePageStatEvent(dwell)` call at
  the qualifying-forward-read branch of `pageTurn()`, BEFORE the page/section mutation.
- `src/ClippingStore.{h,cpp}` — re-add `static readForBook(filePath, "epub", out)`.
- `src/activities/home/HomeActivity.cpp` + `.h` — add the "BookOrbit Library" home-menu entry.
- `src/activities/ActivityManager.{h,cpp}` — add the `goTo*()` entry point.
- Settings: the `uploadReadingStats` opt-in toggle (default OFF) must exist in BOTH
  `SettingsList.h` (drives web UI + JSON persistence) AND the on-device
  `KOReaderSettingsActivity.cpp`. Add `STR_*` keys to `lib/I18n/translations/english.yaml` only —
  never edit generated `I18nKeys.h`/`I18nStrings.*`.

### DO NOT port (C3-only scar tissue — deliberately dropped)
- The hand-rolled manual chunked-transfer decoder in `KOReaderCatalogClient.cpp` (stashed in the
  CrossInk repo as "C3 manual chunked-decode fragmentation fix"). It exists ONLY to dodge C3 heap
  fragmentation. On the S3, use `HTTPClient::writeToStream()` with a `Stream` sink wrapper.
- Do NOT copy `-DHTTP_RX_BUF=4096` style TLS shrinking, and do NOT reintroduce the
  "never render during a network malloc" serialization. The S3 is dual-core with 8 MB PSRAM.
- Keep `WiFiClientSecure::setInsecure()` for now (same trust model as kosync, same credentials,
  same server) — cert pinning is explicitly OUT OF SCOPE for this task.

## NEW FEATURE 1 — SmartScopes browsing + the to-be-read queue

This is the only genuinely new functionality. **It requires NO server change.** Verified live today.

The server already exposes it. `GET <catalogBase>/sections/smart-scopes` returns:
```json
{"section":"smart-scopes","items":[
  {"id":"3","title":"Want to Read","icon":"bookmark",
   "booksHref":"/api/v1/koreader/plugin/catalog/books?sort=title&smartScopeId=3"}, ...]}
```
Sam has **9 SmartScopes**; `smartScopeId=3` ("Want to Read") currently returns **58 books**.
Verified working:
```
GET <catalogBase>/books?sort=title&smartScopeId=3&size=5   -> 200, total:58, all readStatus want_to_read
```

Implement:
1. Add `int smartScopeId = 0;` to `BookOrbitBooksQuery` and append `&smartScopeId=<n>` in
   `fetchBooks()` when `> 0`. This is the whole transport change.
2. Add `KOReaderCatalogClient::fetchSmartScopes(std::vector<BookOrbitSmartScope>& out)` hitting
   `<catalogBase>/sections/smart-scopes`. Parse `id` (**it is a JSON STRING, not an int — parse
   accordingly**), `title`, and `icon`. Cap the list (16 is plenty) and reserve the vector.
3. Add a `SMART_SCOPES` browse mode to `BookOrbitCatalogActivity` that lists the scopes as a menu,
   and on select, runs a normal books query with that `smartScopeId`. **This generalizes: it gives
   Sam his TBR queue AND "Up Next in Series", "Continue Series", etc. from one code path.**
   Do NOT hardcode a "Want to Read" scope id — always fetch the list.
4. Follow the existing navigation idiom exactly: a dedicated menu screen navigated with
   Up/Down/Confirm. Do NOT overload already-bound list buttons.
5. Increment `BROWSE_MODE_COUNT` and keep the SECTIONS landing menu consistent.

## NEW FEATURE 2 — Stats screen degradation (bounded; no server change)

**Decision already made by Sam: do NOT change the server.** Use only what the deployed v2.4.0
dashboard actually returns.

The firmware's `BookOrbitDashboard` currently expects `currentStreak`, `longestStreak`, `goalBooks`,
`goalCompleted`, `goalYear`. **The deployed BookOrbit v2.4.0 no longer sends any of them** to kosync
clients (they moved to `dashboard/widgets/reading-streak`, which is JWT-only and returns 401 under
kosync auth). Confirmed live today.

What the live `/catalog/dashboard` DOES return:
```
username, displayName, totalBooks (549), generatedAt,
browseCounts { inProgress, libraries, authors, series, collections, smartScopes },
continueReading[] (with title/authors/series/progressPercentage/lastReadAt/readStatus/hasCover),
discover[] (12 items), sections[] (8)
```
Required behavior:
- Parse and display `totalBooks` and the `browseCounts.*` values (these are genuinely useful:
  "549 books · 2 in progress · 272 authors · 120 series · 9 SmartScopes").
- Treat the streak/goal fields as **absent, not zero**. Keep the `<0 = unknown` convention already
  in the struct and **render nothing** for unknown values — no "0 day streak", which would be a lie.
- Do not add a server call for streak. Do not fabricate values.

## Acceptance criteria (all must hold)

1. `pio run -e x4pro` **succeeds**. Report the RAM/flash numbers.
   Baseline before your changes: RAM 18.6% (60,924/327,680), Flash 84.3% (5,525,794/6,553,600).
   ⚠️ **Flash is the binding constraint — only ~1 MB free.** If you exceed ~99% flash, STOP and
   report rather than disabling unrelated features to make room.
2. `pio check -e x4pro --fail-on-defect medium --fail-on-defect high` reports no NEW defects.
3. Every ported feature is present and wired: page-stats upload, annotation upload + exchange,
   catalog browse, search, detail + cover, download, mark-finished, dashboard, SmartScopes/TBR.
4. The `uploadReadingStats` toggle exists in BOTH settings surfaces and defaults to OFF.
5. `smartScopeId` reaches the wire. Prove it by printing the exact constructed URL in a test/log.
6. No C3 workaround from the "DO NOT port" list is present.
7. Verify new symbols actually linked (do not trust the compiler alone):
   `~/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-nm .pio/build/x4pro/firmware.elf | grep -i smartscope`
8. `git status` clean at the end; work committed on `feat/bookorbit-x4pro` with focused commits.

## Hard constraints (from AGENTS.md / CLAUDE.md — read them, they are authoritative)

- `new` is NOT nothrow: use `new (std::nothrow)` or `makeUniqueNoThrow<T>()`.
- No exceptions, no `abort()`. `LOG_ERR(...)` + return on failure.
- All user-facing strings via `tr(STR_*)`; logs may be hardcoded.
- `.reserve(N)` before every `push_back` loop.
- HAL classes only (`FsFile`/`HalFile`, `Storage`, `halClock`) — not raw SDK.
- `string_view::data()` is NOT null-terminated.
- Activities: allocate in `onEnter()`, free in reverse in `onExit()`.
- **Libs under `lib/` CANNOT include from `src/`.** A client needing `src/` deps belongs in `src/`.
  This is why `KOReaderCatalogClient` lives in `src/network/`. The simulator env hides this
  violation; only `-e x4pro`/`-e default` catch it.
- Server payload caps (a single over-limit field 400s the whole batch):
  `deviceModel` <= 100, `pluginVersion` <= 20 (use the fixed short constants
  `crossink-ps-1` / `crossink-an-1` — never build these from the version string),
  `deviceId` matches `^[A-Za-z0-9-]{1,100}$`, hash must be 32-hex.
- ArduinoJson v7: `doc["k"] = JsonArray()` serializes as **null**. Use `doc["k"].to<JsonArray>()`.
- Always set `http.setTimeout(~20000)` + `setConnectTimeout` on any POST.
- Keep `deviceModel` containing "CrossInk"/"Crosspoint" — the server fork maps that to the
  `crosspoint` source badge. Consider `CrossInk X4 Pro` so Pro sessions are distinguishable.

## Out of scope (do not do these)

- No server-side changes to BookOrbit. None.
- No cert pinning / TLS trust changes.
- No read-status "finished" sync beyond the existing mark-finished write-back.
- No flashing to hardware (no device attached to this machine; Sam flashes via the web flasher).
- Do not touch the C3 `[env:default]` target's behavior.
