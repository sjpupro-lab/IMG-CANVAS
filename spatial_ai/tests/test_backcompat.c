/* tests/test_backcompat.c — reference-hash regression guard for the
 * V2 refactor. Builds a deterministic mini-scenario end-to-end and
 * fingerprints the grid state after drawing_pass. Any unintended
 * output-changing modification to img_drawing / img_delta_*  will
 * change the hash and fail this test.
 *
 *   The pinned values below were captured on Phase A baseline
 *   (img_region integrated but stamp loop unchanged). Phase B and
 *   Phase C preserve these same values — that is the refactor's
 *   "reference hash matching" guarantee. */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include "img_ce.h"
#include "img_delta_memory.h"
#include "img_drawing.h"
#include "img_noise_memory.h"

#define PASS(name) do { printf("  [TEST] " name " ... PASS\n"); } while (0)

/* ── FNV-1a 64 ─────────────────────────────────────────────
 * No external crypto dep needed — a 64-bit fingerprint is plenty
 * to detect any single-byte drift in the CE grid. */
static uint64_t fnv1a64(const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

/* Hash the CE grid byte-for-byte. Relies on ImgCECell being packed
 * the same way across builds on the same target; that is enforced
 * elsewhere by the existing size checks. */
static uint64_t grid_fingerprint(const ImgCEGrid* g) {
    return fnv1a64(g->cells, IMG_CE_TOTAL * sizeof(ImgCECell));
}

/* Build a small deterministic delta memory exercising all four
 * channels. State keys are hand-chosen so the drawing fallback chain
 * has something to match regardless of the seed grid's exact tags. */
static ImgDeltaMemory* build_test_memory(void) {
    ImgDeltaMemory* m = img_delta_memory_create();
    assert(m);

    struct { uint8_t role, tone, dir, depth, link, sign; uint8_t tier, scale, signi, mode; }
    recipe[] = {
        { 1, 2, 0, 1, 3, 0,  1, 2, 1, 1 },  /* tier1, intensity, pos */
        { 1, 2, 0, 1, 3, 0,  2, 3, 1, 2 },  /* tier2, link, pos */
        { 1, 2, 1, 2, 3, 0,  3, 4, 1, 4 },  /* tier3, depth, pos */
        { 2, 2, 2, 1, 4, 1,  2, 3, 2, 5 },  /* tier2, mood, neg */
        { 2, 2, 2, 1, 4, 1,  1, 2, 1, 7 },  /* tier1, priority, pos */
        { 1, 3, 0, 1, 3, 0,  2, 3, 1, 3 },  /* tier2, direction, pos */
        { 3, 2, 1, 2, 2, 0,  3, 5, 1, 1 },  /* tier3, intensity, big scale */
        { 3, 2, 1, 2, 2, 0,  2, 4, 2, 2 },  /* tier2, link, neg */
    };
    const uint32_t N = sizeof(recipe) / sizeof(recipe[0]);

    for (uint32_t i = 0; i < N; i++) {
        ImgStateKey k = img_state_key_make(
            recipe[i].role, recipe[i].tone, recipe[i].dir,
            recipe[i].depth, recipe[i].link, recipe[i].sign);
        ImgDeltaPayload pl;
        pl.state = img_delta_state_simple(
            recipe[i].tier, recipe[i].scale, recipe[i].signi, recipe[i].mode);
        pl.role_target = 0;
        pl.role_target_on = 0;
        img_delta_memory_add(m, k, pl);
    }
    return m;
}

/* Populate a grid with a deterministic gradient so drawing has real
 * variation to work on without depending on any input image. */
static void seed_grid(ImgCEGrid* g) {
    for (uint32_t y = 0; y < IMG_CE_SIZE; y++) {
        for (uint32_t x = 0; x < IMG_CE_SIZE; x++) {
            uint32_t i = img_ce_idx(y, x);
            ImgCECell* c = &g->cells[i];
            c->core     = (uint8_t)((x * 3 + y * 5) & 0xFF);
            c->link     = (uint8_t)((x * 7 + y * 11) & 0xFF);
            c->delta    = (uint8_t)((x + y) & 0x7F);
            c->priority = (uint8_t)(((x / 8) * 32 + (y / 16) * 16) & 0xFF);
            c->tone_class      = (uint8_t)((x + y) % 4 + 1);
            c->semantic_role   = (uint8_t)((x / 16) % 3 + 1);
            c->direction_class = (uint8_t)((y / 8) % 5);
            c->depth_class     = (uint8_t)((x / 16) % 4 + 1);
            c->delta_sign      = (uint8_t)((x * y) & 0x3);
            c->last_delta_id   = IMG_DELTA_ID_NONE;
        }
    }
}

static void test_drawing_pass_reference_fingerprint(void) {
    /* Pinned on 2026-04-21 after Phase A integration of img_region.
     * The value encodes drawing_pass's full byte-level output for
     * this deterministic scenario. Phase B / C must preserve it. */
    const uint64_t EXPECTED = 0x78EDF71011FBC950ull;

    ImgCEGrid* g = img_ce_grid_create();
    assert(g);
    seed_grid(g);

    ImgDeltaMemory* mem = build_test_memory();
    assert(mem);

    ImgDrawingOptions opt = img_drawing_default_options();
    opt.top_g = 4;
    opt.passes = 2;
    opt.presence_penalty = 0.5;
    opt.skip_zero_cells = 0;

    ImgDrawingStats stats;
    int ok = img_drawing_pass(g, mem, &opt, &stats);
    assert(ok == 1);
    /* Sanity: drawing actually did work. */
    assert(stats.stamps_applied > 0);
    assert(stats.cells_visited  > 0);
    assert(stats.unique_deltas_used > 0);

    uint64_t got = grid_fingerprint(g);

    /* Dev mode: set IMG_BACKCOMPAT_PRINT=1 to print the current hash
     * instead of asserting, so a new reference value can be pinned
     * after an intentional, reviewed change. */
    const char* print_only = getenv("IMG_BACKCOMPAT_PRINT");
    if (print_only && print_only[0] && print_only[0] != '0') {
        printf("    drawing_pass fingerprint = 0x%016llX\n",
               (unsigned long long)got);
    } else {
        if (got != EXPECTED) {
            fprintf(stderr,
                "FAIL: drawing_pass fingerprint drifted.\n"
                "  expected: 0x%016llX\n"
                "  got:      0x%016llX\n"
                "If the change was intentional, re-run with\n"
                "IMG_BACKCOMPAT_PRINT=1 ./build/test_backcompat to\n"
                "print the new value and update the pinned constant.\n",
                (unsigned long long)EXPECTED,
                (unsigned long long)got);
            exit(1);
        }
    }

    img_delta_memory_destroy(mem);
    img_ce_grid_destroy(g);
    PASS("drawing_pass produces the pinned reference fingerprint");
}

/* Confirms Phase A addition of img_region does not disturb the
 * no-op short-circuits of drawing_pass. */
static void test_drawing_pass_noop_paths_untouched(void) {
    ImgCEGrid* g = img_ce_grid_create();
    assert(g);
    seed_grid(g);
    ImgCEGrid* g_before = img_ce_grid_create();
    assert(g_before);
    memcpy(g_before->cells, g->cells, IMG_CE_TOTAL * sizeof(ImgCECell));

    ImgDrawingStats stats;

    /* Empty / NULL memory → no-op success; grid unchanged. */
    assert(img_drawing_pass(g, NULL, NULL, &stats) == 1);
    assert(stats.stamps_applied == 0);
    assert(memcmp(g->cells, g_before->cells,
                  IMG_CE_TOTAL * sizeof(ImgCECell)) == 0);

    ImgDeltaMemory* empty = img_delta_memory_create();
    assert(empty);
    assert(img_drawing_pass(g, empty, NULL, &stats) == 1);
    assert(stats.stamps_applied == 0);
    assert(memcmp(g->cells, g_before->cells,
                  IMG_CE_TOTAL * sizeof(ImgCECell)) == 0);

    img_delta_memory_destroy(empty);
    img_ce_grid_destroy(g);
    img_ce_grid_destroy(g_before);
    PASS("drawing_pass no-op paths remain byte-stable post-Phase A");
}

static void test_nmem_v1_sample_fingerprint(void) {
    /* Pinned post-Phase A. Sample is pure NMEM path; this ensures
     * linkage with region extraction in drawing_pass hasn't altered
     * the noise-memory side. */
    const uint64_t EXPECTED = 0x1EE9CF90A00D73E5ull;

    ImgNoiseMemory nm;
    img_noise_memory_init(&nm);

    /* Observe a couple of hand-built grids so the NMEM has non-zero
     * state. Any deterministic input works — we only care the result
     * is reproducible. */
    ImgCEGrid* g1 = img_ce_grid_create();
    ImgCEGrid* g2 = img_ce_grid_create();
    assert(g1 && g2);
    seed_grid(g1);
    seed_grid(g2);
    /* Inject a lightweight difference between g1 and g2 so observe
     * sees real variation rather than identical snapshots. */
    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) {
        g2->cells[i].core ^= 0x10;
        g2->cells[i].priority = (uint8_t)(g2->cells[i].priority ^ 0x20);
    }
    assert(img_noise_memory_observe(&nm, g1, NULL));
    assert(img_noise_memory_observe(&nm, g2, NULL));

    ImgCEGrid* out = img_ce_grid_create();
    assert(out);
    ImgNoiseSampleOptions so = img_noise_sample_default_options();
    so.seed = 0x42ull;
    so.temperature_q8 = 256;
    assert(img_noise_memory_sample_grid(&nm, out, &so));

    uint64_t got = grid_fingerprint(out);
    const char* print_only = getenv("IMG_BACKCOMPAT_PRINT");
    if (print_only && print_only[0] && print_only[0] != '0') {
        printf("    nmem sample_grid fingerprint = 0x%016llX\n",
               (unsigned long long)got);
    } else {
        if (got != EXPECTED) {
            fprintf(stderr,
                "FAIL: nmem sample_grid fingerprint drifted.\n"
                "  expected: 0x%016llX\n"
                "  got:      0x%016llX\n",
                (unsigned long long)EXPECTED,
                (unsigned long long)got);
            exit(1);
        }
    }

    img_ce_grid_destroy(g1);
    img_ce_grid_destroy(g2);
    img_ce_grid_destroy(out);
    img_noise_memory_free(&nm);
    PASS("nmem sample_grid produces the pinned reference fingerprint");
}

int main(void) {
    printf("=== test_backcompat ===\n");
    test_drawing_pass_noop_paths_untouched();
    test_drawing_pass_reference_fingerprint();
    test_nmem_v1_sample_fingerprint();
    printf("  3/3 passed\n");
    return 0;
}
