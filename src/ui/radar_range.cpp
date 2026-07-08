#include "ui/radar_range.h"

#include "ui/radar_theme.h"

#include <Preferences.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ui::radar {

namespace {

constexpr char kPrefsNamespace[] = "planeradar";
constexpr char kPrefsRangeKey[] = "rangeIdx";
constexpr char kPrefsMilesKey[] = "useMiles";
constexpr char kPrefsRunwaysKey[] = "showRwys";
constexpr char kPrefsColorClassKey[] = "colorCls";
constexpr char kPrefsHeliIconKey[] = "heliIcon";
constexpr char kPrefsShowRouteKey[] = "showRoute";
constexpr char kPrefsSmoothKey[] = "smoothMot";
constexpr char kPrefsTrailsKey[] = "trails";
constexpr char kPrefsSpeedKey[] = "showSpeed";
constexpr char kPrefsIconsMaxKey[] = "iconsMax";
constexpr char kPrefsPerfKey[] = "perfMode";
constexpr char kPrefsHeadingKey[] = "hdgOffset";
constexpr char kPrefsFpsKey[] = "fps";
constexpr int kDefaultFps = 12;
constexpr int kMinFps = 1;
constexpr int kMaxFps = 30;
constexpr uint8_t kDefaultRangeIndex = 1;  // 10 km ring
constexpr float kKmPerMile = 1.609344f;

Preferences s_prefs;
uint8_t s_range_index = kDefaultRangeIndex;
bool s_use_miles = true;
bool s_show_runways = true;
bool s_color_by_class = true;
bool s_heli_icon = true;
bool s_show_route = false;
bool s_smooth_motion = true;
bool s_show_trails = false;
bool s_show_speed = false;
bool s_icons_only_max = false;
bool s_perf_mode = false;
int16_t s_heading_offset = 0;
int16_t s_fps = kDefaultFps;

void saveRangeIndex() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putUChar(kPrefsRangeKey, s_range_index);
  s_prefs.end();
}

void saveUseMiles() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(kPrefsMilesKey, s_use_miles);
  s_prefs.end();
}

void saveShowRunways() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(kPrefsRunwaysKey, s_show_runways);
  s_prefs.end();
}

void saveBoolPref(const char* key, bool value) {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(key, value);
  s_prefs.end();
}

bool portalCheckboxChecked(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  // WiFiManager checkbox submits its value= attribute ("T", or "F" if we prefilled F).
  if ((value[0] == 'T' || value[0] == 't' || value[0] == 'F' || value[0] == 'f') &&
      value[1] == '\0') {
    return true;
  }
  return strcmp(value, "on") == 0;
}

}  // namespace

void rangeInit() {
  if (!s_prefs.begin(kPrefsNamespace, true)) {
    return;
  }
  const uint8_t saved = s_prefs.getUChar(kPrefsRangeKey, kDefaultRangeIndex);
  s_range_index =
      (saved < kRangePresetCount) ? saved : kDefaultRangeIndex;
  s_use_miles = s_prefs.getBool(kPrefsMilesKey, true);
  s_show_runways = s_prefs.getBool(kPrefsRunwaysKey, true);
  s_color_by_class = s_prefs.getBool(kPrefsColorClassKey, true);
  s_heli_icon = s_prefs.getBool(kPrefsHeliIconKey, true);
  s_show_route = s_prefs.getBool(kPrefsShowRouteKey, true);
  s_smooth_motion = s_prefs.getBool(kPrefsSmoothKey, true);
  s_show_trails = s_prefs.getBool(kPrefsTrailsKey, true);
  s_show_speed = s_prefs.getBool(kPrefsSpeedKey, true);
  s_icons_only_max = s_prefs.getBool(kPrefsIconsMaxKey, true);
  // Perf mode default-on for a smooth out-of-box experience; xTaskCreate has a
  // single-loop fallback if the task can't be spawned.
  s_perf_mode = s_prefs.getBool(kPrefsPerfKey, true);
  s_heading_offset = s_prefs.getShort(kPrefsHeadingKey, 0);
  s_fps = s_prefs.getShort(kPrefsFpsKey, kDefaultFps);
  if (s_fps < kMinFps || s_fps > kMaxFps) {
    s_fps = kDefaultFps;
  }
  s_prefs.end();
}

void rangeNext() {
  s_range_index = static_cast<uint8_t>((s_range_index + 1) % kRangePresetCount);
  saveRangeIndex();
}

const RangePreset& rangeCurrent() { return kRangePresets[s_range_index]; }

uint8_t rangeIndex() { return s_range_index; }

float fetchRadiusKm() {
  const float outer_km = rangeCurrent().outer_km;
  const float screen_r_px =
      static_cast<float>(kCenterX - kBeyondRingScreenMarginPx);
  return outer_km * (screen_r_px / static_cast<float>(kGridOuterRadius));
}

bool useMiles() { return s_use_miles; }

bool showRunways() { return s_show_runways; }

bool colorByClass() { return s_color_by_class; }

bool heliIcon() { return s_heli_icon; }

bool showRoute() { return s_show_route; }

bool smoothMotion() { return s_smooth_motion; }

bool showTrails() { return s_show_trails; }

bool showSpeed() { return s_show_speed; }

bool iconsOnlyAtMaxZoom() { return s_icons_only_max; }

bool perfMode() { return s_perf_mode; }

float headingOffsetDeg() { return static_cast<float>(s_heading_offset); }

int frameRateFps() { return s_fps; }

unsigned long renderIntervalMs() {
  return 1000UL / static_cast<unsigned long>(s_fps > 0 ? s_fps : kDefaultFps);
}

