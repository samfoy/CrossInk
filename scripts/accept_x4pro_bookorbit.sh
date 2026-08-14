#!/usr/bin/env bash
# Deterministic acceptance gate for the BookOrbit X4 Pro port.
# Kept in a script because AutoLoop 0.10.x splits verify_cmds list values on commas.
set -uo pipefail

cd /home/sam/workspace/crossink-x4pro || exit 1
export PATH="$HOME/.local/bin:$PATH"

fail=0
note() { printf '%s\n' "$*"; }

note "=== [1/5] build: pio run -e x4pro (ESP32-S3 X4 Pro target) ==="
if pio run -e x4pro 2>&1 | tail -40; then
  note "BUILD: ok"
else
  note "BUILD: FAILED"
  fail=1
fi

# The pipe above masks pio's exit status, so confirm the artifact really exists.
if [ ! -f .pio/build/x4pro/firmware.bin ]; then
  note "BUILD ARTIFACT MISSING: .pio/build/x4pro/firmware.bin"
  fail=1
else
  sz=$(stat -c %s .pio/build/x4pro/firmware.bin)
  note "firmware.bin size: ${sz} bytes"
  # Guard the real constraint: the app partition is 6553600 bytes.
  if [ "$sz" -gt 6488064 ]; then
    note "FLASH OVERRUN RISK: firmware.bin exceeds 99% of the 6553600-byte app partition"
    fail=1
  fi
fi

note "=== [2/5] confirm it is genuinely an ESP32-S3 image ==="
if file .pio/build/x4pro/firmware.bin | grep -q "ESP32-S3"; then
  note "IMAGE TARGET: ESP32-S3 ok"
else
  note "IMAGE TARGET: not an ESP32-S3 image"
  fail=1
fi

note "=== [3/5] static analysis (new medium/high defects) ==="
if pio check -e x4pro --fail-on-defect medium --fail-on-defect high 2>&1 | tail -25; then
  note "CHECK: ok"
else
  note "CHECK: defects reported"
  fail=1
fi

note "=== [4/5] page-stats sync linked; catalog browser fully GONE ==="
NM=$(find "$HOME/.platformio/packages" -name "xtensa-esp32s3-elf-nm" -type f 2>/dev/null | head -1)
if [ -z "$NM" ]; then
  NM=$(find "$HOME/.platformio/packages" -name "xtensa-esp-elf-nm" -type f 2>/dev/null | head -1)
fi
if [ -n "$NM" ] && [ -f .pio/build/x4pro/firmware.elf ]; then
  # The BookOrbit catalog browser was removed (OPDS covers browsing); page-stats
  # sync is independent and MUST still be linked. Assert both directions so a
  # partial removal or an accidental resurrection both fail the gate.
  if "$NM" .pio/build/x4pro/firmware.elf 2>/dev/null | grep -qiE 'uploadPageStats|capturePageStatEvent'; then
    note "LINK: page-stats sync symbols present"
  else
    note "LINK: page-stats sync symbols MISSING from firmware.elf"
    fail=1
  fi
  if "$NM" .pio/build/x4pro/firmware.elf 2>/dev/null | grep -qiE 'smartscope|BookOrbitCatalogActivity|KOReaderCatalogClient'; then
    note "LINK: catalog browser symbols STILL PRESENT (removal incomplete)"
    fail=1
  else
    note "LINK: catalog browser symbols gone"
  fi
else
  note "LINK: could not run nm (missing toolchain or elf)"
  fail=1
fi

note "=== [5/5] C3-only workarounds must NOT be present ==="
if grep -rn "HTTP_RX_BUF" platformio.ini 2>/dev/null | grep -q 4096; then
  note "REGRESSION: C3 TLS buffer shrink (HTTP_RX_BUF=4096) present"
  fail=1
else
  note "no C3 TLS buffer shrink: ok"
fi

if [ "$fail" -eq 0 ]; then
  note "ACCEPTANCE: PASS"
else
  note "ACCEPTANCE: FAIL"
fi
exit "$fail"
