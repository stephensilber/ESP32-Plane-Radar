# AGENTS.md — ESP32 Plane Radar

Everything you need to pick this project up cold: what it is, how to build/flash
it, how it's put together, and — most importantly — **why the performance and
memory decisions are the way they are**, because this device is severely
memory-constrained and it's easy to reintroduce bugs that took a long time to
diagnose.

---

## 1. What this is

A live aircraft radar on a **1.28" round GC9A01 240×240 display** driven by an
**ESP32-C3 Super Mini**. It pulls nearby ADS-B traffic from a public API and
draws it on a radar scope (range rings, cardinal directions, aircraft icons
colored by class, heading/speed vectors, optional trails, origin→destination
route tags, runway overlays for nearby airports). All configuration happens
through a WiFiManager captive/LAN portal — there are no physical controls except
the single BOOT button (tap = cycle range/zoom, 3s hold = factory reset).

---

## 2. Hardware

- **MCU:** ESP32-C3 (single-core RISC-V @160 MHz). This *single core* is the
  root of most performance constraints — Wi-Fi/TLS and rendering share one core.
- **Usable heap:** ~150–180 KB free after Wi-Fi init. This is the budget that
  drives every memory decision below. Treat ~50–70 KB free as "healthy" at
  runtime; below ~20 KB things start failing (TLS can't allocate, portal won't
  render).
- **Display:** GC9A01, 240×240 round, SPI @40 MHz, `invert=true`, `BGR` order.
  Pins (see `include/config.h`): RST=0, CS=1, DC=10, MOSI/SDA=3, SCLK/SCL=4.
  LovyanGFX config in `include/hardware/lgfx_config.hpp`.
- **BOOT button:** GPIO9, active-LOW, interrupt-driven.
  - **Tap** → cycle range preset (5/10/15/25 km rings).
  - **Hold ≥3 s** → **factory reset** (wipes Wi-Fi creds, location, and all
    settings). This is NOT a restart. See gotchas.
- **4 MB flash**, dual-OTA partition layout (`partitions/plane_radar.csv`).

---

## 3. Build & flash

PlatformIO project, single env `supermini`. `pio` may not be on your PATH; use
your PlatformIO install (`~/.platformio/penv/bin/pio`, a venv, or
`python -m platformio`).

```bash
# Build only
pio run -e supermini

# Build AND produce the merged (serial-flashable) image via the post-script
pio run -e supermini -t merge
```

Outputs in `.pio/build/supermini/`:
- **`firmware.bin`** — app-only image. This is the **OTA** image.
- **`firmware-merged.bin`** — bootloader + partition table + app at 0x0. This is
  the **serial-flash** image (produced by `scripts/merge_firmware.py`, a
  `post:` extra_script).

The two shipping copies live in `release/`:
- `release/plane-radar-ota.bin`   ← app-only, for the portal's Update menu
- `release/plane-radar-merged.bin` ← full image, for serial/web.esphome.io

**Convention used throughout this project:** after a build, refresh both files
in `release/` (copy `firmware.bin`→`plane-radar-ota.bin`,
`firmware-merged.bin`→`plane-radar-merged.bin`) and commit. When telling the
user which file to flash, always name the exact file and its build time.

### Libraries (`platformio.ini`)
- `lovyan03/LovyanGFX@^1.2.7` (display)
- `tzapu/WiFiManager@^2.0.17` (captive/LAN config portal + OTA)
- `bblanchon/ArduinoJson@^7.4.2` (v7 elastic `JsonDocument`)
- Embedded asset: `data/ui_font.vlw` (anti-aliased Noto Sans Bold 15) via
  `board_build.embed_files`.
- Build flags of note: `-DWM_MDNS` (portal at `plane-radar.local`),
  `-DWM_NODEBUG`, USB-CDC-on-boot for serial over USB-C.

---

## 4. Flashing: serial vs OTA (READ THIS — it caused days of confusion)

### OTA only works because of the dual-OTA partition table
The **original partition table had a single app slot** ("no OTA slot"). OTA
fundamentally writes the new firmware to a *second* app slot and switches to it —
with one slot there was nowhere to write, so **every OTA silently failed**
(`Updater: premature end` / rejected). `partitions/plane_radar.csv` now has two
1.875 MB `ota_0`/`ota_1` slots.

