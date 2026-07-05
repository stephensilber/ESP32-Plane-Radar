#include "services/route_client.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <cstring>

namespace services::route {

namespace {

constexpr char kApiBase[] = "https://api.adsbdb.com/v0/callsign/";
constexpr size_t kMaxRoutes = 24;
constexpr int kConnectTimeoutMs = 3000;
constexpr unsigned long kRequestTimeoutMs = 6000;

enum class State : uint8_t { Empty, Pending, Resolved, None };

struct Entry {
  char callsign[9];
  char origin[kCodeLen];
  char dest[kCodeLen];
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

bool fetchRoute(const Entry& entry, char* origin, char* dest) {
  String url = kApiBase;
  url += entry.callsign;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(kConnectTimeoutMs);
  http.setTimeout(kRequestTimeoutMs);
  if (!http.begin(client, url)) {
    return false;
  }

  pollNetwork();
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    // 404 = adsbdb has no route for this callsign; treat as a settled "no route".
    http.end();
    return code == HTTP_CODE_NOT_FOUND;
  }

  String payload = http.getString();
  http.end();
  pollNetwork();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    return false;
  }

  JsonObject fr = doc["response"]["flightroute"].as<JsonObject>();
  if (fr.isNull()) {
    return true;  // "unknown callsign" — settled, no route.
  }

  const char* o = fr["origin"]["iata_code"].as<const char*>();
  const char* d = fr["destination"]["iata_code"].as<const char*>();
  if (o != nullptr) {
    strncpy(origin, o, kCodeLen - 1);
    origin[kCodeLen - 1] = '\0';
  }
  if (d != nullptr) {
    strncpy(dest, d, kCodeLen - 1);
    dest[kCodeLen - 1] = '\0';
  }
  return true;
}

}  // namespace

void setPollFn(PollFn fn) { s_poll_fn = fn; }

void note(const char* callsign) {
  if (callsign == nullptr || callsign[0] == '\0' || find(callsign) != nullptr) {
    return;
  }

  Entry& slot = s_routes[s_next_slot];
  s_next_slot = (s_next_slot + 1) % kMaxRoutes;

  strncpy(slot.callsign, callsign, sizeof(slot.callsign) - 1);
  slot.callsign[sizeof(slot.callsign) - 1] = '\0';
  slot.origin[0] = '\0';
  slot.dest[0] = '\0';
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
