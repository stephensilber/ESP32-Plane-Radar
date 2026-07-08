#include "services/adsb_client.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <cctype>
#include <cstring>

#include "config.h"

namespace services::adsb {

namespace {

constexpr char kApiBase[] = "https://opendata.adsb.fi/api/v3/lat/";
constexpr float kKmPerNm = 1.852f;
// 200ms was too tight for a remote TLS connect from the C3; give it room but
// still bound it so a failure can't freeze the render for long.
constexpr int kConnectAttemptMs = 2000;
constexpr unsigned long kRequestTimeoutMs = 4000;
// One attempt per cycle: rapid retries only pile onto adsb.fi's 1 req/s limit
// (and a DNS failure won't recover within a retry burst anyway). The next 3s
// cycle is the retry.
constexpr int kMaxConnectRetries = 1;

// Double-buffered: the fetch (a background task in performance mode) fills the
// inactive buffer, then publishes it by flipping s_active in a single write, so
// the renderer always reads a complete, consistent snapshot.
Aircraft s_aircraft[2][kMaxAircraft];
size_t s_count[2] = {0, 0};
unsigned long s_update_ms[2] = {0, 0};
volatile uint8_t s_active = 0;
PollFn s_poll_fn = nullptr;

void pollNetwork() {
  if (s_poll_fn != nullptr) {
    s_poll_fn();
  }
}

int performGetWithPoll(HTTPClient& http) {
  http.setConnectTimeout(kConnectAttemptMs);
  const unsigned long deadline = millis() + kRequestTimeoutMs;
  int attempts = 0;
  while (millis() < deadline) {
    pollNetwork();
    const int code = http.GET();
    if (code > 0) {
      return code;
    }
    if (code != HTTPC_ERROR_CONNECTION_REFUSED &&
        code != HTTPC_ERROR_NOT_CONNECTED) {
      return code;
    }
    // Cap retries so a persistent failure (e.g. heap too low for TLS) doesn't
    // spin hundreds of times spamming the log instead of failing this cycle.
    if (++attempts >= kMaxConnectRetries) {
      return code;
    }
    delay(150);
  }
  return HTTPC_ERROR_READ_TIMEOUT;
}

float kmToNauticalMiles(float km) { return km / kKmPerNm; }

bool readJsonFloat(const JsonObject& obj, const char* key, float* out) {
  if (obj[key].is<float>() || obj[key].is<double>() || obj[key].is<int>()) {
    *out = obj[key].as<float>();
    return true;
  }
  return false;
}

float pickNoseHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickTrackHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickGroundSpeed(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "gs", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "tas", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "ias", &v)) {
    return v;
  }
  return 0.0f;
}

bool isOnGround(const JsonObject& plane) {
  if (!plane["alt_baro"].is<const char*>()) {
    return false;
  }
  return strcmp(plane["alt_baro"].as<const char*>(), "ground") == 0;
}

void copyJsonStringTrimmed(const JsonObject& obj, const char* key, char* out,
                           size_t out_len) {
  out[0] = '\0';
  if (out_len == 0 || !obj[key].is<const char*>()) {
    return;
  }
  const char* s = obj[key].as<const char*>();
  size_t n = strnlen(s, out_len - 1);
  while (n > 0 && s[n - 1] == ' ') {
    --n;
  }
  memcpy(out, s, n);
  out[n] = '\0';
}

void formatAltitudeTag(const JsonObject& plane, char* out, size_t out_len) {
  out[0] = '\0';
  if (out_len == 0) {
    return;
  }

  if (plane["alt_baro"].is<const char*>()) {
    const char* s = plane["alt_baro"].as<const char*>();
    if (strcmp(s, "ground") == 0) {
      strncpy(out, "GND", out_len - 1);
      out[out_len - 1] = '\0';
      return;
    }
  }

  float alt = 0.0f;
  if (readJsonFloat(plane, "alt_baro", &alt) ||
      readJsonFloat(plane, "alt_geom", &alt)) {
    snprintf(out, out_len, "%d ft", static_cast<int>(lroundf(alt)));
  }
}

/** ICAO airline callsign: 3 letters + at least one digit (e.g. AAL3114). */
bool isAirlineCallsign(const char* callsign) {
  if (callsign[0] == '\0') {
    return false;
  }
  for (int i = 0; i < 3; ++i) {
    if (!isalpha(static_cast<unsigned char>(callsign[i]))) {
      return false;
    }
  }
  bool has_digit = false;
  for (const char* p = callsign + 3; *p != '\0'; ++p) {
    if (isdigit(static_cast<unsigned char>(*p))) {
      has_digit = true;
    }
  }
  return has_digit;
}

/**
 * Best-effort class from what ADS-B exposes:
 *   military — dbFlags bit 0 (authoritative);
 *   commercial — airline-style callsign distinct from the tail number;
 *   private — everything else (GA, tail-number callsigns, blanks).
 */
