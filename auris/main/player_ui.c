// Self contained UI for the QEMU RGB panel.
//
// The esp_lcd_qemu_rgb component exposes its frame buffer (a plain RAM region the
// guest can write to) plus a refresh call, so the screen is composed by direct
// pixel writes instead of a chain of esp_lcd_panel_draw_bitmap() calls - each of
// those blocks until the QEMU display has consumed the previous update, which
// makes incremental drawing needlessly slow.

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_lcd_qemu_rgb.h"

#include "player_ui.h"
#include "ui_font7x8.h"

static const char *TAG = "PLAYER_UI";

// RGB565 palette
#define COLOR_BG        0x0863  // near black navy
#define COLOR_HEADER    0x10C6  // slightly lighter header bar
#define COLOR_ACCENT    0x07FF  // cyan
#define COLOR_TEXT      0xFFFF  // white
#define COLOR_DIM       0x8410  // grey
#define COLOR_BAR_BG    0x2124  // dark grey
#define COLOR_BAR_FILL  0x07E0  // green
#define COLOR_ERROR     0xF800  // red
#define COLOR_WARN      0xFFE0  // yellow
#define COLOR_OK        0x07E0  // green

// Layout
#define MARGIN       20
#define HEADER_H     52
#define ACCENT_H     2

static uint16_t *s_fb;
static int s_width;
static int s_height;

// ---------------------------------------------------------------------------
// Drawing primitives
// ---------------------------------------------------------------------------
static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s_width)  w = s_width - x;
    if (y + h > s_height) h = s_height - y;
    if (w <= 0 || h <= 0) return;

    uint16_t *row = &s_fb[y * s_width + x];
    for (int i = 0; i < h; i++) {
        for (int j = 0; j < w; j++) {
            row[j] = color;
        }
        row += s_width;
    }
}

static int text_width(const char *text, int scale)
{
    int len = (int)strlen(text);
    if (len == 0) return 0;
    return (len * (UI_FONT_W + 1) - 1) * scale;
}