### A partition-table change requires ONE serial flash
You cannot change the partition table over OTA (chicken-and-egg). Any change to
`plane_radar.csv` means the next update must be a **serial flash of the merged
image** (`plane-radar-merged.bin` at `0x0`, e.g. via
https://espressif.github.io/esptool-js/ or https://web.esphome.io/). Enter
download mode by holding BOOT and tapping RST/EN. After that, plain app changes
go over OTA again.

### OTA uses the app-only bin, not the merged bin
The portal **Update** menu expects `plane-radar-ota.bin` (app image). Uploading
the merged image there fails. This was a repeated early mistake.

### OTA must not compete with network fetches
The Performance-mode background task keeps hitting the network every 3 s. During
an OTA upload that starves the transfer and it dies at `pos:0`. Fixed via
`WiFiManager::setPreOtaUpdateCallback` → sets `s_ota_active` (in `wifi_setup.cpp`)
→ `wifiOtaActive()` gates the fetch task and single-loop fetch branches in
`main.cpp`. **This fix only helps once it's already on the device**, so the
transitional flash that introduced it had to be done with Performance mode off
(single-loop mode, where the upload blocks the loop so nothing competes).

---

## 5. Repo layout

```
include/config.h                 Pins, timing constants, default coords, fetch intervals
include/hardware/lgfx_config.hpp GC9A01 LovyanGFX bus/panel config
partitions/plane_radar.csv       Dual-OTA partition table (serial-flash to change)
scripts/merge_firmware.py        post: build step -> firmware-merged.bin
scripts/build_large_airports.py  Regenerates the compiled runway/airport dataset

src/main.cpp                     setup()/loop(), the perf FreeRTOS task, fetch cycle, mode gating
src/services/adsb_client.*       ADS-B fetch: double-buffer, stream parse, JSON filter, poll hook
src/services/route_client.*      Origin/dest lookup from vrs-standing-data.adsb.lol
src/services/trail.*             Fading breadcrumb store (fixed arrays, keyed by hex)
src/services/radar_location.*    Radar center lat/lon (NVS), parse/validate/clear
src/services/wifi_setup.*        WiFiManager portal, injected CSS/JS, params, OTA hook, BOOT button
src/ui/radar_display.*           All rendering: 8bpp frame sprite, icons, tags, trails, rotation
src/ui/radar_range.*             Settings state + NVS (every toggle, FPS, heading, range presets)
src/ui/radar_theme.h             Colors (RGB565 targets) + geometry constants
src/ui/runway_overlay.*          Nearby-airport runway lines + ICAO labels
src/ui/status_screens.*          Boot/portal/connect-failed/reset screens
src/data/large_airports_data.cpp Compiled airport+runway dataset (not SPIFFS)
```

Nothing uses SPIFFS/LittleFS — the font is embedded and the airport data is
compiled in. The filesystem partition is intentionally tiny.

---

## 6. Runtime architecture

### `setup()`
Init BOOT button + display → show setup screen if the force-portal flag is set →
init location + `rangeInit()` (loads all settings from NVS) → read
`perfMode()` into `g_perf_mode` → set the ADS-B/route **poll hook** (see below)
→ connect Wi-Fi (or open portal) → if perf mode, spawn `perfNetTask`
(`xTaskCreate`, 20 KB stack) with a single-loop fallback if the spawn fails.

### `loop()` — two modes
- **Single-loop (perf off):** the main loop does everything sequentially —
  handle BOOT button, pump the portal (`wifiLoop`), and on a timer do a blocking
  fetch+draw, an occasional blocking route lookup, or a render tick (for smooth
  motion). A blocking fetch freezes rendering while it runs; the **poll hook**
  (`setPollFn(wifiLoop)`) keeps the portal responsive during the blocking call.
- **Performance mode (perf on, the default):** `perfNetTask` on its own FreeRTOS
  task does all network I/O; the main loop only renders (at the FPS tick),
  handles buttons, and services the portal. The poll hook is `nullptr` in this
  mode (the main loop pumps the portal itself). This is what makes motion,
  buttons, and the portal stay smooth while the network blocks.

### Data flow
`fetchUpdate()` (adsb_client) pulls positions → publishes into a **double
buffer** → `runFetchCycle()` also records trails and queues route lookups →
render reads the buffer and draws. Route lookups are throttled and decoupled
from the fetch so their TLS handshakes don't hitch every cycle.

---

## 7. The memory story (the most important section)

The whole system lives or dies on the ~150–180 KB heap. Every one of these is a
deliberate fix for an observed failure; **do not regress them.**

### 8-bit (RGB332) frame buffer — the single biggest decision
`ensureFrameSprite()` in `radar_display.cpp` creates the off-screen frame as
`setColorDepth(8)` → **56 KB** instead of 112 KB at 16bpp. This is a full-screen
double buffer for flicker-free compositing (draw everything off-screen, blit
once).

*Why:* the 16bpp (112 KB) buffer starved the heap to ~15 KB. Symptoms were
`SSL - Memory allocation failed (-32512)`, the settings page rendering blank (it
needs ~15 KB to build), and truncated/`IncompleteInput` fetches. Dropping to
8bpp freed 56 KB and fixed all of it. Cost: 256-color palette (RGB332), slightly
coarser anti-aliasing — visually near-invisible at this size.

### Stream-parse the ADS-B response — don't buffer the whole body
`fetchUpdate()` parses **directly from the network stream**
(`deserializeJson(doc, *http.getStreamPtr(), Filter(filter))`) instead of
reading the response into a `String` first.

*Why:* at the widest zoom near busy airspace the response is 30–40 aircraft
(~35 KB). Buffering it needed one contiguous block larger than the biggest free
block, so the read truncated → `InvalidInput`/`IncompleteInput`. Streaming keeps
peak heap independent of aircraft count.

### ArduinoJson field filter
The feed sends ~50 fields per aircraft; we use ~17. A `Filter` document
(`filter["ac"][0][field] = true`) keeps the parsed `JsonDocument` small
regardless of response size. Keep this in sync if you read new fields.

### Double-buffered aircraft data (thread safety)
`s_aircraft[2][kMaxAircraft]` with `volatile uint8_t s_active`. The fetch fills
the inactive buffer and publishes with a single atomic index flip, so the
renderer (main loop) never sees a half-updated frame while the perf task writes.
Trails and the route cache are fixed arrays — cross-task access can cause a
cosmetic one-frame glitch but never a crash; that's an accepted trade for perf
mode.

### Connect/retry tuning
`kConnectAttemptMs = 2000` (200 ms was too tight for a remote TLS connect from
the C3) and `kMaxConnectRetries = 1` (rapid retries piled onto adsb.fi's 1 req/s
limit and spammed SSL errors; the next 3 s poll is the retry).

### Heap health, at a glance
Every successful fetch logs `adsb: N aircraft (freeHeap X, maxAlloc Y)`. Healthy
is `freeHeap` ~50–70 KB. `maxAlloc` (largest contiguous block) matters as much as
total free — TLS needs a ~32 KB contiguous block. **Any new feature that
allocates a large contiguous buffer (a second sprite, a big String, unfiltered
JSON) will bring back the crisis.**

---

## 8. Performance mode

Opt-in background-task mode, now **default-on** for a smooth out-of-box
experience. Toggle: "Performance mode (restart to apply)". Read once at boot into
`g_perf_mode`.

- Spawns `perfNetTask` (`xTaskCreate`, 20 KB stack). If the spawn fails it falls
  back to single-loop (so it can't hang boot).
- Requires a restart to change (the execution model is chosen at boot).
- It reintroduces the biggest transient memory pressure (20 KB task stack + the
  fetch's TLS running concurrently with the portal). This is survivable *because*
  of the 8bpp buffer + stream parse + JSON filter above. If you undo any of
  those, perf mode + portal will collide again.
- Default is **on** in both `rangeInit()` and `unitsReset()`; the `xTaskCreate`
  fallback is the safety net if a fresh/reset device can't spawn the task.

---

## 9. Settings & NVS

Settings live in NVS namespace **`planeradar`** (managed in `radar_range.cpp`).
Wi-Fi/portal state lives in a separate **`wifi`** namespace (avoids NVS handle
conflicts). Actual Wi-Fi credentials live in the system `nvs.net80211` namespace
(managed by esp_wifi/WiFiManager), which OTA never touches.

Each setting follows the same pattern: a `getBool` with a default, a
`saveXFromPortal(checkbox_value)` that calls `saveBoolPref`, a portal checkbox,
and a line in `unitsReset()`. **NVS keys must be ≤15 chars.**

**Defaults (a fresh/reset device boots in "peak" config):** all display/feature
toggles ON (miles, runways, color-by-class, heli icon, route, smooth motion,
trails, speed-not-altitude, icons-only-at-max-zoom, and the five map filters),
**Performance mode ON**, **12 FPS**, range preset index 1 (10 km ring), default
center `33.273735, -96.739407` (`config.h`).

Map filters ("Show on map"): Commercial / Private / Military (by class) and
Helicopters / Planes (by type). An aircraft draws only if **both** its class and
type are enabled — `aircraftVisible()` in `radar_display.cpp`. Hidden aircraft
drop off entirely (icon, tag, and beyond-ring dot).

---

## 10. The config portal (WiFiManager)

- ATC/radar-scope theme injected via `setCustomHeadElement(kPortalCss)` in
  `wifi_setup.cpp` — a self-contained `<style>` + `<script>` string (no external
  assets, so it works on the captive AP). The script also: renames "Setup"→
  "Options", adds a "Back to Options" link on the save page, retitles headings to
  "Aircraft Radar", syncs the FPS slider readout, and injects the "Useful links"
  section (ESP Web, adsb.lol, adsbdb).
- Title set with `s_wm.setTitle("Aircraft Radar")`.
- Menu: `{wifi, param, info, exit, sep, update}` — settings live on their own
  `/param` ("Options") page, deliberately **not** crammed under the Wi-Fi scan
  list, so each page stays small enough to build on the constrained heap.
- **Checkboxes:** created with value `"T"`, `WFM_LABEL_AFTER`, and a mutable
  `char attrs[32]` holding `type="checkbox"` (+ ` checked`). The "checked" state
  comes from that prefill string, which is only rebuilt at portal start — so
  `refreshPortalParamDefaults()` is called again at the **end of the save
  callback**, otherwise a just-toggled box renders stale (looks like it didn't
  save). Don't remove that.
- **Custom-HTML params** (getID()==NULL) render their raw HTML directly — used
  for the Back link, the "Show on map" header, the `<br>` spacer, and the
  draggable compass widget (writes into a hidden `radar_heading` field).
- The **UPDATE** button gets WiFiManager's `.D` "danger" (red) class; it's
  overridden back to theme green via `button#uploadbin{...}` while the **Erase**
  button keeps danger-red.

---

## 11. Gotchas & limitations

- **Heap is the ceiling on everything.** Before adding a feature, ask what it
  allocates. See §7. The `freeHeap/maxAlloc` log is your smoke test.
- **BOOT hold ≥3 s = factory reset, not a restart.** It wipes Wi-Fi + location +
  settings (`resetWifiCredentials` → `markForceConfigPortal` →
  `wifiSetupConnect` calls `eraseWifiCredentials` on next boot). To just restart,
  power-cycle or tap RST/EN. The reset is now guarded to not fire during an OTA
  (`!s_ota_active`).
- **OTA vs merged bin, and partition changes need serial** — see §4.
- **adsb.fi rate-limits at ~1 req/s** (returns HTTP 429). We fetch every 3 s with
  a single connect attempt; don't add aggressive retries.
- **NVS keys ≤15 chars.**
- **LovyanGFX 1.2.7 ships a global `fonts` shim** — do NOT re-add a
  `namespace fonts = lgfx::v1::fonts;` alias (it collides). Prefer
  forward-compatible fixes over pinning library versions.
- **Single core:** in single-loop mode a blocking fetch/lookup freezes rendering
  for its duration; the poll hook only keeps the *portal* alive, not the render.
  Perf mode is the real fix.
- **Route data source** is `vrs-standing-data.adsb.lol/routes/{XX}/{callsign}.json`
  (static, connectionless single request — chosen to avoid a redirect/second
  handshake). Routes are looked up only for commercial aircraft inside the ring,
  throttled to every 12 s.
- Fetch timing: `kAdsbFetchIntervalMs = 3000`, `kRouteLookupIntervalMs = 12000`,
  request timeout 4 s.

---

## 12. Debugging: serial log signatures

Watch the serial monitor (115200). Key lines and what they mean:

| Log line | Meaning |
|---|---|
| `Build: <date> <time>` | Boot stamp — confirm a flash actually landed |
| `Performance mode: network on background task` | perf task spawned OK |
| `Performance mode: task create failed, using single loop` | fell back (rare) |
| `adsb: N aircraft (freeHeap X, maxAlloc Y)` | healthy fetch; watch the heap numbers |
| `adsb: JSON parse error: InvalidInput/IncompleteInput` | truncated read — almost always memory pressure |
| `SSL - Memory allocation failed (-32512)` | heap too low for TLS — memory regression |
| `start_ssl_client: -1` / `DNS Failed` / `adsb: HTTP -1/-11` | network/DNS layer (rate-limit, weak Wi-Fi, or resource exhaustion) |
| `OTA update starting — pausing network fetches` | OTA hook fired; fetches paused |
| `BOOT held — resetting WiFi` | factory reset triggered |

---

## 13. Working conventions in this repo

- Build with the `merge` target, refresh **both** `release/` bins, commit, and
  tell the user the exact filename + build time to flash and whether it's
  OTA-able (app-only change) or needs serial (partition change).
- Comments explain **why**, not what. Match surrounding style.
- Co-author trailer on commits:
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
- There's a companion memory note (`esp32c3-heap-constraints`) capturing the heap
  rules — the authoritative version of §7 is the code + this file.
