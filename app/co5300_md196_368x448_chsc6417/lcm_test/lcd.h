/*
  lcd.h - CO5300 1.96" 368x448 LCD driver, Orange Pi Zero 3W port
  (TK0196M106 module: CO5300 + CHSC6417). Adapted from the proven
  nucleo-l4r5/nucleo-u575 co5300_md196_368x448_chsc6417 ports (vendor
  TK499 init, verbatim registers). Same drawing API as the rk3506
  st7365 lcm_test.

  Bus: single-lane SPI3 on the JP4 header, wrapped commands (see
  spi_hw.h) - one CS frame per transaction, mode 3, write-only.
  No DC / RES / BLK pins wired: the wrapped protocol needs no DC, the
  POR settle rides on LCD_POWER_SETTLE_MS (and the CO5300 software
  reset 01h is deliberately NOT used - see the LCD_RESET() note in
  lcd.c), brightness is DCS 51h.
  Touch: CHSC6417 over TWI0 (/dev/i2c-0, 0x2E) - see touch.c.
  Panel geometry: 368x448, 16bpp (3Ah = 0x55). The vendor demo drives
  columns 0..367 directly, so the X GRAM offset defaults to 0
  (LCD_X_OFFSET); if the image is shifted, try 16.
*/
#ifndef CO5300_LCD_H
#define CO5300_LCD_H

#include <stdint.h>
#include "ref/lcd_fonts.h"

/* ---- panel geometry / demo timing ---- */
#define LCD_Width    368
#define LCD_Height   448
#define COL       368
#define ROW       448
#define COL_Pre   0
#define ROW_Pre   0
#define Delay_Time 500

/* X GRAM offset added to every column address (see header comment). */
#ifndef LCD_X_OFFSET
#define LCD_X_OFFSET 0
#endif

/* Display brightness - DCS WRDISBV (51h), linear DBV/256. The module
 * is much brighter than the LCD panels on the reference boards; the
 * l4r5 port's usable floor is 0x01 (0.78 %), the vendor default 0xA0
 * (62.5 %) reads harsh. The demo loop alternates these two so the
 * panel can be judged at both ends:
 *   vendor sequence / vendor ESP32 example : 0xA0 (62.5 %)
 *   l4r5/u575 ports                        : 0x01 (0.78 %)
 * 51h is the ONLY brightness control (CMD2 pages undocumented). */
#ifndef LCD_BRIGHT_LO
#define LCD_BRIGHT_LO 0x01U
#endif
#ifndef LCD_BRIGHT_HI
#define LCD_BRIGHT_HI 0x28U   /* 15.6 % */
#endif

/* Power-on settle before the first panel command, ms. No reset pin is
 * wired; a cold power-on needs this or the init sequence is lost. */
#define LCD_POWER_SETTLE_MS 500

/* ---- runtime drawing window ---- */
void LCD_SetWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
void LCD_ResetWindow(void);
uint16_t LCD_W(void);
uint16_t LCD_H(void);

/* ---- 24-bit colors (RGB888, converted to RGB565 by LCD_SetColor) -- */
#define LCD_WHITE       0xFFFFFF
#define LCD_BLACK       0x000000
#define LCD_BLUE        0x0000FF
#define LCD_GREEN       0x00FF00
#define LCD_RED         0xFF0000
#define LCD_CYAN        0x00FFFF
#define LCD_MAGENTA     0xFF00FF
#define LCD_YELLOW      0xFFFF00

/* ---- raw RGB565 colors (vendor screens use these directly) ---- */
#define C565_WHITE   0xFFFF
#define C565_BLACK   0x0000
#define C565_BLUE    0x001F
#define C565_RED     0xF800
#define C565_MAGENTA 0xF81F
#define C565_GREEN   0x07E0
#define C565_CYAN    0x7FFF
#define C565_YELLOW  0xFFE0
/* rk3506 lcm_test short names */
#define WHITE   C565_WHITE
#define BLACK   C565_BLACK
#define BLUE    C565_BLUE
#define RED     C565_RED
#define MAGENTA C565_MAGENTA
#define GREEN   C565_GREEN
#define CYAN    C565_CYAN
#define YELLOW  C565_YELLOW

#define ABS(X)  ((X) > 0 ? (X) : -(X))

/* ---- API: init / window / colors ---- */
uint32_t LCD_GetTick(void);
void LCD_RESET(void);
void LCD_IC_Init(void);
void LCD_Init(void);
void LCD_Reinit(void);   /* sleep-in + re-init */
void LCD_SetBrightness(uint8_t dbv);  /* DCS 51h, works at runtime */
void LCD_SetAddress(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);
void LCD_SetColor(uint32_t rgb888);
void LCD_SetBackColor(uint32_t rgb888);
void LCD_Clear(void);
void LCD_ClearRect(uint16_t x, uint16_t y, uint16_t width, uint16_t height);

/* ---- API: vendor demo screens ---- */
void DispColor(uint32_t color);
void DispFrame(void);
void DispGrayHor16(void);
void DispBand(void);
void StopDelay(uint16_t ms);

/* ---- API: ASCII text ---- */
void LCD_SetAsciiFont(pFONT *font);
void LCD_ShowTransparent(uint8_t mode);
void LCD_DisplayChar(uint16_t x, uint16_t y, uint8_t c);
void LCD_DisplayString(uint16_t x, uint16_t y, char *p);

/* ---- API: 2D drawing ---- */
void LCD_DrawPoint(uint16_t x, uint16_t y, uint32_t color);
void LCD_DrawLine_V(uint16_t x, uint16_t y, uint16_t height);
void LCD_DrawLine_H(uint16_t x, uint16_t y, uint16_t width);
void LCD_DrawLine(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);
void LCD_DrawRect(uint16_t x, uint16_t y, uint16_t width, uint16_t height);
void LCD_DrawCircle(uint16_t x, uint16_t y, uint16_t r);
void LCD_FillRect(uint16_t x, uint16_t y, uint16_t width, uint16_t height);
void LCD_FillCircle(uint16_t x, uint16_t y, uint16_t r);

/* ---- API: raw buffer blit (RGB565 words) ---- */
void LCD_CopyBuffer(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                    const uint16_t *data);

#endif /* CO5300_LCD_H */