static void draw_char(int x, int y, char ch, uint16_t color, int scale)
{
    unsigned char code = (unsigned char)ch;
    if (code < UI_FONT_FIRST || code > UI_FONT_LAST) {
        code = '?';
    }

    const uint16_t *glyph = ui_font7x8[code - UI_FONT_FIRST];
    for (int col = 0; col < UI_FONT_W; col++) {
        uint16_t bits = glyph[col];
        for (int row = 0; row < UI_FONT_H; row++) {
            if (bits & (1u << row)) {
                fill_rect(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

static void draw_text(int x, int y, const char *text, uint16_t color, int scale)
{
    for (; *text; text++) {
        draw_char(x, y, *text, color, scale);
        x += (UI_FONT_W + 1) * scale;
    }
}

static void draw_text_centered(int y, const char *text, uint16_t color, int scale)
{
    draw_text((s_width - text_width(text, scale)) / 2, y, text, color, scale);
}

static void draw_text_right(int right, int y, const char *text, uint16_t color, int scale)
{
    draw_text(right - text_width(text, scale), y, text, color, scale);
}

// Copies `src` into `dst`, shortening it with an ellipsis so the rendered text
// never exceeds `max_px` pixels at the given scale.
static void fit_text(char *dst, size_t dst_size, const char *src, int max_px, int scale)
{
    int max_chars = max_px / ((UI_FONT_W + 1) * scale);
    size_t cap = dst_size - 1;
    if (max_chars > (int)cap) max_chars = (int)cap;
    if (max_chars < 4) max_chars = cap < 4 ? (int)cap : 4;

    size_t n = 0;
    while (src[n] && n < (size_t)max_chars) {
        dst[n] = src[n];
        n++;
    }
    if (src[n]) {                                  // text did not fit
        for (int i = 0; i < 3 && n > 0; i++) {
            dst[--n] = '.';
        }
    }
    dst[n] = '\0';
}

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------
static void format_time(char *dst, size_t size, uint32_t seconds)
{
    if (seconds >= 3600) {
        snprintf(dst, size, "%lu:%02lu:%02lu",
                 (unsigned long)(seconds / 3600),
                 (unsigned long)((seconds / 60) % 60),
                 (unsigned long)(seconds % 60));
    } else {
        snprintf(dst, size, "%02lu:%02lu",
                 (unsigned long)(seconds / 60),
                 (unsigned long)(seconds % 60));
    }
}

static uint32_t elapsed_seconds(const player_status_t *st)
{
    if (st->sample_rate == 0) return 0;
    return st->samples_played / st->sample_rate;
}

// Note: the total track length comes from the audio task, not from here. The
// old version derived it here as file_size * 8 / bitrate, which is only right
// for CBR and reported 14:54 for a 4:05 VBR track.

static uint8_t mpeg_version_from_rate(uint32_t sample_rate)
{
    if (sample_rate >= 32000) return 1;    // 32/44.1/48 kHz
    if (sample_rate >= 16000) return 2;    // 16/22.05/24 kHz
    return 25;                            // 8/11.025/12 kHz
}

static const char *phase_text(player_phase_t phase)
{
    switch (phase) {
        case PLAYER_PHASE_IDLE:     return "IDLE";
        case PLAYER_PHASE_LOADING:  return "LOADING";
        case PLAYER_PHASE_PLAYING:  return "NOW PLAYING";
        case PLAYER_PHASE_FINISHED: return "FINISHED";
        case PLAYER_PHASE_ERROR:    return "ERROR";
        default:                    return "?";
    }
}

static uint16_t phase_color(player_phase_t phase)
{
    switch (phase) {
        case PLAYER_PHASE_LOADING:  return COLOR_WARN;
        case PLAYER_PHASE_PLAYING:  return COLOR_OK;
        case PLAYER_PHASE_ERROR:    return COLOR_ERROR;
        default:                    return COLOR_DIM;
    }
}

static uint16_t channels_color(uint8_t channels)
{
    return channels > 1 ? COLOR_TEXT : COLOR_WARN;  // mono through a stereo bus
}

// ---------------------------------------------------------------------------
// Screen sections
// ---------------------------------------------------------------------------
static void draw_header(const player_status_t *st)
{
    fill_rect(0, 0, s_width, HEADER_H, COLOR_HEADER);
    fill_rect(0, HEADER_H - ACCENT_H, s_width, ACCENT_H, COLOR_ACCENT);

    draw_text(MARGIN, 6, "AURIS", COLOR_TEXT, 4);
    draw_text_right(s_width - MARGIN, 16, phase_text(st->phase),
                    phase_color(st->phase), 2);
}

static void draw_track_info(const player_status_t *st)
{
    const int usable = s_width - 2 * MARGIN;
    const char *base = strrchr(st->path, '/');
    base = base ? base + 1 : st->path;

    char name[24];
    fit_text(name, sizeof(name), base, usable, 4);
    draw_text(MARGIN, 70, name, COLOR_TEXT, 4);

    char path[40];
    fit_text(path, sizeof(path), st->path, usable, 2);
    draw_text(MARGIN, 120, path, COLOR_DIM, 2);

    if (st->sample_rate == 0) return;   // nothing decoded yet

    char codec[24];
    snprintf(codec, sizeof(codec), "MPEG-%u LAYER %u",
             (unsigned)mpeg_version_from_rate(st->sample_rate),
             (unsigned)st->layer);
    draw_text(MARGIN, 152, codec, COLOR_ACCENT, 3);

    char format[40];
    snprintf(format, sizeof(format), "%lu.%01lu KHZ  %lu KBPS  %s",
             (unsigned long)(st->sample_rate / 1000),
             (unsigned long)((st->sample_rate / 100) % 10),
             (unsigned long)st->bitrate_kbps,
             st->channels > 1 ? "STEREO" : "MONO");
    draw_text(MARGIN, 192, format, channels_color(st->channels), 2);
}

static uint32_t progress_percent(const player_status_t *st)
{
    // Prefer time: bytes are a poor progress proxy for VBR, where frame sizes
    // vary by a factor of ten, so a byte-fraction bar jumps around.
    if (st->total_seconds > 0) {
        uint32_t pct = (uint32_t)((uint64_t)elapsed_seconds(st) * 100u /
                                  st->total_seconds);
        return pct > 100 ? 100 : pct;
    }
    if (st->file_size > 0 && st->file_pos <= st->file_size) {
        return (uint32_t)((uint64_t)st->file_pos * 100u / st->file_size);
    }
    return 0;
}

static void draw_clock_and_progress(const player_status_t *st)
{
    char clock[16];
    uint32_t elapsed = elapsed_seconds(st);
    if (st->sample_rate == 0) {
        snprintf(clock, sizeof(clock), "--:--");
    } else {
        format_time(clock, sizeof(clock), elapsed);
    }
    draw_text_centered(236, clock, COLOR_TEXT, 6);

    const int bar_x = MARGIN;
    const int bar_y = 320;
    const int bar_w = s_width - 2 * MARGIN;
    const int bar_h = 18;

    fill_rect(bar_x, bar_y, bar_w, bar_h, COLOR_BAR_BG);
    int fill_w = (int)((uint32_t)(bar_w - 4) * progress_percent(st) / 100u);
    fill_rect(bar_x + 2, bar_y + 2, fill_w, bar_h - 4, COLOR_BAR_FILL);
    // 1 px outline drawn as four thin bars
    fill_rect(bar_x, bar_y, bar_w, 1, COLOR_DIM);
    fill_rect(bar_x, bar_y + bar_h - 1, bar_w, 1, COLOR_DIM);
    fill_rect(bar_x, bar_y, 1, bar_h, COLOR_DIM);
    fill_rect(bar_x + bar_w - 1, bar_y, 1, bar_h, COLOR_DIM);

    // Roomy enough for the longest possible "h:mm:ss / ~h:mm:ss" at scale 3
    char times[40];
    if (st->sample_rate == 0) {
        snprintf(times, sizeof(times), "--:-- / --:--");
    } else {
        char total[12];
        format_time(total, sizeof(total), st->total_seconds);
        // The tilde marks a duration the decoder had to estimate. A counted
        // one (Xing/Info frame count) is exact, so it is shown without one.
        snprintf(times, sizeof(times), "%s / %s%s", clock,
                 st->total_exact ? "" : "~", total);
    }
    draw_text(MARGIN, 350, times, COLOR_DIM, 3);

    char percent[8];
    snprintf(percent, sizeof(percent), "%lu%%", (unsigned long)progress_percent(st));
    draw_text_right(s_width - MARGIN, 350, percent, COLOR_BAR_FILL, 3);
}

static void draw_footer(const player_status_t *st)
{
    const int usable = s_width - 2 * MARGIN;

    if (st->message[0]) {
        char message[40];
        fit_text(message, sizeof(message), st->message, usable, 2);
        draw_text_centered(398, message, phase_color(st->phase), 2);
    }

    char left[32];
    if (st->file_size > 0) {
        snprintf(left, sizeof(left), "%.2f MB  %u BIT",
                 (double)st->file_size / (1024.0 * 1024.0),
                 (unsigned)st->bits_per_sample);
    } else {
        snprintf(left, sizeof(left), "NO FILE");
    }
    draw_text(MARGIN, 444, left, COLOR_DIM, 2);
    draw_text_right(s_width - MARGIN, 444, "I2S", COLOR_DIM, 2);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
esp_err_t player_ui_init(esp_lcd_panel_handle_t panel, int width, int height)
{
    void *fb = NULL;
    esp_err_t err = esp_lcd_rgb_qemu_get_frame_buffer(panel, &fb);
    if (err != ESP_OK || fb == NULL) {
        ESP_LOGE(TAG, "No frame buffer available (0x%x)", err);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    s_fb = (uint16_t *)fb;
    // The panel is configured with bpp = RGB_QEMU_BPP_16, so the frame buffer
    // holds width * height RGB565 pixels.
    s_width = width;
    s_height = height;

    fill_rect(0, 0, s_width, s_height, COLOR_BG);
    return esp_lcd_rgb_qemu_refresh(panel);
}

void player_ui_render(esp_lcd_panel_handle_t panel, const player_status_t *st)
{
    if (!s_fb || !panel || !st) return;

    fill_rect(0, 0, s_width, s_height, COLOR_BG);
    draw_header(st);
    draw_track_info(st);
    draw_clock_and_progress(st);
    draw_footer(st);

    esp_lcd_rgb_qemu_refresh(panel);
}
