// tools/harness/screenshot.h
// Headless framebuffer screenshots from the Rigel frame.
//
// Env:
//   HARNESS_SCREENSHOT_FRAMES="300,1200"  frames to capture
//   HARNESS_SCREENSHOT_DIR=<dir>          output dir (default ".")
//
// Writes shot_<frame>.ppm (P6, RGB) per listed frame.

#ifndef BELLATRIX_HARNESS_SCREENSHOT_H
#define BELLATRIX_HARNESS_SCREENSHOT_H

// Call once per frame from the main loop; no-op unless
// HARNESS_SCREENSHOT_FRAMES is set and lists frame_count.
// Additionally, HARNESS_CHIPDUMP="hexaddr:hexlen" writes
// chip_<frame>_<addr>.bin from chip RAM at the same frames.
void harness_maybe_screenshot(long frame_count);

#endif
