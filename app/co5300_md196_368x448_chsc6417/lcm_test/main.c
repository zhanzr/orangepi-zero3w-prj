/*
  main.c - CO5300 368x448 (MD196) + CHSC6417 touch demo for the Orange
  Pi Zero 3W over hardware SPI3 (/dev/spidev3.0), ported from the
  nucleo-l4r5/nucleo-u575 co5300_md196_368x448_chsc6417 references
  (wrapped-command protocol, vendor verbatim init), demo flow from the
  rk3506 st7365_md350_320x480/lcm_test.

  Wiring (JP4, all verified in the DT overlays - see
  ../../docs/co5300_md196_jp4-pinout.md):
    CS  = JP4.24 (PE0, SPI3_CS0 hw via spidev)   CLK = JP4.23 (PE1)
    MOSI= JP4.19 (PE2)          TOUCH I2C: JP4.5 SCL / JP4.3 SDA
  No DC / RES / BLK: wrapped protocol, POR settle, brightness = DCS 51h.
  The panel is an OLED: much brighter than LCDs - the loop alternates
  brightness 1 % / 15.6 % (the LCD backlight 11 %/55 % analog).

  Text: DejaVu Sans (the board's native Linux font) via FreeType -
  banner, info page, fps readout and logo caption; the hardcoded 8x16
  raster font remains only as a fallback if FreeType cannot start.
  Logo: the Orange Pi logo extracted from the vendor user manual
  (ref/logo_opi.h) - replaces the rk3506 Rockchip boot logo.

  Usage: ./lcm_test [-s speed_kHz] (standard SPI, default speed 40000;
  the CO5300 datasheet specifies a 50 MHz maximum write clock, and
  60 MHz corrupted the checkerboard on the STM32 reference boards).
*/
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <unistd.h>

#include "ref/lcd_fonts.h"
#include "ref/lcd_font_1608.h"
#include "ref/logo_opi.h"
#include "ref/logo_tux.h"
#include "lcd.h"
#include "spi_hw.h"
#include "touch.h"
#include "ft_text.h"

#include "log.h"

/* ---- screen layout ---- */
#define SCREEN_W   368
#define SCREEN_H   448
#define FPS_BAND   24
#define ANIM_H     (SCREEN_H - FPS_BAND)
#define BACK_COLOR LCD_BLACK

/* Requested SPI speed in kHz; actual SCK may differ. The app default
 * remains at 40 MHz until the panel image is verified at higher rates.
 * The CO5300 write clock limit is 50 MHz per its datasheet. */
static unsigned g_speed_khz = 40000;
static volatile sig_atomic_t g_stop_requested;
#define INFO_DY    34      /* 26 px DejaVu line + leading */
#define DWELL_MS   4000    /* per-pattern hold so colors can be judged */

/* ---- FreeType status (DejaVu Sans native-font path) ---- */
static int g_ft;
static int g_logo_tux;   /* -l tux: show the known-good test image */

/* ---- UVC camera probe: is a capture camera present at all ---- */
static int uvc_present(void)
{
    DIR *d = opendir("/sys/class/video4linux");
    struct dirent *e;
    int found = -1;

    if (!d)
        return -1;
    while ((e = readdir(d)) != NULL)
    {
        char dev_path[300];
        int fd;
        struct v4l2_capability cap;

        if (strncmp(e->d_name, "video", 5) != 0)
            continue;
        snprintf(dev_path, sizeof(dev_path), "/dev/%s", e->d_name);
        fd = open(dev_path, O_RDWR);
        if (fd < 0)
            continue;
        memset(&cap, 0, sizeof(cap));
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 &&
            (cap.capabilities & V4L2_CAP_DEVICE_CAPS
                 ? (cap.device_caps & V4L2_CAP_VIDEO_CAPTURE)
                 : (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) &&
            strcmp((char *)cap.driver, "uvcvideo") == 0)
        {
            found = 0;
            close(fd);
            break;
        }
        close(fd);
    }
    closedir(d);
    return found;
}

/* ---- OLED brightness alternation (the backlight PWM analog) ----
 * No BL pin: brightness is DCS 51h. The loop alternates between the
 * l4r5 usable floor (0x01 = 0.78 %) and 15.6 % so the panel can be
 * judged at both ends; the LCD demo's 11 %/55 % PWM cycle maps to
 * this. -b <hex> pins a fixed level. */
static uint8_t g_bright;   /* current 51h value, shown on the info page */
static uint8_t g_bright_fix;   /* nonzero = fixed level (-b) */

