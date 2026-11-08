/* Pin assignments for the Waveshare 2.9" V2 (SSD1680, 296×128 B/W) info
 * display on an Adafruit Feather ESP32-C6.
 *
 * The display shares the main FSPI bus (SCK = GPIO21, MOSI = GPIO22) with
 * the Pimoroni Inky Impression.  Only two extra GPIO pins are needed:
 *
 *   CS  → TX   (GPIO16)  free UART TX, unused in this build
 *   D/C → MI   (GPIO23)  the MISO/readback pin; safe to repurpose because
 *                         the main panel is write-only (el133.cpp passes -1
 *                         for MISO in SPI.begin(), leaving GPIO23 free).
 *
 * Hardware reset and BUSY polling are avoided:
 *   RST   – tie to 3.3 V through a 10 kΩ pull-up (no software control needed;
 *            a software reset is sent in the init sequence via cmd 0x12).
 *   BUSY  – a fixed 2-second delay is used instead of polling, which avoids
 *            needing another GPIO.
 *
 * If the display appears upside-down after mounting, set INFO_FLIP to true.
 */
#pragma once

#define INFO_PIN_CS  TX    /* GPIO16 */
#define INFO_PIN_DC  MISO  /* GPIO23 */

/* Set true to rotate the image 180° - useful if the FPC connector ends up
 * at the right side of the frame instead of the left. */
#define INFO_FLIP false
