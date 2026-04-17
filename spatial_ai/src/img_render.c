#include "img_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── small helpers ──────────────────────────────────────── */

static inline uint8_t clamp_u8(int v) {
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

/* Classify a channel value into tier 0..4 per the spec. */
static inline uint8_t tier_of(uint8_t v, ImgRenderTierSpec t) {
    if (v == 0)          return 0;
    if (v <= t.t1_max)   return 1;
    if (v <= t.t2_max)   return 2;
    if (v <= t.t3_max)   return 3;
    return 4;
}

/* ── defaults ───────────────────────────────────────────── */

ImgRenderOptions img_render_default_options(void) {
    ImgRenderOptions o;
    o.cell_px       = 4;
    o.tier_core     = (ImgRenderTierSpec){ 8, 32, 96 };
    o.tier_link     = (ImgRenderTierSpec){ 8, 32, 96 };
    o.tier_delta    = (ImgRenderTierSpec){ 8, 32, 96 };
    o.tier_priority = (ImgRenderTierSpec){ 8, 32, 96 };
    return o;
}

/* ── per-cell slot shape paint ──────────────────────────── */

static void paint_cell_rgb(uint8_t* dst, uint32_t row_stride_bytes,
                           uint8_t cell_px,
                           uint8_t core, uint8_t link,
                           uint8_t delta, uint8_t priority,
                           ImgRenderTierSpec ts_core,
                           ImgRenderTierSpec ts_link,
                           ImgRenderTierSpec ts_delta,
                           ImgRenderTierSpec ts_priority) {
    const uint8_t r_t = tier_of(core,     ts_core);
    const uint8_t g_t = tier_of(link,     ts_link);
    const uint8_t b_t = tier_of(delta,    ts_delta);
    const uint8_t a_t = tier_of(priority, ts_priority);

    const int cx0 = cell_px / 4;
    const int cx1 = cell_px - cell_px / 4;
    const int cy0 = cell_px / 4;
    const int cy1 = cell_px - cell_px / 4;
    const int mid_lo = cell_px / 2 - 1;
    const int mid_hi = cell_px / 2;
    const int corner_span = cell_px / 3;

    for (uint8_t py = 0; py < cell_px; py++) {
        for (uint8_t px = 0; px < cell_px; px++) {
            int rr = 0, gg = 0, bb = 0;

            const int in_center = (px >= cx0 && px < cx1 &&
                                   py >= cy0 && py < cy1);

            /* Cross: middle row/column. With even cell_px the "middle"
             * occupies two pixels so we test both mid_lo and mid_hi. */
            const int in_cross = (px == mid_hi || py == mid_hi ||
                                  (cell_px % 2 == 0 &&
                                   (px == mid_lo || py == mid_lo)));

            /* Corners: 4 corner squares of side corner_span. */
            const int in_corner =
                (px < corner_span  && py < corner_span) ||
                (px >= cell_px - corner_span && py < corner_span) ||
                (px < corner_span  && py >= cell_px - corner_span) ||
                (px >= cell_px - corner_span && py >= cell_px - corner_span);

            const int on_border = (px == 0 || py == 0 ||
                                   px == cell_px - 1 || py == cell_px - 1);

            /* Primary zone contributions. */
            if (r_t && in_center) {
                rr += 40 + 40 * r_t;
                gg +=  8 * r_t;
                bb +=  8 * r_t;
            }
            if (g_t && in_cross) {
                gg += 30 + 35 * g_t;
                rr +=  6 * g_t;
                bb +=  6 * g_t;
            }
            if (b_t && in_corner) {
                bb += 35 + 35 * b_t;
                rr +=  6 * b_t;
                gg +=  6 * b_t;
            }
            if (a_t && on_border) {
                rr += 10 * a_t;
                gg += 10 * a_t;
                bb += 10 * a_t;
            }

            /* Saturated tier (4) bleeds past its primary mask. */
            if (r_t >= 4 && !in_center)  rr += 24;
            if (g_t >= 4 && !in_cross)   gg += 20;
            if (b_t >= 4 && !in_corner)  bb += 20;
            if (a_t >= 4 && !on_border) { rr += 10; gg += 10; bb += 10; }

            /* Small direct magnitude contribution so mid-range values
             * are visible even when they haven't crossed their primary
             * zone threshold. */
            rr += core     >> 4;
            gg += link     >> 4;
            bb += delta    >> 4;
            /* Priority gently lifts all three channels (authority glow). */
            const int a_glow = priority >> 5;
            rr += a_glow;
            gg += a_glow;
            bb += a_glow;

            uint8_t* p = dst + (size_t)py * row_stride_bytes + (size_t)px * 3u;
            p[0] = clamp_u8(rr);
            p[1] = clamp_u8(gg);
            p[2] = clamp_u8(bb);
        }
    }
}

/* ── top-level render ───────────────────────────────────── */

int img_render_ce_grid(const ImgCEGrid* ce,
                       const ImgRenderOptions* opt_or_null,
                       ImgRenderImage* out_img) {
    if (!ce || !ce->cells || !out_img) return 0;

    const ImgRenderOptions opt = opt_or_null ? *opt_or_null
                                             : img_render_default_options();
    if (opt.cell_px < 2) return 0;

    const uint32_t w = ce->width  * opt.cell_px;
    const uint32_t h = ce->height * opt.cell_px;
    const size_t   bytes = (size_t)w * (size_t)h * 3u;

    uint8_t* rgb = (uint8_t*)malloc(bytes);
    if (!rgb) return 0;
    memset(rgb, 0, bytes);

    const uint32_t row_stride = w * 3u;

    for (uint32_t cy = 0; cy < ce->height; cy++) {
        for (uint32_t cx = 0; cx < ce->width; cx++) {
            const ImgCECell* c = &ce->cells[img_ce_idx(cy, cx)];

            uint8_t* dst = rgb + ((size_t)cy * opt.cell_px) * row_stride
                               + ((size_t)cx * opt.cell_px) * 3u;

            paint_cell_rgb(dst, row_stride, opt.cell_px,
                           c->core, c->link, c->delta, c->priority,
                           opt.tier_core, opt.tier_link,
                           opt.tier_delta, opt.tier_priority);
        }
    }

    out_img->width  = w;
    out_img->height = h;
    out_img->rgb    = rgb;
    return 1;
}

int img_render_save_ppm(const char* path, const ImgRenderImage* img) {
    if (!path || !img || !img->rgb) return 0;
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "P6\n%u %u\n255\n", img->width, img->height);
    const size_t n = (size_t)img->width * (size_t)img->height * 3u;
    size_t wrote = fwrite(img->rgb, 1, n, f);
    fclose(f);
    return (wrote == n) ? 1 : 0;
}

void img_render_free_image(ImgRenderImage* img) {
    if (!img) return;
    if (img->rgb) free(img->rgb);
    img->rgb    = NULL;
    img->width  = 0;
    img->height = 0;
}
