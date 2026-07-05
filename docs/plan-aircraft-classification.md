# Plan: Aircraft classification, icons & route tags
Three new, individually-toggleable radar features:

1. **Color the arrow** by class — military / commercial / private.
  
2. **Distinct icon** for helicopters vs. planes.
  
3. **Origin → destination** airport codes on the tag, in a different color from the model.
  

Everything is driven by a couple of new fields we already have access to, plus (for #3 only) a second HTTP lookup. Each feature ships behind a portal checkbox that persists to NVS, matching the existing `Show airport runways` / `Display in miles` pattern.

* * *
## What the data source actually gives us
Confirmed live against the readsb schema (adsb.fi v3 / adsb.lol v2 are identical):

| Field | Meaning | Used for |
|-------|---------|----------|
| `category` | ADS-B emitter category: `A1` light, `A2` small, `A3` large, `A5` heavy, **`A7` rotorcraft** | helicopter icon (#2), commercial/private hint (#1) |
| `dbFlags` | bitmask; **bit 0 (`& 1`) = military** | military color (#1) |
| `r` | registration / tail number (e.g. `N51539`) | private-vs-commercial heuristic (#1) |
| `t` | ICAO type code (e.g. `B738`, `A7` heli type `B412`) | already parsed → model tag |
| `flight` | callsign (e.g. `AAL3114`) | already parsed; airline-vs-tail heuristic (#1) |

**Not present anywhere in the feed: origin/destination.** Feature #3 requires a second data source (see its section).

* * *
## Feature 1 — Color arrow by class (military / commercial / private)
### Classification (heuristic — worth being honest that it's not exact)
Order of checks, computed once at parse time and stored on the `Aircraft` struct:

1. `dbFlags & 1` → **Military**
  
2. Callsign is an airline flight number — 3 alpha chars followed by digits, and not equal to the registration (e.g. `AAL3114`, `UAL2181`) → **Commercial**
  
3. Otherwise (tail-number callsign like `N51539`, or blank) → **Private / GA**
  

`category` can refine step 2/3 as a tiebreaker (A3/A5 lean commercial, A1/A2 lean private), but callsign-shape is the strongest single signal. I'd start with the callsign rule + military flag and treat category as secondary.

> Caveat to set expectations: there is no authoritative "commercial vs private" bit in ADS-B. Police/medical helicopters, bizjets on airline-style callsigns, etc. will occasionally miscolor. Military is reliable; the commercial/private split is a best-effort heuristic.
### Code changes
- `include/services/adsb_client.h` — extend `struct Aircraft`:
  
  - add `enum class Class : uint8_t { Private, Commercial, Military }` (or a plain `uint8_t class_id`)
    
  - add `Aircraft::klass`
    
- `src/services/adsb_client.cpp`
  
  - parse `dbFlags` (int), `category` (`char[3]`), `r` (registration) inside `fillTagFields()` / the per-plane loop (~L268–273).
    
  - add `classifyAircraft(plane, callsign, registration)` returning the class; assign to `s_aircraft[n].klass`.
    
- `include/ui/radar_theme.h` — add three palette targets + `extern uint16_t` handles, mirroring `kColorAircraft`:
  
  - `kColorMilitary` (e.g. olive/yellow-green), `kColorCommercial` (keep current red, or cyan), `kColorPrivate` (green).
    
- `src/ui/radar_display.cpp`
  
  - `initPalette()` (~L177) — build the three colors with the **same BGR R/B swap** guard used for `kColorAircraft` (the GC9A01 is a BGR panel; skipping this makes red render blue).
    
  - add `aircraftColor(const Aircraft&)` → returns the class color when the toggle is on, else `kColorAircraft`.
    
  - `drawAircraft()` (~L538) — pass `aircraftColor(planes[i])` to `drawHeadingTriangle` instead of the fixed `kColorAircraft`. Also feed it to the beyond-ring rim dot (`drawBeyondRingDot`, ~L270) so the color cue is consistent off-ring.
    
### Config toggle
`Color aircraft by type` checkbox → `s_color_by_class` in `radar_range.cpp` (NVS key e.g. `colorCls`), plus getter `colorByClass()` and `saveColorByClass...()`, wired into `wifi_setup.cpp` exactly like `s_param_runways`.

* * *
## Feature 2 — Helicopter icon vs. plane icon
`category == "A7"` is the rotorcraft signal (confirmed live: 4 helicopters in the sample). Clean, no heuristic needed.
### Icon design (at ~16 px this has to stay legible)
- **Plane**: keep the existing filled heading triangle (`drawHeadingTriangle`).
  
- **Helicopter**: a **rotor glyph** — a small center hub dot with two crossed rotor blades (an "X" or "+") drawn as `drawWideLine`s, optionally rotated with heading. A rotor cross reads unmistakably as "helicopter" at small size and is cheap to draw. (Alternative considered: an "H" in a circle — rejected, too fussy to render crisply at 16 px.)
  

I'd keep the blades rotating with `nose_deg` so it still conveys heading like the triangle does; the speed vector logic is unchanged.
### Code changes
- `src/services/adsb_client.cpp` — store `category` (or just a `bool is_rotor` derived from `category=="A7"`) on `Aircraft`.
  
- `include/ui/radar_theme.h` — add rotor geometry constants (`kHeliRotorLenPx`, `kHeliHubRadiusPx`, `kHeliBladeHalfWidth`).
  
- `src/ui/radar_display.cpp` — add `drawHelicopter(cx, cy, heading, color)`; in `drawAircraft()` branch on `is_rotor` to call it instead of `drawHeadingTriangle`. Color still comes from Feature 1's `aircraftColor()` so the two features compose.
  
### Config toggle
`Distinct helicopter icon` checkbox → `s_heli_icon` (NVS `heliIcon`), same wiring. When off, helicopters draw as triangles.

* * *
## Feature 3 — Origin → destination airport codes on the tag
**This is the biggest lift** because the route is not in the ADS-B feed. It needs a second network call and caching. Flagging the tradeoffs up front so we can decide scope.
### Data source options
| Source | Shape | Notes |
|--------|-------|-------|
| **adsbdb.com** `GET /v0/callsign/{callsign}` | returns `flightroute` with origin/destination ICAO **and IATA** codes | Free, no key, one call per callsign. Simple. Best fit. |
| **adsb.lol** `POST /api/0/routeset` | batch: `[{callsign, lat, lng}, …]` → routes | One call for all visible aircraft; heavier request. |

Recommendation: **adsbdb.com per-callsign with an on-device cache**, since only a handful of aircraft are on-ring at once and callsigns repeat across the 5 s polls.
### Design
- New service `services/route_client` (`.h`/`.cpp`):
  
  - small fixed cache: `callsign → {origin[5], dest[5], bool resolved}`, ~16–32 entries, LRU/round-robin eviction (mirrors the fixed-array style of `adsb_client`).
    
  - `lookupRoute(callsign)` returns cached result immediately; enqueues a fetch if unknown.
    
  - a `routeLoop()` / rate-limited pump that resolves **one** callsign per radar poll (or a couple), reusing the existing `WiFiClientSecure` + `performGetWithPoll` polling pattern so it never blocks the render loop. adsbdb has rate limits — one lookup per tick is plenty since routes are static for a flight.
    
  - GA / private flights and blank callsigns simply resolve to "no route" and draw nothing.
    
- Store resolved `origin[5]` / `dest[5]` back onto the matching `Aircraft` (or fetch from cache at draw time).
  
### Tag rendering (the "different color" part)
Current tag = 3 stacked lines (callsign / type / altitude), each a single-color `drawString` (`drawAircraftTag`, ~L405). For the route we append to the **type line**:

```
AAL3114
A321  JFK→LAX      ← "A321" in kColorTagType, "JFK→LAX" in a new kColorTagRoute
28000 ft
```

Implementation: after drawing `type`, draw the route string at `anchor_x ± textWidth(type) + gap` with `kColorTagRoute`. Needs care for the **right-anchored (east) side**, where text is `top_right` datum — measure both segments and lay them out so the combined block still respects the screen-edge clamp in `drawAircraftTag`. `measureTagBlockWidth` (~L381) must include `type + route` width so the block doesn't clip.

Codes: prefer IATA (`JFK`, 3 char) over ICAO (`KJFK`, 4 char) to save horizontal space on the 240 px display. Separator `→` (or `-` if the embedded VLW font lacks the arrow glyph — worth checking `data/ui_font.vlw`).
### Config toggle
`Show flight route` checkbox → `s_show_route` (NVS `showRoute`). When off, no route lookups happen at all (saves the extra traffic).
### Scope note
Features 1 & 2 are self-contained and cheap (parse two more fields + a draw branch). **Feature 3 adds a networked subsystem** (second API, cache, rate limiting, two-color line layout). Reasonable to land 1 & 2 first, then 3 as a follow-up — or cut 3 if the added HTTP traffic/complexity isn't wanted on the C3.

* * *
## Cross-cutting: config plumbing summary
All three toggles follow the established pattern, so the portal work is mechanical:

- `src/ui/radar_range.cpp` — three new `bool` statics + NVS keys in the `planeradar` namespace, `saveXFromPortal()` setters (reuse `portalCheckboxChecked`), getters, and reset them in `unitsReset()`.
  
- `include/ui/radar_range.h` — declare the getters/setters.
  
- `src/services/wifi_setup.cpp` — three `WiFiManagerParameter` checkboxes + `checked` prefill in `refreshPortalParamDefaults()` + save calls in `onPortalParamsSaved()` + `addParameter()` in `attachPortalParams()`.
  
- **README.md** — add the three fields to the "Custom fields" table and Aircraft section.
  
## Files touched (at a glance)
| File | 1 · color | 2 · heli | 3 · route |
| --- | :---: | :---: | :---: |
| `include/services/adsb_client.h` | ✓   | ✓   | ✓   |
| `src/services/adsb_client.cpp` | ✓   | ✓   | ✓   |
| `include/ui/radar_theme.h` | ✓   | ✓   | ✓   |
| `src/ui/radar_display.cpp` | ✓   | ✓   | ✓   |
| `include/ui/radar_range.h` + `.cpp` | ✓   | ✓   | ✓   |
| `src/services/wifi_setup.cpp` | ✓   | ✓   | ✓   |
| `services/route_client.h` + `.cpp` (new) |     |     | ✓   |
| `README.md` | ✓   | ✓   | ✓   |
## Open questions for you
1. {==**Color scheme** — what colors for each class? Proposed: Military = olive/yellow-green, Commercial = red (current), Private = green. (Grid is green already; may want a different private color to avoid clashing.)==}{>>those work<<}{id="c3" by="user" at="2026-07-05T05:06:30.717Z"}
  
2. {==**Route API** — OK to add adsbdb.com as a second dependency, or keep the device single-source? Affects whether #3 ships.==}{>>yes it just shouldn't block rendering<<}{id="c1" by="user" at="2026-07-05T05:06:04.184Z"}
  
3. **ICAO vs IATA** codes for the route — {==`JFK→LAX`==}{>>yes compact<<}{id="c2" by="user" at="2026-07-05T05:06:17.642Z"} (compact) vs `KJFK→KLAX` (matches the runway overlay labels)?
  
4. **Ship all three together, or land 1 & 2 first** and treat 3 as a follow-up?