static void brightness_init(void)
{
    g_bright = LCD_BRIGHT_LO;
}

static void brightness_set(int loop)
{
    uint8_t v;

    if (g_bright_fix)
        v = g_bright_fix;              /* -b override: no alternation */
    else
        v = (loop % 2) ? LCD_BRIGHT_LO : LCD_BRIGHT_HI;

    if (v == g_bright)
        return;
    g_bright = v;
    LCD_SetBrightness(v);
}

/* ---- touch state ---- */
static int g_touch_ok;
static int g_touch_down;

#define TOUCH_POLL_MS 20

static void request_stop(int signo)
{
    (void)signo;
    g_stop_requested = 1;
}

/* poll + print transitions. Touch_Sample() rejects mismatched I2C
 * reports; longer hysteresis follows the STM32 reference ports. */
static void touch_poll(void)
{
    static uint8_t dn_run, up_run;
    int raw, x, y;

    if (!g_touch_ok)
        return;
    raw = Touch_Sample();
    if (raw < 0)
        return;
    x = Touch_X();
    y = Touch_Y();
    if (raw) { dn_run = (dn_run < 3) ? dn_run + 1 : 3; up_run = 0; }
    else     { up_run = (up_run < 6) ? up_run + 1 : 6; dn_run = 0; }

    if (!g_touch_down && dn_run >= 3)
    {
        g_touch_down = 1;
        log_printf(LOG_LVL_INFO, "[TOUCH] down X=%d Y=%d (%d-Y=%d)",
               x, y, SCREEN_H, SCREEN_H - y);
    }
    else if (g_touch_down && up_run >= 6)
    {
        g_touch_down = 0;
        log_printf(LOG_LVL_INFO, "[TOUCH] release");
    }
}

static void touch_dwell(uint32_t ms)
{
    uint32_t start = LCD_GetTick();

    do
    {
        touch_poll();
        usleep(TOUCH_POLL_MS * 1000);
    } while (!g_stop_requested && LCD_GetTick() - start < ms);
}

/* ---- FPS counter ---- */
static volatile uint32_t g_frames;
static uint32_t g_last_frames;
static uint32_t g_fps_last_tick;
static uint32_t g_fps_color = LCD_WHITE;

static uint16_t rgb565(uint32_t rgb888);

static void fps_reset(void)
{
    g_frames = 0;
    g_last_frames = 0;
    g_fps_last_tick = LCD_GetTick();
}

static void fps_update(void)
{
    uint32_t now = LCD_GetTick();

    if (now - g_fps_last_tick >= 1000)
    {
        uint32_t fps = g_frames - g_last_frames;
        char buf[10];

        g_last_frames = g_frames;
        g_fps_last_tick = now;
        snprintf(buf, sizeof buf, "FPS:%u", fps);
        if (g_ft)
            ft_draw(FT_FACE_REGULAR, 1, (uint16_t)(ANIM_H + 2), 20, buf,
                    rgb565(g_fps_color), BLACK);
        else
        {
            LCD_SetColor(g_fps_color);
            LCD_ShowTransparent(1);
            LCD_DisplayString(1, (uint16_t)(ANIM_H + 4), buf);
            LCD_ShowTransparent(0);
        }
        if (fps)
            log_printf(LOG_LVL_DEBUG, "[fps] %u", fps);
    }
}

static void paint_fps_band(void)
{
    LCD_SetColor(BACK_COLOR);
    LCD_SetBackColor(BACK_COLOR);
    LCD_FillRect(0, (uint16_t)ANIM_H, SCREEN_W, FPS_BAND);
}

