#pragma once

// Secret dev option: three START presses in a row on About's "FTB: fosterbarnes" row toggle it.
// While on, START saves a screenshot instead of exiting. Kept in RAM only, so it is off on every
// launch.

bool screenshot_mode_on();
void screenshot_mode_toggle();
// Saves both screens' last finished frames as one 400x480 BMP (bottom screen centered below the
// top) in sdmc:/ftb/screenshots/. Call between frames.
void screenshot_take();
