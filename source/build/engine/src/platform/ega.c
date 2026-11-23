#include "platform/ega.h"
#include "platform/vga.h"

#include <string.h>

#include "host.h"
#include "enhanced/enhanced.h"   /* ENH: */

static u8 planes[4][EGA_PLANE_SIZE];
static u16 start_addr;
static int line_compare = 200;
static bool dirty = true;
/* Mode 0Dh defaults after INT 10h AH=0 (200-line values), overscan 0. */
static u8 palette_reg[17] = { 0, 1, 2, 3, 4, 5, 6, 7, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0 };

const u32 ega_palette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

/* EGA plane byte with its eight bits spread into pixel nibbles, leftmost first. */
static const u32 ega_expand[256] = {
    0x00000000u, 0x10000000u, 0x01000000u, 0x11000000u, 0x00100000u, 0x10100000u, 0x01100000u, 0x11100000u,
    0x00010000u, 0x10010000u, 0x01010000u, 0x11010000u, 0x00110000u, 0x10110000u, 0x01110000u, 0x11110000u,
    0x00001000u, 0x10001000u, 0x01001000u, 0x11001000u, 0x00101000u, 0x10101000u, 0x01101000u, 0x11101000u,
    0x00011000u, 0x10011000u, 0x01011000u, 0x11011000u, 0x00111000u, 0x10111000u, 0x01111000u, 0x11111000u,
    0x00000100u, 0x10000100u, 0x01000100u, 0x11000100u, 0x00100100u, 0x10100100u, 0x01100100u, 0x11100100u,
    0x00010100u, 0x10010100u, 0x01010100u, 0x11010100u, 0x00110100u, 0x10110100u, 0x01110100u, 0x11110100u,
    0x00001100u, 0x10001100u, 0x01001100u, 0x11001100u, 0x00101100u, 0x10101100u, 0x01101100u, 0x11101100u,
    0x00011100u, 0x10011100u, 0x01011100u, 0x11011100u, 0x00111100u, 0x10111100u, 0x01111100u, 0x11111100u,
    0x00000010u, 0x10000010u, 0x01000010u, 0x11000010u, 0x00100010u, 0x10100010u, 0x01100010u, 0x11100010u,
    0x00010010u, 0x10010010u, 0x01010010u, 0x11010010u, 0x00110010u, 0x10110010u, 0x01110010u, 0x11110010u,
    0x00001010u, 0x10001010u, 0x01001010u, 0x11001010u, 0x00101010u, 0x10101010u, 0x01101010u, 0x11101010u,
    0x00011010u, 0x10011010u, 0x01011010u, 0x11011010u, 0x00111010u, 0x10111010u, 0x01111010u, 0x11111010u,
    0x00000110u, 0x10000110u, 0x01000110u, 0x11000110u, 0x00100110u, 0x10100110u, 0x01100110u, 0x11100110u,
    0x00010110u, 0x10010110u, 0x01010110u, 0x11010110u, 0x00110110u, 0x10110110u, 0x01110110u, 0x11110110u,
    0x00001110u, 0x10001110u, 0x01001110u, 0x11001110u, 0x00101110u, 0x10101110u, 0x01101110u, 0x11101110u,
    0x00011110u, 0x10011110u, 0x01011110u, 0x11011110u, 0x00111110u, 0x10111110u, 0x01111110u, 0x11111110u,
    0x00000001u, 0x10000001u, 0x01000001u, 0x11000001u, 0x00100001u, 0x10100001u, 0x01100001u, 0x11100001u,
    0x00010001u, 0x10010001u, 0x01010001u, 0x11010001u, 0x00110001u, 0x10110001u, 0x01110001u, 0x11110001u,
    0x00001001u, 0x10001001u, 0x01001001u, 0x11001001u, 0x00101001u, 0x10101001u, 0x01101001u, 0x11101001u,
    0x00011001u, 0x10011001u, 0x01011001u, 0x11011001u, 0x00111001u, 0x10111001u, 0x01111001u, 0x11111001u,
    0x00000101u, 0x10000101u, 0x01000101u, 0x11000101u, 0x00100101u, 0x10100101u, 0x01100101u, 0x11100101u,
    0x00010101u, 0x10010101u, 0x01010101u, 0x11010101u, 0x00110101u, 0x10110101u, 0x01110101u, 0x11110101u,
    0x00001101u, 0x10001101u, 0x01001101u, 0x11001101u, 0x00101101u, 0x10101101u, 0x01101101u, 0x11101101u,
    0x00011101u, 0x10011101u, 0x01011101u, 0x11011101u, 0x00111101u, 0x10111101u, 0x01111101u, 0x11111101u,
    0x00000011u, 0x10000011u, 0x01000011u, 0x11000011u, 0x00100011u, 0x10100011u, 0x01100011u, 0x11100011u,
    0x00010011u, 0x10010011u, 0x01010011u, 0x11010011u, 0x00110011u, 0x10110011u, 0x01110011u, 0x11110011u,
    0x00001011u, 0x10001011u, 0x01001011u, 0x11001011u, 0x00101011u, 0x10101011u, 0x01101011u, 0x11101011u,
    0x00011011u, 0x10011011u, 0x01011011u, 0x11011011u, 0x00111011u, 0x10111011u, 0x01111011u, 0x11111011u,
    0x00000111u, 0x10000111u, 0x01000111u, 0x11000111u, 0x00100111u, 0x10100111u, 0x01100111u, 0x11100111u,
    0x00010111u, 0x10010111u, 0x01010111u, 0x11010111u, 0x00110111u, 0x10110111u, 0x01110111u, 0x11110111u,
    0x00001111u, 0x10001111u, 0x01001111u, 0x11001111u, 0x00101111u, 0x10101111u, 0x01101111u, 0x11101111u,
    0x00011111u, 0x10011111u, 0x01011111u, 0x11011111u, 0x00111111u, 0x10111111u, 0x01111111u, 0x11111111u,
};

