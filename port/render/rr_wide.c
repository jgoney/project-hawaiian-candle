/* The wide picture's ground: see rr_wide.h. */
#include "rr_wide.h"

void rr_wide_dashboard(RrWide *w) {
    w->dash.first = 179;
    w->dash.end = 220;
    w->dash.left_from = 64;
    w->dash.right_from = 255;
    w->dash.columns = 3;
    w->dash.fill = 50;
}

void rr_wide_blocks(RrWideBlock block[2]) {
    block[0] = (RrWideBlock){.first = 352, .end = 672, .mark = 24, .road_high = 1, .period = 8,
                             .fit_low = -352, .fit_high = 320};
    block[1] = (RrWideBlock){.first = 352, .end = 672, .mark = 24, .road_high = 0, .period = 8,
                             .fit_low = 0, .fit_high = 384};
}

int rr_wide_is_ground_row(const RrVdp *v, int which, int line, const RrWideBlock *b) {
    return rr_vdp_is_block_row(v, which, line, b);
}

void rr_wide_centre(const uint32_t *wide_px, const uint32_t *narrow_px, int scale, int margin, int lines,
                    const uint8_t *have, const uint8_t *object_cover, const RrStrips *strips, RrWideCentre *out) {
    (void)lines; /* every row's middle is the narrow row, wide line or not */
    size_t across = (size_t)RR_SCREEN_W * (size_t)scale, stride = across + 2 * (size_t)margin * (size_t)scale;
    if (strips != NULL && strips->scale != scale) strips = NULL;
    for (int row = 0; row < RR_SCREEN_H * scale; row++) {
        const uint32_t *wide = wide_px + (size_t)row * stride + (size_t)margin * (size_t)scale;
        const uint32_t *narrow = narrow_px + (size_t)row * across;
        out->pixels += across;
        for (size_t c = 0; c < across; c++) {
            if (wide[c] == narrow[c]) continue;
            out->differing++;
            if (object_cover != NULL && object_cover[(size_t)row * across + c]) out->object++;
            else if (strips != NULL && (int64_t)c >= strips->from[row] && (int64_t)c < strips->to[row]) out->dash++;
            else if (have != NULL && have[row / scale]) out->road_line++;
            else out->other++;
        }
    }
}

enum { GRASS = 1, MARK = 2 };

/* What a plane's pixel is, by its tile: one of the row's block cells is
 * grass, one of the cells beside the block (where the marks are) a mark. */
static int kind_of(const RrVdp *v, int which, int line, const RrWideBlock *b, uint16_t entry) {
    uint32_t base = which == 0 ? (uint32_t)(v->reg[2] & 0x38) << 10 : (uint32_t)(v->reg[4] & 7) << 13;
    unsigned width = 128, py = ((unsigned)line + (v->vsram[which] & 0x3FF)) & 0xFF;
    int mark_cells = (b->mark + 7) / 8, found = 0;
    for (int cell = b->first / 8 - mark_cells; cell < b->end / 8 + mark_cells; cell++) {
        uint32_t at = (base + ((py >> 3) * width + ((unsigned)cell & (width - 1))) * 2) & 0xFFFF;
        if ((uint16_t)(v->vram[at] << 8 | v->vram[(at + 1) & 0xFFFF]) != entry) continue;
        if (cell >= b->first / 8 && cell < b->end / 8) return GRASS;
        found = MARK;
    }
    return found;
}

void rr_wide_ground(const RrPixelSource *sources, const RrRoadShifts *road, const RrVdp *v, int line,
                    int scale, int margin, RrWideGroundCount *out) {
    if (road == NULL || road->scale != scale || line < 0 || line >= RR_SCREEN_H || !road->have[line]) return;
    RrWideBlock block[2];
    rr_wide_blocks(block);
    int stride = (RR_SCREEN_W + 2 * margin) * scale, beyond = 0;
    for (int r = 0; r < scale; r++) {
        int at = line * scale + r;
        if (road->beyond_a[at] != 0 || road->beyond_b[at] != 0) beyond = 1;
        /* The edges in output pixels from the screen's left edge, rounded down as the picture's are. */
        int64_t left = (((int64_t)road->a[at] + road->beyond_a[at]) * scale >> 16) + (int64_t)0x2A0 * scale;
        int64_t right = (((int64_t)road->b[at] + road->beyond_b[at]) * scale >> 16) - (int64_t)0x2A0 * scale;
        const RrPixelSource *row = sources + (size_t)r * (size_t)stride;
        for (int c = 0; c < stride; c++) {
            if (c == margin * scale) c = (margin + RR_SCREEN_W) * scale; /* over the screen itself */
            if (c >= stride) break;
            const RrPixelSource *p = &row[c];
            if (p->layer == RR_SRC_SPRITE) continue;
            int64_t x = c - (int64_t)margin * scale;
            out->examined++;
            if (p->layer == RR_SRC_BACKDROP) {
                if (x >= left && x < right) out->road++;
                else out->wrong++;
            } else if (p->layer == RR_SRC_PLANE_A || p->layer == RR_SRC_PLANE_B) {
                int which = p->layer == RR_SRC_PLANE_A ? 0 : 1;
                int kind = kind_of(v, which, line, &block[which], p->detail);
                int64_t reach = (int64_t)block[which].mark * scale;
                if (kind == GRASS && (which == 0 ? x < left : x >= right)) out->grass++;
                else if (kind == MARK && (which == 0 ? x >= left && x < left + reach : x >= right - reach && x < right))
                    out->mark++;
                else out->wrong++;
            } else {
                out->wrong++;
            }
        }
    }
    out->beyond_lines += (unsigned)beyond;
}

void rr_wide_sky(const RrPixelSource *sources, int scale, int margin, RrWideSky *out) {
    int stride = (RR_SCREEN_W + 2 * margin) * scale;
    for (int r = 0; r < scale; r++) {
        const RrPixelSource *row = sources + (size_t)r * (size_t)stride;
        for (int c = 0; c < stride; c++) {
            if (c == margin * scale) c = (margin + RR_SCREEN_W) * scale;
            if (c >= stride) break;
            out->examined++;
            if (row[c].layer == RR_SRC_PANEL) out->panel++;
            else if (row[c].layer == RR_SRC_PLANE_A || row[c].layer == RR_SRC_PLANE_B) out->plane++;
            else if (row[c].layer == RR_SRC_BACKDROP) out->backdrop++;
        }
    }
}
