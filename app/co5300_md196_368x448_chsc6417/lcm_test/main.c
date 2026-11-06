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

  Usage: ./lcm_test [-s speed_kHz] (default 40000 = 40 MHz; the CO5300
  allows ~47 MHz writes, 60 MHz corrupts on the reference boards).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <unistd.h>

#include "ref/lcd_fonts.h"
#include "ref/lcd_font_1608.h"
#include "lcd.h"
#include "logo64.h"
#include "spi_hw.h"
#include "touch.h"

#include "log.h"

/* ---- screen layout ---- */
#define SCREEN_W   368
#define SCREEN_H   448
#define FPS_BAND   24
#define ANIM_H     (SCREEN_H - FPS_BAND)
#define BACK_COLOR LCD_BLACK

/* requested SPI speed (kHz) - shared with the info page */
static unsigned g_speed_khz = 40000;
#define INFO_DY    20      /* 16 px Font16 + 4 px leading */
#define DWELL_MS   4000    /* per-pattern hold so colors can be judged */

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
 * this. */
static uint8_t g_bright;   /* current 51h value, shown on the info page */

static void brightness_init(void)
{
    g_bright = LCD_BRIGHT_LO;
}

static void brightness_set(int loop)
{
    uint8_t v = (loop % 2) ? LCD_BRIGHT_LO : LCD_BRIGHT_HI;

    if (v == g_bright)
        return;
    g_bright = v;
    LCD_SetBrightness(v);
}

/* ---- touch state ---- */
static int g_touch_ok;
static int g_touch_down;

#define TOUCH_POLL_MS 20

/* poll + print transitions, console output identical to the l4r5 port */
static void touch_poll(void)
{
    uint8_t b[3];
    int down, x, y;

    if (!g_touch_ok)
        return;
    Touch_Read(b, sizeof b);
    down = b[0] & 0x03;
    if (down && !g_touch_down)
    {
        x = (int)(((b[0] & 0x40) >> 6) << 8 | b[1]);
        y = (int)(((b[0] & 0x80) >> 7) << 8 | b[2]);
        log_printf(LOG_LVL_INFO, "[TOUCH] down X=%d Y=%d (%d-Y=%d)",
               x, y, SCREEN_H, SCREEN_H - y);
    }
    else if (!down && g_touch_down)
    {
        log_printf(LOG_LVL_INFO, "[TOUCH] release");
    }
    g_touch_down = down;
}

static void touch_dwell(uint32_t ms)
{
    uint32_t start = LCD_GetTick();

    do
    {
        touch_poll();
        usleep(TOUCH_POLL_MS * 1000);
    } while (LCD_GetTick() - start < ms);
}

/* ---- FPS counter ---- */static volatile uint32_t g_frames;
static uint32_t g_last_frames;
static uint32_t g_fps_last_tick;
static uint32_t g_fps_color = LCD_WHITE;

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

        g_last_frames = g_frames;
        g_fps_last_tick = now;
        {
            char buf[10];

            buf[0] = 'F'; buf[1] = 'P'; buf[2] = 'S'; buf[3] = ':';
            buf[4] = (char)('0' + (fps / 100) % 10);
            buf[5] = (char)('0' + (fps / 10) % 10);
            buf[6] = (char)('0' + fps % 10);
            buf[7] = '\0';
            LCD_SetColor(g_fps_color);
            LCD_ShowTransparent(1);
            LCD_DisplayString(1, (uint16_t)(ANIM_H + 4), buf);
            LCD_ShowTransparent(0);
            if (fps)
                log_printf(LOG_LVL_DEBUG, "[fps] %u", fps);
        }
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

#define G_ROWS 5   /* 5 rows x 736 B payload - one CS frame per row,
                      one spi_write() per transaction (< spidev 4096) */

static void draw_gradient(int hue_a, int hue_b, uint16_t *row)
{
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

            spi_xfer(cmd, (const uint8_t *)(row + (uint32_t)k * SCREEN_W),
                     (size_t)SCREEN_W * 2U);
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
    } while (t < ms);

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
    LCD_SetAddress(0, 0, SCREEN_W - 1, SCREEN_H - 1);
    for (uint32_t r = 0; r < SCREEN_H; r++)
    {
        uint8_t cmd = (r == 0) ? 0x2C : 0x3C;

        spi_xfer(cmd, (const uint8_t *)(s_fb + (uint32_t)r * SCREEN_W),
                 (size_t)SCREEN_W * 2U);
    }
}

static void fb_char_scaled(uint8_t c, int x0, int y0, int scale,
                           uint16_t fg, uint16_t bg)
{
    if (c < 0x20 || c > 0x7E)
        return;
    {
        int index = c - 0x20;

        for (int row = 0; row < 16; row++)
        {
            uint8_t bits = asc2_1608[index * 16 + row];

            for (int col = 0; col < 8; col++)
            {
                /* font glyphs are LSB-first: bit 0 = leftmost pixel */
                uint16_t v = (bits >> col) & 1u ? fg : bg;

                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                    {
                        int px = x0 + col * scale + sx;
                        int py = y0 + row * scale + sy;

                        if (px >= 0 && px < SCREEN_W &&
                            py >= 0 && py < SCREEN_H)
                            s_fb[py * SCREEN_W + px] = v;
                    }
            }
        }
    }
}

