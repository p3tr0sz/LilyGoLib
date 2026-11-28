"""Builds the watch's accent *title* fonts: the Alibaba display face with Latin-1,
Latin Extended-A and a few punctuation marks, one font per title size.

This is the title-font twin of gen_latin_fonts.py (which does the same for the
Montserrat *body* fonts). Page titles, Wi-Fi SSIDs and the watch-face date are
drawn with the Alibaba display font, whose committed tables (src/font/
font_alibaba_<size>_v9.c) only cover ASCII (range 32-127), so an accented
character (é, ą, ö, …) renders as a tofu box.

Each generated font holds *only* the accented Latin glyphs and falls back to the
stock ASCII font_alibaba_<size>, so plain text and digits look exactly as before.
ui_define.h points font_alibaba_<size> at these on the watch (FACTORY_WATCH_UI).

Only the title sizes 12 and 24 px are extended: 24 px covers page titles, SSIDs
and the watch-face date; 12 px covers small/round-screen titles. The 60 and 100
px fonts are the clock, which only ever renders digits and a colon, so extending
them would be pure flash waste on a flash-tight target.

The committed Alibaba fonts were produced with lv_font_conv from
ALIBABA-PUHUITI-BOLD.TTF (see each .c file's `Opts:` banner). That TTF is not
vendored in this repo; obtain the same face and pass its path:

Run from examples/factory:
    python3 -I tools/gen_title_latin_fonts.py <path/to/ALIBABA-PUHUITI-BOLD.TTF>

Writes src/font/font_title_latin.c (the glyph tables) and src/font/
font_title_latin.h (a sentinel header that ui_define.h keys off via __has_include
and that declares the new symbols). Nothing is written if the TTF is missing;
the script never fabricates a font table.
"""
import pathlib
import re
import sys

from PIL import ImageDraw, ImageFont, Image

SIZES = [12, 24]                        # title sizes only (see module docstring)
RANGE = (0xA0, 0x17F)                   # Latin-1 Supplement and Latin Extended-A
EXTRA = [0x2013, 0x2014, 0x2018, 0x2019, 0x201A, 0x201C, 0x201D, 0x201E, 0x2026, 0x20AC]
FONT_DIR = pathlib.Path(__file__).resolve().parent.parent / "src" / "font"
OUT_C = FONT_DIR / "font_title_latin.c"
OUT_H = FONT_DIR / "font_title_latin.h"


def base_metrics(size):
    """line_height etc. of the stock font_alibaba_<size>, so accents align with ASCII."""
    text = (FONT_DIR / ("font_alibaba_%d_v9.c" % size)).read_text()
    get = lambda key: int(re.search(r"\." + key + r" = (-?\d+)", text).group(1))
    return {k: get(k) for k in ("line_height", "base_line", "underline_position", "underline_thickness")}


def glyph(font, code):
    ch = chr(code)
    adv = round(font.getlength(ch) * 16)
    x0, y0, x1, y1 = font.getbbox(ch, anchor="ls")
    w, h = x1 - x0, y1 - y0
    if w <= 0 or h <= 0:
        return adv, 0, 0, 0, 0, []
    img = Image.new("L", (w, h), 0)
    ImageDraw.Draw(img).text((-x0, -y0), ch, font=font, fill=255, anchor="ls")
    px = [min(15, (v + 8) >> 4) for v in img.tobytes()]
    return adv, w, h, x0, -y1, px


def pack4(px):
    if len(px) % 2:
        px = px + [0]
    return [(px[i] << 4) | px[i + 1] for i in range(0, len(px), 2)]


