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
constexpr int kConnectAttemptMs = 200;
constexpr unsigned long kRequestTimeoutMs = 10000;

Aircraft s_aircraft[kMaxAircraft];
size_t s_aircraft_count = 0;
unsigned long s_last_update_ms = 0;
PollFn s_poll_fn = nullptr;

// Persistent so the TLS connection is kept alive between polls (the adsb feed
// sends Content-Length + Connection: keep-alive), skipping the ~hundreds-of-ms
// handshake on most fetches.
WiFiClientSecure s_client;
HTTPClient s_http;
bool s_http_inited = false;

void pollNetwork() {
  if (s_poll_fn != nullptr) {
    s_poll_fn();
  }
}

void ensureHttpInit() {
  if (s_http_inited) {
    return;
  }
  s_client.setInsecure();
  s_http.setReuse(true);
  s_http_inited = true;
}

void closeConnection() {
  s_http.end();
  s_client.stop();
}

int performGetWithPoll(HTTPClient& http) {
  http.setConnectTimeout(kConnectAttemptMs);
  const unsigned long deadline = millis() + kRequestTimeoutMs;
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
    delay(5);
  }
  return HTTPC_ERROR_READ_TIMEOUT;
}

bool readResponseBodyWithPoll(HTTPClient& http, String& payload) {
  WiFiClient* stream = http.getStreamPtr();
  if (stream == nullptr) {
    return false;
  }

  const int content_length = http.getSize();
  if (content_length > 0) {
    payload.reserve(static_cast<unsigned>(content_length + 1));
  }

  uint8_t buffer[512];
  const unsigned long deadline = millis() + kRequestTimeoutMs;
  while (millis() < deadline) {
    pollNetwork();
    const int available = stream->available();
    if (available > 0) {
      const int to_read =
          available > static_cast<int>(sizeof(buffer)) ? static_cast<int>(sizeof(buffer))
                                                       : available;
      const int read_bytes = stream->readBytes(buffer, to_read);
      if (read_bytes > 0) {
        payload.concat(reinterpret_cast<const char*>(buffer),
                       static_cast<unsigned>(read_bytes));
      }
    }
    if (content_length > 0 &&
        static_cast<int>(payload.length()) >= content_length) {
      break;
    }
    if (!http.connected() && stream->available() <= 0) {
      break;
    }
    delay(1);
  }

  return payload.length() > 0;
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

size_t aircraftCount() { return s_aircraft_count; }

const Aircraft* aircraftList() { return s_aircraft; }

bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km) {
  const float dist_nm = kmToNauticalMiles(fetch_radius_km);

  String url = kApiBase;
  url += String(center_lat, 6);
  url += "/lon/";
  url += String(center_lon, 6);
  url += "/dist/";
  url += String(dist_nm, 1);

  ensureHttpInit();

  if (!s_http.begin(s_client, url)) {
    Serial.println("adsb: http.begin failed");
    closeConnection();
    return false;
  }
  s_http.setTimeout(kRequestTimeoutMs);

  int code = performGetWithPoll(s_http);
  if (code != HTTP_CODE_OK) {
    // A kept-alive socket may have gone stale; drop it and try once more fresh
    // so a reuse failure never costs a whole update cycle.
    closeConnection();
    if (s_http.begin(s_client, url)) {
      s_http.setTimeout(kRequestTimeoutMs);
      code = performGetWithPoll(s_http);
    }
  }
  if (code != HTTP_CODE_OK) {
    Serial.printf("adsb: HTTP %d\n", code);
    closeConnection();
    return false;
  }

  const int content_len = s_http.getSize();
  String payload;
  if (!readResponseBodyWithPoll(s_http, payload)) {
    Serial.println("adsb: empty response");
    closeConnection();
    return false;
  }
  if (content_len > 0) {
    s_http.end();  // Content-Length response: keep the TLS connection open.
  } else {
    closeConnection();  // Chunked/unknown: our poll-read relies on close.
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("adsb: JSON parse error: %s\n", err.c_str());
    return false;
  }

  JsonArray ac = doc["ac"].as<JsonArray>();
  if (ac.isNull()) {
    s_aircraft_count = 0;
    s_last_update_ms = millis();
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

    s_aircraft[n].lat = plane["lat"].as<float>();
    s_aircraft[n].lon = plane["lon"].as<float>();
    s_aircraft[n].nose_deg = pickNoseHeading(plane);
    s_aircraft[n].track_deg = pickTrackHeading(plane);
    s_aircraft[n].gs_knots = pickGroundSpeed(plane);
    fillTagFields(&s_aircraft[n], plane);
    ++n;
  }

  s_aircraft_count = n;
  s_last_update_ms = millis();
  Serial.printf("adsb: %u aircraft\n", static_cast<unsigned>(n));
  return true;
}

float secondsSinceUpdate() {
  if (s_last_update_ms == 0) {
    return 0.0f;
  }
  return static_cast<float>(millis() - s_last_update_ms) / 1000.0f;
}

}  // namespace services::adsb
