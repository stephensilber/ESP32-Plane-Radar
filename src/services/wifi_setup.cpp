#include "services/wifi_setup.h"

#include <WiFi.h>
#include <WiFiManager.h>

#include <cstdio>

#include <Preferences.h>
#include <esp_system.h>
#include <esp_wifi.h>

#ifdef WM_MDNS
#include <ESPmDNS.h>
#endif

#include "config.h"
#include "services/radar_location.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

portMUX_TYPE s_boot_mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool s_boot_tap_pending = false;
volatile bool s_boot_is_down = false;
volatile unsigned long s_boot_down_ms = 0;
bool s_long_press_handled = false;
bool s_boot_interrupt_attached = false;

void IRAM_ATTR onBootButtonIsr() {
  const bool down = digitalRead(config::kBootPin) == LOW;
  const unsigned long now = millis();
  portENTER_CRITICAL_ISR(&s_boot_mux);
  if (down) {
    s_boot_is_down = true;
    s_boot_down_ms = now;
  } else if (s_boot_is_down) {
    const unsigned long held = now - s_boot_down_ms;
    if (held >= config::kBootTapMinMs && held < config::kBootResetHoldMs) {
      s_boot_tap_pending = true;
    }
    s_boot_is_down = false;
  }
  portEXIT_CRITICAL_ISR(&s_boot_mux);
}

void initBootButton() {
  pinMode(config::kBootPin, INPUT_PULLUP);
  if (s_boot_interrupt_attached) {
    return;
  }
  attachInterrupt(digitalPinToInterrupt(static_cast<uint8_t>(config::kBootPin)),
                  onBootButtonIsr, CHANGE);
  s_boot_interrupt_attached = true;
}

