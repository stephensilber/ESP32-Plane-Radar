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
bool g_perf_mode = false;  // network I/O on a background task (read once at boot)
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

/** Data-only fetch: pull positions, record trails, register routes. No drawing,
 *  so it is safe to run from the background task in performance mode. */
bool runFetchCycle() {
  const float fetch_km = ui::radar::fetchRadiusKm();
  if (!services::adsb::fetchUpdate(services::location::lat(),
                                   services::location::lon(), fetch_km)) {
    return false;
  }
  if (ui::radar::showTrails()) {
    const size_t n = services::adsb::aircraftCount();
    const services::adsb::Aircraft* planes = services::adsb::aircraftList();
    for (size_t i = 0; i < n; ++i) {
      services::trail::append(planes[i].hex, planes[i].lat, planes[i].lon);
    }
  }
  if (ui::radar::showRoute()) {
    noteVisibleRoutes();
  }
  return true;
}

void fetchAndDrawAircraft() {
  // Paint the orange fetch dot before we block, so it's visible for the whole
  // (single-loop) fetch even though nothing redraws during it.
  ui::radarSetFetchActive(true);
  ui::radarDisplayRefreshAircraft();
  const bool ok = runFetchCycle();
  ui::radarSetFetchActive(false);
  if (ok) {
    ui::radarDisplayRefreshAircraft();
  }
  handleBootButton();
}

/** Performance mode: all blocking network I/O on its own FreeRTOS task, so the
 *  main loop (render + buttons + portal) never stalls on a fetch or lookup. */
void perfNetTask(void*) {
  unsigned long last_fetch = 0;
  unsigned long last_route = 0;
  for (;;) {
    // Defer network work (its ~32KB TLS alloc) while the heap is tight — e.g.
    // the portal is mid-page-build — so the two don't collide and blank it.
    if (WiFi.status() == WL_CONNECTED &&
        ESP.getMaxAllocHeap() > config::kNetHeapFloorBytes) {
      const unsigned long now = millis();
      if (now - last_fetch >= config::kAdsbFetchIntervalMs) {
        last_fetch = now;
        ui::radarSetFetchActive(true);
        runFetchCycle();
        ui::radarSetFetchActive(false);
      } else if (ui::radar::showRoute() &&
                 now - last_route >= config::kRouteLookupIntervalMs) {
        last_route = now;
        ui::radarSetFetchActive(true);
        services::route::pump();
        ui::radarSetFetchActive(false);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(25));
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
  g_perf_mode = ui::radar::perfMode();
  // The poll hook pumps the portal during a blocking fetch; only needed in the
  // single-loop mode. In performance mode the fetch runs on its own task and
  // the main loop services the portal on its own.
  services::adsb::setPollFn(g_perf_mode ? nullptr : wifiLoop);
  services::route::setPollFn(g_perf_mode ? nullptr : wifiLoop);

  if (wifiSetupConnect()) {
    showRadarIfConnected();
  }

  if (g_perf_mode) {
    if (xTaskCreate(perfNetTask, "net", 20480, nullptr, 1, nullptr) == pdPASS) {
      Serial.println("Performance mode: network on background task");
    } else {
      // Couldn't spawn the task — fall back to the single-loop path.
      g_perf_mode = false;
      services::adsb::setPollFn(wifiLoop);
      services::route::setPollFn(wifiLoop);
      Serial.println("Performance mode: task create failed, using single loop");
    }
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
    } else if (!g_perf_mode &&
               millis() - g_last_adsb_fetch_ms >= config::kAdsbFetchIntervalMs) {
      g_last_adsb_fetch_ms = millis();
      fetchAndDrawAircraft();
      g_last_render_ms = millis();
    } else if (!g_perf_mode && ui::radar::showRoute() &&
               millis() - g_last_route_ms >= config::kRouteLookupIntervalMs) {
      // One blocking route lookup, spread out from the fetch so its TLS
      // handshake only hitches motion occasionally instead of every cycle.
      g_last_route_ms = millis();
      ui::radarSetFetchActive(true);
      ui::radarDisplayRefreshAircraft();
      services::route::pump();
      ui::radarSetFetchActive(false);
      ui::radarDisplayRefreshAircraft();
      g_last_render_ms = millis();
    } else if ((g_perf_mode || ui::radar::smoothMotion()) &&
               millis() - g_last_render_ms >= ui::radar::renderIntervalMs()) {
      // Re-render between fetches for smooth motion. In performance mode the
      // background task supplies fresh data; here we just draw it.
      g_last_render_ms = millis();
      ui::radarDisplayRefreshAircraft();
      handleBootButton();
    }
  }

  delay(10);
}
