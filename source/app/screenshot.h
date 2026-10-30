#pragma once

// Secret dev option: holding START on About toggles it. While on, releasing START saves a
// screenshot instead of exiting, and the top bar shows a dot. Kept in RAM only, so it is off on
// every launch.

bool screenshot_mode_on();
// The START hold that toggles the mode never takes a shot on its release.
void screenshot_mode_toggle();
// False on the frame a shot is taken from, so the dot stays out of the picture.
bool screenshot_indicator_on();
// Call once per frame after ui_update. A START release queues a shot; the next frame is drawn
// without the dot and saved as one 400x480 BMP (bottom screen centered below the top) in
// sdmc:/ftb/screenshots/.
void screenshot_update();