void saveMilesFromPortal(const char* checkbox_value) {
  s_use_miles = portalCheckboxChecked(checkbox_value);
  saveUseMiles();
  Serial.printf("Distance units: %s\n", s_use_miles ? "miles" : "km");
}

void saveRunwaysFromPortal(const char* checkbox_value) {
  s_show_runways = portalCheckboxChecked(checkbox_value);
  saveShowRunways();
  Serial.printf("Runway overlay: %s\n", s_show_runways ? "on" : "off");
}

void saveColorByClassFromPortal(const char* checkbox_value) {
  s_color_by_class = portalCheckboxChecked(checkbox_value);
  saveBoolPref(kPrefsColorClassKey, s_color_by_class);
  Serial.printf("Color by class: %s\n", s_color_by_class ? "on" : "off");
}

void saveHeliIconFromPortal(const char* checkbox_value) {
  s_heli_icon = portalCheckboxChecked(checkbox_value);
  saveBoolPref(kPrefsHeliIconKey, s_heli_icon);
  Serial.printf("Helicopter icon: %s\n", s_heli_icon ? "on" : "off");
}

void saveShowRouteFromPortal(const char* checkbox_value) {
  s_show_route = portalCheckboxChecked(checkbox_value);
  saveBoolPref(kPrefsShowRouteKey, s_show_route);
  Serial.printf("Flight route: %s\n", s_show_route ? "on" : "off");
}

void saveSmoothMotionFromPortal(const char* checkbox_value) {
  s_smooth_motion = portalCheckboxChecked(checkbox_value);
  saveBoolPref(kPrefsSmoothKey, s_smooth_motion);
  Serial.printf("Smooth motion: %s\n", s_smooth_motion ? "on" : "off");
}

void saveTrailsFromPortal(const char* checkbox_value) {
  s_show_trails = portalCheckboxChecked(checkbox_value);
  saveBoolPref(kPrefsTrailsKey, s_show_trails);
  Serial.printf("Trails: %s\n", s_show_trails ? "on" : "off");
}

void saveSpeedFromPortal(const char* checkbox_value) {
  s_show_speed = portalCheckboxChecked(checkbox_value);
  saveBoolPref(kPrefsSpeedKey, s_show_speed);
  Serial.printf("Tag metric: %s\n", s_show_speed ? "speed" : "altitude");
}

void saveIconsOnlyFromPortal(const char* checkbox_value) {
  s_icons_only_max = portalCheckboxChecked(checkbox_value);
  saveBoolPref(kPrefsIconsMaxKey, s_icons_only_max);
  Serial.printf("Icons-only at max zoom: %s\n", s_icons_only_max ? "on" : "off");
}

void savePerfModeFromPortal(const char* checkbox_value) {
  s_perf_mode = portalCheckboxChecked(checkbox_value);
  saveBoolPref(kPrefsPerfKey, s_perf_mode);
  Serial.printf("Performance mode: %s (restart to apply)\n",
                s_perf_mode ? "on" : "off");
}

void saveFpsFromPortal(const char* value) {
  int fps = (value != nullptr) ? atoi(value) : kDefaultFps;
  if (fps < kMinFps) fps = kMinFps;
  if (fps > kMaxFps) fps = kMaxFps;
  s_fps = static_cast<int16_t>(fps);
  if (s_prefs.begin(kPrefsNamespace, false)) {
    s_prefs.putShort(kPrefsFpsKey, s_fps);
    s_prefs.end();
  }
  Serial.printf("Frame rate: %d fps\n", fps);
}

void saveHeadingFromPortal(const char* value) {
  int deg = (value != nullptr) ? atoi(value) : 0;
  deg = ((deg % 360) + 360) % 360;
  s_heading_offset = static_cast<int16_t>(deg);
  if (s_prefs.begin(kPrefsNamespace, false)) {
    s_prefs.putShort(kPrefsHeadingKey, s_heading_offset);
    s_prefs.end();
  }
  Serial.printf("Heading offset: %d deg\n", deg);
}

void formatRing3Label(char* buf, size_t len, float ring3_km, bool use_miles) {
  if (use_miles) {
    const int mi = static_cast<int>(lroundf(ring3_km / kKmPerMile));
    snprintf(buf, len, "%dmi", mi);
  } else {
    const int km = static_cast<int>(lroundf(ring3_km));
    snprintf(buf, len, "%dkm", km);
  }
}

void formatCurrentRing3Label(char* buf, size_t len) {
  formatRing3Label(buf, len, rangeCurrent().ring3_km, s_use_miles);
}

void unitsReset() {
  s_use_miles = true;
  s_show_runways = true;
  s_color_by_class = true;
  s_heli_icon = true;
  s_show_route = true;
  s_smooth_motion = true;
  s_show_trails = true;
  s_show_speed = true;
  s_icons_only_max = true;
  s_perf_mode = true;
  s_heading_offset = 0;
  s_fps = kDefaultFps;
  if (s_prefs.begin(kPrefsNamespace, false)) {
    s_prefs.remove(kPrefsMilesKey);
    s_prefs.remove(kPrefsRunwaysKey);
    s_prefs.remove(kPrefsColorClassKey);
    s_prefs.remove(kPrefsHeliIconKey);
    s_prefs.remove(kPrefsShowRouteKey);
    s_prefs.remove(kPrefsSmoothKey);
    s_prefs.remove(kPrefsTrailsKey);
    s_prefs.remove(kPrefsSpeedKey);
    s_prefs.remove(kPrefsIconsMaxKey);
    s_prefs.remove(kPrefsPerfKey);
    s_prefs.remove(kPrefsHeadingKey);
    s_prefs.remove(kPrefsFpsKey);
    s_prefs.end();
  }
}

}  // namespace ui::radar