namespace {

/** Separate from planeradar prefs (rangeInit) to avoid NVS handle conflicts. */
constexpr char kWifiPrefsNamespace[] = "wifi";
constexpr char kPrefsForcePortalKey[] = "portal";

bool s_force_config_portal = false;
WiFiManager s_wm;
bool s_wm_configured = false;

void ensureWifiManager();
void startLanWebPortal();
void stopLanWebPortal();
bool wifiLinkUp();

constexpr int kCoordParamLen = 20;
constexpr char kCoordInputAttrs[] =
    " type=\"number\" step=\"0.000001\"";

// ATC / radar-scope theme injected into every portal page's <head>.
// Self-contained (no external fonts/assets) so it works on the captive AP.
constexpr char kPortalCss[] = R"CSS(<style>
:root{--fg:#4dffa6;--dim:#1f8f5c;--acc:#00e676;--amber:#ffcf40}
body{margin:0 auto;max-width:520px;padding:16px;color:var(--fg);background:#02100a;font-family:ui-monospace,monospace}
h1,h2,h3{color:var(--acc);text-transform:uppercase;letter-spacing:2px}
button,input[type=submit]{width:100%;color:var(--fg);background:#064a2a;border:1px solid var(--acc);border-radius:6px;padding:11px;margin-top:6px;font-weight:700;text-transform:uppercase;letter-spacing:1px}
input[type=text],input[type=password],input[type=number],select{width:100%;color:var(--fg);background:#02160d;border:1px solid var(--dim);border-radius:4px;padding:9px}
input[type=checkbox]{accent-color:var(--acc);transform:scale(1.25);margin:8px 8px 8px 0}
a{color:var(--acc)}
.bk{display:inline-block;margin:0 0 14px;padding:8px 14px;border:1px solid var(--dim);border-radius:6px;text-transform:uppercase;letter-spacing:1px;font-weight:700}
.cmp{margin:16px auto;text-align:center}
.cmp-up{color:var(--amber);font-weight:700;font-size:12px;margin-bottom:6px}
.cmp-dial{position:relative;width:170px;height:170px;margin:0 auto;border-radius:50%;border:2px solid var(--acc);background:#031f12;touch-action:none;cursor:grab;user-select:none}
.cmp-tick{position:absolute;top:-9px;left:50%;margin-left:-6px;border-left:6px solid transparent;border-right:6px solid transparent;border-bottom:10px solid var(--amber)}
.cmp-rose{position:absolute;inset:0}
.cmp-rose b{position:absolute;font-weight:700;font-size:15px}
.cmp-rose .n{top:16px;left:50%;margin-left:-5px;color:var(--amber)}
.cmp-rose .s{bottom:16px;left:50%;margin-left:-5px}
.cmp-rose .e{right:10px;top:50%;margin-top:-8px}
.cmp-rose .w{left:10px;top:50%;margin-top:-8px}
.cmp-val{margin-top:8px;font-size:13px}
</style>)CSS";

WiFiManagerParameter s_param_back("<a href=\"/\" class=\"bk\">&#8592; Back</a>");

WiFiManagerParameter s_param_lat("radar_lat", "Latitude (deg)", "0",
                                kCoordParamLen, kCoordInputAttrs);
WiFiManagerParameter s_param_lon("radar_lon", "Longitude (deg)", "0",
                                kCoordParamLen, kCoordInputAttrs);

char s_miles_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_miles("use_miles", "Display distances in miles", "T", 2,
                                   s_miles_checkbox_attrs, WFM_LABEL_AFTER);

char s_runways_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_runways("show_runways", "Show airport runways", "T", 2,
                                     s_runways_checkbox_attrs, WFM_LABEL_AFTER);

char s_color_class_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_color_class("color_class",
                                         "Color aircraft by type", "T", 2,
                                         s_color_class_checkbox_attrs,
                                         WFM_LABEL_AFTER);

char s_heli_icon_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_heli_icon("heli_icon", "Distinct helicopter icon",
                                       "T", 2, s_heli_icon_checkbox_attrs,
                                       WFM_LABEL_AFTER);

char s_show_route_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_show_route("show_route",
                                        "Show flight route (origin/destination)",
                                        "T", 2, s_show_route_checkbox_attrs,
                                        WFM_LABEL_AFTER);

char s_smooth_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_smooth("smooth_motion",
                                    "Smooth motion (dead-reckoning)", "T", 2,
                                    s_smooth_checkbox_attrs, WFM_LABEL_AFTER);

WiFiManagerParameter s_param_fps("radar_fps", "Frame rate (FPS, 1-30)", "10", 4,
                                 " type=\"number\" min=\"1\" max=\"30\" step=\"1\"");

// Interactive radar-rotation compass. Drag the rose so N points to real north
// relative to the top of the screen; the chosen offset is written into the
// hidden radar_heading field that WiFiManager reads on save.
constexpr char kCompassWidget[] = R"HTML(<div class="cmp">
<div class="cmp-up">&#9650; TOP OF SCREEN</div>
<div class="cmp-dial" id="cmpDial"><div class="cmp-tick"></div>
<div class="cmp-rose" id="cmpRose"><b class="n">N</b><b class="e">E</b><b class="s">S</b><b class="w">W</b></div></div>
<div class="cmp-val">Rotation <b id="cmpVal">0</b>&deg;</div></div>
<script>(function(){function I(){var d=document.getElementById('cmpDial'),
r=document.getElementById('cmpRose'),
h=document.getElementById('radar_heading')||document.getElementsByName('radar_heading')[0],
v=document.getElementById('cmpVal');if(!d||!h||d.dataset.i){return;}d.dataset.i=1;
var g=parseInt(h.value||'0',10)||0;function A(){g=((g%360)+360)%360;
r.style.transform='rotate('+g+'deg)';h.value=g;if(v){v.textContent=g;}}A();
var dn=false,sa=0,sg=0;function an(e){var b=d.getBoundingClientRect(),
cx=b.left+b.width/2,cy=b.top+b.height/2,
x=(e.touches?e.touches[0].clientX:e.clientX),
y=(e.touches?e.touches[0].clientY:e.clientY);
return Math.atan2(x-cx,-(y-cy))*180/Math.PI;}
function D(e){dn=true;sa=an(e);sg=g;e.preventDefault();}
function M(e){if(!dn){return;}g=Math.round(sg+(an(e)-sa));A();e.preventDefault();}
function U(){dn=false;}
d.addEventListener('pointerdown',D);window.addEventListener('pointermove',M);
window.addEventListener('pointerup',U);
d.addEventListener('touchstart',D,{passive:false});
window.addEventListener('touchmove',M,{passive:false});
window.addEventListener('touchend',U);}
document.addEventListener('DOMContentLoaded',I);
window.addEventListener('load',I);setTimeout(I,300);})();</script>)HTML";

WiFiManagerParameter s_param_compass(kCompassWidget);
WiFiManagerParameter s_param_heading("radar_heading", "", "0", 6,
                                     "type=\"hidden\"");

void refreshPortalParamDefaults() {
  char lat_buf[kCoordParamLen + 1];
  char lon_buf[kCoordParamLen + 1];
  snprintf(lat_buf, sizeof(lat_buf), "%.6f", services::location::lat());
  snprintf(lon_buf, sizeof(lon_buf), "%.6f", services::location::lon());
  s_param_lat.setValue(lat_buf, kCoordParamLen);
  s_param_lon.setValue(lon_buf, kCoordParamLen);
  snprintf(s_miles_checkbox_attrs, sizeof(s_miles_checkbox_attrs), "type=\"checkbox\"%s",
           ui::radar::useMiles() ? " checked" : "");
  s_param_miles.setValue("T", 2);
  snprintf(s_runways_checkbox_attrs, sizeof(s_runways_checkbox_attrs),
           "type=\"checkbox\"%s", ui::radar::showRunways() ? " checked" : "");
  s_param_runways.setValue("T", 2);
  snprintf(s_color_class_checkbox_attrs, sizeof(s_color_class_checkbox_attrs),
           "type=\"checkbox\"%s", ui::radar::colorByClass() ? " checked" : "");
  s_param_color_class.setValue("T", 2);
  snprintf(s_heli_icon_checkbox_attrs, sizeof(s_heli_icon_checkbox_attrs),
           "type=\"checkbox\"%s", ui::radar::heliIcon() ? " checked" : "");
  s_param_heli_icon.setValue("T", 2);
  snprintf(s_show_route_checkbox_attrs, sizeof(s_show_route_checkbox_attrs),
           "type=\"checkbox\"%s", ui::radar::showRoute() ? " checked" : "");
  s_param_show_route.setValue("T", 2);
  snprintf(s_smooth_checkbox_attrs, sizeof(s_smooth_checkbox_attrs),
           "type=\"checkbox\"%s", ui::radar::smoothMotion() ? " checked" : "");
  s_param_smooth.setValue("T", 2);
  char fps_buf[5];
  snprintf(fps_buf, sizeof(fps_buf), "%d", ui::radar::frameRateFps());
  s_param_fps.setValue(fps_buf, 4);
  char heading_buf[6];
  snprintf(heading_buf, sizeof(heading_buf), "%d",
           static_cast<int>(ui::radar::headingOffsetDeg()));
  s_param_heading.setValue(heading_buf, 6);
}

void onPortalParamsSaved() {
  if (!services::location::saveFromStrings(s_param_lat.getValue(),
                                           s_param_lon.getValue())) {
    Serial.println("Invalid lat/lon in portal — keeping previous location");
  }
  ui::radar::saveMilesFromPortal(s_param_miles.getValue());
  ui::radar::saveRunwaysFromPortal(s_param_runways.getValue());
  ui::radar::saveColorByClassFromPortal(s_param_color_class.getValue());
  ui::radar::saveHeliIconFromPortal(s_param_heli_icon.getValue());
  ui::radar::saveShowRouteFromPortal(s_param_show_route.getValue());
  ui::radar::saveSmoothMotionFromPortal(s_param_smooth.getValue());
  ui::radar::saveFpsFromPortal(s_param_fps.getValue());
  ui::radar::saveHeadingFromPortal(s_param_heading.getValue());
}

void attachPortalParams(WiFiManager& wm) {
  refreshPortalParamDefaults();
  wm.addParameter(&s_param_back);
  wm.addParameter(&s_param_lat);
  wm.addParameter(&s_param_lon);
  wm.addParameter(&s_param_miles);
  wm.addParameter(&s_param_runways);
  wm.addParameter(&s_param_color_class);
  wm.addParameter(&s_param_heli_icon);
  wm.addParameter(&s_param_show_route);
  wm.addParameter(&s_param_smooth);
  wm.addParameter(&s_param_fps);
  wm.addParameter(&s_param_compass);
  wm.addParameter(&s_param_heading);
  wm.setSaveParamsCallback(onPortalParamsSaved);
}

void markForceConfigPortal() {
  s_force_config_portal = true;
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, false)) {
    return;
  }
  prefs.putBool(kPrefsForcePortalKey, true);
  prefs.end();
}

bool consumeForceConfigPortal() {
  if (s_force_config_portal) {
    s_force_config_portal = false;
    Preferences prefs;
    if (prefs.begin(kWifiPrefsNamespace, false)) {
      prefs.remove(kPrefsForcePortalKey);
      prefs.end();
    }
    return true;
  }

  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  if (!pending) {
    return false;
  }

  if (prefs.begin(kWifiPrefsNamespace, false)) {
    prefs.remove(kPrefsForcePortalKey);
    prefs.end();
  }
  return true;
}

bool storedWifiCredentials() {
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
    WiFi.mode(WIFI_STA);
    delay(50);
  }

  wifi_config_t conf = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) {
    return false;
  }
  return conf.sta.ssid[0] != '\0';
}

