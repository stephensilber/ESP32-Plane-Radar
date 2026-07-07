#pragma once

#include <cstdint>

namespace services::trail {

/** ~90 s of history at the 3 s poll interval. */
constexpr int kMaxPoints = 30;

/** Ring buffer of past fixes for one aircraft. */
struct Trail {
  float lat[kMaxPoints];
  float lon[kMaxPoints];
  uint8_t count;  // valid points, <= kMaxPoints
  uint8_t head;   // next write index
};

/** Record an aircraft's current position, keyed by its ICAO hex. */
void append(const char* hex, float lat, float lon);

/** Trail for an aircraft, or nullptr if none recorded. */
const Trail* get(const char* hex);

/** i-th point in chronological order (0 = oldest). */
void pointAt(const Trail& t, int i, float* lat, float* lon);

/** Forget all trails (e.g. when the feature is toggled off). */
void clear();

}  // namespace services::trail
