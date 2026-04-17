#include "img_render.h"
#include "img_ce.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_passed = 0;
static int tests_total  = 0;

#define TEST(name) do {                 \
    tests_total++;                      \
    printf("  [TEST] %s ... ", name);   \
} while (0)

#define PASS() do {                     \
    tests_passed++;                     \
    printf("PASS\n");                   \
} while (0)

/* ── helpers ─────────────────────────────────────────────── */

static unsigned long long sum_rgb(const ImgRenderImage* img) {
    unsigned long long s = 0;
    size_t n = (size_t)img->width * (size_t)img->height * 3u;
    for (size_t i = 0; i < n; i++) s += img->rgb[i];
    return s;
}

static void set_single_cell(ImgCEGrid* ce, uint32_t cy, uint32_t cx,
                            uint8_t core, uint8_t link,
                            uint8_t delta, uint8_t priority) {
    ImgCECell* c = &ce->cells[img_ce_idx(cy, cx)];
    memset(c, 0, sizeof(*c));
    c->core     = core;
    c->link     = link;
    c->delta    = delta;
    c->priority = priority;
    c->last_delta_id = IMG_DELTA_ID_NONE;
}

/* Sum channel c (0=R,1=G,2=B) of one rendered cell block. */
static unsigned long long cell_block_channel_sum(const ImgRenderImage* img,
                                                 uint8_t cell_px,
                                                 uint32_t cx, uint32_t cy,
                                                 int ch) {
    unsigned long long s = 0;
    const uint32_t row_stride = img->width * 3u;
    for (uint8_t py = 0; py < cell_px; py++) {
        for (uint8_t px = 0; px < cell_px; px++) {
            const uint32_t x = cx * cell_px + px;
            const uint32_t y = cy * cell_px + py;
            s += img->rgb[(size_t)y * row_stride + (size_t)x * 3u + ch];
        }
    }
    return s;
}

/* ── defaults ───────────────────────────────────────────── */

static void test_default_options(void) {
    TEST("default options: cell_px=4, tier thresholds {8,32,96}");

    ImgRenderOptions o = img_render_default_options();
    assert(o.cell_px == 4);
    assert(o.tier_core.t1_max     ==  8);
    assert(o.tier_core.t2_max     == 32);
    assert(o.tier_core.t3_max     == 96);
    assert(o.tier_link.t1_max     ==  8);
    assert(o.tier_delta.t2_max    == 32);
    assert(o.tier_priority.t3_max == 96);

    PASS();
}

/* ── empty grid → all-zero image ─────────────────────────── */

static void test_empty_grid_is_black(void) {
    TEST("empty CE grid renders to all-zero pixels");

    ImgCEGrid* ce = img_ce_grid_create();
    assert(ce);

    ImgRenderImage img = {0};
    assert(img_render_ce_grid(ce, NULL, &img));
    assert(img.width  == IMG_CE_SIZE * 4);
    assert(img.height == IMG_CE_SIZE * 4);
    assert(sum_rgb(&img) == 0);

    img_render_free_image(&img);
    img_ce_grid_destroy(ce);
    PASS();
}

/* ── slot shape: channel-specific mass shows in its zone ─ */

static void test_slot_shape_per_channel(void) {
    TEST("each channel lights its own zone more than others");

    ImgCEGrid* ce = img_ce_grid_create();
    assert(ce);

    /* Put each channel alone in a different cell so we can isolate
     * which image zone it lights up. Tier-1 magnitude (value = 8)
     * stays inside the primary mask and avoids tier-4 bleed. */
    set_single_cell(ce, /*cy=*/10, /*cx=*/10, /*core*/    8, 0, 0, 0);
    set_single_cell(ce, /*cy=*/10, /*cx=*/20, /*link*/    0, 8, 0, 0);
    set_single_cell(ce, /*cy=*/20, /*cx=*/10, /*delta*/   0, 0, 8, 0);
    set_single_cell(ce, /*cy=*/20, /*cx=*/20, /*priority*/0, 0, 0, 8);

    ImgRenderImage img = {0};
    ImgRenderOptions opt = img_render_default_options();
    opt.cell_px = 8;                   /* bigger block reveals zones */
    assert(img_render_ce_grid(ce, &opt, &img));

    /* Channel-sum helper args: (img, cell_px, cx, cy, ch).
     * Core cell at (cy=10, cx=10): R dominates (center mass). */
    {
        unsigned long long r = cell_block_channel_sum(&img, opt.cell_px, 10, 10, 0);
        unsigned long long g = cell_block_channel_sum(&img, opt.cell_px, 10, 10, 1);
        unsigned long long b = cell_block_channel_sum(&img, opt.cell_px, 10, 10, 2);
        assert(r > g && r > b);
    }

    /* Link cell at (cy=10, cx=20): G dominates (cross). */
    {
        unsigned long long r = cell_block_channel_sum(&img, opt.cell_px, 20, 10, 0);
        unsigned long long g = cell_block_channel_sum(&img, opt.cell_px, 20, 10, 1);
        unsigned long long b = cell_block_channel_sum(&img, opt.cell_px, 20, 10, 2);
        assert(g > r && g > b);
    }

    /* Delta cell at (cy=20, cx=10): B dominates (corners). */
    {
        unsigned long long r = cell_block_channel_sum(&img, opt.cell_px, 10, 20, 0);
        unsigned long long g = cell_block_channel_sum(&img, opt.cell_px, 10, 20, 1);
        unsigned long long b = cell_block_channel_sum(&img, opt.cell_px, 10, 20, 2);
        assert(b > r && b > g);
    }

    /* Priority cell at (cy=20, cx=20): authority glow + border lift
     * paints R, G, B equally. */
    {
        unsigned long long r = cell_block_channel_sum(&img, opt.cell_px, 20, 20, 0);
        unsigned long long g = cell_block_channel_sum(&img, opt.cell_px, 20, 20, 1);
        unsigned long long b = cell_block_channel_sum(&img, opt.cell_px, 20, 20, 2);
        assert(r > 0 && g > 0 && b > 0);
        assert(r == g && g == b);
    }

    img_render_free_image(&img);
    img_ce_grid_destroy(ce);
    PASS();
}