void eraseWifiCredentials() {
  stopLanWebPortal();
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_OFF);
  delay(100);

  ensureWifiManager();
  WiFi.persistent(true);
  s_wm.resetSettings();
  s_wm.erase();
  WiFi.disconnect(true, true);
  WiFi.persistent(false);

  WiFi.mode(WIFI_OFF);
  delay(100);
}

void resetWifiCredentials() {
  markForceConfigPortal();
  eraseWifiCredentials();
  services::location::clear();
  ui::radar::unitsReset();
  Serial.println("WiFi credentials, location, and units cleared");
}

void onConfigPortalApStarted(WiFiManager*) {
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  statusScreenPortal();
#ifdef WM_MDNS
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("Setup portal: http://%s.local (or http://%s)\n",
                  config::kPortalHostname, config::kPortalIp);
  } else {
    Serial.printf("Setup portal: http://%s (mDNS unavailable)\n", config::kPortalIp);
  }
#else
  Serial.printf("Setup portal: http://%s\n", config::kPortalIp);
#endif
}

bool wifiLinkUp() {
  return WiFi.status() == WL_CONNECTED &&
         WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

void ensureWifiManager() {
  if (s_wm_configured) {
    return;
  }
  s_wm.setConfigPortalTimeout(config::kWifiPortalTimeoutSec);
  s_wm.setAPStaticIPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                           IPAddress(255, 255, 255, 0));
  s_wm.setHostname(config::kPortalHostname);
  s_wm.setAPCallback(onConfigPortalApStarted);
  s_wm.setCustomHeadElement(kPortalCss);
  // Settings on their own "Setup" page (/param), not crammed under the Wi-Fi
  // scan list — keeps each page small enough to build on the constrained heap.
  static const char* kPortalMenu[] = {"wifi", "param", "info", "exit", "sep",
                                      "update"};
  s_wm.setMenu(kPortalMenu, 6);
  attachPortalParams(s_wm);
  s_wm_configured = true;
}

