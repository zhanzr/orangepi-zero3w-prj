/*
  ft_text.h - FreeType text rendering (DejaVu Sans, the native Linux
  font already installed on the board) for the CO5300 panel demo.

  Replaces the hardcoded 8x16 raster font for the banner, info page,
  fps readout and logo caption: text is rasterized at any pixel size
  and alpha-blended (anti-aliased) into an RGB565 line buffer, then
  blitted with the normal row-chunked transactions (one CS frame per
  row, see spi_hw.h).
*/
#ifndef FT_TEXT_H
#define FT_TEXT_H

#include <stdint.h>

/* open the two faces used by the demo; paths as installed by the
 * fonts-dejavu-core package. returns 0 on success (both faces). */
int  ft_init(const char *regular_path, const char *bold_path);
void ft_close(void);

/* face selector for ft_draw/ft_width */
#define FT_FACE_REGULAR 0
#define FT_FACE_BOLD    1

/* draw one line of text with its top-left corner at (x, y); px is the
 * pixel size (em height). The line is composed over bg565 into a line
 * buffer (the panel is write-only, so true transparency is not
 * possible - anti-aliased edges blend toward bg565) and blitted.
 * Clipped to the 368-wide panel. Returns 0 on success. */
int  ft_draw(int face, uint16_t x, uint16_t y, uint16_t px, const char *s,
             uint16_t fg565, uint16_t bg565);

/* rendered width in pixels at the given size, for centering */
uint16_t ft_width(int face, uint16_t px, const char *s);

#endif /* FT_TEXT_H */
