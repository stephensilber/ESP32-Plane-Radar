#include "services/trail.h"

#include <Arduino.h>

#include <cmath>
#include <cstring>

namespace services::trail {

namespace {

constexpr int kMaxAircraft = 40;
constexpr float kKmPerDeg = 111.0f;
/** Ignore fixes closer than this to the previous point (parked / jitter). */
constexpr float kMinStepKm = 0.03f;

struct Entry {
  char hex[7];
  Trail trail;
  unsigned long last_ms;
  bool used;
};

Entry s_entries[kMaxAircraft];

Entry* find(const char* hex) {
  for (Entry& e : s_entries) {
    if (e.used && strcmp(e.hex, hex) == 0) {
      return &e;
    }
  }
  return nullptr;
}

Entry* allocate(const char* hex) {
  Entry* slot = nullptr;
  for (Entry& e : s_entries) {
    if (!e.used) {
      slot = &e;
      break;
    }
    if (slot == nullptr || e.last_ms < slot->last_ms) {
      slot = &e;  // fall back to evicting the least-recently-seen
    }
  }
  slot->used = true;
  strncpy(slot->hex, hex, sizeof(slot->hex) - 1);
  slot->hex[sizeof(slot->hex) - 1] = '\0';
  slot->trail.count = 0;
  slot->trail.head = 0;
  return slot;
}

}  // namespace

void append(const char* hex, float lat, float lon) {
  if (hex == nullptr || hex[0] == '\0') {
    return;
  }
  Entry* e = find(hex);
  if (e == nullptr) {
    e = allocate(hex);
  }

  if (e->trail.count > 0) {
    const uint8_t last = (e->trail.head + kMaxPoints - 1) % kMaxPoints;
    const float dx = (lon - e->trail.lon[last]) * kKmPerDeg;
    const float dy = (lat - e->trail.lat[last]) * kKmPerDeg;
    if (sqrtf(dx * dx + dy * dy) < kMinStepKm) {
      e->last_ms = millis();
      return;  // barely moved — don't clutter the trail.
    }
  }

  e->trail.lat[e->trail.head] = lat;
  e->trail.lon[e->trail.head] = lon;
  e->trail.head = (e->trail.head + 1) % kMaxPoints;
  if (e->trail.count < kMaxPoints) {
    ++e->trail.count;
  }
  e->last_ms = millis();
}

const Trail* get(const char* hex) {
  if (hex == nullptr || hex[0] == '\0') {
    return nullptr;
  }
  const Entry* e = find(hex);
  return (e != nullptr && e->trail.count > 0) ? &e->trail : nullptr;
}

void pointAt(const Trail& t, int i, float* lat, float* lon) {
  const int idx = (t.head - t.count + i + 2 * kMaxPoints) % kMaxPoints;
  *lat = t.lat[idx];
  *lon = t.lon[idx];
}

void clear() {
  for (Entry& e : s_entries) {
    e.used = false;
  }
}

}  // namespace services::trail
