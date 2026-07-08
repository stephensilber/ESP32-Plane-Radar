/**
 * Plane Radar — WiFi setup, then radar UI on the round GC9A01 display.
 */

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "hardware/display.h"
#include "services/adsb_client.h"
#include "services/radar_location.h"
#include "services/route_client.h"
#include "services/trail.h"
#include "services/wifi_setup.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

namespace {

bool g_radar_visible = false;
unsigned long g_wifi_down_since = 0;
unsigned long g_last_reconnect_ms = 0;
unsigned long g_last_adsb_fetch_ms = 0;
unsigned long g_last_render_ms = 0;
unsigned long g_last_route_ms = 0;

void showRadarIfConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    g_radar_visible = false;
    return;
  }
  ui::radarDisplayDraw();
  g_radar_visible = true;
}

void onRangeTap() {
  ui::radar::rangeNext();
  char range_label[12];
  ui::radar::formatCurrentRing3Label(range_label, sizeof(range_label));
  Serial.printf("Range: %s (outer ~%.0f km)\n", range_label,
                ui::radar::rangeCurrent().outer_km);

  if (g_radar_visible && WiFi.status() == WL_CONNECTED) {
    ui::radarDisplayDraw();
  }
}

void handleBootButton() {
  bootButtonPollLongPress();
  if (bootButtonConsumeTap()) {
    onRangeTap();
  }
}

/** Queue routes only for commercial aircraft inside the ring — beyond-ring
 *  targets are just dots with no tag, so their route is never shown. */
void noteVisibleRoutes() {
  const size_t n = services::adsb::aircraftCount();
  const services::adsb::Aircraft* planes = services::adsb::aircraftList();
  const double clat = services::location::lat();
  const double clon = services::location::lon();
  const float outer_km = ui::radar::rangeCurrent().outer_km;
  for (size_t i = 0; i < n; ++i) {
    if (planes[i].klass != services::adsb::Class::Commercial) {
      continue;
    }
    const float dx = static_cast<float>(planes[i].lon - clon) * 111.0f;
    const float dy = static_cast<float>(planes[i].lat - clat) * 111.0f;
    if (dx * dx + dy * dy > outer_km * outer_km) {
      continue;  // beyond the ring — no tag, so no route needed.
    }
    services::route::note(planes[i].callsign, planes[i].lat, planes[i].lon,
                          planes[i].track_deg);
  }
}

void fetchAndDrawAircraft() {
  const float fetch_km = ui::radar::fetchRadiusKm();
  if (!services::adsb::fetchUpdate(services::location::lat(),
                                   services::location::lon(), fetch_km)) {
    handleBootButton();
    return;
  }
  ui::radarDisplayRefreshAircraft();
  handleBootButton();

  if (ui::radar::showTrails()) {
    const size_t n = services::adsb::aircraftCount();
    const services::adsb::Aircraft* planes = services::adsb::aircraftList();
    for (size_t i = 0; i < n; ++i) {
      services::trail::append(planes[i].hex, planes[i].lat, planes[i].lon);
    }
  }

  // Register callsigns (cheap); the blocking lookup runs on its own throttled
  // schedule in loop() so it doesn't stall motion every fetch.
  if (ui::radar::showRoute()) {
    noteVisibleRoutes();
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("Plane Radar");
  // Build stamp so a flash can be confirmed from the serial log.
  Serial.printf("Build: %s %s\n", __DATE__, __TIME__);

  bootButtonInit();
  displayInit();
  if (wifiShowsSetupScreenOnBoot()) {
    statusScreenPortal();
  }
  services::location::init();
  ui::radar::rangeInit();
  services::adsb::setPollFn(wifiLoop);
  services::route::setPollFn(wifiLoop);

  if (wifiSetupConnect()) {
    showRadarIfConnected();
  }
}

void loop() {
  handleBootButton();
  wifiLoop();

  if (WiFi.status() != WL_CONNECTED) {
    if (g_radar_visible) {
      Serial.println("WiFi lost — will reconnect");
      g_radar_visible = false;
    }

    if (g_wifi_down_since == 0) {
      g_wifi_down_since = millis();
    }

    const unsigned long down_ms = millis() - g_wifi_down_since;
    if (down_ms >= config::kWifiDownGraceMs &&
        millis() - g_last_reconnect_ms >= config::kWifiReconnectIntervalMs) {
      g_last_reconnect_ms = millis();
      if (wifiReconnect()) {
        g_wifi_down_since = 0;
        showRadarIfConnected();
      }
    }
  } else {
    g_wifi_down_since = 0;
    if (!g_radar_visible) {
      showRadarIfConnected();
      g_last_render_ms = millis();
    } else if (millis() - g_last_adsb_fetch_ms >= config::kAdsbFetchIntervalMs) {
      g_last_adsb_fetch_ms = millis();
      fetchAndDrawAircraft();
      g_last_render_ms = millis();
    } else if (ui::radar::showRoute() &&
               millis() - g_last_route_ms >= config::kRouteLookupIntervalMs) {
      // One blocking route lookup, spread out from the fetch so its TLS
      // handshake only hitches motion occasionally instead of every cycle.
      g_last_route_ms = millis();
      services::route::pump();
      g_last_render_ms = millis();
    } else if (ui::radar::smoothMotion() &&
               millis() - g_last_render_ms >= ui::radar::renderIntervalMs()) {
      // Re-render between fetches so dead-reckoned motion stays smooth.
      g_last_render_ms = millis();
      ui::radarDisplayRefreshAircraft();
      handleBootButton();
    }
  }

  delay(10);
}
