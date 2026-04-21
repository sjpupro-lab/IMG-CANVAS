/* tests/test_img_level.c — 4-level hierarchy (Phase B).
 *
 *   Covers:
 *     - img_level_infer_from_region rule table
 *     - tags0 / tags1 MSB level encode/decode round-trips
 *     - default per-level pass budget constant
 *     - img_region_map_extract populates region.level correctly
 *     - NMEM save/load round-trips at v2 with level bits set
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "img_ce.h"
#include "img_level.h"
#include "img_region.h"
#include "img_noise_memory.h"

#define PASS(name) do { printf("  [TEST] " name " ... PASS\n"); } while (0)

static void test_infer_rules(void) {
    /* Rule: cell_count >= 512 → CLAUSE regardless of other signals. */
    assert(img_level_infer_from_region(IMG_DEPTH_BACKGROUND,
                                       IMG_FLOW_NONE, 10, 512)
           == IMG_LEVEL_CLAUSE);
    assert(img_level_infer_from_region(IMG_DEPTH_FOREGROUND,
                                       IMG_FLOW_HORIZONTAL, 200, 1000)
           == IMG_LEVEL_CLAUSE);

    /* Foreground depth (below the 512 cutoff) → AREA. */
    assert(img_level_infer_from_region(IMG_DEPTH_FOREGROUND,
                                       IMG_FLOW_NONE, 100, 50)
           == IMG_LEVEL_AREA);

    /* High priority (band_hi ≥ 180) alone also lifts to AREA. */
    assert(img_level_infer_from_region(IMG_DEPTH_BACKGROUND,
                                       IMG_FLOW_NONE, 200, 50)
           == IMG_LEVEL_AREA);

    /* Directional flow → LINE. */
    assert(img_level_infer_from_region(IMG_DEPTH_BACKGROUND,
                                       IMG_FLOW_HORIZONTAL, 50, 50)
           == IMG_LEVEL_LINE);

    /* Everything else (small, dark, flat, non-oriented) → POINT. */
    assert(img_level_infer_from_region(IMG_DEPTH_BACKGROUND,
                                       IMG_FLOW_NONE, 50, 50)
           == IMG_LEVEL_POINT);
    PASS("img_level_infer_from_region honours the 4-level rule table");
}

static void test_tag_bits_round_trip(void) {
    uint8_t base0 = 0x3Fu;  /* packed tone/flow/mood, MSB still clear */
    uint8_t base1 = 0x5Fu;  /* packed depth/role/tier, MSB still clear */

    for (uint8_t lvl = 0; lvl < IMG_LEVEL_COUNT; lvl++) {
        uint8_t t0 = img_level_tag0_set(base0, lvl);
        uint8_t t1 = img_level_tag1_set(base1, lvl);

        /* Low 7 bits of tags0 and tags1 must be untouched. */
        assert((t0 & 0x7Fu) == (base0 & 0x7Fu));
        assert((t1 & 0x7Fu) == (base1 & 0x7Fu));

        /* Round-trip the level. */
        uint8_t got = img_level_decode(t0, t1);
        assert(got == lvl);
    }
    PASS("level bits round-trip through tags0/tags1 MSBs");
}

static void test_default_passes(void) {
    /* Sanity: the pass budget matches the spec table from §5.3.1. */
    assert(IMG_LEVEL_DEFAULT_PASSES[IMG_LEVEL_POINT]  == 2);
    assert(IMG_LEVEL_DEFAULT_PASSES[IMG_LEVEL_LINE]   == 3);
    assert(IMG_LEVEL_DEFAULT_PASSES[IMG_LEVEL_AREA]   == 2);
    assert(IMG_LEVEL_DEFAULT_PASSES[IMG_LEVEL_CLAUSE] == 1);
    PASS("IMG_LEVEL_DEFAULT_PASSES matches the spec schedule");
}

static void seed_mixed_grid(ImgCEGrid* g) {
    /* Full-grid uniform low-priority bg → single CLAUSE region. */
    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) {
        ImgCECell* c = &g->cells[i];
        c->priority = 20;
        c->depth_class = IMG_DEPTH_BACKGROUND;
        c->direction_class = IMG_FLOW_NONE;
        c->semantic_role = 1;
    }
    /* A small high-priority foreground patch in the corner. Using
     * band=10 keeps it separate from the dark background. */
    for (uint32_t y = 0; y < 8; y++)
        for (uint32_t x = 0; x < 8; x++) {
            uint32_t i = img_ce_idx(y, x);
            g->cells[i].priority = 220;
            g->cells[i].depth_class = IMG_DEPTH_FOREGROUND;
        }
    /* A horizontal line of mid-priority oriented cells. */
    for (uint32_t x = 32; x < 48; x++) {
        uint32_t i = img_ce_idx(32, x);
        g->cells[i].priority = 60;
        g->cells[i].direction_class = IMG_FLOW_HORIZONTAL;
    }
}

