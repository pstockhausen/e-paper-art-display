/* info_display.h: Waveshare 2.9" V2 (SSD1680, 296×128 B/W) caption panel.
 *
 * Shows artwork title and "Artist · Year" beneath the main 13.3" display.
 * Text is rendered using the project's existing Roboto 10pt font and sent
 * over the shared FSPI bus.
 *
 * Call info_display_begin() once in setup(), then info_display_show() after
 * each artfeed_show().  The panel holds its image with no power once updated.
 *
 * Pins are defined in src/info_display_pins.h.
 * Requires ENABLE_INFO_DISPLAY in src/config.h.
 */
#pragma once

#include "artfeed.h"

/* Configure the GPIO pins and leave the panel in deep sleep. */
void info_display_begin();

/* Render title and "artist · year" and do a full refresh (~2 s).
 * If meta fields are empty the filename slug is parsed as a fallback.
 * `filename` is the .bin name from the manifest (used as fallback only). */
void info_display_show(const ArtworkMeta &meta, const char *filename);