def font_source(ttf, size):
    font = ImageFont.truetype(str(ttf), size)
    codes = list(range(RANGE[0], RANGE[1] + 1)) + EXTRA
    bitmap, dsc = [], ["    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0}"]
    for code in codes:
        adv, w, h, ox, oy, px = glyph(font, code)
        dsc.append("    {.bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d, .ofs_x = %d, .ofs_y = %d}"
                   % (len(bitmap), adv, w, h, ox, oy))
        bitmap += pack4(px)
    m = base_metrics(size)
    n = "font_alibaba_latin_%d" % size
    rows = [", ".join("0x%02x" % b for b in bitmap[i:i + 24]) for i in range(0, len(bitmap), 24)]
    extra = ", ".join("0x%x" % (c - EXTRA[0]) for c in EXTRA)
    return """
/* ── %(size)d px ── */
static LV_ATTRIBUTE_LARGE_CONST const uint8_t %(n)s_bitmap[] = {
    %(bitmap)s
};

static const lv_font_fmt_txt_glyph_dsc_t %(n)s_glyph_dsc[] = {
%(dsc)s
};

static const uint16_t %(n)s_extra_list[] = {%(extra)s};

static const lv_font_fmt_txt_cmap_t %(n)s_cmaps[] = {
    {.range_start = 0x%(r0)X, .range_length = %(rlen)d, .glyph_id_start = 1,
     .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY},
    {.range_start = 0x%(e0)X, .range_length = %(elen)d, .glyph_id_start = %(eid)d,
     .unicode_list = %(n)s_extra_list, .glyph_id_ofs_list = NULL, .list_length = %(ecount)d,
     .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY},
};

static const lv_font_fmt_txt_dsc_t %(n)s_dsc = {
    .glyph_bitmap = %(n)s_bitmap,
    .glyph_dsc = %(n)s_glyph_dsc,
    .cmaps = %(n)s_cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 2,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 0,
};

const lv_font_t %(n)s = {
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = %(line_height)d,
    .base_line = %(base_line)d,
    .subpx = LV_FONT_SUBPX_NONE,
    .underline_position = %(underline_position)d,
    .underline_thickness = %(underline_thickness)d,
    .dsc = &%(n)s_dsc,
    .fallback = &font_alibaba_%(size)d,   /* ASCII, digits and symbols */
};
""" % dict(size=size, n=n, bitmap=",\n    ".join(rows), dsc=",\n".join(dsc), extra=extra,
           r0=RANGE[0], rlen=RANGE[1] - RANGE[0] + 1, e0=EXTRA[0], elen=EXTRA[-1] - EXTRA[0] + 1,
           eid=1 + RANGE[1] - RANGE[0] + 1, ecount=len(EXTRA), **m)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    ttf = pathlib.Path(sys.argv[1])
    if not ttf.is_file():
        sys.exit("ERROR: Alibaba TTF not found: %s\n"
                 "Obtain the same ALIBABA-PUHUITI-BOLD.TTF used for the committed\n"
                 "font_alibaba_*.c files and pass its path." % ttf)

    body = ["/* Generated by tools/gen_title_latin_fonts.py; edit that instead.",
            " * Alibaba PuHuiTi Bold, accents only; falls back to the stock ASCII",
            " * font_alibaba_<size>. See the script. */",
            "#include <lvgl.h>",
            "",
            "#if defined(ARDUINO_T_WATCH_S3_ULTRA)   /* The watch UI only (FACTORY_WATCH_UI) */",
            ""]
    body += ["LV_FONT_DECLARE(font_alibaba_%d);" % s for s in SIZES]
    body += [font_source(ttf, s) for s in SIZES]
    body.append("#endif\n")
    OUT_C.write_text("\n".join(body))

    hdr = ["/* Generated by tools/gen_title_latin_fonts.py; edit that instead.",
           " * Its presence switches watch page titles to the Latin-extended Alibaba",
           " * fonts (see ui_define.h). */",
           "#pragma once",
           "#include <lvgl.h>"]
    hdr += ["LV_FONT_DECLARE(font_alibaba_latin_%d);" % s for s in SIZES]
    OUT_H.write_text("\n".join(hdr) + "\n")

    print("wrote", OUT_C, "(%d KB)" % (OUT_C.stat().st_size // 1024))
    print("wrote", OUT_H)


if __name__ == "__main__":
    main()