static void test_region_map_fills_level(void) {
    ImgCEGrid* g = img_ce_grid_create();
    assert(g);
    seed_mixed_grid(g);

    ImgRegionMap m;
    assert(img_region_map_extract(g, 10, &m));

    /* Expect at least 3 regions: background clause, FG area, line. */
    int saw_clause = 0, saw_area = 0, saw_line = 0;
    for (uint32_t r = 0; r < m.region_count; r++) {
        switch (m.regions[r].level) {
            case IMG_LEVEL_CLAUSE: saw_clause = 1; break;
            case IMG_LEVEL_AREA:   saw_area   = 1; break;
            case IMG_LEVEL_LINE:   saw_line   = 1; break;
            default: break;
        }
    }
    assert(saw_clause);
    assert(saw_area);
    assert(saw_line);

    img_region_map_free(&m);
    img_ce_grid_destroy(g);
    PASS("img_region_map_extract populates region.level per the rule table");
}

static void test_nmem_v2_round_trip(void) {
    /* Build a minimal NMEM by hand so we can set level bits on a
     * known sample, save as v2, load, and verify the bits survive. */
    ImgNoiseMemory nm;
    img_noise_memory_init(&nm);

    /* Put a single non-default sample in cell 0, slot 0, with every
     * possible level code stored on a different (cell, slot) pair so
     * we cover the round-trip for all four levels. */
    for (uint8_t lvl = 0; lvl < IMG_LEVEL_COUNT; lvl++) {
        ImgNoiseSample* s = &nm.cell_priors[lvl].top_k[0];
        s->ce_r = 10 + lvl;
        s->ce_g = 20 + lvl;
        s->ce_b = 30 + lvl;
        s->ce_a = 40 + lvl;
        s->tags0 = img_level_tag0_set(0x05u, lvl);
        s->tags1 = img_level_tag1_set(0x11u, lvl);
        s->direction = 1;
        s->weight = 7;
    }

    const char* path = "out/test_nmem_v2_roundtrip.nmem";
    assert(img_noise_memory_save_versioned(&nm, path, IMG_NOISE_VERSION_V2));

    ImgNoiseMemory loaded;
    img_noise_memory_init(&loaded);
    assert(img_noise_memory_load(&loaded, path));

    for (uint8_t lvl = 0; lvl < IMG_LEVEL_COUNT; lvl++) {
        const ImgNoiseSample* s = &loaded.cell_priors[lvl].top_k[0];
        assert(s->ce_r == (uint8_t)(10 + lvl));
        assert(s->ce_a == (uint8_t)(40 + lvl));
        /* Crucial: MSB level bits survived the save/load. */
        assert(img_level_decode(s->tags0, s->tags1) == lvl);
        /* And the non-level bits of tags0/tags1 are intact. */
        assert((s->tags0 & 0x7Fu) == 0x05u);
        assert((s->tags1 & 0x7Fu) == 0x11u);
    }

    img_noise_memory_free(&loaded);
    img_noise_memory_free(&nm);
    remove(path);
    PASS("NMEM v2 save/load preserves level bits in tags0/tags1 MSBs");
}

static void test_nmem_v1_load_as_level_zero(void) {
    /* Save a mint-v1 NMEM with zero tag MSBs; load it back and
     * confirm every sample decodes as level 0. This verifies the
     * stated "v1 파일 로드 → level 0 간주" rule. */
    ImgNoiseMemory nm;
    img_noise_memory_init(&nm);
    /* Seed something non-default so there's data to inspect. */
    nm.cell_priors[5].top_k[2].ce_r = 99;
    nm.cell_priors[5].top_k[2].tags0 = 0x3Fu;   /* MSB=0 by construction */
    nm.cell_priors[5].top_k[2].tags1 = 0x5Fu;   /* MSB=0 by construction */

    const char* path = "out/test_nmem_v1_load.nmem";
    assert(img_noise_memory_save(&nm, path));   /* defaults to v1 */

    ImgNoiseMemory loaded;
    img_noise_memory_init(&loaded);
    assert(img_noise_memory_load(&loaded, path));

    const ImgNoiseSample* s = &loaded.cell_priors[5].top_k[2];
    assert(s->ce_r == 99);
    assert(img_level_decode(s->tags0, s->tags1) == IMG_LEVEL_POINT);

    img_noise_memory_free(&loaded);
    img_noise_memory_free(&nm);
    remove(path);
    PASS("NMEM v1 files load with every sample decoding as level 0");
}

int main(void) {
    printf("=== test_img_level ===\n");
    test_infer_rules();
    test_tag_bits_round_trip();
    test_default_passes();
    test_region_map_fills_level();
    test_nmem_v1_load_as_level_zero();
    test_nmem_v2_round_trip();
    printf("  6/6 passed\n");
    return 0;
}
