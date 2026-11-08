/* info_display.cpp: see info_display.h.
 *
 * SSD1680 controller, Waveshare 2.9" V2, 296×128 B/W.
 *
 * The display is mounted landscape (296 px wide, 128 px tall).  The SSD1680's
 * native scan direction is 128 source pins × 296 gate lines, so the landscape
 * render buffer is transposed on-the-fly while streaming to the panel.
 *
 * Text is drawn with roboto_draw() into a 1-bit landscape buffer using a
 * threshold plot callback, then sent as the transposed native frame.
 */
#include "info_display.h"
#include "info_display_pins.h"

#include <Arduino.h>
#include <ArduinoLog.h>
#include <SPI.h>

#include "el133.h"       /* el133_spi_bus() */
#include "artfeed.h"     /* ArtworkMeta     */
#include "roboto10.h"
#include "roboto24.h"
#include "roboto_font.h"

/* ---- Panel geometry ------------------------------------------------------ */

static const int NATIVE_W = 128; /* source pins  (short axis) */
static const int NATIVE_H = 296; /* gate lines   (long axis)  */

/* Landscape coordinates (how we render text). */
static const int LAND_W  = NATIVE_H; /* 296 pixels wide */
static const int LAND_H  = NATIVE_W; /* 128 pixels tall */
static const int LAND_ROW_BYTES = (LAND_W + 7) / 8; /* 37 bytes per row  */
static const size_t BUF_BYTES   = (size_t)LAND_H * LAND_ROW_BYTES; /* 4736 */

static const SPISettings SPI_CFG(4000000, MSBFIRST, SPI_MODE0);

/* ---- SPI helpers --------------------------------------------------------- */

static void cmd(uint8_t c)
{
    digitalWrite(INFO_PIN_DC, LOW);
    digitalWrite(INFO_PIN_CS, LOW);
    el133_spi_bus().beginTransaction(SPI_CFG);
    el133_spi_bus().transfer(c);
    el133_spi_bus().endTransaction();
    digitalWrite(INFO_PIN_CS, HIGH);
}

static void dat(uint8_t d)
{
    digitalWrite(INFO_PIN_DC, HIGH);
    digitalWrite(INFO_PIN_CS, LOW);
    el133_spi_bus().beginTransaction(SPI_CFG);
    el133_spi_bus().transfer(d);
    el133_spi_bus().endTransaction();
    digitalWrite(INFO_PIN_CS, HIGH);
}

static void dat_buf(const uint8_t *buf, size_t len)
{
    digitalWrite(INFO_PIN_DC, HIGH);
    digitalWrite(INFO_PIN_CS, LOW);
    el133_spi_bus().beginTransaction(SPI_CFG);
    el133_spi_bus().writeBytes(buf, len);
    el133_spi_bus().endTransaction();
    digitalWrite(INFO_PIN_CS, HIGH);
}

/* ---- SSD1680 init -------------------------------------------------------- */

static void ssd1680_init()
{
    cmd(0x12); delay(10); /* Software reset */

    /* Driver output control: NATIVE_H - 1 = 295 = 0x0127 */
    cmd(0x01); dat(0x27); dat(0x01); dat(0x00);

    /* Data entry mode: X increment, Y increment */
    cmd(0x11); dat(0x03);

    /* RAM X start/end: 0 to (NATIVE_W/8 - 1) = 0x0F */
    cmd(0x44); dat(0x00); dat(0x0F);

    /* RAM Y start/end: 0 to 295 */
    cmd(0x45); dat(0x00); dat(0x00); dat(0x27); dat(0x01);

    /* Border waveform, temperature sensor */
    cmd(0x3C); dat(0x05);
    cmd(0x18); dat(0x80);

    /* Display update control 1 */
    cmd(0x21); dat(0x00); dat(0x80);

    /* Set RAM counters to origin */
    cmd(0x4E); dat(0x00);
    cmd(0x4F); dat(0x00); dat(0x00);

    delay(10);
}

/* Send the 4736-byte landscape buffer transposed to native SSD1680 layout.
 *
 * Native layout: 296 gate lines × 16 bytes (= 128 source bits each).
 * For each landscape column x (= gate line), collect 128 source bits from
 * the landscape rows and write one 16-byte native row.
 */
