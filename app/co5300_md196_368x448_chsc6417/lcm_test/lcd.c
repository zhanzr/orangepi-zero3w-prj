/*
  lcd.c - CO5300 1.96" 368x448 LCD driver, Orange Pi Zero 3W port.
  Ported from the nucleo-l4r5 co5300_md196_368x448_chsc6417 (vendor
  TK499 TK0196M106 example, verbatim registers), drawing API shared
  with the rk3506 st7365 lcm_test.

  The CO5300 wrapped-command protocol: every command is one CS frame -
  the 4-byte header 02 00 <cmd> 00, then the parameter / pixel bytes in
  the same frame (spi_xfer keeps header+payload together). Every pixel
  burst is chunked into whole-row transactions: the window is set once
  (2A/2B), the first row goes out as 2C and the remaining rows as 3C
  (write memory continue).

  Bus: /dev/spidev3.0 mode 3, write-only (no readback anywhere; MISO
  is not wired). No reset pin wired - the vendor sequence hardware-
  resets first; here the POR settle rides on LCD_POWER_SETTLE_MS.
  Touch: CHSC6417 over TWI0 (/dev/i2c-0) - see touch.c.
*/
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include "lcd.h"
#include "spi_hw.h"

/* ---- runtime drawing window ---- */
static uint16_t s_win_x = 0, s_win_y = 0;
static uint16_t s_win_w = COL, s_win_h = ROW;

void LCD_SetWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    if (x >= COL || y >= ROW) { return; }
    if (w == 0U || h == 0U)   { return; }
    if (x + w > COL) { w = (uint16_t)(COL - x); }
    if (y + h > ROW) { h = (uint16_t)(ROW - y); }
    s_win_x = x;
    s_win_y = y;
    s_win_w = w;
    s_win_h = h;
}

void LCD_ResetWindow(void)
{
    s_win_x = 0U;
    s_win_y = 0U;
    s_win_w = COL;
    s_win_h = ROW;
}

uint16_t LCD_W(void)
{
    return s_win_w;
}

uint16_t LCD_H(void)
{
    return s_win_h;
}

