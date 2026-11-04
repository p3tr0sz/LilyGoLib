"""Builds the watch's accent fonts: Montserrat Medium with Latin-1, Latin Extended-A
and a few punctuation marks, one font per size the factory app uses.

Each font holds only those glyphs and falls back to LVGL's built-in
lv_font_montserrat_<size> (ASCII and the symbols), so plain text looks exactly
as before. ui_define.h points lv_font_montserrat_<size> at these on the watch.

Run from examples/factory:
    python3 -I tools/gen_latin_fonts.py <lvgl dir>
<lvgl dir> is an LVGL checkout, e.g. the PlatformIO libdeps copy; it provides
scripts/built_in_font/Montserrat-Medium.ttf (the font the built-ins are made from)
and the built-ins' line metrics.
"""
import pathlib
import re
import sys

from PIL import Image, ImageDraw, ImageFont

SIZES = [10, 12, 14, 16, 18, 20, 22, 24, 28, 32, 48]
RANGE = (0xA0, 0x17F)                   # Latin-1 Supplement and Latin Extended-A
EXTRA = [0x2013, 0x2014, 0x2018, 0x2019, 0x201A, 0x201C, 0x201D, 0x201E, 0x2026, 0x20AC]
OUT = pathlib.Path(__file__).resolve().parent.parent / "src" / "font" / "font_latin.c"


def builtin_metrics(lvgl, size):
    text = (lvgl / "src" / "font" / ("lv_font_montserrat_%d.c" % size)).read_text()
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


def font_source(ttf, lvgl, size):
    font = ImageFont.truetype(str(ttf), size)
    codes = list(range(RANGE[0], RANGE[1] + 1)) + EXTRA
    bitmap, dsc = [], ["    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0}"]
    for code in codes:
        adv, w, h, ox, oy, px = glyph(font, code)
        dsc.append("    {.bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d, .ofs_x = %d, .ofs_y = %d}"
                   % (len(bitmap), adv, w, h, ox, oy))
        bitmap += pack4(px)
    m = builtin_metrics(lvgl, size)
    n = "font_latin_%d" % size
    rows = [", ".join("0x%02x" % b for b in bitmap[i:i + 24]) for i in range(0, len(bitmap), 24)]
    extra = ", ".join("0x%x" % (c - EXTRA[0]) for c in EXTRA)
    return """
/* ── %(size)d px ── */
#if LV_FONT_MONTSERRAT_%(size)d
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
    .fallback = &lv_font_montserrat_%(size)d,   /* ASCII and symbols */
};
#endif
""" % dict(size=size, n=n, bitmap=",\n    ".join(rows), dsc=",\n".join(dsc), extra=extra,
           r0=RANGE[0], rlen=RANGE[1] - RANGE[0] + 1, e0=EXTRA[0], elen=EXTRA[-1] - EXTRA[0] + 1,
           eid=1 + RANGE[1] - RANGE[0] + 1, ecount=len(EXTRA), **m)


def check(ttf, lvgl, size):
    """Our rasterizer must match lv_font_conv's metrics, or accents would sit oddly."""
    text = (lvgl / "src" / "font" / ("lv_font_montserrat_%d.c" % size)).read_text()
    # Glyph id 34 is 'A' (ids start at 1 for U+0020).
    want = re.findall(r"\{\.bitmap_index = \d+, \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), "
                      r"\.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}", text)[34]
    adv, w, h, ox, oy, _ = glyph(ImageFont.truetype(str(ttf), size), ord("A"))
    got = (adv, w, h, ox, oy)
    print("  %2d px 'A': built-in %s, ours %s" % (size, tuple(int(v) for v in want), got))


def main():
    lvgl = pathlib.Path(sys.argv[1])
    ttf = lvgl / "scripts" / "built_in_font" / "Montserrat-Medium.ttf"
    for size in (12, 24, 48):
        check(ttf, lvgl, size)
    body = ["/* Generated by tools/gen_latin_fonts.py; edit that instead.",
            " * Montserrat Medium (SIL Open Font License 1.1), accents only; see the script. */",
            "#include <lvgl.h>",
            "",
            "#if defined(ARDUINO_T_WATCH_S3_ULTRA)   /* The watch UI only (FACTORY_WATCH_UI) */"]
    body += [font_source(ttf, lvgl, s) for s in SIZES]
    body.append("#endif\n")
    OUT.write_text("\n".join(body))
    print("wrote", OUT, "(%d KB)" % (OUT.stat().st_size // 1024))


if __name__ == "__main__":
    main()
