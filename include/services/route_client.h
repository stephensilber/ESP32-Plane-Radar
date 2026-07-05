#pragma once

#include <cstddef>

namespace services::route {

/** IATA code buffer size: 3 letters + null. */
constexpr size_t kCodeLen = 4;

/** Hook invoked during HTTP I/O to keep WiFi/portal alive. Optional. */
using PollFn = void (*)();
void setPollFn(PollFn fn);

/** Register a callsign that may have a route (commercial flights). Idempotent. */
void note(const char* callsign);

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
