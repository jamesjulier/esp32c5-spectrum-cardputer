/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * esp32c5-spectrum for M5Stack CardputerZero (320 x 170)
 *
 * Plug an ESP32-C5 board (Waveshare 1.47", Waveshare 2.8" Touch, LilyGO T-Dongle-C5,
 * or Seeed XIAO ESP32-C5) into the CardputerZero's side USB port and see the live
 * 2.4 & 5 GHz spectrum on the CardputerZero screen.
 *
 * Radio backend: ESP-SDR by Florian Euchner (ESPARGOS), GPL-3.0.
 * Spectrum maths & UI design: James Julier (https://github.com/jamesjulier/esp32c5-spectrum).
 *
 * Levels are relative: dB above the noise floor, never dBm (ESP-SDR's gain is not calibrated).
 * Receive only: nothing transmits.
 */
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#ifdef __APPLE__
#define USE_SDL 1
#endif
#ifdef USE_SDL
#include <SDL.h>
#endif
#ifdef __linux__
#include <linux/fb.h>
#include <linux/input.h>
#include <sys/mman.h>
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <IOKit/serial/ioss.h>
#endif

#include "eng_font.h"
#include "eng_font_aa.h"
#include "logo_icon.h"

#define SCREEN_W 320
#define SCREEN_H 170

#define FFT_N        1024
#define SEG_MHZ      32
#define DC_MASK_BINS 6
#define RANGE_DB     48
#define PAD_DB       4
#define GREEN_DB     36
#define FLOOR_CODE   80
#define BUSY_DB      6
#define LEV          (RANGE_DB + PAD_DB)
#define MAX_SEGS     8
#define MAX_CH       32

/* Screen layout for 320 x 170 */
#define HDR_H        20
#define TRACE_Y      20
#define TRACE_H      68
#define WF_Y         (TRACE_Y + TRACE_H + 1)
#define WF_H         59
#define MARK_Y       (WF_Y + WF_H + 1)
#define SCORE_Y      ( SCREEN_H - 9 )

#define DENS_Y       20
#define DENS_H       128

typedef struct {
    const char *name;
    const char *short_name;
    unsigned start, end;
    unsigned ch_first, ch_last, ch_step, ch_label_every;
    unsigned ch_base_mhz;
} band_t;

static const band_t BANDS[] = {
    { "2.4 GHz",          "2.4G",      2402, 2482,   1,  13, 1, 5, 2407 },
    { "5 GHz ch 36-64",   "5G 36-64",  5170, 5330,  36,  64, 4, 8, 5000 },
    { "5 GHz ch 100-144", "5G 100-144",5490, 5730, 100, 144, 4, 8, 5000 },
    { "5 GHz ch 149-165", "5G 149-165",5735, 5855, 149, 165, 4, 8, 5000 },
};
#define BAND_COUNT (sizeof(BANDS) / sizeof(BANDS[0]))

typedef enum { MODE_LIVE = 0, MODE_DENSITY = 1, MODE_CHANNELS = 2 } view_mode_t;
typedef enum { LINK_SEARCHING = 0, LINK_CONNECTING = 1, LINK_CONNECTED = 2, LINK_DEMO = 3 } link_kind_t;

/* Shared application state protected by g_lock */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile bool g_running = true;

static unsigned g_band_idx = 0;
static int      g_zoom_ch  = 0;     /* 0 = full band, >0 = 40 MHz around channel */
static view_mode_t g_mode  = MODE_LIVE;
static bool     g_peak_on  = true;
static bool     g_avg_on   = false;
static bool     g_overlay  = false;
static bool     g_paused   = false;
static bool     g_phosphor = false;
static bool     g_help_on  = false;
static bool     g_force_demo = false;
static bool     g_demo_allowed = false;
static int      g_cursor_x = -1;    /* -1 = off, 0..SCREEN_W-1 = active marker */
static uint32_t g_generation = 1;

static link_kind_t g_link = LINK_SEARCHING;
static char     g_board_name[64] = "";
static char     g_port_name[64]  = "";
static char     g_toast[96]      = "";
static uint64_t g_toast_until_ms = 0;

/* Per-band measurement state */
static bool     g_has_data = false;
static uint8_t  g_col[SCREEN_W];
static uint8_t  g_shown[SCREEN_W];
static uint8_t  g_peak[SCREEN_W];
static float    g_avg_lin[SCREEN_W];
static double   g_long_sum[SCREEN_W];
static uint32_t g_long_n = 0;
static float    g_seg_floor[MAX_SEGS];

static uint8_t  g_wf[WF_H][SCREEN_W];
static int      g_wf_rows = 0;
static float    g_row_sum[SCREEN_W];
static int      g_row_n = 0;
static uint64_t g_row_t0_ms = 0;

static uint32_t g_dens[SCREEN_W][LEV];
static uint32_t g_sweeps = 0;
static int8_t   g_ch_busy[MAX_CH];
static uint32_t g_ch_hot[MAX_CH];
static uint32_t g_ch_total[MAX_CH];

/* Framebuffer (32-bit ARGB8888 for SDL texture) */
static uint32_t g_fb[SCREEN_H * SCREEN_W];

static uint64_t now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000);
}

static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    if (g_phosphor) {
        /* Green phosphor oscilloscope mode */
        int lum = (r * 77 + g * 150 + b * 29) >> 8;
        int pg = lum + (g >> 2);
        if (pg > 255) pg = 255;
        return 0xFF000000u | ((uint32_t)(lum >> 3) << 16) | ((uint32_t)pg << 8) | (uint32_t)(lum >> 2);
    }
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static inline float to_lin(uint8_t code) { return powf(10.0f, (float)code / 20.0f); }
static inline uint8_t to_code(float lin) {
    float c = lin > 0.0f ? 20.0f * log10f(lin) : 0.0f;
    return (uint8_t)(c < 0.0f ? 0 : c > 255.0f ? 255 : c + 0.5f);
}
static inline float db_of(uint8_t code) { return ((float)code - (float)FLOOR_CODE) / 2.0f; }

static band_t current_view_band(void) {
    band_t b = BANDS[g_band_idx % BAND_COUNT];
    if (g_zoom_ch > 0) {
        unsigned c = b.ch_base_mhz + 5 * (unsigned)g_zoom_ch;
        unsigned lo = c > 20 ? c - 20 : b.start;
        if (lo < b.start) lo = b.start;
        if (lo + 40 > b.end) lo = b.end - 40;
        b.start = lo;
        b.end = lo + 40;
        b.ch_label_every = b.ch_step;
    }
    return b;
}

static void reset_measurements_locked(void) {
    g_generation++;
    g_has_data = false;
    memset(g_col, 0, sizeof(g_col));
    memset(g_shown, 0, sizeof(g_shown));
    memset(g_peak, 0, sizeof(g_peak));
    for (int x = 0; x < SCREEN_W; x++) {
        g_avg_lin[x] = -1.0f;
        g_long_sum[x] = 0.0;
        g_row_sum[x] = 0.0f;
    }
    g_long_n = 0;
    for (int i = 0; i < MAX_SEGS; i++) g_seg_floor[i] = -1.0f;
    memset(g_wf, 0, sizeof(g_wf));
    g_wf_rows = 0;
    g_row_n = 0;
    g_row_t0_ms = now_ms();
    memset(g_dens, 0, sizeof(g_dens));
    g_sweeps = 0;
    for (int i = 0; i < MAX_CH; i++) {
        g_ch_busy[i] = -1;
        g_ch_hot[i] = 0;
        g_ch_total[i] = 0;
    }
}

static void show_toast_locked(const char *msg, uint64_t duration_ms) {
    snprintf(g_toast, sizeof(g_toast), "%s", msg);
    g_toast_until_ms = now_ms() + duration_ms;
}

/* ─── Drawing Primitives & Anti-Aliased Fonts ────────────────────────────── */

static inline void put_px(int x, int y, uint32_t col) {
    if ((unsigned)x < SCREEN_W && (unsigned)y < SCREEN_H) {
        g_fb[y * SCREEN_W + x] = col;
    }
}

static void fill_rect(int x, int y, int w, int h, uint32_t col) {
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = (x + w) > SCREEN_W ? SCREEN_W : (x + w);
    int y1 = (y + h) > SCREEN_H ? SCREEN_H : (y + h);
    for (int r = y0; r < y1; r++) {
        uint32_t *row = &g_fb[r * SCREEN_W];
        for (int c = x0; c < x1; c++) row[c] = col;
    }
}

static void draw_rect_outline(int x, int y, int w, int h, uint32_t col) {
    fill_rect(x, y, w, 1, col);
    fill_rect(x, y + h - 1, w, 1, col);
    fill_rect(x, y, 1, h, col);
    fill_rect(x + w - 1, y, 1, h, col);
}