/* ── different states → different output ─────────────────── */

static void test_different_states_differ(void) {
    TEST("different CE states produce different pixels");

    ImgCEGrid* a = img_ce_grid_create();
    ImgCEGrid* b = img_ce_grid_create();
    assert(a && b);

    set_single_cell(a, 5, 5, 100, 0, 0, 0);
    set_single_cell(b, 5, 5, 0, 100, 0, 0);

    ImgRenderImage ia = {0}, ib = {0};
    assert(img_render_ce_grid(a, NULL, &ia));
    assert(img_render_ce_grid(b, NULL, &ib));

    const size_t n = (size_t)ia.width * ia.height * 3u;
    assert(n == (size_t)ib.width * ib.height * 3u);
    assert(memcmp(ia.rgb, ib.rgb, n) != 0);

    img_render_free_image(&ia);
    img_render_free_image(&ib);
    img_ce_grid_destroy(a);
    img_ce_grid_destroy(b);
    PASS();
}

/* ── tier 4 saturation bleeds outside primary mask ───────── */

static void test_tier4_bleed(void) {
    TEST("tier-4 (saturated) core raises pixels outside the center mask");

    ImgCEGrid* ce_low  = img_ce_grid_create();
    ImgCEGrid* ce_sat  = img_ce_grid_create();
    assert(ce_low && ce_sat);

    /* One cell with tier-1 core, another with tier-4 core (saturated). */
    set_single_cell(ce_low, 4, 4, /*core*/   8, 0, 0, 0);
    set_single_cell(ce_sat, 4, 4, /*core*/ 255, 0, 0, 0);

    ImgRenderOptions opt = img_render_default_options();
    opt.cell_px = 8;

    ImgRenderImage img_low = {0}, img_sat = {0};
    assert(img_render_ce_grid(ce_low, &opt, &img_low));
    assert(img_render_ce_grid(ce_sat, &opt, &img_sat));

    /* Sample a pixel in the non-center zone: (0, 0) of the block
     * (corner, outside the center square). Tier-4 should push this
     * above tier-1 due to the bleed rule. */
    const uint32_t x = 4 * opt.cell_px + 0;
    const uint32_t y = 4 * opt.cell_px + 0;
    const size_t off = (size_t)y * img_low.width * 3u + (size_t)x * 3u;
    const uint8_t low_r = img_low.rgb[off];
    const uint8_t sat_r = img_sat.rgb[off];
    assert(sat_r > low_r);

    img_render_free_image(&img_low);
    img_render_free_image(&img_sat);
    img_ce_grid_destroy(ce_low);
    img_ce_grid_destroy(ce_sat);
    PASS();
}

/* ── PPM save roundtrip ──────────────────────────────────── */

static void test_ppm_save_roundtrip(void) {
    TEST("PPM(P6) save writes header + pixels exactly");

    ImgCEGrid* ce = img_ce_grid_create();
    assert(ce);
    set_single_cell(ce, 2, 3, 50, 0, 0, 0);

    ImgRenderOptions opt = img_render_default_options();
    opt.cell_px = 4;
    ImgRenderImage img = {0};
    assert(img_render_ce_grid(ce, &opt, &img));

    const char* path = "build/test_img_render_sample.ppm";
    assert(img_render_save_ppm(path, &img));

    FILE* f = fopen(path, "rb");
    assert(f);
    char magic[3] = {0};
    uint32_t w = 0, h = 0, maxv = 0;
    int n = fscanf(f, "%2s %u %u %u", magic, &w, &h, &maxv);
    assert(n == 4);
    assert(magic[0] == 'P' && magic[1] == '6');
    assert(w == img.width);
    assert(h == img.height);
    assert(maxv == 255);
    /* Skip exactly one whitespace after the header value, then read pixels. */
    fgetc(f);
    size_t expect = (size_t)w * h * 3u;
    uint8_t* buf = (uint8_t*)malloc(expect);
    assert(buf);
    size_t got = fread(buf, 1, expect, f);
    fclose(f);
    assert(got == expect);
    assert(memcmp(buf, img.rgb, expect) == 0);

    free(buf);
    img_render_free_image(&img);
    img_ce_grid_destroy(ce);
    PASS();
}

int main(void) {
    printf("=== test_img_render ===\n");

    test_default_options();
    test_empty_grid_is_black();
    test_slot_shape_per_channel();
    test_different_states_differ();
    test_tier4_bleed();
    test_ppm_save_roundtrip();

    printf("  %d/%d passed\n\n", tests_passed, tests_total);
    return (tests_passed == tests_total) ? 0 : 1;
}
