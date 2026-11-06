/*
  ft_text.c - FreeType text rendering for the CO5300 demo (see .h).

  The board ships libfreetype.so.6 and /usr/share/fonts/truetype/
  dejavu/ - the standard Linux font stack. Glyphs are rendered at the
  requested pixel size, alpha-blended into an RGB565 line buffer and
  pushed with LCD_CopyBuffer (row-chunked wrapped transactions).
*/
#include <ft2build.h>
#include FT_FREETYPE_H

#include <stdio.h>
#include <string.h>

#include "ft_text.h"
#include "lcd.h"
#include "log.h"

static FT_Library s_lib;
static FT_Face    s_face[2];   /* FT_FACE_REGULAR, FT_FACE_BOLD */

/* line staging buffer: full panel width, up to 72 px tall lines */
#define LINE_MAX_W 368
#define LINE_MAX_H 96
static uint16_t s_line[LINE_MAX_W * LINE_MAX_H];

static uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t g)
{
    uint32_t gr = g;

    uint32_t r = (((fg >> 11) & 0x1F) * gr + ((bg >> 11) & 0x1F) * (255 - gr)) >> 8;
    uint32_t gg = (((fg >> 5) & 0x3F) * gr + ((bg >> 5) & 0x3F) * (255 - gr)) >> 8;
    uint32_t b = ((fg & 0x1F) * gr + (bg & 0x1F) * (255 - gr)) >> 8;

    return (uint16_t)((r << 11) | (gg << 5) | b);
}

int ft_init(const char *regular_path, const char *bold_path)
{
    if (FT_Init_FreeType(&s_lib) != 0)
    {
        log_printf(LOG_LVL_ERROR, "[FT] init failed");
        return -1;
    }
    if (FT_New_Face(s_lib, regular_path, 0, &s_face[FT_FACE_REGULAR]) != 0 ||
        FT_New_Face(s_lib, bold_path, 0, &s_face[FT_FACE_BOLD]) != 0)
    {
        log_printf(LOG_LVL_ERROR, "[FT] cannot open %s / %s",
                   regular_path, bold_path);
        return -1;
    }
    log_printf(LOG_LVL_INFO, "[FT] fonts: %s + %s (%.20s)",
               regular_path, bold_path,
               s_face[FT_FACE_REGULAR]->family_name);
    return 0;
}

void ft_close(void)
{
    for (int i = 0; i < 2; i++)
        if (s_face[i])
            FT_Done_Face(s_face[i]);
    if (s_lib)
        FT_Done_FreeType(s_lib);
    s_face[0] = s_face[1] = NULL;
}

/* compose one line into s_line and blit */
int ft_draw(int face, uint16_t x, uint16_t y, uint16_t px, const char *s,
            uint16_t fg565, uint16_t bg565)
{
    FT_Face f = s_face[face];
    uint16_t w;
    uint32_t pen_x = 0;
    int glyph_over = 0;

    if (!f || !s || !*s)
        return -1;
    if (FT_Set_Pixel_Sizes(f, 0, px) != 0)
        return -1;

    w = ft_width(face, px, s);
    if (w > LINE_MAX_W)
        w = LINE_MAX_W;
    if (w == 0)
        return -1;

    /* line height = ascender..descender (the full em box); the baseline
     * sits `asc` px below the buffer top, so glyphs render complete */
    int32_t asc  = (int32_t)(f->size->metrics.ascender >> 6);
    int32_t desc = (int32_t)(f->size->metrics.descender >> 6); /* <= 0 */
    uint16_t h = (uint16_t)(asc - desc + 1);
    if (h > LINE_MAX_H)
        h = LINE_MAX_H;
    uint16_t base = (uint16_t)((asc > h) ? h - 1 : asc);

    /* bg fill */
    for (uint32_t i = 0; i < (uint32_t)w * h; i++)
        s_line[i] = bg565;

    for (const char *p = s; *p; p++)
    {
        uint8_t c = (uint8_t)*p;

        if (c < 0x20)
            continue;
        if (FT_Load_Char(f, c, FT_LOAD_RENDER) != 0)
            continue;
        FT_GlyphSlot slot = f->glyph;
        FT_Bitmap *bm = &slot->bitmap;

        if (slot->bitmap_left < 0 || pen_x > 0x10000)
            glyph_over = 1;
        if (glyph_over)
            break;

        for (uint32_t r = 0; r < bm->rows; r++)
        {
            int32_t dy = base - slot->bitmap_top + (int32_t)r;

            if (dy < 0 || dy >= h)
                continue;
            for (uint32_t cx = 0; cx < bm->width; cx++)
            {
                int32_t dx = (int32_t)pen_x + slot->bitmap_left + (int32_t)cx;

                if (dx < 0 || dx >= w)
                    continue;
                uint8_t g = (bm->pixel_mode == FT_PIXEL_MODE_GRAY)
                                ? bm->buffer[r * bm->pitch + cx]
                                : 255;
                s_line[(uint32_t)dy * w + dx] =
                    blend565(fg565, bg565, g);
            }
        }
        pen_x = (uint32_t)((int64_t)pen_x + (slot->advance.x >> 6));
        if (pen_x >= w)
            break;
    }

    LCD_CopyBuffer(x, y, w, h, s_line);
    return 0;
}

uint16_t ft_width(int face, uint16_t px, const char *s)
{
    FT_Face f = s_face[face];
    uint32_t w = 0;

    if (!f || !s)
        return 0;
    if (FT_Set_Pixel_Sizes(f, 0, px) != 0)
        return 0;
    for (const char *p = s; *p; p++)
    {
        if ((uint8_t)*p < 0x20)
            continue;
        if (FT_Load_Char(f, (uint8_t)*p, FT_LOAD_RENDER) != 0)
            continue;
        w = (uint32_t)((int64_t)w + (f->glyph->advance.x >> 6));
    }
    return (uint16_t)w;
}