static void draw_line(int x0, int y0, int x1, int y1, uint32_t col) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        put_px(x0, y0, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static inline uint32_t blend_argb(uint32_t bg, uint32_t fg, uint8_t a) {
    int rb = (bg >> 16) & 0xFF, gb = (bg >> 8) & 0xFF, bb = bg & 0xFF;
    int rf = (fg >> 16) & 0xFF, gf = (fg >> 8) & 0xFF, bf = fg & 0xFF;
    int r = rb + ((rf - rb) * a) / 255;
    int g = gb + ((gf - gb) * a) / 255;
    int b = bb + ((bf - bb) * a) / 255;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void draw_sprite(int x0, int y0, int w, int h, const uint32_t *px) {
    for (int r = 0; r < h; r++) {
        int py = y0 + r;
        if ((unsigned)py >= SCREEN_H) continue;
        for (int c = 0; c < w; c++) {
            int px_x = x0 + c;
            if ((unsigned)px_x >= SCREEN_W) continue;
            uint32_t src = px[r * w + c];
            uint8_t a = (uint8_t)(src >> 24);
            if (a == 0) continue;
            uint32_t col = rgb((src >> 16) & 0xFF, (src >> 8) & 0xFF, src & 0xFF);
            uint32_t *dst = &g_fb[py * SCREEN_W + px_x];
            *dst = (a == 255) ? col : blend_argb(*dst, col, a);
        }
    }
}

static int aa_index(const char **s) {
    unsigned char c = (unsigned char)**s;
    if (c >= 0x20 && c < 0x7F) { (*s)++; return c - 0x20; }
    if (c == 0xC2 && (unsigned char)(*s)[1] == 0xB7) { *s += 2; return 95; } /* · */
    if (c == 0xE2 && (unsigned char)(*s)[1] == 0x80 && (unsigned char)(*s)[2] == 0x94) { *s += 3; return 96; } /* — */
    (*s)++;
    return 0;
}

static int aa_width(const char *s, const eng_aa_font_t *f) {
    int w = 0;
    while (*s) w += f->g[aa_index(&s)].adv;
    return w;
}

static void aa_text(int x, int top, const char *s, const eng_aa_font_t *f, uint32_t col) {
    int base = top + f->ascent;
    while (*s) {
        const eng_glyph_t *g = &f->g[aa_index(&s)];
        int gx = x + g->left, gy = base - g->top;
        for (int r = 0; r < g->h; r++) {
            int py = gy + r;
            if ((unsigned)py >= SCREEN_H) continue;
            const uint8_t *cov = f->px + g->off + r * g->w;
            for (int c = 0; c < g->w; c++) {
                int px = gx + c;
                if (cov[c] == 0 || (unsigned)px >= SCREEN_W) continue;
                uint32_t *d = &g_fb[py * SCREEN_W + px];
                *d = cov[c] == 255 ? col : blend_argb(*d, col, cov[c]);
            }
        }
        x += g->adv;
    }
}

/* Colour ramps matching esp32c5-spectrum */
static uint32_t trace_fill_color(int ty, int h) {
    float db = (float)(h - 1 - ty) * (RANGE_DB + PAD_DB) / (float)h - PAD_DB;
    float t = db <= 0.0f ? 0.0f : db >= (float)GREEN_DB ? 1.0f : db / (float)GREEN_DB;
    float hue = t * 120.0f;
    uint8_t r, g;
    if (hue < 60.0f) { r = 255; g = (uint8_t)(hue / 60.0f * 255.0f); }
    else             { r = (uint8_t)((120.0f - hue) / 60.0f * 255.0f); g = 255; }
    return rgb(r, g, 0);
}

static uint32_t trace_edge_color(int ty, int h) {
    float db = (float)(h - 1 - ty) * (RANGE_DB + PAD_DB) / (float)h - PAD_DB;
    float t = db <= 0.0f ? 0.0f : db >= (float)GREEN_DB ? 1.0f : db / (float)GREEN_DB;
    float hue = t * 120.0f;
    uint8_t r = hue < 60.0f ? 255 : (uint8_t)((120.0f - hue) / 60.0f * 255.0f);
    return rgb((uint8_t)(r + (255 - r) / 2), 255, 130);
}

static uint32_t heat_color(uint8_t v) {
    if (v < 16) return rgb(0, 0, 0);
    if (v < 96)  return rgb(0, (uint8_t)((v - 16) * 2), (uint8_t)(80 + (v - 16)));
    if (v < 176) return rgb((uint8_t)((v - 96) * 3), 200, (uint8_t)(160 - (v - 96) * 2));
    return rgb(255, (uint8_t)(200 - (v - 176) * 2), 0);
}

static uint32_t dens_color(float pr) {
    if (pr <= 0.0f) return rgb(0, 0, 0);
    float t = log10f(1.0f + pr * 99.0f) / 2.0f;
    if (t > 1.0f) t = 1.0f;
    if (t < 0.25f) {
        float f = t / 0.25f;
        return rgb(0, (uint8_t)(140 * f), (uint8_t)(90 + (255 - 90) * f));
    } else if (t < 0.5f) {
        float f = (t - 0.25f) / 0.25f;
        return rgb(0, (uint8_t)(140 + (220 - 140) * f), (uint8_t)(255 - (255 - 90) * f));
    } else if (t < 0.75f) {
        float f = (t - 0.5f) / 0.25f;
        return rgb((uint8_t)(255 * f), (uint8_t)(220 + (230 - 220) * f), (uint8_t)(90 * (1.0f - f)));
    } else {
        float f = (t - 0.75f) / 0.25f;
        return rgb(255, (uint8_t)(230 - (230 - 40) * f), 0);
    }
}

static uint32_t busy_color(int pct) {
    if (pct < 0)  return rgb(55, 65, 81);
    if (pct < 15) return rgb(0x22, 0xC5, 0x5E); /* green */
    if (pct < 35) return rgb(0xFB, 0xBF, 0x24); /* amber */
    if (pct < 60) return rgb(0xF9, 0x73, 0x16); /* orange */
    return rgb(0xEF, 0x44, 0x44);               /* red */
}

static int code_to_y(float code, int h) {
    float db_above = (code - (float)FLOOR_CODE) / 2.0f + PAD_DB;
    int py = (int)(db_above * h / (RANGE_DB + PAD_DB));
    if (py < 0) py = 0;
    if (py > h - 1) py = h - 1;
    return h - 1 - py;
}

static uint8_t code_to_heat(uint8_t code) {
    float db_above = ((float)code - (float)FLOOR_CODE) / 2.0f;
    if (db_above <= 0.0f) return 0;
    float v = db_above * 255.0f / (float)GREEN_DB;
    return v > 255.0f ? 255 : (uint8_t)v;
}

static int mhz_to_x(const band_t *v, float mhz) {
    return (int)((mhz - (float)v->start) * SCREEN_W / (float)(v->end - v->start));
}

static unsigned nearest_channel(const band_t *v, float mhz) {
    unsigned best = v->ch_first;
    float bd = 1e9f;
    for (unsigned ch = v->ch_first; ch <= v->ch_last; ch += v->ch_step) {
        float d = fabsf(mhz - (float)(v->ch_base_mhz + 5 * ch));
        if (d < bd) { bd = d; best = ch; }
    }
    return best;
}

static int best_quiet_channel(const band_t *v, int *out_pct) {
    int best = -1, bp = 101, i = 0;
    for (unsigned ch = v->ch_first; ch <= v->ch_last && i < MAX_CH; ch += v->ch_step, i++) {
        if (g_ch_busy[i] < 0) continue;
        if (v->ch_base_mhz < 3000 && ch != 1 && ch != 6 && ch != 11) continue;
        if (g_ch_busy[i] < bp) { bp = g_ch_busy[i]; best = (int)ch; }
    }
    *out_pct = bp;
    return best;
}

/* ─── Fold Segment & Record Sweep ────────────────────────────────────────── */

static void fold_segment_locked(const band_t *v, unsigned k, unsigned centre, const uint8_t *codes) {
    uint8_t sh[FFT_N];
    for (int i = 0; i < FFT_N; i++) sh[i] = codes[(i + FFT_N / 2) % FFT_N];
    int c = FFT_N / 2;
    uint8_t lo = sh[c - DC_MASK_BINS - 1], hi = sh[c + DC_MASK_BINS + 1];
    for (int i = c - DC_MASK_BINS; i <= c + DC_MASK_BINS; i++) sh[i] = (uint8_t)((lo + hi) / 2);

    const float mhz_per_bin = 80.0f / (float)FFT_N;
    const float mhz_per_col = (float)(v->end - v->start) / (float)SCREEN_W;
    int x0 = -1, x1 = -1;
    for (int x = 0; x < SCREEN_W; x++) {
        float f = (float)v->start + ((float)x + 0.5f) * mhz_per_col;
        float off = f - (float)centre;
        if (off < -(float)SEG_MHZ / 2.0f || off >= (float)SEG_MHZ / 2.0f) continue;
        int b0 = c + (int)((off - mhz_per_col / 2.0f) / mhz_per_bin);
        int b1 = c + (int)((off + mhz_per_col / 2.0f) / mhz_per_bin);
        if (b1 < b0) b1 = b0;
        uint8_t m = 0;
        for (int b = b0; b <= b1; b++)
            if (b >= 0 && b < FFT_N && sh[b] > m) m = sh[b];
        g_col[x] = m;
        if (x0 < 0) x0 = x;
        x1 = x;
    }
    if (x0 < 0) return;

    uint8_t sorted[SCREEN_W];
    int n = x1 - x0 + 1;
    memcpy(sorted, &g_col[x0], (size_t)n);
    for (int i = 1; i < n; i++) {
        uint8_t key = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > key) { sorted[j + 1] = sorted[j]; j--; }
        sorted[j + 1] = key;
    }
    float now_fl = (float)sorted[n * 15 / 100];
    float *fl = &g_seg_floor[k < MAX_SEGS ? k : MAX_SEGS - 1];
    *fl = (*fl < 0.0f) ? now_fl : (*fl * 0.85f + now_fl * 0.15f);
    for (int x = x0; x <= x1; x++) {
        int v2 = (int)g_col[x] - (int)(*fl + 0.5f) + FLOOR_CODE;
        g_col[x] = (uint8_t)(v2 < 0 ? 0 : v2 > 255 ? 255 : v2);
    }
}

static void finish_sweep_locked(const band_t *v) {
    g_has_data = true;
    g_sweeps++;

    for (int x = 0; x < SCREEN_W; x++) {
        if (g_col[x] > g_peak[x]) g_peak[x] = g_col[x];
        float p = to_lin(g_col[x]);
        g_avg_lin[x] = (g_avg_lin[x] < 0.0f) ? p : (g_avg_lin[x] * 0.75f + p * 0.25f);
        g_shown[x] = to_code(g_avg_lin[x]);
        g_long_sum[x] += p;
        g_row_sum[x] += p;

        int bin = (int)floorf(db_of(g_col[x]) + (float)PAD_DB);
        if (bin < 0) bin = 0;
        if (bin >= LEV) bin = LEV - 1;
        g_dens[x][bin]++;
    }
    g_long_n++;
    g_row_n++;

    /* Update channel busy stats */
    int idx = 0;
    for (unsigned ch = v->ch_first; ch <= v->ch_last && idx < MAX_CH; ch += v->ch_step, idx++) {
        float c = (float)(v->ch_base_mhz + 5 * ch);
        int x0 = mhz_to_x(v, c - 10.0f);
        int x1 = mhz_to_x(v, c + 10.0f);
        if (x0 < 0 || x1 > SCREEN_W) { g_ch_busy[idx] = -1; continue; }
        for (int x = x0; x < x1; x++) {
            g_ch_total[idx]++;
            if (db_of(g_col[x]) >= (float)BUSY_DB) g_ch_hot[idx]++;
        }
        if (g_ch_total[idx] > 0) {
            g_ch_busy[idx] = (int8_t)((g_ch_hot[idx] * 100ULL) / g_ch_total[idx]);
        }
    }

    /* Waterfall row every 500 ms */
    uint64_t t = now_ms();
    if (t - g_row_t0_ms >= 500 && g_row_n > 0) {
        memmove(&g_wf[1][0], &g_wf[0][0], (WF_H - 1) * SCREEN_W);
        for (int x = 0; x < SCREEN_W; x++) {
            g_wf[0][x] = code_to_heat(to_code(g_row_sum[x] / (float)g_row_n));
            g_row_sum[x] = 0.0f;
        }
        g_row_n = 0;
        g_row_t0_ms = t;
        if (g_wf_rows < WF_H) g_wf_rows++;
    }
}

/* ─── Synthetic Demo Spectrum Generator (when no USB board or in Demo mode) ─ */

static uint32_t s_rng = 0xC5A32026u;
static float rand_u01(void) {
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (float)(s_rng & 0xFFFFFF) / (float)0x1000000;
}

static void generate_demo_segment(unsigned centre, uint8_t *codes) {
    uint64_t t = now_ms();
    if (centre < 3000) {
        /* Smooth packet duty-cycle per capture */
        bool burst1  = rand_u01() < 0.65f;
        bool burst6  = rand_u01() < 0.90f;
        bool burst11 = rand_u01() < 0.08f;
        float lvl1   = 17.0f + rand_u01() * 5.0f;
        float lvl6   = 25.0f + rand_u01() * 6.0f;
        float lvl11  = 9.0f  + rand_u01() * 3.0f;
        for (int shifted = 0; shifted < FFT_N; shifted++) {
            float off_mhz = ((float)shifted - (float)(FFT_N / 2)) * (80.0f / (float)FFT_N);
            float f_mhz = (float)centre + off_mhz;
            float db = (rand_u01() + rand_u01() - 1.0f) * 2.0f;

            float d1 = fabsf(f_mhz - 2412.0f);
            if (burst1 && d1 < 10.5f) {
                float shape = d1 < 8.5f ? 1.0f : (10.5f - d1) / 2.0f;
                db += shape * (lvl1 + (rand_u01() - 0.5f) * 3.0f);
            }
            float d6 = fabsf(f_mhz - 2437.0f);
            if (burst6 && d6 < 10.5f) {
                float shape = d6 < 8.5f ? 1.0f : (10.5f - d6) / 2.0f;
                db += shape * (lvl6 + (rand_u01() - 0.5f) * 3.0f);
            }
            float d11 = fabsf(f_mhz - 2462.0f);
            if (burst11 && d11 < 10.5f) {
                float shape = d11 < 8.5f ? 1.0f : (10.5f - d11) / 2.0f;
                db += shape * (lvl11 + (rand_u01() - 0.5f) * 2.5f);
            }
            if (g_zoom_ch == 9) {
                float mw = fabsf(f_mhz - (2450.0f + sinf((float)t * 0.006f) * 4.0f));
                if (mw < 6.0f) db += (6.0f - mw) * 5.2f;
            }
            int code = FLOOR_CODE + (int)(db * 2.0f);
            if (code < 40) code = 40;
            if (code > 250) code = 250;
            int nat = (shifted + FFT_N / 2) % FFT_N;
            codes[nat] = (uint8_t)code;
        }
        return;
    }

    for (int i = 0; i < FFT_N; i++) {
        int shifted = (i + FFT_N / 2) % FFT_N;
        float off_mhz = ((float)shifted - (float)(FFT_N / 2)) * (80.0f / (float)FFT_N);
        float f_mhz = (float)centre + off_mhz;
        float db = (rand_u01() + rand_u01() - 1.0f) * 2.2f;

        if (centre < 5400) {
            /* 5 GHz UNII-1: 40 MHz wide Wi-Fi 6 on Ch 36+40 (5180-5200 MHz) */
            float d38 = fabsf(f_mhz - 5190.0f);
            if (d38 < 20.0f) {
                float shape = d38 < 17.5f ? 1.0f : (20.0f - d38) / 2.5f;
                if (rand_u01() < 0.70f) db += shape * (22.0f + rand_u01() * 6.0f);
            }
            float d48 = fabsf(f_mhz - 5240.0f);
            if (d48 < 11.0f && rand_u01() < 0.30f) {
                db += (1.0f - d48 / 11.0f) * 16.0f;
            }
        } else if (centre < 5732) {
            /* 5 GHz DFS: Ch 100 (5500 MHz) */
            float d100 = fabsf(f_mhz - 5500.0f);
            if (d100 < 11.0f && rand_u01() < 0.45f) {
                db += (1.0f - d100 / 11.0f) * 19.0f;
            }
        } else {
            /* 5 GHz UNII-3: Ch 149 (5745 MHz) & Ch 153 (5765 MHz) */
            float d149 = fabsf(f_mhz - 5745.0f);
            if (d149 < 11.0f && rand_u01() < 0.75f) {
                float shape = d149 < 8.5f ? 1.0f : (11.0f - d149) / 2.5f;
                db += shape * (21.0f + rand_u01() * 6.0f);
            }
        }

        int code = FLOOR_CODE + (int)(db * 2.0f);
        if (code < 40) code = 40;
        if (code > 250) code = 250;
        /* generate_demo_segment outputs in natural FFT order (bin 0 at centre) */
        int nat = (shifted + FFT_N / 2) % FFT_N;
        codes[nat] = (uint8_t)code;
    }
}

/* ─── USB Serial Port Auto-Discovery & ESP-SDR Protocol ──────────────────── */

static int open_serial_port(const char *path) {
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;

    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) {
        close(fd);
        return -1;
    }
    cfmakeraw(&tio);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~CRTSCTS;
    tio.c_cflag &= ~CSTOPB;
    tio.c_cflag &= ~PARENB;
    tio.c_cflag &= ~CSIZE;
    tio.c_cflag |= CS8;

#ifdef __APPLE__
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);
    tcsetattr(fd, TCSANOW, &tio);
    speed_t speed = 2000000;
    ioctl(fd, IOSSIOSPEED, &speed);
