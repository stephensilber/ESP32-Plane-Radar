#pragma once

#include <cstddef>

namespace services::route {

/** IATA code buffer size: 3 letters + null. */
constexpr size_t kCodeLen = 4;

/** Hook invoked during HTTP I/O to keep WiFi/portal alive. Optional. */
using PollFn = void (*)();
void setPollFn(PollFn fn);

/**
 * Register a callsign that may have a route (commercial flights). Idempotent.
 * The position/track are used to pick the current leg of a multi-stop routing
 * (e.g. DFW-CZM-DFW) when the route is resolved.
 */
void note(const char* callsign, float lat, float lon, float track_deg);

/**
 * Resolve at most one still-unknown callsign via adsbdb.com. Blocks on one
 * HTTP request, so call it AFTER the frame is drawn (routes are static; a
 * one-cycle delay before a route first appears is imperceptible).
 */
void pump();

/** Fill origin/dest (IATA) if the callsign has a known route. */
bool lookup(const char* callsign, char* origin, char* dest);

/** Forget all cached routes. */
void clear();

}  // namespace services::route