/* ---- HSV gradient sweep ---- */
static uint32_t hsv_to_rgb(int h, int s, int v)
{
    int region = (h / 600) % 6;
    int fpart  = h % 600;
    int p = v * (255 - s) / 255;
    int q = v * (255 - (s * fpart) / 600) / 255;
    int t = v * (255 - (s * (600 - fpart)) / 600) / 255;
    int r, g, b;

    switch (region)
    {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default:r = v; g = p; b = q; break;
    }
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

#define G_ROWS 5   /* five staged RGB565 rows */

static void draw_gradient(int hue_a, int hue_b, uint16_t *row)
{
    static uint8_t row_wire[SCREEN_W * 2];
    LCD_SetAddress(0, 0, (uint16_t)(SCREEN_W - 1), (uint16_t)(ANIM_H - 1));
    for (int y0 = 0; y0 < ANIM_H; y0 += G_ROWS)
    {
        int rows = (ANIM_H - y0) < G_ROWS ? (ANIM_H - y0) : G_ROWS;

        for (int k = 0; k < rows; k++)
        {
            int frac = (y0 + k) * 1000 / ANIM_H;
            int hue  = hue_a + (hue_b - hue_a) * frac / 1000;
            uint32_t c = hsv_to_rgb(hue, 255, 255);
            uint16_t px = (uint16_t)(((c >> 8) & 0xF800) |
                                     ((c >> 5) & 0x07E0) |
                                     ((c >> 3) & 0x001F));

            for (int x = 0; x < SCREEN_W; x++)
                row[(uint32_t)k * SCREEN_W + x] = px;
        }
        /* first chunk starts the RAM write (2C), later chunks are
         * 3C continue - each is its own CS frame, never split */
        for (int k = 0; k < rows; k++)
        {
            uint8_t cmd = (y0 == 0 && k == 0) ? 0x2C : 0x3C;
            const uint16_t *src = row + (uint32_t)k * SCREEN_W;

            for (int x = 0; x < SCREEN_W; x++)
            {
                row_wire[x * 2] = (uint8_t)(src[x] >> 8);
                row_wire[x * 2 + 1] = (uint8_t)src[x];
            }
            spi_xfer(cmd, row_wire, sizeof row_wire);
        }
    }
}

static void gradient_demo(uint32_t ms)
{
    static uint16_t row[SCREEN_W * G_ROWS];
    uint32_t frames = 0;

    LCD_SetBackColor(BACK_COLOR);
    paint_fps_band();
    fps_reset();

    uint32_t start = LCD_GetTick();
    uint32_t t = 0;

    do
    {
        int hue_a = (int)(t * 3600 / ms);
        int hue_b = hue_a + 1800;

        if (hue_b >= 3600)
            hue_b -= 3600;
        draw_gradient(hue_a, hue_b, row);
        frames++;
        g_frames++;
        fps_update();
        t = LCD_GetTick() - start;
    } while (!g_stop_requested && t < ms);

    log_printf(LOG_LVL_INFO, "  gradient: %u frames in %u ms = %u fps",
           frames, t, frames * 1000u / (t ? t : 1));
}

/* ---- framebuffer helpers (logo + scaled text) ---- */
static uint16_t s_fb[SCREEN_W * SCREEN_H];

static uint16_t rgb565(uint32_t rgb888)
{
    return (uint16_t)(((rgb888 >> 8) & 0xF800) |
                      ((rgb888 >> 5) & 0x07E0) |
                      ((rgb888 >> 3) & 0x001F));
}

static void fb_blit(void)
{
    /* the panel wants each RGB565 word high-byte first on the wire, but
     * s_fb holds host (little-endian) words - swap into a staging row
     * (the fills already do this via s_row; without it the fb path
     * displays byte-swapped colors) */
    static uint8_t s_row_fb[SCREEN_W * 2];

    LCD_SetAddress(0, 0, SCREEN_W - 1, SCREEN_H - 1);
    for (uint32_t r = 0; r < SCREEN_H; r++)
    {
        const uint16_t *src = s_fb + r * SCREEN_W;

        for (int c = 0; c < SCREEN_W; c++)
        {
            s_row_fb[c * 2]     = (uint8_t)(src[c] >> 8);
            s_row_fb[c * 2 + 1] = (uint8_t)(src[c] & 0xFF);
        }
        spi_xfer((r == 0) ? 0x2C : 0x3C, s_row_fb,
                 (size_t)SCREEN_W * 2U);
    }
}

static void fb_clear(uint16_t color)
{
    for (uint32_t i = 0; i < (uint32_t)SCREEN_W * SCREEN_H; i++)
        s_fb[i] = color;
}

/* the logo screen: OPi pinwheel (vendor manual) by default, or the
 * known-good configng-tux test image with -l tux */
static void logo_screen(void)
{
    const uint16_t *img = g_logo_tux ? logo_tux_rgb565 : logo_opi_rgb565;
    int iw = g_logo_tux ? (int)logo_tux_w : (int)logo_opi_w;
    int ih = g_logo_tux ? (int)logo_tux_h : (int)logo_opi_h;
    const char *caption = g_logo_tux ? "tux (color test)" : "Orange Pi Zero 3W";
    int lx = (SCREEN_W - iw) / 2;
    int ly = ((int)ANIM_H - ih) / 2 - 10;

    fb_clear(BLACK);
    if (lx < 0) lx = 0;
    if (ly < 0) ly = 0;
    for (int y = 0; y < ih && (ly + y) < (int)ANIM_H; y++)
        for (int x = 0; x < iw && (lx + x) < SCREEN_W; x++)
            s_fb[(uint32_t)(ly + y) * SCREEN_W + lx + x] =
                img[(uint32_t)y * iw + x];
    fb_blit();
    if (g_ft)
        ft_draw(FT_FACE_BOLD, 0, (uint16_t)(ANIM_H - 30), 26,
                caption, YELLOW, BLACK);
    else
    {
        LCD_SetAsciiFont(&ASCII_Font16);
        LCD_SetColor(LCD_YELLOW);
        LCD_SetBackColor(LCD_BLACK);
        LCD_DisplayString(136, (uint16_t)(ANIM_H - 20), "OPi-Zero3W");
    }
}

/* font-size showcase: DejaVu Sans at 4 sizes (ft path), 8x16 raster
 * fallback otherwise */
static void font_screen(void)
{
    fb_clear(BLACK);
    fb_blit();
    if (g_ft)
    {
        ft_draw(FT_FACE_REGULAR, 4, 8,   16, "DejaVu Sans 16", WHITE, BLACK);
        ft_draw(FT_FACE_REGULAR, 4, 44,  24, "DejaVu Sans 24", CYAN, BLACK);
        ft_draw(FT_FACE_BOLD,    4, 96,  32, "DejaVu Sans Bold 32", GREEN, BLACK);
        ft_draw(FT_FACE_BOLD,    4, 160, 48, "DejaVu Bold 48", MAGENTA, BLACK);
        ft_draw(FT_FACE_BOLD,    4, 232, 64, "Bold 64", YELLOW, BLACK);
    }
    else
    {
        LCD_SetAsciiFont(&ASCII_Font16);
        LCD_SetColor(LCD_WHITE);
        LCD_SetBackColor(LCD_BLACK);
        LCD_DisplayString(4, 8, "raster 8x16 fallback");
    }
}

/* ---- timed TEST_STAND (with dwell) ----
 * NOTE: DispColor takes RAW RGB565 (C565_*) - the RGB888 LCD_* macros
 * truncate to garbage (LCD_RED = 0xFF0000 -> 0x0000 = black). */
static void disp_red(void)   { DispColor(C565_RED); }
static void disp_green(void) { DispColor(C565_GREEN); }
static void disp_blue(void)  { DispColor(C565_BLUE); }
static void disp_white(void) { DispColor(C565_WHITE); }
static void disp_black(void) { DispColor(C565_BLACK); }

static void timed_screen(const char *name, void (*fn)(void))
{
    uint32_t t0 = LCD_GetTick();

    fn();
    log_printf(LOG_LVL_INFO, "  %-6s: %u ms (hold %d s)", name,
           LCD_GetTick() - t0, DWELL_MS / 1000);
    touch_dwell(DWELL_MS);
}

static void TEST_STAND(void)
{
    timed_screen("frame", DispFrame);
    if (g_stop_requested) return;
    timed_screen("gray",  DispGrayHor16);
    if (g_stop_requested) return;
    timed_screen("band",  DispBand);
    if (g_stop_requested) return;
    timed_screen("red",   disp_red);
    if (g_stop_requested) return;
    timed_screen("green", disp_green);
    if (g_stop_requested) return;
    timed_screen("blue",  disp_blue);
    if (g_stop_requested) return;
    timed_screen("white", disp_white);
    if (g_stop_requested) return;
    timed_screen("black", disp_black);
}

/* ---- info page ---- */
static void info_demo(uint32_t ms, uint8_t invert)
{
    char buf[64];
    const char *pixfmt_label = "RGB565 standard";
    uint32_t fg = invert ? LCD_BLACK : LCD_WHITE;
    uint32_t bg = invert ? LCD_WHITE : LCD_BLACK;
    unsigned eff = spi_actual_khz();

    log_printf(LOG_LVL_INFO,
           "[LCD] info%s: pixfmt=RGB%s spi=%u kHz uvc=%s",
           invert ? " (inverted)" : "",
           "565 standard",
           spi_requested_khz(),
           uvc_present() >= 0 ? "present" : "none");

    LCD_SetColor(fg);
    LCD_SetBackColor(bg);
    LCD_Clear();
    paint_fps_band();
    g_fps_color = fg;

    if (g_ft)
    {
        uint16_t y = 10;

        ft_draw(FT_FACE_BOLD, 4, y, 30, "CO5300 368x448", rgb565(fg), rgb565(bg));
        y += 38;
        snprintf(buf, sizeof buf, "%s", pixfmt_label);
        ft_draw(FT_FACE_REGULAR, 4, y, 26, buf, rgb565(fg), rgb565(bg));
        y += INFO_DY;
        snprintf(buf, sizeof buf, "%uM SPI", g_speed_khz / 1000);
        ft_draw(FT_FACE_REGULAR, 4, y, 26, buf, rgb565(fg), rgb565(bg));
        y += INFO_DY;
        if (eff)
        {
            snprintf(buf, sizeof buf, "eff %uM", eff / 1000);
            ft_draw(FT_FACE_REGULAR, 4, y, 26, buf, rgb565(fg), rgb565(bg));
            y += INFO_DY;
        }
        snprintf(buf, sizeof buf, "uvc:%s", uvc_present() >= 0 ? "yes" : "none");
        ft_draw(FT_FACE_REGULAR, 4, y, 26, buf, rgb565(fg), rgb565(bg));
        y += INFO_DY;
        snprintf(buf, sizeof buf, "touch:%s", g_touch_ok ? "ok" : "none");
        ft_draw(FT_FACE_REGULAR, 4, y, 26, buf, rgb565(fg), rgb565(bg));
        y += INFO_DY;
        snprintf(buf, sizeof buf, "bright 0x%02X", g_bright);
        ft_draw(FT_FACE_REGULAR, 4, y, 26, buf, rgb565(fg), rgb565(bg));
    }
    else
    {
        LCD_SetAsciiFont(&ASCII_Font16);
        LCD_SetColor(fg);
        LCD_SetBackColor(bg);
        LCD_DisplayString(4, 8, "CO5300 368x448 RGB565 standard");
        LCD_DisplayString(4, 30, "uvc/touch see console");
    }

    uint32_t start = LCD_GetTick();

    do
    {
        fps_update();
        touch_poll();
        usleep(50000);
    } while (!g_stop_requested && LCD_GetTick() - start < ms);
}

/* ---- big-font banner page (DejaVu Bold via ft) ---- */
static void banner_page(const char *l1, const char *l2,
                        uint32_t fg, uint32_t bg, uint32_t ms)
{
    if (g_ft)
    {
        uint16_t w1 = ft_width(FT_FACE_BOLD, 56, l1);
        uint16_t w2 = ft_width(FT_FACE_BOLD, 56, l2);
        int x1 = (SCREEN_W - (int)w1) / 2;
        int x2 = (SCREEN_W - (int)w2) / 2;

        if (x1 < 0) x1 = 0;
        if (x2 < 0) x2 = 0;
        LCD_Clear();
        ft_draw(FT_FACE_BOLD, (uint16_t)x1, 150, 56, l1, rgb565(fg), rgb565(bg));
        ft_draw(FT_FACE_BOLD, (uint16_t)x2, 240, 56, l2, rgb565(fg), rgb565(bg));
    }
    else
    {
        /* raster fallback: 2x-scaled 8x16 (the pre-ft banner) */
        int x1 = (SCREEN_W - (int)strlen(l1) * 8 * 2) / 2;
        int x2 = (SCREEN_W - (int)strlen(l2) * 8 * 2) / 2;

        if (x1 < 0) x1 = 0;
        if (x2 < 0) x2 = 0;
        fb_clear(rgb565(bg));
        for (int yy = 0; yy < 448; yy++)
            for (int xx = 0; xx < SCREEN_W; xx++)
                s_fb[(uint32_t)yy * SCREEN_W + xx] =
                    (xx >= x1 && xx < x1 + (int)strlen(l1) * 16 &&
                     yy >= 150 && yy < 206) ||
                    (xx >= x2 && xx < x2 + (int)strlen(l2) * 16 &&
                     yy >= 240 && yy < 296)
                        ? rgb565(fg) : rgb565(bg);
        /* simple solid-banner approximation, no glyphs in this path */
        fb_blit();
    }
    paint_fps_band();

    touch_dwell(ms);
}

/* ---- full pattern set ---- */
static void run_patterns(void)
{
    log_printf(LOG_LVL_INFO, "[LCD] phase: info");
    info_demo(5000, 0);
    if (g_stop_requested) return;

    log_printf(LOG_LVL_INFO, "[LCD] phase: info (inverted colors)");
    info_demo(5000, 1);
    if (g_stop_requested) return;

    log_printf(LOG_LVL_INFO, "[LCD] phase: vendor TEST_STAND (write time + %d s dwell)",
           DWELL_MS / 1000);
    TEST_STAND();
    if (g_stop_requested) return;

    log_printf(LOG_LVL_INFO, "[LCD] phase: gradient");
    gradient_demo(1000);
    if (g_stop_requested) return;
    touch_dwell(DWELL_MS);
    if (g_stop_requested) return;

    log_printf(LOG_LVL_INFO, "[LCD] phase: OPi logo + caption");
    logo_screen();
    touch_dwell(DWELL_MS);
    if (g_stop_requested) return;

    log_printf(LOG_LVL_INFO, "[LCD] phase: DejaVu font sizes");
    font_screen();
    touch_dwell(DWELL_MS);
}

int main(int argc, char **argv)
{
    log_init("lcm_test");
    log_config_from_argv(argc, argv);

    setvbuf(stdout, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++)
    {
        if (argv[i][0] == '-' && argv[i][1] && !argv[i][2])
        {
            switch (argv[i][1])
            {
            case 's': g_speed_khz = (unsigned)atoi(argv[++i]); break;
            case 'b': g_bright_fix = (uint8_t)strtoul(argv[++i], NULL, 0); break;
            case 'l': g_logo_tux = (strcmp(argv[++i], "tux") == 0); break;
            default:
                log_printf(LOG_LVL_INFO,
                       "usage: lcm_test [-s speed_kHz] [-b bright_hex]"
                       " [-l tux] (standard default, speed 40000)");
                return 1;
            }
        }
        else
        {
            log_printf(LOG_LVL_INFO,
                   "usage: lcm_test [-s speed_kHz] [-b bright_hex]"
                   " [-l tux] (standard default, speed 40000)");
            return 1;
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = request_stop;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) < 0 ||
        sigaction(SIGTERM, &sa, NULL) < 0)
    {
        log_printf(LOG_LVL_ERROR, "sigaction: %s", strerror(errno));
        return 1;
    }

    log_printf(LOG_LVL_INFO,
           "CO5300 %dx%d (MD196) + CHSC6417, wrapped SPI mode 3 (hw SPI3):",
           SCREEN_W, SCREEN_H);
    log_printf(LOG_LVL_INFO,
           "CLK=JP4.23 SI00=JP4.19 SI01=JP4.21 CS=JP4.24 TOUCH=JP4.5/JP4.3 (i2c-0 0x2E)");

    brightness_init();

    /* FreeType text (DejaVu Sans - the board's native font); falls
     * back to the 8x16 raster font if it cannot start */
    g_ft = (ft_init("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf")
            == 0);
    if (!g_ft)
        log_printf(LOG_LVL_WARN, "[FT] unavailable - raster font fallback");

    /* touch first, so its status lands in the init log */
    if (Touch_Init() == 0 && Touch_SelfTest())
        g_touch_ok = 1;
    log_printf(LOG_LVL_INFO, "[TOUCH] self-test: %s",
           g_touch_ok ? "ACK (chip present)" : "no bus / no ACK");

    if (spi_open(g_speed_khz * 1000) != 0)
        return 1;

    /* bring the panel up at a gentle 10 MHz - the CO5300 init sequence
     * (wrapped commands at CS frame granularity) is timing-sensitive
     * at very high SCK - then jump to the target rate for the pixel
     * workload. Without this, -s >= 80000 corrupts the init itself and
     * the panel never leaves sleep. */
    spi_set_speed(10000000);
    if (LCD_Init() != 0)
    {
        log_printf(LOG_LVL_ERROR, "LCD initialization failed");
        spi_close();
        return 1;
    }
    LCD_SetAsciiFont(&ASCII_Font16);
    paint_fps_band();
    spi_set_speed(g_speed_khz * 1000);

    for (int loop = 1; !g_stop_requested; loop++)
    {
        brightness_set(loop);
        log_printf(LOG_LVL_INFO, "[LCD] loop %d: brightness 0x%02X (%u%%)",
               loop, g_bright, (unsigned)(g_bright * 100u / 256u));

        log_printf(LOG_LVL_INFO, "[LCD] phase: HARDWARE banner");
        banner_page("now will do", "HW SPI3 test",
                    LCD_BLACK, LCD_CYAN, 3000);
        if (g_stop_requested)
            break;

        log_printf(LOG_LVL_INFO,
               "[LCD] running patterns on HARDWARE SPI3 @ %u MHz",
               g_speed_khz / 1000);
        run_patterns();
    }

    spi_close();
    return 0;
}