uint32_t LCD_GetTick(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

void StopDelay(uint16_t ms)
{
    usleep(ms * 1000u);
}

void delay_ms(uint32_t ms)
{
    usleep(ms * 1000u);
}

/* One pixel row of RGB565 bytes (big-endian on the wire). */
static uint8_t s_row[COL * 2U];

/* No reset pin is wired on this connector. The CO5300 does have a
 * software reset (DCS 01h), but the datasheet forbids the obvious use
 * of it:
 *
 *   "It will be necessary to wait 5msec before sending new command
 *    following software reset."
 *   "If Software Reset is applied during Sleep Out mode, it will be
 *    necessary to wait 120msec before sending Sleep Out command."
 *   "Software Reset command cannot be sent during Sleep Out sequence."
 *
 * LCD_Reinit() needs exactly that sequence (reset -> sleep out ->
 * init), so a 01h there has to be followed by a 120 ms hold *and* a
 * fresh sleep-out; carrying straight on leaves the panel behind the
 * rule and it stops accepting pixels (documented on the l4r5 port).
 * This settle stands in for the vendor's hardware reset. */
void LCD_RESET(void)
{
    delay_ms(120);
}

/* =====================================================================
   Init sequence - CO5300 (vendored TK0196M106 TK499 example, verbatim
   registers and order): command access unlock, sleep out, source/GIP
   setup, 16bpp COLMOD, brightness (51h/53h - brightness is driven
   over SPI, there is no BL pin), display on.
   ===================================================================== */
void LCD_IC_Init(void)
{
    static const uint8_t p00[] = { 0x00 };
    static const uint8_t pC4[] = { 0x80 };
    static const uint8_t p3A[] = { 0x55 };              /* 16bpp        */
    static const uint8_t p53[] = { 0x20 };              /* display ctrl */
    static const uint8_t p63[] = { 0xFF };
    static const uint8_t p51[] = { LCD_BRIGHT_LO };
    static const uint8_t p58[] = { 0x07 };

    spi_xfer(0xFE, p00, sizeof p00);        /* command access unlock */

    spi_xfer(0x11, NULL, 0);                /* sleep out             */
    delay_ms(120);                          /* datasheet: 120 ms to become
                                             * Sleep Out (booster on) */

    spi_xfer(0xFE, p00, sizeof p00);        /* command access unlock */
    spi_xfer(0xC4, pC4, sizeof pC4);
    spi_xfer(0x3A, p3A, sizeof p3A);        /* COLMOD 16bpp          */
    spi_xfer(0x53, p53, sizeof p53);        /* CTRL display: BC_EN=1,
                                             * DIM_EN=0               */
    spi_xfer(0x63, p63, sizeof p63);        /* HBM brightness (inert:
                                             * 66h defaults to 0)     */
    spi_xfer(0x51, p51, sizeof p51);        /* display brightness     */
    spi_xfer(0x58, p58, sizeof p58);

    spi_xfer(0x29, NULL, 0);                /* display on            */
    delay_ms(80);                           /* vendor: 80 ms         */
}

void LCD_Init(void)
{
    /* Power-on settle: on a COLD power-on the first commands can
     * arrive while the panel is still starting up and are lost. The
     * LCD_Reinit() path skips this (panel already up). */
    delay_ms(LCD_POWER_SETTLE_MS);

    LCD_RESET();
    LCD_IC_Init();

    /* Clear to black. */
    DispColor(C565_BLACK);
}

/* Re-init an already-running panel: display off, sleep in, then the
 * full init from as clean a state as we can reach without a reset
 * line (01h cannot be used mid-sequence - see LCD_RESET()). */
void LCD_Reinit(void)
{
    spi_xfer(0x28, NULL, 0);      /* display off */
    spi_xfer(0x10, NULL, 0);      /* sleep in    */
    delay_ms(20);                 /* datasheet: >=5 ms before new cmds */

    LCD_RESET();
    LCD_IC_Init();
}

/* DCS WRDISBV (51h) - works at runtime, used by the demo loop. */
void LCD_SetBrightness(uint8_t dbv)
{
    spi_xfer(0x51, &dbv, 1U);
}

/* =====================================================================
   Pixel streaming. The drawing window is set once (2A/2B), then each
   row of s_row goes out as one transaction: 2C for the first row, 3C
   (write memory continue) for the rest.
   ===================================================================== */
void LCD_SetAddress(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    uint8_t p[4];

    x1 = (uint16_t)(x1 + s_win_x + LCD_X_OFFSET);
    y1 = (uint16_t)(y1 + s_win_y);
    x2 = (uint16_t)(x2 + s_win_x + LCD_X_OFFSET);
    y2 = (uint16_t)(y2 + s_win_y);

    p[0] = (uint8_t)(x1 >> 8);
    p[1] = (uint8_t)x1;
    p[2] = (uint8_t)(x2 >> 8);
    p[3] = (uint8_t)x2;
    spi_xfer(0x2A, p, 4U);

    p[0] = (uint8_t)(y1 >> 8);
    p[1] = (uint8_t)y1;
    p[2] = (uint8_t)(y2 >> 8);
    p[3] = (uint8_t)y2;
    spi_xfer(0x2B, p, 4U);
}

static uint16_t rgb888_to_rgb565(uint32_t c)
{
    uint16_t r = (uint16_t)((c & 0x00F80000UL) >> 8);
    uint16_t g = (uint16_t)((c & 0x0000FC00UL) >> 5);
    uint16_t b = (uint16_t)((c & 0x000000F8UL) >> 3);
    return (uint16_t)(r | g | b);
}

/* Current foreground/background RGB565 + text-transparent flag. */
static uint16_t s_Color     = C565_BLACK;
static uint16_t s_BackColor = C565_BLACK;
static uint8_t  s_Transparent = 0;
static pFONT  *s_AsciiFont = NULL;

void LCD_SetColor(uint32_t rgb888)
{
    s_Color = rgb888_to_rgb565(rgb888);
}

void LCD_SetBackColor(uint32_t rgb888)
{
    s_BackColor = rgb888_to_rgb565(rgb888);
}

/* Solid fill: set the window, then stream `rows` identical rows. */
static void lcd_fill(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,
                     uint16_t color)
{
    uint16_t w = (uint16_t)(x2 - x1 + 1U);
    uint16_t n = (uint16_t)(y2 - y1 + 1U);
    uint32_t len = (uint32_t)w * 2U;
    uint16_t r;

    LCD_SetAddress(x1, y1, x2, y2);
    for (r = 0; r < w; r++)
    {
        s_row[r * 2U]     = (uint8_t)(color >> 8);
        s_row[r * 2U + 1U] = (uint8_t)color;
    }
    for (r = 0; r < n; r++)
    {
        spi_xfer((r == 0U) ? 0x2C : 0x3C, s_row, len);
    }
}

void LCD_Clear(void)
{
    lcd_fill(0, 0, (uint16_t)(s_win_w - 1U), (uint16_t)(s_win_h - 1U),
             s_BackColor);
}

void LCD_ClearRect(uint16_t x, uint16_t y, uint16_t width, uint16_t height)
{
    if ((width == 0U) || (height == 0U)) { return; }
    lcd_fill(x, y, (uint16_t)(x + width - 1U), (uint16_t)(y + height - 1U),
             s_BackColor);
}

/* =====================================================================
   Vendor demo screens - identical geometry/ordering to the reference.
   Each row of s_row goes out as one whole-row transaction (2C first,
   3C after).
   ===================================================================== */
void DispColor(uint32_t color)
{
    lcd_fill(0, 0, (uint16_t)(LCD_W() - 1U), (uint16_t)(LCD_H() - 1U),
             (uint16_t)color);
}

void DispFrame(void)
{
    uint16_t w = LCD_W();
    uint16_t h = LCD_H();
    uint16_t i, r, len = (uint16_t)(w * 2U);

    LCD_SetAddress(0, 0, (uint16_t)(w - 1U), (uint16_t)(h - 1U));

    /* Top / bottom rows: red, (w-2) white pixels, blue. */
    s_row[0] = 0xF8; s_row[1] = 0x00;
    for (i = 1; i < w - 1U; i++)
    {
        s_row[i * 2U] = 0xFF; s_row[i * 2U + 1U] = 0xFF;
    }
    s_row[(w - 1U) * 2U] = 0x00; s_row[(w - 1U) * 2U + 1U] = 0x1F;
    spi_xfer(0x2C, s_row, len);

    /* Interior rows: red, (w-2) black pixels, blue. */
    for (i = 1; i < w - 1U; i++)
    {
        s_row[i * 2U] = 0x00; s_row[i * 2U + 1U] = 0x00;
    }
    for (r = 1; r < h - 1U; r++)
    {
        spi_xfer(0x3C, s_row, len);
    }

    /* Bottom row: red, (w-2) white pixels, blue. */
    for (i = 1; i < w - 1U; i++)
    {
        s_row[i * 2U] = 0xFF; s_row[i * 2U + 1U] = 0xFF;
    }
    spi_xfer(0x3C, s_row, len);
}

void DispGrayHor16(void)
{
    uint16_t w = LCD_W();
    uint16_t h = LCD_H();
    uint16_t unit = (uint16_t)(w / 16U);
    uint16_t lead = (uint16_t)(w % 16U);
    uint16_t j, r, x, len = (uint16_t)(w * 2U);

    LCD_SetAddress(0, 0, (uint16_t)(w - 1U), (uint16_t)(h - 1U));

    for (x = 0; x < lead; x++)
    {
        s_row[x * 2U] = 0x00; s_row[x * 2U + 1U] = 0x00;
    }
    for (j = 0; j < 16U; j++)
    {
        uint16_t c = (uint16_t)(((((j * 2U) << 3) | ((j * 4U) >> 3)) << 8) |
                                (((j * 4U) << 5) | (j * 2U)));
        for (r = 0; r < unit; r++)
        {
            s_row[(lead + r) * 2U]     = (uint8_t)(c >> 8);
            s_row[(lead + r) * 2U + 1U] = (uint8_t)c;
        }
        lead = (uint16_t)(lead + unit);
    }
    for (r = 0; r < h; r++)
    {
        spi_xfer((r == 0U) ? 0x2C : 0x3C, s_row, len);
    }
}

void DispBand(void)
{
    /* 8 DISTINCT bands - doubled pairs just look like 4 wide bands */
    static const uint16_t color[8] = { 0xF800, 0x07E0, 0x001F, 0xFFE0,
                                       0xF81F, 0x07FF, 0xFFFF, 0x0000 };
    uint16_t w = LCD_W();
    uint16_t h = LCD_H();
    uint16_t band = (uint16_t)(h / 8U);
    uint16_t rem  = (uint16_t)(h % 8U);
    uint16_t r, i, len = (uint16_t)(w * 2U);
    uint8_t  first = 1U;

    LCD_SetAddress(0, 0, (uint16_t)(w - 1U), (uint16_t)(h - 1U));

    for (i = 0; i < 8U; i++)
    {
        for (r = 0; r < w; r++)
        {
            s_row[r * 2U]     = (uint8_t)(color[i] >> 8);
            s_row[r * 2U + 1U] = (uint8_t)color[i];
        }
        for (r = 0; r < band; r++)
        {
            spi_xfer(first ? 0x2C : 0x3C, s_row, len);
            first = 0U;
        }
    }
    for (r = 0; r < w; r++)
    {
        s_row[r * 2U]     = (uint8_t)(color[7] >> 8);
        s_row[r * 2U + 1U] = (uint8_t)color[7];
    }
    for (r = 0; r < rem; r++)
    {
        spi_xfer(first ? 0x2C : 0x3C, s_row, len);
        first = 0U;
    }
}

/* =====================================================================
   Colors, addressing, text and 2D drawing - the standard st7789-style
   drawing API, adapted to the 368x448 CO5300.
   ===================================================================== */

/* ---- ASCII text ---- */
void LCD_SetAsciiFont(pFONT *font)
{
    s_AsciiFont = font;
}

void LCD_ShowTransparent(uint8_t mode)
{
    s_Transparent = mode;
}

void LCD_DisplayChar(uint16_t x, uint16_t y, uint8_t c)
{
    uint16_t index;
    uint8_t  disChar;
    /* Must hold the LARGEST font's pixels (8x16 = 128); the 6x12 font
     * only fills the first 72. */
    uint16_t Buff[8 * 16];

    if ((s_AsciiFont == NULL) || (c < 0x20U) || (c > 0x7EU))
    {
        return;
    }
    c -= 0x20U;   /* table starts at space */

    if (s_Transparent)
    {
        uint16_t bytesPerRow = (uint16_t)(s_AsciiFont->Sizes / s_AsciiFont->Height);
        for (uint16_t row = 0; row < s_AsciiFont->Height; row++)
        {
            for (uint16_t col = 0; col < s_AsciiFont->Width; col++)
            {
                disChar = s_AsciiFont->pTable[(uint16_t)c * s_AsciiFont->Sizes
                          + (uint16_t)row * bytesPerRow + (col / 8)];
                if (disChar & (uint8_t)(1U << (col % 8)))
                {
                    LCD_DrawPoint((uint16_t)(x + col), (uint16_t)(y + row),
                                  s_Color);
                }
            }
        }
        return;
    }

    index = 0;
    /* Row-major fill matching the font layout: each glyph row is
     * bytesPerRow bytes, bit 0 = leftmost pixel. Filling linearly from
     * the raw bit stream instead smears the 6x12 font (its 6-bit rows
     * are byte-packed with 2 padding bits that bleed into the next
     * row). */
    uint16_t bytesPerRow = (uint16_t)(s_AsciiFont->Sizes / s_AsciiFont->Height);
    for (uint16_t row = 0; row < s_AsciiFont->Height; row++)
    {
        for (uint16_t col = 0; col < s_AsciiFont->Width; col++)
        {
            disChar = s_AsciiFont->pTable[(uint16_t)c * s_AsciiFont->Sizes
                      + (uint16_t)row * bytesPerRow + (col / 8)];
            Buff[index++] = (disChar & (uint8_t)(1U << (col % 8)))
                            ? s_Color : s_BackColor;
        }
    }
    LCD_CopyBuffer(x, y, s_AsciiFont->Width, s_AsciiFont->Height, Buff);
}

void LCD_DisplayString(uint16_t x, uint16_t y, char *p)
{
    while (x < COL && *p != '\0')
    {
        LCD_DisplayChar(x, y, (uint8_t)*p);
        x += (uint16_t)((s_AsciiFont != NULL) ? s_AsciiFont->Width : 6);
        p++;
    }
}

/* ---- 2D drawing ---- */
void LCD_DrawPoint(uint16_t x, uint16_t y, uint32_t color)
{
    uint8_t p[2];

    LCD_SetAddress(x, y, x, y);
    p[0] = (uint8_t)(color >> 8);
    p[1] = (uint8_t)color;
    spi_xfer(0x2C, p, 2U);
}

void LCD_DrawLine_V(uint16_t x, uint16_t y, uint16_t height)
{
    if (height == 0U) { return; }
    lcd_fill(x, y, x, (uint16_t)(y + height - 1U), s_Color);
}

void LCD_DrawLine_H(uint16_t x, uint16_t y, uint16_t width)
{
    if (width == 0U) { return; }
    lcd_fill(x, y, (uint16_t)(x + width - 1U), y, s_Color);
}

void LCD_DrawLine(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    int16_t deltax = 0, deltay = 0, x = 0, y = 0, xinc1 = 0, xinc2 = 0;
    int16_t yinc1 = 0, yinc2 = 0, den = 0, num = 0, numadd = 0;
    int16_t numpixels = 0, curpixel = 0;

    deltax = ABS((int16_t)x2 - (int16_t)x1);
    deltay = ABS((int16_t)y2 - (int16_t)y1);
    x = x1; y = y1;

    if (x2 >= x1) { xinc1 = 1; xinc2 = 1; } else { xinc1 = -1; xinc2 = -1; }
    if (y2 >= y1) { yinc1 = 1; yinc2 = 1; } else { yinc1 = -1; yinc2 = -1; }

    if (deltax >= deltay)
    {
        xinc1 = 0; yinc2 = 0; den = deltax; num = deltax / 2;
        numadd = deltay; numpixels = deltax;
    }
    else
    {
        xinc2 = 0; yinc1 = 0; den = deltay; num = deltay / 2;
        numadd = deltax; numpixels = deltay;
    }
    for (curpixel = 0; curpixel <= numpixels; curpixel++)
    {
        LCD_DrawPoint((uint16_t)x, (uint16_t)y, s_Color);
        num += numadd;
        if (num >= den)
        {
            num -= den;
            x += xinc1;
            y += yinc1;
        }
        x += xinc2;
        y += yinc2;
    }
}

void LCD_DrawRect(uint16_t x, uint16_t y, uint16_t width, uint16_t height)
{
    LCD_DrawLine_H(x, y, width);
    LCD_DrawLine_H(x, (uint16_t)(y + height - 1), width);
    LCD_DrawLine_V(x, y, height);
    LCD_DrawLine_V((uint16_t)(x + width - 1), y, height);
}

void LCD_DrawCircle(uint16_t x, uint16_t y, uint16_t r)
{
    int16_t Xadd = -(int16_t)r, Yadd = 0, err = 2 - 2 * (int16_t)r, e2;
    do
    {
        LCD_DrawPoint((uint16_t)(x - Xadd), (uint16_t)(y + Yadd), s_Color);
        LCD_DrawPoint((uint16_t)(x + Xadd), (uint16_t)(y + Yadd), s_Color);
        LCD_DrawPoint((uint16_t)(x + Xadd), (uint16_t)(y - Yadd), s_Color);
        LCD_DrawPoint((uint16_t)(x - Xadd), (uint16_t)(y - Yadd), s_Color);
        e2 = err;
        if (e2 <= Yadd)
        {
            Yadd++;
            err += (int16_t)(Yadd * 2 + 1);
            if (-Xadd == Yadd && e2 <= Xadd) { e2 = 0; }
        }
        if (e2 > Xadd)
        {
            Xadd++;
            err += (int16_t)(Xadd * 2 + 1);
        }
    }
    while (Xadd <= 0);
}

void LCD_FillRect(uint16_t x, uint16_t y, uint16_t width, uint16_t height)
{
    if ((width == 0U) || (height == 0U)) { return; }
    lcd_fill(x, y, (uint16_t)(x + width - 1U), (uint16_t)(y + height - 1U),
             s_Color);
}

void LCD_FillCircle(uint16_t x, uint16_t y, uint16_t r)
{
    int32_t  D;
    uint32_t CurX, CurY;

    D = 3 - ((int32_t)r << 1);
    CurX = 0;
    CurY = r;
    while (CurX <= CurY)
    {
        if (CurY > 0)
        {
            LCD_DrawLine_V((uint16_t)(x - CurX), (uint16_t)(y - CurY),
                           (uint16_t)(2 * CurY));
            LCD_DrawLine_V((uint16_t)(x + CurX), (uint16_t)(y - CurY),
                           (uint16_t)(2 * CurY));
        }
        if (CurX > 0)
        {
            LCD_DrawLine_V((uint16_t)(x - CurY), (uint16_t)(y - CurX),
                           (uint16_t)(2 * CurX));
            LCD_DrawLine_V((uint16_t)(x + CurY), (uint16_t)(y - CurX),
                           (uint16_t)(2 * CurX));
        }
        if (D < 0)
        {
            D += (int32_t)(CurX << 2) + 6;
        }
        else
        {
            D += (int32_t)((CurX - CurY) << 2) + 10;
            CurY--;
        }
        CurX++;
    }
    LCD_DrawCircle(x, y, r);
}

void LCD_CopyBuffer(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                    const uint16_t *data)
{
    uint16_t r, c, len;
    uint8_t  first = 1U;
    uint16_t clip_w = width;

    if ((width == 0U) || (height == 0U)) { return; }

    /* The row staging buffer is COL wide; clip so a full-width request
     * can never overflow it. */
    if (clip_w > COL) { clip_w = COL; }
    len = (uint16_t)(clip_w * 2U);

    LCD_SetAddress(x, y, (uint16_t)(x + clip_w - 1U),
                   (uint16_t)(y + height - 1U));
    for (r = 0; r < height; r++)
    {
        for (c = 0; c < clip_w; c++)
        {
            uint16_t v = data[(uint32_t)r * width + c];
            s_row[c * 2U]     = (uint8_t)(v >> 8);
            s_row[c * 2U + 1U] = (uint8_t)v;
        }
        spi_xfer(first ? 0x2C : 0x3C, s_row, len);
        first = 0U;
    }
}