#else
#ifdef B2000000
    cfsetispeed(&tio, B2000000);
    cfsetospeed(&tio, B2000000);
#else
    cfsetispeed(&tio, B921600);
    cfsetospeed(&tio, B921600);
#endif
    tcsetattr(fd, TCSANOW, &tio);
#endif

    /* Release DTR & RTS so we don't hold ESP32-C5 in reset */
    int status = 0;
    if (ioctl(fd, TIOCMGET, &status) == 0) {
        status &= ~(TIOCM_DTR | TIOCM_RTS);
        ioctl(fd, TIOCMSET, &status);
    }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

static bool find_esp_port(char *out_path, size_t cap) {
    const char *env_port = getenv("ESP_PORT");
    if (env_port && *env_port) {
        snprintf(out_path, cap, "%s", env_port);
        return true;
    }
    DIR *d = opendir("/dev");
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "ttyACM", 6) == 0 ||
            strncmp(e->d_name, "ttyUSB", 6) == 0 ||
            strncmp(e->d_name, "cu.usbmodem", 11) == 0 ||
            strncmp(e->d_name, "cu.usbserial", 12) == 0) {
            snprintf(out_path, cap, "/dev/%s", e->d_name);
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

typedef struct {
    int fd;
    uint8_t buf[65536];
    size_t len;
} serial_conn_t;

static bool serial_write_line(int fd, const char *cmd) {
    char line[128];
    int n = snprintf(line, sizeof(line), "%s\n", cmd);
    ssize_t w = write(fd, line, (size_t)n);
    return w == n;
}

static bool serial_fill(serial_conn_t *c, int timeout_ms) {
    if (timeout_ms < 0) timeout_ms = 0;
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(c->fd, &rfds);
    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    int r = select(c->fd + 1, &rfds, NULL, NULL, &tv);
    if (r <= 0) return false;
    if (c->len >= sizeof(c->buf)) c->len = 0;
    ssize_t nr = read(c->fd, c->buf + c->len, sizeof(c->buf) - c->len);
    if (nr <= 0) return false;
    c->len += (size_t)nr;
    return true;
}

static bool serial_read_line(serial_conn_t *c, char *out, size_t cap, int timeout_ms) {
    uint64_t deadline = now_ms() + (uint64_t)timeout_ms;
    for (;;) {
        for (size_t i = 0; i < c->len; i++) {
            if (c->buf[i] == '\n') {
                size_t copy_len = i < cap - 1 ? i : cap - 1;
                memcpy(out, c->buf, copy_len);
                out[copy_len] = '\0';
                if (copy_len > 0 && out[copy_len - 1] == '\r') out[copy_len - 1] = '\0';
                memmove(c->buf, c->buf + i + 1, c->len - (i + 1));
                c->len -= (i + 1);
                return true;
            }
        }
        int64_t rem = (int64_t)deadline - (int64_t)now_ms();
        if (rem <= 0) return false;
        if (!serial_fill(c, (int)rem)) {
            if ((int64_t)deadline - (int64_t)now_ms() <= 0) return false;
        }
    }
}

static bool serial_sync(serial_conn_t *c, int total_ms) {
    char nonce[32], expected[48], line[256];
    snprintf(nonce, sizeof(nonce), "%u", (unsigned)(now_ms() % 1000000000ULL));
    snprintf(expected, sizeof(expected), "SYNC %s", nonce);
    uint64_t end = now_ms() + (uint64_t)total_ms;
    while (now_ms() < end) {
        if (!serial_write_line(c->fd, expected)) return false;
        uint64_t round_end = now_ms() + 800;
        if (round_end > end) round_end = end;
        while (now_ms() < round_end) {
            if (!serial_read_line(c, line, sizeof(line), (int)(round_end - now_ms()))) break;
            if (strcmp(line, expected) == 0) return true;
        }
    }
    return false;
}

static int index_of_spc1(const uint8_t *b, size_t n) {
    if (n < 4) return -1;
    for (size_t i = 0; i + 3 < n; i++) {
        if (b[i] == 'S' && b[i + 1] == 'P' && b[i + 2] == 'C' && b[i + 3] == '1') return (int)i;
    }
    return -1;
}

static bool serial_capture_segment(serial_conn_t *c, unsigned centre, uint8_t *out_codes) {
    char cmd[64], line[256];
    snprintf(cmd, sizeof(cmd), "FREQ %u", centre);
    if (!serial_write_line(c->fd, cmd)) return false;
    if (!serial_read_line(c, line, sizeof(line), 2000) || strncmp(line, "OK", 2) != 0) return false;

    snprintf(cmd, sizeof(cmd), "SPEC 50 1 1 0 0 %d", FFT_N);
    if (!serial_write_line(c->fd, cmd)) return false;
    if (!serial_read_line(c, line, sizeof(line), 2000) || strncmp(line, "SPEC ", 5) != 0) return false;

    double sum[FFT_N];
    memset(sum, 0, sizeof(sum));
    int frames = 0;
    uint64_t deadline = now_ms() + 2500;

    for (;;) {
        while (c->len < 4) {
            int64_t rem = (int64_t)deadline - (int64_t)now_ms();
            if (rem <= 0 || !serial_fill(c, (int)rem)) return false;
        }
        if (memcmp(c->buf, "SPC1", 4) == 0) {
            const size_t frame_sz = 28 + FFT_N + 4;
            while (c->len < frame_sz) {
                int64_t rem = (int64_t)deadline - (int64_t)now_ms();
                if (rem <= 0 || !serial_fill(c, (int)rem)) return false;
            }
            const uint8_t *codes = c->buf + 28;
            for (int i = 0; i < FFT_N; i++) {
                sum[i] += pow(10.0, (double)codes[i] / 20.0);
            }
            memmove(c->buf, c->buf + frame_sz, c->len - frame_sz);
            c->len -= frame_sz;
            frames++;
        } else {
            int sp = index_of_spc1(c->buf, c->len);
            int nl = -1;
            for (size_t i = 0; i < c->len; i++) {
                if (c->buf[i] == '\n') { nl = (int)i; break; }
            }
            if (nl >= 0 && (sp < 0 || nl < sp)) {
                if (!serial_read_line(c, line, sizeof(line), 500)) return false;
                if (strncmp(line, "SPECEND", 7) == 0) break;
            } else if (sp > 0) {
                memmove(c->buf, c->buf + sp, c->len - (size_t)sp);
                c->len -= (size_t)sp;
            } else {
                int64_t rem = (int64_t)deadline - (int64_t)now_ms();
                if (rem <= 0 || !serial_fill(c, (int)rem)) return false;
            }
        }
    }
    if (frames == 0) return false;
    for (int i = 0; i < FFT_N; i++) {
        double v = 20.0 * log10(sum[i] / (double)frames);
        int code = (int)(v + 0.5);
        out_codes[i] = (uint8_t)(code < 0 ? 0 : code > 255 ? 255 : code);
    }
    return true;
}

static void *worker_thread(void *arg) {
    (void)arg;
    serial_conn_t conn = { .fd = -1, .len = 0 };
    uint8_t codes[FFT_N];

    while (g_running) {
        pthread_mutex_lock(&g_lock);
        bool force_demo = g_force_demo;
        bool demo_allowed = g_demo_allowed;
        bool paused = g_paused;
        band_t v = current_view_band();
        uint32_t gen = g_generation;
        pthread_mutex_unlock(&g_lock);

        if (paused) {
            usleep(80000);
            continue;
        }

        if (force_demo) {
            if (conn.fd >= 0) { close(conn.fd); conn.fd = -1; conn.len = 0; }
            pthread_mutex_lock(&g_lock);
            g_link = LINK_DEMO;
            snprintf(g_board_name, sizeof(g_board_name), "DEMO SIMULATOR");
            pthread_mutex_unlock(&g_lock);

            unsigned span = v.end - v.start;
            unsigned segs = (span + SEG_MHZ - 1) / SEG_MHZ;
            for (unsigned k = 0; k < segs && g_running; k++) {
                unsigned centre = v.start + SEG_MHZ / 2 + k * SEG_MHZ;
                generate_demo_segment(centre, codes);
                usleep(28000);
                pthread_mutex_lock(&g_lock);
                if (gen == g_generation) fold_segment_locked(&v, k, centre, codes);
                pthread_mutex_unlock(&g_lock);
            }
            pthread_mutex_lock(&g_lock);
            if (gen == g_generation) finish_sweep_locked(&v);
            pthread_mutex_unlock(&g_lock);
            continue;
        }

        /* Try hardware USB serial connection */
        if (conn.fd < 0) {
            char port_path[64];
            if (find_esp_port(port_path, sizeof(port_path))) {
                pthread_mutex_lock(&g_lock);
                g_link = LINK_CONNECTING;
                snprintf(g_port_name, sizeof(g_port_name), "%s", port_path);
                pthread_mutex_unlock(&g_lock);

                int fd = open_serial_port(port_path);
                if (fd >= 0) {
                    conn.fd = fd;
                    conn.len = 0;
                    if (!serial_sync(&conn, 2500)) {
                        /* Pulse RTS reset into program */
                        int st = TIOCM_RTS;
                        ioctl(conn.fd, TIOCMSET, &st);
                        usleep(120000);
                        st = 0;
                        ioctl(conn.fd, TIOCMSET, &st);
                        usleep(1200000);
                        conn.len = 0;
                    }
                    if (serial_sync(&conn, 3000)) {
                        char line[256];
                        serial_write_line(conn.fd, "INFO");
                        serial_read_line(&conn, line, sizeof(line), 2000);

                        char board[64] = "ESP32-C5 SDR";
                        serial_write_line(conn.fd, "ID?");
                        if (serial_read_line(&conn, line, sizeof(line), 1200) && strncmp(line, "ID ", 3) == 0 && strlen(line) > 21) {
                            snprintf(board, sizeof(board), "%s", line + 21);
                        } else {
                            serial_sync(&conn, 2000);
                        }
                        const char *init_cmds[] = { "BANDWIDTH 0", "DC 1", "GAIN HARDWARE" };
                        for (int i = 0; i < 3; i++) {
                            serial_write_line(conn.fd, init_cmds[i]);
                            serial_read_line(&conn, line, sizeof(line), 1500);
                        }
                        pthread_mutex_lock(&g_lock);
                        g_link = LINK_CONNECTED;
                        snprintf(g_board_name, sizeof(g_board_name), "%s", board);
                        reset_measurements_locked();
                        show_toast_locked(board, 3500);
                        pthread_mutex_unlock(&g_lock);
                    } else {
                        close(conn.fd);
                        conn.fd = -1;
                    }
                }
            }

            if (conn.fd < 0) {
                pthread_mutex_lock(&g_lock);
                if (demo_allowed) {
                    g_link = LINK_DEMO;
                    snprintf(g_board_name, sizeof(g_board_name), "DEMO (NO USB)");
                } else {
                    g_link = LINK_SEARCHING;
                }
                pthread_mutex_unlock(&g_lock);

                if (demo_allowed) {
                    unsigned span = v.end - v.start;
                    unsigned segs = (span + SEG_MHZ - 1) / SEG_MHZ;
                    for (unsigned k = 0; k < segs && g_running; k++) {
                        unsigned centre = v.start + SEG_MHZ / 2 + k * SEG_MHZ;
                        generate_demo_segment(centre, codes);
                        usleep(28000);
                        pthread_mutex_lock(&g_lock);
                        if (gen == g_generation) fold_segment_locked(&v, k, centre, codes);
                        pthread_mutex_unlock(&g_lock);
                    }
                    pthread_mutex_lock(&g_lock);
                    if (gen == g_generation) finish_sweep_locked(&v);
                    pthread_mutex_unlock(&g_lock);
                } else {
                    usleep(400000);
                }
                continue;
            }
        }

        /* Connected to real ESP32-C5 board: sweep segments */
        unsigned span = v.end - v.start;
        unsigned segs = (span + SEG_MHZ - 1) / SEG_MHZ;
        bool ok = true;
        for (unsigned k = 0; k < segs && g_running; k++) {
            pthread_mutex_lock(&g_lock);
            bool aborted = (gen != g_generation) || g_paused || g_force_demo;
            pthread_mutex_unlock(&g_lock);
            if (aborted) { ok = false; break; }

            unsigned centre = v.start + SEG_MHZ / 2 + k * SEG_MHZ;
            if (!serial_capture_segment(&conn, centre, codes)) {
                /* Try one resync */
                serial_write_line(conn.fd, "");
                usleep(150000);
                conn.len = 0;
                if (!serial_sync(&conn, 2000) || !serial_capture_segment(&conn, centre, codes)) {
                    close(conn.fd);
                    conn.fd = -1;
                    pthread_mutex_lock(&g_lock);
                    g_link = LINK_SEARCHING;
                    show_toast_locked("USB board disconnected", 3000);
                    pthread_mutex_unlock(&g_lock);
                    ok = false;
                    break;
                }
            }
            pthread_mutex_lock(&g_lock);
            if (gen == g_generation) fold_segment_locked(&v, k, centre, codes);
            pthread_mutex_unlock(&g_lock);
        }
        if (ok) {
            pthread_mutex_lock(&g_lock);
            if (gen == g_generation) finish_sweep_locked(&v);
            pthread_mutex_unlock(&g_lock);
        }
    }

    if (conn.fd >= 0) close(conn.fd);
    return NULL;
}

/* ─── Screen Rendering (320 x 170) ───────────────────────────────────────── */

static void draw_channel_overlay(const band_t *v, int top_y, int h) {
    const uint32_t pal[4] = {
        rgb(0x60, 0xA5, 0xFA),
        rgb(0x34, 0xD3, 0x99),
        rgb(0xA7, 0x8B, 0xFA),
        rgb(0xFB, 0xBF, 0x24)
    };
    int ov_top = top_y + 10;
    int floor_y = top_y + h - 1;
    int idx = 0;
    for (unsigned ch = v->ch_first; ch <= v->ch_last; ch += v->ch_step) {
        if (v->ch_base_mhz < 3000 && ch != 1 && ch != 6 && ch != 11) continue;
        float c = (float)(v->ch_base_mhz + 5 * ch);
        int x0 = mhz_to_x(v, c - 11.0f);
        int xa = mhz_to_x(v, c - 9.0f);
        int xb = mhz_to_x(v, c + 9.0f);
        int x1 = mhz_to_x(v, c + 11.0f);
        uint32_t col = pal[idx % 4];
        idx++;

        draw_line(x0, floor_y, xa, ov_top, col);
        draw_line(xa, ov_top, xb, ov_top, col);
        draw_line(xb, ov_top, x1, floor_y, col);

        char tag[8];
        snprintf(tag, sizeof(tag), "%u", ch);
        int tw = aa_width(tag, &eng_font_label) + 4;
        int cx = (xa + xb) / 2;
        if (cx - tw / 2 >= 0 && cx + tw / 2 < SCREEN_W) {
            fill_rect(cx - tw / 2, ov_top - 6, tw, 12, rgb(0, 0, 0));
            draw_rect_outline(cx - tw / 2, ov_top - 6, tw, 12, col);
            aa_text(cx - tw / 2 + 2, ov_top - 6, tag, &eng_font_label, col);
        }
    }
}

static void draw_bottom_strip(const band_t *v) {
    uint32_t c_mark  = rgb(167, 139, 250);
    uint32_t c_muted = rgb(160, 165, 190);
    uint32_t c_text  = rgb(245, 247, 250);

    int pitch = mhz_to_x(v, (float)(v->ch_base_mhz + 5 * v->ch_step)) - mhz_to_x(v, (float)v->ch_base_mhz);
    int bw = pitch - 2 < 4 ? 4 : pitch - 2 > 18 ? 18 : pitch - 2;

    unsigned i = 0;
    for (unsigned ch = v->ch_first; ch <= v->ch_last && i < MAX_CH; ch += v->ch_step, i++) {
        int x = mhz_to_x(v, (float)(v->ch_base_mhz + 5 * ch));
        if (x < 0 || x >= SCREEN_W) continue;
        /* Tick mark */
        fill_rect(x, MARK_Y, 1, 3, c_mark);
        /* Busy scorecard color pill at bottom */
        fill_rect(x - bw / 2, SCORE_Y + 4, bw, 4, busy_color(g_ch_busy[i]));

        int step_div = (int)(v->ch_label_every / v->ch_step);
        if (step_div < 1) step_div = 1;
        if ((int)i % step_div == 0) {
            char lbl[12];
            snprintf(lbl, sizeof(lbl), "%u", ch);
            int lw = aa_width(lbl, &eng_font_label);
            if (x - lw / 2 >= 2 && x + lw / 2 <= SCREEN_W - 2) {
                aa_text(x - lw / 2, MARK_Y + 2, lbl, &eng_font_label, c_muted);
            }
        }
    }

    /* Quietest channel badge on bottom right */
    int pct = 0;
    int best = best_quiet_channel(v, &pct);
    if (best > 0) {
        char qbuf[40];
        snprintf(qbuf, sizeof(qbuf), "Best ch %d \xC2\xB7 %d%%", best, pct);
        int qw = aa_width(qbuf, &eng_font_label) + 6;
        fill_rect(SCREEN_W - qw - 2, MARK_Y + 1, qw, 13, rgb(12, 18, 28));
        draw_rect_outline(SCREEN_W - qw - 2, MARK_Y + 1, qw, 13, busy_color(pct));
        aa_text(SCREEN_W - qw + 1, MARK_Y + 1, qbuf, &eng_font_label, c_text);
    }
}

static void render_live_mode(const band_t *v) {
    uint32_t c_grid = rgb(40, 44, 68);
    uint32_t c_peak = rgb(180, 185, 200);
    uint32_t c_avg  = rgb(196, 181, 253);
    uint32_t c_muted = rgb(150, 155, 180);

    int col_y[SCREEN_W], peak_y[SCREEN_W], avg_y[SCREEN_W];
    for (int x = 0; x < SCREEN_W; x++) {
        col_y[x]  = code_to_y(g_shown[x], TRACE_H);
        peak_y[x] = code_to_y(g_peak[x], TRACE_H);
        uint8_t av = g_long_n ? to_code((float)(g_long_sum[x] / (double)g_long_n)) : FLOOR_CODE;
        avg_y[x]  = code_to_y(av, TRACE_H);
    }

    /* Draw Trace */
    for (int ty = 0; ty < TRACE_H; ty++) {
        int y = TRACE_Y + ty;
        bool grid = false;
        for (int db = 0; db <= RANGE_DB; db += 12) {
            if (ty == code_to_y((float)(FLOOR_CODE + db * 2), TRACE_H)) grid = true;
        }
        uint32_t fill_c = trace_fill_color(ty, TRACE_H);
        uint32_t edge_c = trace_edge_color(ty, TRACE_H);
        for (int x = 0; x < SCREEN_W; x++) {
            uint32_t c = (grid && (x & 3) == 0) ? c_grid : rgb(0, 0, 0);
            if (g_has_data) {
                if (ty > col_y[x]) c = fill_c;
                if (ty == col_y[x] || (x && ty < col_y[x] && ty >= col_y[x - 1])) c = edge_c;
                if (g_avg_on && g_long_n > 0) {
                    int a = avg_y[x], b = x ? avg_y[x - 1] : avg_y[x];
                    int lo = a < b ? a : b, hi = a < b ? b : a;
                    if (ty >= lo && ty <= hi) c = c_avg;
                }
                if (g_peak_on && ty < col_y[x]) {
                    int a = peak_y[x], b = x ? peak_y[x - 1] : peak_y[x];
                    int lo = a < b ? a : b, hi = a < b ? b : a;
                    if (ty >= lo && ty <= hi) c = c_peak;
                }
            }
            put_px(x, y, c);
        }
    }

    if (g_overlay) draw_channel_overlay(v, TRACE_Y, TRACE_H);

    /* dB labels on left edge */
    for (int db = 12; db <= 36; db += 12) {
        char b[12];
        snprintf(b, sizeof(b), "+%d", db);
        int top = TRACE_Y + code_to_y((float)(FLOOR_CODE + db * 2), TRACE_H) - 11;
        int w = aa_width(b, &eng_font_label) + 4;
        fill_rect(1, top, w, 11, rgb(0, 0, 0));
        aa_text(3, top, b, &eng_font_label, c_muted);
    }

    /* Separator line */
    fill_rect(0, WF_Y - 1, SCREEN_W, 1, rgb(30, 35, 55));

    /* Draw Waterfall */
    for (int r = 0; r < WF_H; r++) {
        int y = WF_Y + r;
        for (int x = 0; x < SCREEN_W; x++) {
            put_px(x, y, heat_color(g_wf[r][x]));
        }
    }

    /* Cursor marker line */
    if (g_cursor_x >= 0 && g_cursor_x < SCREEN_W) {
        for (int y = TRACE_Y; y < WF_Y + WF_H; y++) {
            if (y & 2) put_px(g_cursor_x, y, rgb(255, 255, 255));
        }
    }

    draw_bottom_strip(v);
}

static void render_density_mode(const band_t *v) {
    uint32_t c_grid  = rgb(40, 44, 68);
    uint32_t c_muted = rgb(150, 155, 180);

    for (int r = 0; r < DENS_H; r++) {
        int y = DENS_Y + r;
        int bin = LEV - 1 - (r * LEV / DENS_H);
        if (bin < 0) bin = 0;
        if (bin >= LEV) bin = LEV - 1;
        for (int x = 0; x < SCREEN_W; x++) {
            float pr = g_sweeps > 0 ? (float)g_dens[x][bin] / (float)g_sweeps : 0.0f;
            put_px(x, y, dens_color(pr));
        }
    }

    for (int db = 12; db <= 36; db += 12) {
        int y = DENS_Y + DENS_H - 1 - ((db + PAD_DB) * DENS_H / LEV);
        for (int x = 0; x < SCREEN_W; x += 4) put_px(x, y, c_grid);
        char b[16];
        snprintf(b, sizeof(b), "+%d dB", db);
        int w = aa_width(b, &eng_font_label) + 4;
        fill_rect(2, y - 11, w, 11, rgb(0, 0, 0));
        aa_text(4, y - 11, b, &eng_font_label, c_muted);
    }

    if (g_cursor_x >= 0 && g_cursor_x < SCREEN_W) {
        for (int y = DENS_Y; y < DENS_Y + DENS_H; y++) {
            if (y & 2) put_px(g_cursor_x, y, rgb(255, 255, 255));
        }
    }

    draw_bottom_strip(v);
}

static void render_channels_mode(const band_t *v) {
    uint32_t c_grid  = rgb(40, 44, 68);
    uint32_t c_muted = rgb(150, 155, 180);
    uint32_t c_text  = rgb(245, 247, 250);

    int top = 32, base = 136;
    for (int pct = 25; pct <= 100; pct += 25) {
        int y = base - (base - top) * pct / 100;
        for (int x = 28; x < SCREEN_W - 6; x += 4) put_px(x, y, c_grid);
        char b[12];
        snprintf(b, sizeof(b), "%d%%", pct);
        aa_text(4, y - 6, b, &eng_font_label, c_muted);
    }

    int count = 0;
    for (unsigned ch = v->ch_first; ch <= v->ch_last && count < MAX_CH; ch += v->ch_step) count++;
    if (count > 0) {
        float bw = (float)(SCREEN_W - 36) / (float)count;
        int i = 0;
        for (unsigned ch = v->ch_first; ch <= v->ch_last && i < count; ch += v->ch_step, i++) {
            int x = 32 + (int)(i * bw);
            int w = (int)bw - 4;
            if (w < 4) w = 4;
            int pct = g_ch_busy[i];
            int bh = pct < 0 ? 0 : (base - top) * pct / 100;
            uint32_t col = busy_color(pct);
            fill_rect(x + 2, base - bh, w, bh, col);

            char pstr[12];
            if (pct < 0) snprintf(pstr, sizeof(pstr), "-");
            else         snprintf(pstr, sizeof(pstr), "%d%%", pct);
            int pw = aa_width(pstr, &eng_font_label);
            if (bw >= 18.0f || (i % 2 == 0)) {
                aa_text(x + 2 + (w - pw) / 2, base - bh - 12, pstr, &eng_font_label, c_text);
            }

            char cstr[12];
            snprintf(cstr, sizeof(cstr), "%u", ch);
            int cw = aa_width(cstr, &eng_font_label);
            aa_text(x + 2 + (w - cw) / 2, base + 3, cstr, &eng_font_label, c_muted);
        }
    }

    int qpct = 0;
    int best = best_quiet_channel(v, &qpct);
    char footer[80];
    if (best > 0) {
        snprintf(footer, sizeof(footer), "Quietest channel: ch %d (%d%% busy above +6 dB)", best, qpct);
    } else {
        snprintf(footer, sizeof(footer), "Measuring channel activity (+6 dB above noise floor)...");
    }
    aa_text(8, 152, footer, &eng_font_text, c_text);
}

static void render_waiting_overlay(void) {
    fill_rect(18, 28, SCREEN_W - 36, 114, rgb(12, 16, 28));
    draw_rect_outline(18, 28, SCREEN_W - 36, 114, rgb(59, 130, 246));

    draw_sprite(28, 38, LOGO36_W, LOGO36_H, LOGO36_PX);

    const char *t1 = "Plug in ESP32-C5 Board";
    const char *t2 = "Side USB port (/dev/ttyACM* / ttyUSB*)";
    const char *t3 = "Press D for Demo Mode  \xC2\xB7  H for Controls";

    aa_text(72, 38, t1, &eng_font_title, rgb(255, 255, 255));
    aa_text(72, 62, t2, &eng_font_label, rgb(160, 175, 205));
    aa_text((SCREEN_W - aa_width(t3, &eng_font_text)) / 2, 108, t3, &eng_font_text, rgb(52, 211, 153));
}

static void render_help_modal(void) {
    fill_rect(12, 16, SCREEN_W - 24, SCREEN_H - 28, rgb(10, 14, 24));
    draw_rect_outline(12, 16, SCREEN_W - 24, SCREEN_H - 28, rgb(167, 139, 250));

    aa_text(22, 21, "CardputerZero Spectrum Controls", &eng_font_text, rgb(255, 255, 255));
    const char *lines[] = {
        "1-4 / Tab : Switch 2.4 GHz & 5 GHz bands",
        "V         : Cycle Live / Density / Channels view",
        "Left/Right: Move frequency cursor readout",
        "Z / Enter : Zoom 40 MHz around cursor channel",
        "P / A / O : Toggle Peak Hold / Average / Ch Overlay",
        "Space / C : Freeze trace / Clear history",
        "D / H / Q : Toggle Demo Mode / Help / Quit"
    };
    for (int i = 0; i < 7; i++) {
        aa_text(22, 41 + i * 16, lines[i], &eng_font_label, rgb(200, 205, 225));
    }
}

static void render_frame(void) {
    pthread_mutex_lock(&g_lock);
    band_t v = current_view_band();

    /* Clear screen */
    fill_rect(0, 0, SCREEN_W, SCREEN_H, rgb(0, 0, 0));

    /* Top Header Bar (20 px) with official logo + status dot */
    fill_rect(0, 0, SCREEN_W, HDR_H - 1, rgb(10, 14, 24));
    fill_rect(0, HDR_H - 1, SCREEN_W, 1, rgb(35, 42, 65));

    draw_sprite(3, 2, LOGO16_W, LOGO16_H, LOGO16_PX);
    uint32_t dot_col = g_link == LINK_CONNECTED ? rgb(0x22, 0xC5, 0x5E) :
                       g_link == LINK_DEMO      ? rgb(0xFB, 0xBF, 0x24) :
                                                  rgb(0x60, 0xA5, 0xFA);
    fill_rect(17, 14, 4, 4, dot_col);

    /* Band title */
    char title[48];
    if (g_zoom_ch > 0) snprintf(title, sizeof(title), "%s \xC2\xB7 Ch %d", v.short_name, g_zoom_ch);
    else               snprintf(title, sizeof(title), "%s", v.name);
    aa_text(24, 1, title, &eng_font_title, rgb(255, 255, 255));

    /* Center / Right Header Info: Cursor readout OR range + mode badges */
    int left_end = 24 + aa_width(title, &eng_font_title) + 8;
    char right_buf[64];
    const char *mode_tag = g_mode == MODE_LIVE ? "LIVE" : g_mode == MODE_DENSITY ? "DENS" : "CHAN";
    if (g_paused) mode_tag = "FROZEN";
    else if (g_link == LINK_DEMO) mode_tag = "DEMO";

    if (g_cursor_x >= 0 && g_has_data) {
        float mhz = (float)v.start + ((float)g_cursor_x + 0.5f) * (float)(v.end - v.start) / (float)SCREEN_W;
        int db = (int)(db_of(g_shown[g_cursor_x]) + 0.5f);
        if (db < 0) db = 0;
        unsigned ch = nearest_channel(&v, mhz);
        snprintf(right_buf, sizeof(right_buf), "%.0fMHz ch%u +%ddB", mhz, ch, db);
        int rw = aa_width(right_buf, &eng_font_text);
        aa_text(SCREEN_W - rw - 6, 3, right_buf, &eng_font_text, rgb(52, 211, 153));
    } else {
        char range_str[32];
        snprintf(range_str, sizeof(range_str), "%u-%u MHz", v.start, v.end);
        if (left_end + aa_width(range_str, &eng_font_label) < SCREEN_W - 65) {
            aa_text(left_end, 4, range_str, &eng_font_label, rgb(140, 148, 175));
        }
        snprintf(right_buf, sizeof(right_buf), "%s%s",
                 (g_peak_on && g_mode == MODE_LIVE && !g_paused) ? "PEAK \xC2\xB7 " : "",
                 mode_tag);
        int rw = aa_width(right_buf, &eng_font_label);
        aa_text(SCREEN_W - rw - 6, 4, right_buf, &eng_font_label,
                g_paused ? rgb(0xFB, 0xBF, 0x24) : rgb(175, 182, 205));
    }

    /* Main Content Area */
    if (g_mode == MODE_LIVE) {
        render_live_mode(&v);
    } else if (g_mode == MODE_DENSITY) {
        render_density_mode(&v);
    } else {
        render_channels_mode(&v);
    }

    if (!g_has_data && g_link == LINK_SEARCHING) {
        render_waiting_overlay();
    }

    /* Toast message */
    if (g_toast[0] && now_ms() < g_toast_until_ms) {
        int tw = aa_width(g_toast, &eng_font_label) + 12;
        if (tw > SCREEN_W - 12) tw = SCREEN_W - 12;
        int tx = (SCREEN_W - tw) / 2;
        fill_rect(tx, 24, tw, 16, rgb(15, 23, 42));
        draw_rect_outline(tx, 24, tw, 16, rgb(56, 189, 248));
        aa_text(tx + 6, 26, g_toast, &eng_font_label, rgb(240, 249, 255));
    }

    if (g_help_on) {
        render_help_modal();
    }

    pthread_mutex_unlock(&g_lock);
}

/* ─── Input Handling & Easter Eggs ───────────────────────────────────────── */

enum {
    AK_UP = 1001, AK_DOWN, AK_LEFT, AK_RIGHT,
    AK_ESC, AK_TAB, AK_ENTER, AK_SPACE, AK_SLASH
};

static const int KONAMI[10] = {
    AK_UP, AK_UP, AK_DOWN, AK_DOWN,
    AK_LEFT, AK_RIGHT, AK_LEFT, AK_RIGHT,
    'b', 'a'
};
static int s_key_hist[10];
static char s_typed[10] = "";

static void handle_app_key(int k, bool shift) {
    pthread_mutex_lock(&g_lock);

    /* Konami code detector */
    memmove(&s_key_hist[0], &s_key_hist[1], 9 * sizeof(int));
    s_key_hist[9] = k;
    if (memcmp(s_key_hist, KONAMI, sizeof(KONAMI)) == 0) {
        g_phosphor = !g_phosphor;
        show_toast_locked(g_phosphor ? "Phosphor oscilloscope mode" : "Standard colour mode", 3000);
        pthread_mutex_unlock(&g_lock);
        return;
    }

    /* "microwave" easter egg detector */
    if (k >= 'a' && k <= 'z') {
        size_t len = strlen(s_typed);
        if (len >= 9) {
            memmove(s_typed, s_typed + 1, 8);
            s_typed[8] = (char)k;
            s_typed[9] = '\0';
        } else {
            s_typed[len] = (char)k;
            s_typed[len + 1] = '\0';
        }
        if (strcmp(s_typed, "microwave") == 0) {
            s_typed[0] = '\0';
            g_band_idx = 0;
            g_zoom_ch = 9;
            reset_measurements_locked();
            show_toast_locked("Microwave leak band: 2.45 GHz (Ch 9)", 4500);
            pthread_mutex_unlock(&g_lock);
            return;
        }
    }

    if (g_help_on && (k == AK_ESC || k == 'h' || k == AK_ENTER || k == AK_SPACE)) {
        g_help_on = false;
        pthread_mutex_unlock(&g_lock);
        return;
    }

    switch (k) {
    case AK_ESC:
        if (g_cursor_x >= 0) {
            g_cursor_x = -1;
        } else if (g_zoom_ch > 0) {
            g_zoom_ch = 0;
            reset_measurements_locked();
        } else {
            g_running = false;
        }
        break;
    case 'q':
        g_running = false;
        break;
    case '1':
    case '2':
    case '3':
    case '4': {
        unsigned idx = (unsigned)(k - '1');
        if (idx < BAND_COUNT && (g_band_idx != idx || g_zoom_ch != 0)) {
            g_band_idx = idx;
            g_zoom_ch = 0;
            reset_measurements_locked();
        }
        break;
    }
    case AK_TAB:
    case 'n':
    case 'b':
        g_band_idx = (g_band_idx + 1) % BAND_COUNT;
        g_zoom_ch = 0;
        reset_measurements_locked();
        break;
    case 'v':
    case 'm':
        g_mode = (view_mode_t)((g_mode + 1) % 3);
        show_toast_locked(g_mode == MODE_LIVE ? "View: Live + Waterfall" :
                          g_mode == MODE_DENSITY ? "View: Spectral Density" :
                                                   "View: Channel Busy Scorecard", 2000);
        break;
    case 'p':
        g_peak_on = !g_peak_on;
        show_toast_locked(g_peak_on ? "Peak Hold: ON" : "Peak Hold: OFF", 1800);
        break;
    case 'a':
        g_avg_on = !g_avg_on;
        show_toast_locked(g_avg_on ? "Long Average Line: ON" : "Long Average Line: OFF", 1800);
        break;
    case 'o':
        g_overlay = !g_overlay;
        show_toast_locked(g_overlay ? "Wi-Fi Channel Overlay: ON" : "Wi-Fi Channel Overlay: OFF", 1800);
        break;
    case AK_SPACE:
        g_paused = !g_paused;
        show_toast_locked(g_paused ? "Trace Frozen" : "Resumed Live Sweep", 1800);
        break;
    case 'c':
        memset(g_peak, 0, sizeof(g_peak));
        reset_measurements_locked();
        show_toast_locked("Cleared history & peak hold", 1800);
        break;
    case 'd':
        g_force_demo = !g_force_demo;
        g_demo_allowed = g_force_demo;
        reset_measurements_locked();
        show_toast_locked(g_force_demo ? "Demo Simulator: ON" : "USB Hardware Mode", 2200);
        break;
    case 'h':
    case AK_SLASH:
        g_help_on = !g_help_on;
        break;
    case AK_LEFT: {
        int step = shift ? 12 : 3;
        if (g_cursor_x < 0) g_cursor_x = SCREEN_W / 2;
        else g_cursor_x = (g_cursor_x - step < 0) ? 0 : g_cursor_x - step;
        break;
    }
    case AK_RIGHT: {
        int step = shift ? 12 : 3;
        if (g_cursor_x < 0) g_cursor_x = SCREEN_W / 2;
        else g_cursor_x = (g_cursor_x + step >= SCREEN_W) ? SCREEN_W - 1 : g_cursor_x + step;
        break;
    }
    case 'z':
    case AK_ENTER:
        if (g_zoom_ch > 0) {
            g_zoom_ch = 0;
            reset_measurements_locked();
            show_toast_locked("Zoom: Full Band", 1800);
        } else {
            band_t b = current_view_band();
            int cx = g_cursor_x >= 0 ? g_cursor_x : SCREEN_W / 2;
            float mhz = (float)b.start + ((float)cx + 0.5f) * (float)(b.end - b.start) / (float)SCREEN_W;
            g_zoom_ch = (int)nearest_channel(&b, mhz);
            reset_measurements_locked();
            char msg[48];
            snprintf(msg, sizeof(msg), "Zoomed to Channel %d (40 MHz)", g_zoom_ch);
            show_toast_locked(msg, 2000);
        }
        break;
    default:
        break;
    }

    pthread_mutex_unlock(&g_lock);
}

#ifdef USE_SDL
static void save_bmp_to_png(const char *bmp_path) {
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormatFrom(
        g_fb, SCREEN_W, SCREEN_H, 32, SCREEN_W * 4, SDL_PIXELFORMAT_ARGB8888);
    if (surf) {
        SDL_SaveBMP(surf, bmp_path);
        SDL_FreeSurface(surf);
    }
}

static int map_sdl_key(SDL_Keycode k) {
    if (k >= SDLK_a && k <= SDLK_z) return (int)k;
    if (k >= SDLK_0 && k <= SDLK_9) return (int)k;
    switch (k) {
        case SDLK_UP: return AK_UP;
        case SDLK_DOWN: return AK_DOWN;
        case SDLK_LEFT: return AK_LEFT;
        case SDLK_RIGHT: return AK_RIGHT;
        case SDLK_ESCAPE: return AK_ESC;
        case SDLK_TAB: return AK_TAB;
        case SDLK_RETURN: return AK_ENTER;
        case SDLK_SPACE: return AK_SPACE;
        case SDLK_SLASH: return AK_SLASH;
        default: return 0;
    }
}
#else
/* Linux Framebuffer (/dev/fb0) & evdev (/dev/input/event*) backend for CardputerZero */
static int map_evdev_key(uint16_t code) {
    switch (code) {
        case KEY_1: return '1';
        case KEY_2: return '2';
        case KEY_3: return '3';
        case KEY_4: return '4';
        case KEY_A: return 'a';
        case KEY_B: return 'b';
        case KEY_C: return 'c';
        case KEY_D: return 'd';
        case KEY_E: return 'e';
        case KEY_H: return 'h';
        case KEY_I: return 'i';
        case KEY_M: return 'm';
        case KEY_N: return 'n';
        case KEY_O: return 'o';
        case KEY_P: return 'p';
        case KEY_Q: return 'q';
        case KEY_R: return 'r';
        case KEY_V: return 'v';
        case KEY_W: return 'w';
        case KEY_Z: return 'z';
        case KEY_UP: return AK_UP;
        case KEY_DOWN: return AK_DOWN;
        case KEY_LEFT:
        case KEY_COMMA: return AK_LEFT;
        case KEY_RIGHT:
        case KEY_DOT: return AK_RIGHT;
        case KEY_ESC:
        case KEY_BACKSPACE: return AK_ESC;
        case KEY_TAB: return AK_TAB;
        case KEY_ENTER: return AK_ENTER;
        case KEY_SPACE: return AK_SPACE;
        case KEY_SLASH: return AK_SLASH;
        default: return 0;
    }
}

static int open_evdev_fds(int *fds, int max_fds) {
    DIR *d = opendir("/dev/input");
    if (!d) return 0;
    struct dirent *e;
    int count = 0;
    while ((e = readdir(d)) != NULL && count < max_fds) {
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        unsigned long evbit = 0;
        if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), &evbit) >= 0 && (evbit & (1UL << EV_KEY))) {
            fds[count++] = fd;
        } else {
            close(fd);
        }
    }
    closedir(d);
    return count;
}
#endif