static bool compose(u32 *xrgb)
{
    if (!dirty) return false;
    dirty = false;
    u32 rgb[16];
    for (int i = 0; i < 16; i++) {
        u8 v = palette_reg[i];              /* 200-line decoding: bits 0-2 BGR, bit 4 intensity */
        rgb[i] = ega_palette[(v & 7) | (v >> 1 & 8)];
    }
    for (int y = 0; y < 200; y++) {
        u16 row = y < line_compare ? (u16)(start_addr + y * EGA_BYTES_PER_LINE)
                                   : (u16)((y - line_compare) * EGA_BYTES_PER_LINE);
        u32 *out = xrgb + y * 320;
        for (int b = 0; b < EGA_BYTES_PER_LINE; b++) {
            u16 o = (u16)(row + b);
            u32 colors = ega_expand[planes[0][o]] | ega_expand[planes[1][o]] << 1
                       | ega_expand[planes[2][o]] << 2 | ega_expand[planes[3][o]] << 3;
            out[0] = rgb[colors & 15];
            out[1] = rgb[(colors >> 4) & 15];
            out[2] = rgb[(colors >> 8) & 15];
            out[3] = rgb[(colors >> 12) & 15];
            out[4] = rgb[(colors >> 16) & 15];
            out[5] = rgb[(colors >> 20) & 15];
            out[6] = rgb[(colors >> 24) & 15];
            out[7] = rgb[colors >> 28];
            out += 8;
        }
    }
    return true;
}

static bool compose_enh(u32 *xrgb) { return enh_compose(compose, xrgb); }     /* ENH: scaled output frame */

void ega_show(void)
{
    dirty = true;
    if (enh_hires())                                                                /* ENH: */
        host_set_frame_source_logical(compose_enh, 320 * enh.res_scale, 200 * enh.res_scale, 320, 200);
    else
        host_set_frame_source(compose, 320, 200);
}

void ega_init(void)
{
    memset(planes, 0, sizeof planes);
    start_addr = 0;
    line_compare = 200;
    ega_show();
}

u8 *ega_plane(int plane) { return planes[plane & 3]; }

u16 ega_get_start(void) { return start_addr; }                                  /* ENH: */

void ega_set_start(u16 offset)
{
    if (offset != start_addr) { start_addr = offset; dirty = true; }
}

void ega_set_line_compare(int line)
{
    if (line < 0) line = 0;
    if (line > 200) line = 200;
    if (line != line_compare) { line_compare = line; dirty = true; }
}

void ega_touch(void) { dirty = true; }

void ega_set_palette_reg(int reg, u8 value)
{
    if (reg < 0 || reg > 16) return;
    if (palette_reg[reg] != value) { palette_reg[reg] = value; dirty = true; }
}

u8 ega_get_palette_reg(int reg)
{
    return (reg >= 0 && reg <= 16) ? palette_reg[reg] : 0;
}

u8 vga_rd(u16 off)
{
    for (int k = 0; k < 4; k++) vga.latch[k] = planes[k][off];
    if (vga.mode & 0x08) {                               /* read mode 1: colour compare */
        u8 differ = 0;
        for (int k = 0; k < 4; k++) {
            if (!(vga.dont_care >> k & 1)) continue;
            u8 want = (vga.compare >> k & 1) ? 0xFF : 0x00;
            differ |= (u8)(vga.latch[k] ^ want);
        }
        return (u8)~differ;
    }
    return vga.latch[vga.read_map & 3];
}