static void fb_string_scaled(const char *s, int x0, int y0, int scale,
                             uint16_t fg, uint16_t bg)
{
    int x = x0;

    while (*s)
    {
        fb_char_scaled((uint8_t)*s, x, y0, scale, fg, bg);
        x += 8 * scale;
        s++;
    }
}

static void fb_clear(uint16_t color)
{
    for (uint32_t i = 0; i < (uint32_t)SCREEN_W * SCREEN_H; i++)
        s_fb[i] = color;
}

/* stock boot logo (logo64.h, RK3506 resource.img origin), 2x scaled,
 * caption underneath - the lcm_logo screen of the ssd1317 project.
 * logo64 layout: page-major, 64 cols x 48 rows, bit = row in page. */
static void logo_screen(void)
{
    fb_clear(BLACK);
    for (int y = 0; y < 48; y++)
        for (int x = 0; x < 64; x++)
        {
            uint8_t byte = logo64_fb[(y / 8) * 64 + x];
            uint16_t c = (byte >> (y % 8)) & 1 ? WHITE : BLACK;

            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++)
                    s_fb[(176 + y * 2 + dy) * SCREEN_W + 120 + x * 2 + dx] = c;
        }
    fb_string_scaled("OPi-Zero3W", 144, 280, 1, YELLOW, BLACK);
    fb_blit();
}

/* scaled font screen: 1x / 2x / 3x of the 8x16 font */
static void scaled_font_screen(void)
{
    fb_clear(BLACK);
    fb_string_scaled("OPi-Zero3W", 144, 8, 1, WHITE, BLACK);
    fb_string_scaled("OPi", 160, 48, 2, CYAN, BLACK);
    fb_string_scaled("Zero3W", 112, 128, 3, GREEN, BLACK);
    fb_blit();
}

/* ---- timed TEST_STAND (with dwell) ---- */
static void disp_red(void)   { DispColor(LCD_RED); }
static void disp_green(void) { DispColor(LCD_GREEN); }
static void disp_blue(void)  { DispColor(LCD_BLUE); }
static void disp_white(void) { DispColor(LCD_WHITE); }
static void disp_black(void) { DispColor(LCD_BLACK); }

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
    timed_screen("gray",  DispGrayHor16);
    timed_screen("band",  DispBand);
    timed_screen("red",   disp_red);
    timed_screen("green", disp_green);
    timed_screen("blue",  disp_blue);
    timed_screen("white", disp_white);
    timed_screen("black", disp_black);
}

/* ---- info page ---- */
static void info_demo(uint32_t ms, uint8_t invert)
{
    char buf[32];
    uint32_t fg = invert ? LCD_BLACK : LCD_WHITE;
    uint32_t bg = invert ? LCD_WHITE : LCD_BLACK;

    log_printf(LOG_LVL_INFO,
           "[LCD] info%s: pixfmt=RGB565 spi=%u kHz (eff %u kHz) uvc=%s",
           invert ? " (inverted)" : "", spi_requested_khz(),
           spi_actual_khz(), uvc_present() >= 0 ? "present" : "none");

    LCD_SetAsciiFont(&ASCII_Font16);
    LCD_SetColor(fg);
    LCD_SetBackColor(bg);
    LCD_Clear();

    LCD_SetColor(bg);
    LCD_SetBackColor(bg);
    LCD_FillRect(0, (uint16_t)ANIM_H, SCREEN_W, FPS_BAND);
    LCD_SetColor(fg);
    LCD_SetBackColor(bg);
    g_fps_color = fg;

    const int ix = 2;
    int y = 4;

    snprintf(buf, sizeof buf, "CO5300 %dx%d", SCREEN_W, SCREEN_H);
    LCD_DisplayString((uint16_t)ix, (uint16_t)y, buf);
    y += INFO_DY;

    snprintf(buf, sizeof buf, "RGB565");
    LCD_DisplayString((uint16_t)ix, (uint16_t)y, buf);
    y += INFO_DY;

    snprintf(buf, sizeof buf, "%uM SPI", g_speed_khz / 1000);
    LCD_DisplayString((uint16_t)ix, (uint16_t)y, buf);
    y += INFO_DY;

    {
        unsigned eff = spi_actual_khz();

        if (eff)
        {
            snprintf(buf, sizeof buf, "eff %uM", eff / 1000);
            LCD_DisplayString((uint16_t)ix, (uint16_t)y, buf);
            y += INFO_DY;
        }
    }

    snprintf(buf, sizeof buf, "uvc:%s", uvc_present() >= 0 ? "yes" : "none");
    LCD_DisplayString((uint16_t)ix, (uint16_t)y, buf);
    y += INFO_DY;

    snprintf(buf, sizeof buf, "touch:%s", g_touch_ok ? "ok" : "none");
    LCD_DisplayString((uint16_t)ix, (uint16_t)y, buf);
    y += INFO_DY;

    snprintf(buf, sizeof buf, "bright:0x%02X", g_bright);
    LCD_DisplayString((uint16_t)ix, (uint16_t)y, buf);

    g_fps_color = LCD_WHITE;

    uint32_t start = LCD_GetTick();

    do
    {
        fps_update();
        touch_poll();
        usleep(50000);
    } while (LCD_GetTick() - start < ms);
}