int main(int argc, char **argv) {
    const char *screenshot_bmp = NULL;
    int scale = 1;
    bool borderless = true;

#ifdef __APPLE__
    scale = 3;
    borderless = false;
    g_demo_allowed = true;
#endif

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--demo") == 0) {
            g_force_demo = true;
            g_demo_allowed = true;
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            const char *m = argv[++i];
            if (strcmp(m, "density") == 0) g_mode = MODE_DENSITY;
            else if (strcmp(m, "channels") == 0) g_mode = MODE_CHANNELS;
            else g_mode = MODE_LIVE;
        } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            scale = atoi(argv[++i]);
            if (scale < 1) scale = 1;
            if (scale > 6) scale = 6;
            borderless = false;
        } else if (strcmp(argv[i], "--screenshot-bmp") == 0 && i + 1 < argc) {
            screenshot_bmp = argv[++i];
            g_force_demo = true;
            g_demo_allowed = true;
        }
    }

    reset_measurements_locked();

#ifdef USE_SDL
    if (screenshot_bmp) {
        band_t v = current_view_band();
        uint8_t codes[FFT_N];
        g_overlay = true;
        for (int sweep = 0; sweep < WF_H; sweep++) {
            unsigned span = v.end - v.start;
            unsigned segs = (span + SEG_MHZ - 1) / SEG_MHZ;
            for (unsigned k = 0; k < segs; k++) {
                unsigned centre = v.start + SEG_MHZ / 2 + k * SEG_MHZ;
                generate_demo_segment(centre, codes);
                fold_segment_locked(&v, k, centre, codes);
            }
            g_row_t0_ms = 0;
            finish_sweep_locked(&v);
        }
        g_link = LINK_CONNECTED;
        snprintf(g_board_name, sizeof(g_board_name), "Waveshare ESP32-C5");
        render_frame();
        if (SDL_Init(SDL_INIT_VIDEO) == 0) {
            save_bmp_to_png(screenshot_bmp);
            SDL_Quit();
        }
        return 0;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    Uint32 win_flags = borderless ? SDL_WINDOW_BORDERLESS : SDL_WINDOW_SHOWN;
    SDL_Window *win = SDL_CreateWindow(
        "CardputerZero · ESP32-C5 Spectrum Analyser",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SCREEN_W * scale, SCREEN_H * scale, win_flags);
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    SDL_Surface *icon_surf = SDL_CreateRGBSurfaceWithFormatFrom(
        (void *)LOGO36_PX, LOGO36_W, LOGO36_H, 32, LOGO36_W * 4, SDL_PIXELFORMAT_ARGB8888);
    if (icon_surf) {
        SDL_SetWindowIcon(win, icon_surf);
        SDL_FreeSurface(icon_surf);
    }
    SDL_RenderSetLogicalSize(ren, SCREEN_W, SCREEN_H);
    SDL_RaiseWindow(win);

    SDL_Texture *tex = SDL_CreateTexture(
        ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H);

    pthread_t tid;
    pthread_create(&tid, NULL, worker_thread, NULL);

    while (g_running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                g_running = false;
            } else if (ev.type == SDL_KEYDOWN) {
                int ak = map_sdl_key(ev.key.keysym.sym);
                if (ak) handle_app_key(ak, (ev.key.keysym.mod & KMOD_SHIFT) != 0);
            } else if (ev.type == SDL_MOUSEBUTTONDOWN) {
                pthread_mutex_lock(&g_lock);
                int mx = ev.button.x;
                int my = ev.button.y;
                if (my < HDR_H) {
                    g_overlay = !g_overlay;
                } else if (my < MARK_Y) {
                    if (g_cursor_x >= 0 && abs(mx - g_cursor_x) <= 6) g_cursor_x = -1;
                    else if (mx >= 0 && mx < SCREEN_W) g_cursor_x = mx;
                } else {
                    band_t b = current_view_band();
                    float mhz = (float)b.start + ((float)mx + 0.5f) * (float)(b.end - b.start) / (float)SCREEN_W;
                    int ch = (int)nearest_channel(&b, mhz);
                    g_zoom_ch = (g_zoom_ch == ch) ? 0 : ch;
                    reset_measurements_locked();
                }
                pthread_mutex_unlock(&g_lock);
            }
        }

        render_frame();
        SDL_UpdateTexture(tex, NULL, g_fb, SCREEN_W * 4);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
        SDL_Delay(16);
    }

    pthread_join(tid, NULL);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
