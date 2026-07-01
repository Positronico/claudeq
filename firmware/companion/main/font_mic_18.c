/*******************************************************************************
 * Size: 18 px
 * Bpp: 4
 * Opts: --font fa-solid-900.ttf --size 18 --bpp 4 --format lvgl --range 0xF130 --no-compress --lv-include lvgl.h -o font_mic_18.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl.h"
#endif

#ifndef FONT_MIC_18
#define FONT_MIC_18 1
#endif

#if FONT_MIC_18

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+F130 "" */
    0x0, 0x0, 0x2a, 0xdb, 0x30, 0x0, 0x0, 0x0,
    0x2f, 0xff, 0xff, 0x30, 0x0, 0x0, 0x9, 0xff,
    0xff, 0xfc, 0x0, 0x0, 0x0, 0xcf, 0xff, 0xff,
    0xe0, 0x0, 0x0, 0xc, 0xff, 0xff, 0xff, 0x0,
    0x0, 0x0, 0xcf, 0xff, 0xff, 0xf0, 0x0, 0x0,
    0xc, 0xff, 0xff, 0xff, 0x0, 0x7, 0xe0, 0xcf,
    0xff, 0xff, 0xf0, 0xca, 0x9f, 0x1c, 0xff, 0xff,
    0xff, 0xe, 0xc9, 0xf1, 0xbf, 0xff, 0xff, 0xd0,
    0xfb, 0x6f, 0x57, 0xff, 0xff, 0xf9, 0x2f, 0x91,
    0xfc, 0xb, 0xff, 0xfc, 0x1a, 0xf4, 0x8, 0xfa,
    0x4, 0x64, 0x8, 0xfb, 0x0, 0xa, 0xfe, 0x97,
    0x9d, 0xfc, 0x0, 0x0, 0x6, 0xdf, 0xff, 0xe7,
    0x0, 0x0, 0x0, 0x0, 0x5f, 0x70, 0x0, 0x0,
    0x0, 0x4, 0xab, 0xfc, 0xa6, 0x0, 0x0, 0x0,
    0x9f, 0xff, 0xff, 0xc0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 216, .box_w = 13, .box_h = 19, .ofs_x = 0, .ofs_y = -3}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 61744, .range_length = 1, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t font_mic_18 = {
#else
lv_font_t font_mic_18 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 19,          /*The maximum line height required by the font*/
    .base_line = 3,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -1,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if FONT_MIC_18*/