void startLanWebPortal() {
  if (!wifiLinkUp() || s_wm.getWebPortalActive() ||
      s_wm.getConfigPortalActive()) {
    return;
  }
  refreshPortalParamDefaults();
  WiFi.mode(WIFI_STA);
  s_wm.setConfigPortalBlocking(false);
#ifdef WM_MDNS
  MDNS.end();
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.addService("http", "tcp", 80);
  }
#endif
  s_wm.startWebPortal();
  Serial.printf("LAN config: http://%s.local or http://%s\n",
                config::kPortalHostname, WiFi.localIP().toString().c_str());
}

void stopLanWebPortal() {
  if (!s_wm.getWebPortalActive()) {
    return;
  }
  s_wm.stopWebPortal();
#ifdef WM_MDNS
  MDNS.end();
#endif
}

void prepareSta() {
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(WIFI_PS_NONE);
  WiFi.setAutoReconnect(true);
}

void startStaConnect(const String& ssid, const String& pass) {
  prepareSta();
  if (ssid.length() > 0) {
    WiFi.begin(ssid.c_str(), pass.c_str());
  } else {
    WiFi.begin();
  }
}

bool waitForLinkWithUi(const char* ssid_for_ui, unsigned long attempt_ms) {
  const unsigned long deadline = millis() + attempt_ms;
  while (millis() < deadline) {
    if (wifiLinkUp()) {
      return true;
    }
    bootButtonPollLongPress();
    statusScreenConnectingTick();
    delay(config::kWifiConnectingFrameMs);
  }
  return wifiLinkUp();
}

bool tryConnectWithUi(const String& ssid, const String& pass, bool show_ui) {
  if (wifiLinkUp()) {
    return true;
  }

  const char* ui_ssid = ssid.length() > 0 ? ssid.c_str() : "network";
  if (show_ui) {
    statusScreenConnectingBegin(ui_ssid);
  }

  for (uint8_t attempt = 1; attempt <= config::kWifiConnectAttempts; ++attempt) {
    if (attempt > 1) {
      Serial.printf("WiFi connect retry %u/%u\n", attempt,
                    config::kWifiConnectAttempts);
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      delay(400);
    }

    startStaConnect(ssid, pass);

    if (waitForLinkWithUi(ui_ssid, config::kWifiConnectAttemptMs)) {
      return true;
    }
  }

  return false;
}