#else
    (void)screenshot_bmp;
    (void)scale;
    (void)borderless;

    int fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        perror("open /dev/fb0");
        return 1;
    }
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        perror("ioctl fb0");
        close(fb_fd);
        return 1;
    }
    size_t fb_size = (size_t)finfo.line_length * vinfo.yres;
    uint8_t *fb_mem = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED) {
        perror("mmap fb0");
        close(fb_fd);
        return 1;
    }

    int in_fds[16];
    int n_in = open_evdev_fds(in_fds, 16);
    bool shift_down = false;

    pthread_t tid;
    pthread_create(&tid, NULL, worker_thread, NULL);

    while (g_running) {
        for (int i = 0; i < n_in; i++) {
            struct input_event ev;
            while (read(in_fds[i], &ev, sizeof(ev)) == sizeof(ev)) {
                if (ev.type == EV_KEY) {
                    if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT) {
                        shift_down = (ev.value != 0);
                    } else if (ev.value == 1) {
                        int ak = map_evdev_key(ev.code);
                        if (ak) handle_app_key(ak, shift_down);
                    }
                }
            }
        }

        render_frame();

        /* Blit g_fb (ARGB8888) to /dev/fb0 (RGB565 or 32-bit) */
        int max_y = (int)vinfo.yres < SCREEN_H ? (int)vinfo.yres : SCREEN_H;
        int max_x = (int)vinfo.xres < SCREEN_W ? (int)vinfo.xres : SCREEN_W;
        if (vinfo.bits_per_pixel == 16) {
            for (int y = 0; y < max_y; y++) {
                uint16_t *dst = (uint16_t *)(fb_mem + (size_t)y * finfo.line_length);
                const uint32_t *src = &g_fb[y * SCREEN_W];
                for (int x = 0; x < max_x; x++) {
                    uint32_t c = src[x];
                    uint8_t r = (uint8_t)(c >> 16);
                    uint8_t g = (uint8_t)(c >> 8);
                    uint8_t b = (uint8_t)c;
                    dst[x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                }
            }
        } else {
            for (int y = 0; y < max_y; y++) {
                uint32_t *dst = (uint32_t *)(fb_mem + (size_t)y * finfo.line_length);
                memcpy(dst, &g_fb[y * SCREEN_W], (size_t)max_x * 4);
            }
        }

        usleep(25000);
    }

    pthread_join(tid, NULL);
    for (int i = 0; i < n_in; i++) close(in_fds[i]);
    munmap(fb_mem, fb_size);
    close(fb_fd);
    return 0;
#endif
}