static void send_frame(const uint8_t *land)
{
    cmd(0x24); /* Write B/W RAM */

    for (int x = 0; x < LAND_W; x++)
    {
        /* Landscape column x → native gate line.
         * INFO_FLIP reverses both axes so the image is rotated 180°. */
        int gx = INFO_FLIP ? (LAND_W - 1 - x) : x;

        uint8_t row[NATIVE_W / 8];
        memset(row, 0xFF, sizeof(row)); /* all white */

        for (int y = 0; y < LAND_H; y++)
        {
            int gy = INFO_FLIP ? (LAND_H - 1 - y) : y;

            bool black = !(land[gy * LAND_ROW_BYTES + gx / 8] & (0x80 >> (gx % 8)));
            if (black)
                row[y / 8] &= ~(0x80 >> (y % 8));
        }
        dat_buf(row, sizeof(row));
    }
}

static void ssd1680_refresh_and_sleep()
{
    cmd(0x22); dat(0xF7); /* Full update waveform */
    cmd(0x20);            /* Master activation    */
    delay(2100);          /* ~2 s for B/W full refresh */

    cmd(0x10); dat(0x01); /* Enter deep sleep mode 1 */
}

/* ---- 1-bit text rendering ------------------------------------------------ */

struct Canvas1bit
{
    uint8_t *buf;
    int w, h;
};

static void plot(int32_t x, int32_t y, uint8_t alpha, void *ctx)
{
    const Canvas1bit *c = (const Canvas1bit *)ctx;
    if (alpha < 8 || x < 0 || y < 0 || x >= c->w || y >= c->h)
        return;
    /* Clear the bit = black pixel (SSD1680: 1=white, 0=black) */
    c->buf[y * ((c->w + 7) / 8) + x / 8] &= ~(0x80 >> (x % 8));
}

/* ---- Slug parser (fallback when meta.txt is absent) ---------------------- */

/* Convert "girl-with-a-pearl-earring" to "Girl with a Pearl Earring". */
static void unslugify(const char *slug, char *out, size_t out_len)
{
    static const char *const lc[] = {
        "a", "an", "the", "of", "in", "on", "at", "by",
        "for", "with", "and", "or", "but", "to", nullptr
    };

    char tmp[128];
    strlcpy(tmp, slug, sizeof(tmp));

    size_t pos = 0;
    bool first_word = true;
    char *word = strtok(tmp, "-");
    while (word != nullptr)
    {
        bool keep_lc = false;
        if (!first_word)
        {
            for (int i = 0; lc[i]; i++)
            {
                if (strcmp(word, lc[i]) == 0)
                {
                    keep_lc = true;
                    break;
                }
            }
        }

        if (pos > 0 && pos < out_len - 1)
            out[pos++] = ' ';

        for (size_t i = 0; word[i] && pos < out_len - 1; i++, pos++)
        {
            out[pos] = (!keep_lc && i == 0) ? toupper(word[i]) : word[i];
        }

        first_word = false;
        word = strtok(nullptr, "-");
    }
    out[pos] = '\0';
}

/* Extract artist and title from a .bin filename stem such as
 * "uxmi_004_johannes-vermeer_girl-with-a-pearl-earring" or plain "my-photo". */
static void parse_filename(const char *name, ArtworkMeta &out)
{
    char stem[128];
    strlcpy(stem, name, sizeof(stem));

    /* Strip .bin extension if present. */
    char *dot = strrchr(stem, '.');
    if (dot)
        *dot = '\0';

    /* Strip 4-char random prefix (e.g. "uxmi_"). */
    if (strlen(stem) > 5 && stem[4] == '_')
    {
        bool all_alnum = true;
        for (int i = 0; i < 4; i++)
            if (!isalnum(stem[i]))
            {
                all_alnum = false;
                break;
            }
        if (all_alnum)
            memmove(stem, stem + 5, strlen(stem + 5) + 1);
    }

    /* Strip leading NNN_ number prefix. */
    {
        char *p = stem;
        while (isdigit(*p))
            p++;
        if (*p == '_')
            memmove(stem, p + 1, strlen(p + 1) + 1);
    }

    /* Split "artist-slug_title-slug" at the first underscore. */
    char *underscore = strchr(stem, '_');
    if (underscore)
    {
        *underscore = '\0';
        unslugify(stem, out.artist, sizeof(out.artist));
        unslugify(underscore + 1, out.title, sizeof(out.title));
    }
    else
    {
        unslugify(stem, out.title, sizeof(out.title));
        out.artist[0] = '\0';
    }
}

/* ---- Public API ---------------------------------------------------------- */

void info_display_begin()
{
    pinMode(INFO_PIN_CS, OUTPUT);
    pinMode(INFO_PIN_DC, OUTPUT);
    digitalWrite(INFO_PIN_CS, HIGH);
    digitalWrite(INFO_PIN_DC, HIGH);

    /* Leave the panel in deep sleep; it draws no current until we wake it. */
    ssd1680_init();
    ssd1680_refresh_and_sleep();
}

