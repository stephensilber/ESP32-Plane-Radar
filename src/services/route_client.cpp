#include "services/route_client.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <cmath>
#include <cstring>

namespace services::route {

namespace {

// adsb.lol route DB: GET redirects to vrs-standing-data; more current than
// adsbdb for reused flight numbers (e.g. AAL409 -> DFW-LIR, not JFK-MIA).
constexpr char kApiBase[] = "https://api.adsb.lol/api/0/route/";
constexpr size_t kMaxRoutes = 24;
// Kept short: this GET blocks the render loop (redirect + fresh TLS), so a slow
// route server must fail fast rather than freeze aircraft motion for seconds.
constexpr int kConnectTimeoutMs = 1500;
constexpr unsigned long kRequestTimeoutMs = 2500;

enum class State : uint8_t { Empty, Pending, Resolved, None };

struct Entry {
  char callsign[9];
  char origin[kCodeLen];
  char dest[kCodeLen];
  float lat;
  float lon;
  float track_deg;
  State state;
};

Entry s_routes[kMaxRoutes];
size_t s_next_slot = 0;  // round-robin eviction cursor
PollFn s_poll_fn = nullptr;

void pollNetwork() {
  if (s_poll_fn != nullptr) {
    s_poll_fn();
  }
}

Entry* find(const char* callsign) {
  for (Entry& e : s_routes) {
    if (e.state != State::Empty && strcmp(e.callsign, callsign) == 0) {
      return &e;
    }
  }
  return nullptr;
}

Entry* firstPending() {
  for (Entry& e : s_routes) {
    if (e.state == State::Pending) {
      return &e;
    }
  }
  return nullptr;
}

void copyCode(char* out, const char* start, size_t n) {
  if (n >= kCodeLen) {
    n = kCodeLen - 1;
  }
  memcpy(out, start, n);
  out[n] = '\0';
}

/** Split "DFW-LIR" (or multi-leg "A-B-C") into first/last IATA codes. */
bool parseAirportCodes(const char* codes, char* origin, char* dest) {
  const char* first_dash = strchr(codes, '-');
  const char* last_dash = strrchr(codes, '-');
  if (first_dash == nullptr || first_dash == codes || last_dash[1] == '\0') {
    return false;
  }
  copyCode(origin, codes, static_cast<size_t>(first_dash - codes));
  copyCode(dest, last_dash + 1, strlen(last_dash + 1));
  return origin[0] != '\0' && dest[0] != '\0';
}

struct Airport {
  char iata[kCodeLen];
  float lat;
  float lon;
};

constexpr int kMaxLegs = 6;

int parseAirports(JsonArray arr, Airport* out) {
  int n = 0;
  for (JsonObject a : arr) {
    if (n >= kMaxLegs) {
      break;
    }
    const char* code = a["iata"].as<const char*>();
    if (code == nullptr || code[0] == '\0' || !a["lat"].is<float>() ||
        !a["lon"].is<float>()) {
      continue;
    }
    copyCode(out[n].iata, code, strlen(code));
    out[n].lat = a["lat"].as<float>();
    out[n].lon = a["lon"].as<float>();
    ++n;
  }
  return n;
}

/**
 * For a multi-stop routing, choose the segment the aircraft is currently on.
 * Score = cross-track distance to the segment + a heavy penalty for the
 * destination not being ahead (via track), which disambiguates out-and-back
 * routes that share an endpoint (e.g. DFW-CZM-DFW). Returns the origin index.
 */
int pickLeg(const Airport* ap, int n, float plat, float plon, float track_deg) {
  if (n <= 2) {
    return 0;
  }
  constexpr float kDeg = 0.01745329252f;
  constexpr float kKmPerDeg = 111.0f;
  const float coslat = cosf(plat * kDeg);

  float best_score = 1e30f;
  int best_i = 0;
  for (int i = 0; i + 1 < n; ++i) {
    const float ax = (ap[i].lon - plon) * coslat * kKmPerDeg;
    const float ay = (ap[i].lat - plat) * kKmPerDeg;
    const float bx = (ap[i + 1].lon - plon) * coslat * kKmPerDeg;
    const float by = (ap[i + 1].lat - plat) * kKmPerDeg;
    const float vx = bx - ax;
    const float vy = by - ay;
    const float len2 = vx * vx + vy * vy;

    // Plane is the origin; project it onto the clamped segment.
    float t = (len2 > 0.0f) ? -(ax * vx + ay * vy) / len2 : 0.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float cx = ax + t * vx;
    const float cy = ay + t * vy;
    const float cross_km = sqrtf(cx * cx + cy * cy);

    float bearing_to_b = atan2f(bx, by) / kDeg;
    float diff = bearing_to_b - track_deg;
    while (diff > 180.0f) diff -= 360.0f;
    while (diff < -180.0f) diff += 360.0f;
    const float align = cosf(diff * kDeg);  // 1 = heading toward B, -1 = away

    const float score = cross_km + (1.0f - align) * 1000.0f;
    if (score < best_score) {
      best_score = score;
      best_i = i;
    }
  }
  return best_i;
}

bool fetchRoute(const Entry& entry, char* origin, char* dest) {
  String url = kApiBase;
  url += entry.callsign;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(kConnectTimeoutMs);
  http.setTimeout(kRequestTimeoutMs);
  // Route lookup 302-redirects to the vrs-standing-data host.
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) {
    return false;
  }