bool connectSavedNetwork(bool show_ui) {
  if (!storedWifiCredentials()) {
    return false;
  }

  ensureWifiManager();
  const String ssid = s_wm.getWiFiSSID();
  if (ssid.length() == 0) {
    return false;
  }
  const String pass = s_wm.getWiFiPass();
  return tryConnectWithUi(ssid, pass, show_ui);
}

bool openConfigPortal() {
  stopLanWebPortal();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(50);
  statusScreenPortal();
  s_wm.setConfigPortalBlocking(false);
  s_wm.startConfigPortal(config::kPortalApName);
  while (s_wm.getConfigPortalActive()) {
    bootButtonPollLongPress();
    if (s_wm.process()) {
      return true;
    }
    delay(10);
  }
  return wifiLinkUp();
}

}  // namespace

bool wifiShowsSetupScreenOnBoot() {
  if (s_force_config_portal) {
    return true;
  }
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  return pending;
}

bool wifiBootButtonPressed() {
  return digitalRead(config::kBootPin) == LOW;
}

void bootButtonInit() { initBootButton(); }

bool bootButtonConsumeTap() {
  portENTER_CRITICAL(&s_boot_mux);
  const bool tap = s_boot_tap_pending;
  if (tap) {
    s_boot_tap_pending = false;
  }
  portEXIT_CRITICAL(&s_boot_mux);
  return tap;
}

void bootButtonPollLongPress() {
  if (wifiBootButtonPressed()) {
    portENTER_CRITICAL(&s_boot_mux);
    if (!s_boot_is_down) {
      s_boot_is_down = true;
      s_boot_down_ms = millis();
    }
    const unsigned long down_ms = s_boot_down_ms;
    portEXIT_CRITICAL(&s_boot_mux);

    if (!s_long_press_handled &&
        millis() - down_ms >= config::kBootResetHoldMs) {
      s_long_press_handled = true;
      Serial.println("BOOT held — resetting WiFi");
      wifiResetCredentialsAndReboot();
    }
  } else {
    portENTER_CRITICAL(&s_boot_mux);
    s_boot_is_down = false;
    portEXIT_CRITICAL(&s_boot_mux);
    s_long_press_handled = false;
  }
}

void wifiResetCredentialsAndReboot() {
  resetWifiCredentials();
  statusScreenWifiReset();
  delay(800);
  esp_restart();
}

bool wifiReconnect() {
  initBootButton();
  Serial.println("WiFi reconnecting...");
  return connectSavedNetwork(true);
}

void wifiLoop() {
  ensureWifiManager();
  if (wifiLinkUp()) {
    if (!s_wm.getWebPortalActive() && !s_wm.getConfigPortalActive()) {
      startLanWebPortal();
    }
    if (s_wm.getWebPortalActive() || s_wm.getConfigPortalActive()) {
      bootButtonPollLongPress();
      s_wm.process();
    }
  } else {
    stopLanWebPortal();
  }
}

bool wifiSetupConnect() {
  initBootButton();
  ensureWifiManager();

  const bool force_portal = consumeForceConfigPortal();
  WiFi.setAutoReconnect(false);

  if (force_portal) {
    eraseWifiCredentials();
    WiFi.mode(WIFI_OFF);
    delay(100);
  }

  if (force_portal) {
    Serial.println("Opening WiFi setup portal (after reset)");
    if (openConfigPortal() && wifiLinkUp()) {
      WiFi.setAutoReconnect(true);
      Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str());
      return true;
    }
    Serial.println("WiFi connection failed");
    statusScreenConnectFailed();
    return false;
  }

  Serial.println("Connecting to WiFi (portal opens if needed)...");

  if (wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  if (storedWifiCredentials() && connectSavedNetwork(true)) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  if (storedWifiCredentials()) {
    Serial.println("Saved WiFi could not connect — opening setup portal");
  } else {
    Serial.println("No saved WiFi — opening setup portal");
  }

  if (openConfigPortal() && wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  Serial.println("WiFi connection failed");
  statusScreenConnectFailed();
  return false;
}
