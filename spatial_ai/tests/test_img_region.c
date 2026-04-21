/* tests/test_img_region.c — A-region flood fill (Phase A). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "img_ce.h"
#include "img_region.h"

#define PASS(name) do { printf("  [TEST] " name " ... PASS\n"); } while (0)

static void fill_uniform_priority(ImgCEGrid* g, uint8_t pri) {
    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) {
        g->cells[i].priority = pri;
        g->cells[i].semantic_role = 1;
        g->cells[i].depth_class   = 2;
    }
}

static void test_uniform_grid_single_region(void) {
    ImgCEGrid* g = img_ce_grid_create();
    assert(g);
    fill_uniform_priority(g, 128);

    ImgRegionMap m;
    assert(img_region_map_extract(g, 0, &m));
    assert(m.region_count == 1);
    assert(m.regions[0].cell_count == IMG_CE_TOTAL);
    assert(m.regions[0].a_band_lo == 128);
    assert(m.regions[0].a_band_hi == 128);
    /* every cell should map to region 0 */
    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++)
        assert(m.cell_to_region[i] == 0);

    img_region_map_free(&m);
    img_ce_grid_destroy(g);
    PASS("uniform priority grid → exactly one region");
}

static void test_two_halves_two_regions(void) {
    ImgCEGrid* g = img_ce_grid_create();
    assert(g);
    /* Left half priority=10, right half priority=200 — with band=0
     * they cannot merge across the vertical seam. */
    for (uint32_t y = 0; y < IMG_CE_SIZE; y++) {
        for (uint32_t x = 0; x < IMG_CE_SIZE; x++) {
            g->cells[img_ce_idx(y, x)].priority =
                (x < IMG_CE_SIZE / 2) ? 10 : 200;
        }
    }
    ImgRegionMap m;
    assert(img_region_map_extract(g, 0, &m));
    assert(m.region_count == 2);

    /* Raster order → first seed is (0,0)=10 → region 0 has priority 10. */
    assert(m.regions[0].a_band_lo == 10);
    assert(m.regions[1].a_band_lo == 200);
    assert(m.regions[0].cell_count == IMG_CE_TOTAL / 2);
    assert(m.regions[1].cell_count == IMG_CE_TOTAL / 2);

    img_region_map_free(&m);
    img_ce_grid_destroy(g);
    PASS("two distinct A halves → two regions, correct split");
}

static void test_band_width_merges(void) {
    ImgCEGrid* g = img_ce_grid_create();
    assert(g);
    /* Checkerboard of priorities 100 / 110 — band=0 → many regions,
     * band=16 → one region covering everything. */
    for (uint32_t y = 0; y < IMG_CE_SIZE; y++) {
        for (uint32_t x = 0; x < IMG_CE_SIZE; x++) {
            g->cells[img_ce_idx(y, x)].priority =
                ((x + y) & 1) ? 110 : 100;
        }
    }
    ImgRegionMap strict;
    assert(img_region_map_extract(g, 0, &strict));
    /* Fully disconnected single cells → region count == IMG_CE_TOTAL */
    assert(strict.region_count == IMG_CE_TOTAL);
    img_region_map_free(&strict);

    ImgRegionMap loose;
    assert(img_region_map_extract(g, 16, &loose));
    assert(loose.region_count == 1);
    assert(loose.regions[0].cell_count == IMG_CE_TOTAL);
    assert(loose.regions[0].a_band_lo == 100);
    assert(loose.regions[0].a_band_hi == 110);
    img_region_map_free(&loose);

    img_ce_grid_destroy(g);
    PASS("band width collapses checkerboard from N regions → 1");
}

static void test_cell_to_region_consistency(void) {
    ImgCEGrid* g = img_ce_grid_create();
    assert(g);
    fill_uniform_priority(g, 50);
    /* Inject a 4x4 high-priority island in the centre. */
    for (uint32_t y = 30; y < 34; y++)
        for (uint32_t x = 30; x < 34; x++)
            g->cells[img_ce_idx(y, x)].priority = 250;

    ImgRegionMap m;
    assert(img_region_map_extract(g, 10, &m));
    assert(m.region_count == 2);

    /* Every cell listed in a region's pool must round-trip through
     * cell_to_region back to that region's index. */
    for (uint32_t r = 0; r < m.region_count; r++) {
        const ImgRegion* reg = &m.regions[r];
        for (uint32_t k = 0; k < reg->cell_count; k++) {
            uint32_t ci = m.cell_ids[reg->cells_offset + k];
            assert(ci < IMG_CE_TOTAL);
            assert(m.cell_to_region[ci] == r);
        }
    }

    /* Total cell coverage equals the grid. */
    uint32_t total = 0;
    for (uint32_t r = 0; r < m.region_count; r++)
        total += m.regions[r].cell_count;
    assert(total == IMG_CE_TOTAL);

    img_region_map_free(&m);
    img_ce_grid_destroy(g);
    PASS("cell_to_region round-trips; cell_ids pool covers all cells");
}

static void mark_visited_op(ImgCECell* cell, uint32_t ci, void* user) {
    (void)cell; (void)ci;
    uint32_t* counter = (uint32_t*)user;
    (*counter)++;
}

static void test_region_apply_visits_all(void) {
    ImgCEGrid* g = img_ce_grid_create();
    assert(g);
    fill_uniform_priority(g, 77);

    ImgRegionMap m;
    assert(img_region_map_extract(g, 0, &m));
    assert(m.region_count == 1);

    uint32_t visits = 0;
    img_region_apply(&m, &m.regions[0], g, mark_visited_op, &visits);
    assert(visits == IMG_CE_TOTAL);

    img_region_map_free(&m);
    img_ce_grid_destroy(g);
    PASS("img_region_apply visits every cell in the region");
}

static void test_null_and_empty_safe(void) {
    ImgRegionMap m;
    memset(&m, 0, sizeof(m));

    assert(img_region_map_extract(NULL, 0, &m) == 0);
    /* After failure, free should be a safe no-op. */
    img_region_map_free(&m);
    img_region_map_free(NULL);
    /* Double-free also safe */
    img_region_map_free(&m);
    PASS("NULL inputs return 0, free is safe on zero-init");
}

int main(void) {
    printf("=== test_img_region ===\n");
    test_uniform_grid_single_region();
    test_two_halves_two_regions();
    test_band_width_merges();
    test_cell_to_region_consistency();
    test_region_apply_visits_all();
    test_null_and_empty_safe();
    printf("  6/6 passed\n");
    return 0;
}