void info_display_show(const ArtworkMeta &meta, const char *filename)
{
    /* Resolve metadata: use what artfeed gave us, fall back to the filename. */
    ArtworkMeta m = meta;
    if (m.title[0] == '\0' || m.artist[0] == '\0')
    {
        ArtworkMeta from_file = {};
        parse_filename(filename, from_file);
        if (m.title[0]  == '\0') strlcpy(m.title,  from_file.title,  sizeof(m.title));
        if (m.artist[0] == '\0') strlcpy(m.artist, from_file.artist, sizeof(m.artist));
    }

    /* Build artist-year line: "Artist · Year" or just "Artist". */
    char subtitle[128];
    if (m.year[0] != '\0')
        snprintf(subtitle, sizeof(subtitle), "%s \xC2\xB7 %s", m.artist, m.year);
    else
        strlcpy(subtitle, m.artist, sizeof(subtitle));

    /* Allocate the landscape framebuffer (4736 bytes, all white). */
    uint8_t *buf = (uint8_t *)malloc(BUF_BYTES);
    if (!buf)
    {
        Log.error("info_display: no heap for frame buffer" CR);
        return;
    }
    memset(buf, 0xFF, BUF_BYTES);

    Canvas1bit canvas = {buf, LAND_W, LAND_H};

    /* ---------------------------------------------------------------
     * Layout (landscape, 296 × 128 px):
     *
     *   8 px top pad
     *   Title     Roboto24  baseline y = 8 + 47 = 55   (ascender24 = 47)
     *   4 px gap
     *   Artist·Yr Roboto10  baseline y = 55+12+4+20 = 91  (desc24=12, asc10=20)
     *   Genre     Roboto10  baseline y = 91+4+20 = 115   (desc10=4)
     *   9 px bottom pad (128 − 119 = 9)
     * ---------------------------------------------------------------*/
    const int pad_x  = 8;
    const int max_w  = LAND_W - 2 * pad_x; /* 280 px */

    const int asc24  = Roboto24.ascender;   /* 47 */
    const int desc24 = Roboto24.advance_y - asc24; /* 59 − 47 = 12 */
    const int asc10  = Roboto10.ascender;   /* 20 */
    const int desc10 = Roboto10.advance_y - asc10; /* 24 − 20 = 4 */

    const int y_title    = 8 + asc24;              /* 55 */
    const int y_subtitle = y_title + desc24 + 4 + asc10; /* 91 */
    const int y_genre    = y_subtitle + desc10 + asc10;  /* 115 */

    /* -- Title (Roboto24, truncated with "…" if it exceeds max_w) -- */
    {
        char display_title[sizeof(m.title) + 4]; /* room for UTF-8 "…" */
        strlcpy(display_title, m.title, sizeof(display_title));

        if (roboto_measure(&Roboto24, display_title) > max_w)
        {
            size_t len = strlen(display_title);
            while (len > 0)
            {
                /* Back up over UTF-8 continuation bytes (0x80–0xBF). */
                len--;
                while (len > 0 && (display_title[len] & 0xC0) == 0x80)
                    len--;
                display_title[len] = '\0';

                char tmp[sizeof(display_title)];
                snprintf(tmp, sizeof(tmp), "%s\xE2\x80\xA6", display_title); /* "…" */
                if (roboto_measure(&Roboto24, tmp) <= max_w)
                {
                    strlcpy(display_title, tmp, sizeof(display_title));
                    break;
                }
            }
        }

        int32_t x = pad_x;
        roboto_draw(&Roboto24, display_title, &x, y_title, plot, &canvas);
    }

    /* -- Separator line between title and subtitle -- */
    {
        int line_y = y_title + desc24 + 1;
        int row    = line_y * LAND_ROW_BYTES;
        for (int x = pad_x; x < LAND_W - pad_x; x++)
            buf[row + x / 8] &= ~(0x80 >> (x % 8));
    }

    /* -- Artist · Year (Roboto10) -- */
    {
        int32_t x = pad_x;
        roboto_draw(&Roboto10, subtitle, &x, y_subtitle, plot, &canvas);
    }

    /* -- Genre / movement (Roboto10) -- */
    if (m.movement[0] != '\0')
    {
        int32_t x = pad_x;
        roboto_draw(&Roboto10, m.movement, &x, y_genre, plot, &canvas);
    }

    /* Wake the panel, send the frame, trigger refresh, sleep again. */
    ssd1680_init();
    send_frame(buf);
    ssd1680_refresh_and_sleep();

    free(buf);
    Log.notice("Info display: \"%s\" / \"%s\" [%s]" CR,
               m.title, subtitle, m.movement[0] ? m.movement : "—");
}
