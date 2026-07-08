#pragma once

namespace ui {

/** Draw the static sonar/radar grid (black disc, green overlay, labels). */
void radarDisplayDraw();

/** Redraw aircraft only (blits cached grid; no full-screen clear). */
void radarDisplayRefreshAircraft();

/** Tint the center dot to show a network fetch is in flight. */
void radarSetFetchActive(bool active);

}  // namespace ui