  pollNetwork();
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    // 404 = no route on file for this callsign; settled, don't retry.
    http.end();
    return code == HTTP_CODE_NOT_FOUND;
  }

  String payload = http.getString();
  http.end();
  pollNetwork();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    return true;  // 200 but not the expected JSON — settled, no route.
  }

  // Prefer the airport list (has coordinates) so we can pick the current leg of
  // a multi-stop routing; fall back to the plain first/last code string.
  JsonArray airports = doc["_airports"].as<JsonArray>();
  Airport ap[kMaxLegs];
  const int n = airports.isNull() ? 0 : parseAirports(airports, ap);
  if (n >= 2) {
    const int i = pickLeg(ap, n, entry.lat, entry.lon, entry.track_deg);
    strcpy(origin, ap[i].iata);
    strcpy(dest, ap[i + 1].iata);
    return true;
  }

  const char* codes = doc["_airport_codes_iata"].as<const char*>();
  if (codes == nullptr) {
    return true;  // settled, no route.
  }
  parseAirportCodes(codes, origin, dest);
  return true;
}

}  // namespace

void setPollFn(PollFn fn) { s_poll_fn = fn; }

void note(const char* callsign, float lat, float lon, float track_deg) {
  if (callsign == nullptr || callsign[0] == '\0') {
    return;
  }

  // Keep a pending entry's position fresh so the leg pick uses the latest fix.
  Entry* existing = find(callsign);
  if (existing != nullptr) {
    if (existing->state == State::Pending) {
      existing->lat = lat;
      existing->lon = lon;
      existing->track_deg = track_deg;
    }
    return;
  }

  Entry& slot = s_routes[s_next_slot];
  s_next_slot = (s_next_slot + 1) % kMaxRoutes;

  strncpy(slot.callsign, callsign, sizeof(slot.callsign) - 1);
  slot.callsign[sizeof(slot.callsign) - 1] = '\0';
  slot.origin[0] = '\0';
  slot.dest[0] = '\0';
  slot.lat = lat;
  slot.lon = lon;
  slot.track_deg = track_deg;
  slot.state = State::Pending;
}

void pump() {
  Entry* entry = firstPending();
  if (entry == nullptr) {
    return;
  }

  char origin[kCodeLen] = "";
  char dest[kCodeLen] = "";
  if (!fetchRoute(*entry, origin, dest)) {
    return;  // transient failure — stay Pending, retry next cycle.
  }

  if (origin[0] != '\0' && dest[0] != '\0') {
    strcpy(entry->origin, origin);
    strcpy(entry->dest, dest);
    entry->state = State::Resolved;
    Serial.printf("route: %s %s->%s\n", entry->callsign, origin, dest);
  } else {
    entry->state = State::None;
  }
}

bool lookup(const char* callsign, char* origin, char* dest) {
  const Entry* e = find(callsign);
  if (e == nullptr || e->state != State::Resolved) {
    return false;
  }
  strcpy(origin, e->origin);
  strcpy(dest, e->dest);
  return true;
}

void clear() {
  for (Entry& e : s_routes) {
    e.state = State::Empty;
  }
  s_next_slot = 0;
}

}  // namespace services::route