Class classifyAircraft(const JsonObject& plane, const char* callsign) {
  float flags = 0.0f;
  if (readJsonFloat(plane, "dbFlags", &flags) &&
      (static_cast<int>(flags) & 1) != 0) {
    return Class::Military;
  }

  char reg[9];
  copyJsonStringTrimmed(plane, "r", reg, sizeof(reg));
  if (isAirlineCallsign(callsign) &&
      (reg[0] == '\0' || strcmp(callsign, reg) != 0)) {
    return Class::Commercial;
  }
  return Class::Private;
}

bool isRotorcraft(const JsonObject& plane) {
  return plane["category"].is<const char*>() &&
         strcmp(plane["category"].as<const char*>(), "A7") == 0;
}

void fillTagFields(Aircraft* ac, const JsonObject& plane) {
  copyJsonStringTrimmed(plane, "hex", ac->hex, sizeof(ac->hex));

  copyJsonStringTrimmed(plane, "flight", ac->callsign, sizeof(ac->callsign));
  if (ac->callsign[0] == '\0') {
    copyJsonStringTrimmed(plane, "hex", ac->callsign, sizeof(ac->callsign));
  }

  copyJsonStringTrimmed(plane, "t", ac->type, sizeof(ac->type));
  formatAltitudeTag(plane, ac->alt, sizeof(ac->alt));

  ac->klass = classifyAircraft(plane, ac->callsign);
  ac->is_rotor = isRotorcraft(plane);
}

}  // namespace

void setPollFn(PollFn fn) { s_poll_fn = fn; }

size_t aircraftCount() { return s_count[s_active]; }

const Aircraft* aircraftList() { return s_aircraft[s_active]; }

bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km) {
  const float dist_nm = kmToNauticalMiles(fetch_radius_km);

  String url = kApiBase;
  url += String(center_lat, 6);
  url += "/lon/";
  url += String(center_lon, 6);
  url += "/dist/";
  url += String(dist_nm, 1);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("adsb: http.begin failed");
    return false;
  }

  http.setTimeout(kRequestTimeoutMs);
  const int code = performGetWithPoll(http);
  if (code != HTTP_CODE_OK) {
    Serial.printf("adsb: HTTP %d\n", code);
    http.end();
    return false;
  }

  // Parse only the fields we use — the feed carries ~50 per aircraft, so the
  // filter keeps the document small. Critically, we parse straight from the
  // network stream rather than buffering the whole response into a String:
  // at the widest zoom a 30-40KB body can't fit a contiguous heap block, which
  // was truncating the read (InvalidInput / IncompleteInput). Streaming reads
  // it incrementally and only retains the filtered fields.
  JsonDocument filter;
  static const char* const kFields[] = {
      "lat",      "lon",      "true_heading", "mag_heading", "track",
      "dir",      "gs",       "tas",          "ias",         "alt_baro",
      "alt_geom", "hex",      "flight",       "t",           "dbFlags",
      "r",        "category"};
  for (const char* f : kFields) {
    filter["ac"][0][f] = true;
  }

  WiFiClient* stream = http.getStreamPtr();
  JsonDocument doc;
  const DeserializationError err =
      stream ? deserializeJson(doc, *stream,
                               DeserializationOption::Filter(filter))
             : DeserializationError(DeserializationError::EmptyInput);
  http.end();
  if (err) {
    Serial.printf("adsb: JSON parse error: %s\n", err.c_str());
    return false;
  }

  const uint8_t w = s_active ^ 1;  // fill the inactive buffer

  JsonArray ac = doc["ac"].as<JsonArray>();
  if (ac.isNull()) {
    s_count[w] = 0;
    s_update_ms[w] = millis();
    s_active = w;  // publish
    return true;
  }

  size_t n = 0;
  for (JsonObject plane : ac) {
    if (n >= kMaxAircraft) {
      break;
    }
    if (!plane["lat"].is<float>() || !plane["lon"].is<float>()) {
      continue;
    }
    if (isOnGround(plane) && !config::kAdsbShowGroundAircraft) {
      continue;
    }

    s_aircraft[w][n].lat = plane["lat"].as<float>();
    s_aircraft[w][n].lon = plane["lon"].as<float>();
    s_aircraft[w][n].nose_deg = pickNoseHeading(plane);
    s_aircraft[w][n].track_deg = pickTrackHeading(plane);
    s_aircraft[w][n].gs_knots = pickGroundSpeed(plane);
    fillTagFields(&s_aircraft[w][n], plane);
    ++n;
  }

  s_count[w] = n;
  s_update_ms[w] = millis();
  s_active = w;  // publish the completed snapshot
  Serial.printf("adsb: %u aircraft (freeHeap %u, maxAlloc %u)\n",
                static_cast<unsigned>(n), ESP.getFreeHeap(),
                ESP.getMaxAllocHeap());
  return true;
}

float secondsSinceUpdate() {
  const unsigned long t = s_update_ms[s_active];
  if (t == 0) {
    return 0.0f;
  }
  return static_cast<float>(millis() - t) / 1000.0f;
}

}  // namespace services::adsb