/* ---- big-font banner page (2x scaled 8x16, fb path) ---- */
static void banner_page(const char *l1, const char *l2,
                        uint32_t fg, uint32_t bg, uint32_t ms)
{
    int x1 = (SCREEN_W - (int)strlen(l1) * 8 * 2) / 2;
    int x2 = (SCREEN_W - (int)strlen(l2) * 8 * 2) / 2;

    if (x1 < 0) x1 = 0;
    if (x2 < 0) x2 = 0;

    fb_clear(rgb565(bg));
    fb_string_scaled(l1, x1, 100, 2, rgb565(fg), rgb565(bg));
    fb_string_scaled(l2, x2, 148, 2, rgb565(fg), rgb565(bg));
    fb_blit();
    paint_fps_band();

    touch_dwell(ms);
}

/* ---- full pattern set ---- */
static void run_patterns(void)
{
    log_printf(LOG_LVL_INFO, "[LCD] phase: info");
    info_demo(5000, 0);

    log_printf(LOG_LVL_INFO, "[LCD] phase: info (inverted colors)");
    info_demo(5000, 1);

    log_printf(LOG_LVL_INFO, "[LCD] phase: vendor TEST_STAND (write time + %d s dwell)",
           DWELL_MS / 1000);
    TEST_STAND();

    log_printf(LOG_LVL_INFO, "[LCD] phase: gradient");
    gradient_demo(1000);
    touch_dwell(DWELL_MS);

    log_printf(LOG_LVL_INFO, "[LCD] phase: board boot logo (2x) + caption");
    logo_screen();
    touch_dwell(DWELL_MS);

    log_printf(LOG_LVL_INFO, "[LCD] phase: scaled font (1x/2x/3x)");
    scaled_font_screen();
    touch_dwell(DWELL_MS);
}

int main(int argc, char **argv)
{
    log_init("lcm_test");
    log_config_from_argv(argc, argv);
    unsigned speed_khz = g_speed_khz;

    setvbuf(stdout, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++)
    {
        if (argv[i][0] == '-' && argv[i][1] && !argv[i][2])
        {
            switch (argv[i][1])
            {
            case 's': g_speed_khz = (unsigned)atoi(argv[++i]); break;
            default:
                log_printf(LOG_LVL_INFO, "usage: lcm_test [-s speed_kHz]");
                return 1;
            }
        }
        else
        {
            log_printf(LOG_LVL_INFO, "usage: lcm_test [-s speed_kHz]");
            return 1;
        }
    }

    log_printf(LOG_LVL_INFO,
           "CO5300 %dx%d (MD196) + CHSC6417, wrapped SPI mode 3 (hw SPI3):",
           SCREEN_W, SCREEN_H);
    log_printf(LOG_LVL_INFO,
           "CLK=JP4.23 MOSI=JP4.19 CS=JP4.24 TOUCH=JP4.5/JP4.3 (i2c-0 0x2E)");

    brightness_init();

    /* touch first, so its status lands in the init log */
    if (Touch_Init() == 0)
    {
        g_touch_ok = Touch_SelfTest();
        log_printf(LOG_LVL_INFO, "[TOUCH] self-test: %s (chip id 0xA7: 0x%02X)",
               g_touch_ok ? "ACK (chip present)" : "no ACK",
               Touch_ChipId());
    }
    else
    {
        log_printf(LOG_LVL_ERROR, "[TOUCH] no bus: %s", TOUCH_I2C_DEV);
    }

    if (spi_open(speed_khz * 1000) != 0)
        return 1;

    LCD_Init();
    LCD_SetAsciiFont(&ASCII_Font16);
    paint_fps_band();

    for (int loop = 1;; loop++)
    {
        brightness_set(loop);
        log_printf(LOG_LVL_INFO, "[LCD] loop %d: brightness 0x%02X (%u%%)",
               loop, g_bright, (unsigned)(g_bright * 100u / 256u));

        log_printf(LOG_LVL_INFO, "[LCD] phase: HARDWARE banner");
        banner_page("now will do", "HW SPI3 test",
                    LCD_BLACK, LCD_CYAN, 3000);

        log_printf(LOG_LVL_INFO,
               "[LCD] running patterns on HARDWARE SPI3 @ %u MHz",
               g_speed_khz / 1000);
        run_patterns();
    }

    return 0;
}