void vga_wr(u16 off, u8 v)
{
    const bool track = enh.track;
    u8 changed = 0;
    for (int k = 0; k < 4; k++) {
        if (!(vga.map_mask >> k & 1)) continue;
        u8 *p = &planes[k][off];
        u8 x;
        switch (vga.mode & 3) {
        case 1:                                          /* latches, no function / bit mask */
            if (track) changed |= *p ^ vga.latch[k];
            *p = vga.latch[k];
            continue;
        case 2:
            x = (v >> k & 1) ? 0xFF : 0x00;
            break;
        case 3:                                          /* VGA write mode 3: never used by the game */
            x = (vga.set_reset >> k & 1) ? 0xFF : 0x00;
            break;
        default:
            x = (vga.enable_sr >> k & 1) ? ((vga.set_reset >> k & 1) ? 0xFF : 0x00) : v;
            break;
        }
        u8 l = vga.latch[k];
        switch (vga.func >> 3 & 3) {
        case 1: x &= l; break;
        case 2: x |= l; break;
        case 3: x ^= l; break;
        default: break;
        }
        u8 m = vga.bit_mask;
        if ((vga.mode & 3) == 3) m &= v;
        u8 next = (u8)((x & m) | (l & (u8)~m));
        if (track) changed |= *p ^ next;
        *p = next;
    }
    if (track) enh_vram_write(off, changed);
    dirty = true;
}

void vga_fill_self(u16 seg, u16 off, u16 count)
{
    if (!count) return;
    u16 first = vram_off(seg, off);
    if (is_vram(seg) && count <= EGA_PLANE_SIZE - first && count <= EGA_PLANE_SIZE - off &&
        (vga.mode & 3) == 0 && (vga.func & 0x18) == 0 && vga.bit_mask == 0xff &&
        (vga.enable_sr & 15) == 15 && (vga.map_mask & 15) == 15 &&
        (!enh.track || enh_vram_fill(first, count))) {
        for (int k = 0; k < 4; k++) {
            /* The final read loads the byte before the final write, even with set/reset. */
            vga.latch[k] = planes[k][first + count - 1];
            memset(planes[k] + first, (vga.set_reset >> k & 1) ? 0xff : 0, count);
        }
        dirty = true;
        return;
    }
    while (count--) {
        vwr(seg, off, vrd(seg, off));
        off++;
    }
}

static unsigned fast_masked_blits;

unsigned vga_fast_masked_blits(void) { return fast_masked_blits; }

static u16 mask_word(const u8 *bytes, u16 offset)
{
    return (u16)(bytes[offset] | bytes[(u16)(offset + 1)] << 8);
}

bool vga_mask_or_rows(FarPtr d, u16 seg, u16 s, u16 m, s16 sstr, s16 wb, s16 h, s16 dstr)
{
    unsigned plane = vga.read_map & 3;
    unsigned write_plane = 1u << plane;
    if (!is_vram(d.seg) || (vga.mode & 0x0b) != 0 || (vga.func & 0x18) != 0 ||
        vga.bit_mask != 0xff || (vga.map_mask & 15) != write_plane || (vga.enable_sr & write_plane))
        return false;

    u8 *dst = planes[plane];
    const bool from_vram = is_vram(seg);
    const u8 *src = from_vram ? dst : mp(seg, 0);
    if (from_vram) {
        s = vram_off(seg, s);
        m = vram_off(seg, m);
    }
    u16 out = vram_off(d.seg, d.off);
    const bool track = enh.track;
    const bool odd = wb & 1;
    for (int row = 0; row < h; row++) {
        for (int i = wb - 2; i >= odd; i -= 2) {
            u16 offset = (u16)(out + i);
            u16 high = (u16)(offset + 1);
            u16 mask = mask_word(src, (u16)(m + i));
            u16 before = mask_word(dst, offset);
            u16 next = (u16)((mask & before) | mask_word(src, (u16)(s + i)));
            /* Full-byte replacement ignores latches. Preserve the final read for the next VGA operation. */
            if (row + 1 == h && i == 0) vga_rd(from_vram ? (u16)(s + 1) : high);
            dst[offset] = (u8)next;
            dst[high] = (u8)(next >> 8);
            if (track) {
                u16 changed = before ^ next;
                enh_vram_mask_write(offset, (u8)changed, (u8)~mask);
                enh_vram_mask_write(high, (u8)(changed >> 8), (u8)~(mask >> 8));
            }
        }
        if (odd) {
            u8 mask = src[m];
            u8 before = dst[out];
            u8 next = (u8)((mask & before) | src[s]);
            if (row + 1 == h) vga_rd(from_vram ? s : out);
            dst[out] = next;
            if (track) enh_vram_mask_write(out, before ^ next, (u8)~mask);
        }
        s = (u16)(s + sstr);
        m = (u16)(m + sstr);
        out = (u16)(out + dstr);
    }
    dirty = true;
    ++fast_masked_blits;
    return true;
}
