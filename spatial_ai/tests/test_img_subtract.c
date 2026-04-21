/* tests/test_img_subtract.c — subtract-domain equivalence (Phase C).
 *
 * Subtract representation is only useful if it is *indistinguishable*
 * from the classic add representation at the byte level. These tests
 * nail that identity down end-to-end:
 *
 *   1. sat_add on cell and sat_sub on (255 - cell) land on the same
 *      byte for every saturating input pair.
 *   2. img_subtract_from_grid / _to_grid is a pure round-trip.
 *   3. For any delta unit applied to any cell, img_delta_apply and
 *      img_subtract_apply_unit land on identical cell bytes.
 *   4. Saturation: cells at channel extremes stay clamped correctly.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "img_ce.h"
#include "img_delta_memory.h"
#include "img_subtract.h"

#define PASS(name) do { printf("  [TEST] " name " ... PASS\n"); } while (0)

static int sat_add_s16(int v, int s) {
    int r = v + s;
    if (r < 0)   return 0;
    if (r > 255) return 255;
    return r;
}

static void test_saturating_arith_identity(void) {
    /* Exhaustive over representative (cell, add) pairs.
     * Covers the full signed range of int16 add that any delta can
     * legitimately emit (±256 is already well past saturation). */
    int failures = 0;
    for (int cell = 0; cell <= 255; cell++) {
        for (int add = -300; add <= 300; add++) {
            int classic = sat_add_s16(cell, add);

            /* Subtract-domain equivalent. acc = 255 - cell; apply
             * the delta as a subtract; reconstruct cell via 255 - acc. */
            int acc = 255 - cell;
            int new_acc = acc - add;
            if (new_acc < 0)   new_acc = 0;
            if (new_acc > 255) new_acc = 255;
            int subtracted = 255 - new_acc;

            if (classic != subtracted) failures++;
        }
    }
    assert(failures == 0);
    PASS("sat_add and 255-sat_sub are bit-equal over the full input grid");
}

static void randomize_grid(ImgCEGrid* g, uint32_t seed) {
    /* Lightweight xorshift so the test stays deterministic. */
    uint32_t s = seed ? seed : 1u;
    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        g->cells[i].core     = (uint8_t)(s        & 0xFF);
        g->cells[i].link     = (uint8_t)((s >> 8) & 0xFF);
        g->cells[i].delta    = (uint8_t)((s >> 16)& 0xFF);
        g->cells[i].priority = (uint8_t)((s >> 24)& 0xFF);
        g->cells[i].tone_class       = (uint8_t)((s >> 1)  & 0x3);
        g->cells[i].semantic_role    = (uint8_t)((s >> 3)  & 0x7);
        g->cells[i].direction_class  = (uint8_t)((s >> 6)  & 0x7);
        g->cells[i].depth_class      = (uint8_t)((s >> 9)  & 0x3);
        g->cells[i].delta_sign       = (uint8_t)((s >> 11) & 0x3);
        g->cells[i].last_delta_id    = IMG_DELTA_ID_NONE;
    }
}

static void test_grid_round_trip(void) {
    ImgCEGrid* g = img_ce_grid_create();
    ImgCEGrid* g2 = img_ce_grid_create();
    assert(g && g2);
    randomize_grid(g, 0xC0FFEEu);
    memcpy(g2->cells, g->cells, IMG_CE_TOTAL * sizeof(ImgCECell));

    ImgSubtractContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    img_subtract_from_grid(&ctx, g);
    /* Mutate g to prove to_grid writes over it rather than being a no-op. */
    memset(g->cells, 0, IMG_CE_TOTAL * sizeof(ImgCECell));
    img_subtract_to_grid(&ctx, g);

    /* RGBA channels must round-trip byte-for-byte. Tags are not
     * stored in ctx; to_grid leaves them alone (we zeroed them first,
     * so they should still be zero). */
    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) {
        assert(g->cells[i].core     == g2->cells[i].core);
        assert(g->cells[i].link     == g2->cells[i].link);
        assert(g->cells[i].delta    == g2->cells[i].delta);
        assert(g->cells[i].priority == g2->cells[i].priority);
        /* tag fields are zero because we wiped them; verifies
         * to_grid intentionally does not touch them. */
        assert(g->cells[i].tone_class == 0);
        assert(g->cells[i].semantic_role == 0);
    }

    img_ce_grid_destroy(g);
    img_ce_grid_destroy(g2);
    PASS("img_subtract_from_grid → _to_grid preserves RGBA bytes");
}

static ImgDeltaMemory* build_tiny_mem(void) {
    ImgDeltaMemory* m = img_delta_memory_create();
    struct { uint8_t role, tone, dir, depth, link, sign; uint8_t tier, scale, signi, mode; }
    recipe[] = {
        { 1, 2, 0, 1, 3, 0,  1, 2, 1, 1 },
        { 1, 2, 0, 1, 3, 0,  2, 3, 1, 2 },
        { 2, 2, 2, 1, 4, 1,  2, 3, 2, 5 },
        { 2, 2, 2, 1, 4, 1,  1, 2, 1, 7 },
        { 3, 2, 1, 2, 2, 0,  3, 5, 1, 1 },
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

static void test_apply_unit_equivalence(void) {
    /* For every delta in memory, apply it to every cell via both
     * paths and assert the resulting ImgCECell bytes are identical. */
    ImgDeltaMemory* mem_add = build_tiny_mem();
    ImgDeltaMemory* mem_sub = build_tiny_mem();
    assert(mem_add && mem_sub);

    ImgCEGrid* g_add = img_ce_grid_create();
    ImgCEGrid* g_sub = img_ce_grid_create();
    assert(g_add && g_sub);

    uint32_t mismatch_cells = 0;
    const uint32_t n_deltas = img_delta_memory_count(mem_add);
    for (uint32_t d = 0; d < n_deltas; d++) {
        const ImgDeltaUnit* unit_add = img_delta_memory_get(mem_add, d);
        const ImgDeltaUnit* unit_sub = img_delta_memory_get(mem_sub, d);
        assert(unit_add && unit_sub);

        randomize_grid(g_add, 0xABCDEF01u + d);
        memcpy(g_sub->cells, g_add->cells, IMG_CE_TOTAL * sizeof(ImgCECell));

        ImgSubtractContext ctx;
        memset(&ctx, 0, sizeof(ctx));
        img_subtract_from_grid(&ctx, g_sub);

        /* Apply the same delta to a stride of cells through both paths. */
        for (uint32_t i = 0; i < IMG_CE_TOTAL; i += 37) {
            img_delta_apply(&g_add->cells[i], mem_add, unit_add);
            img_subtract_apply_unit(&ctx, i, &g_sub->cells[i],
                                    mem_sub, unit_sub);
        }

        for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) {
            if (memcmp(&g_add->cells[i], &g_sub->cells[i],
                       sizeof(ImgCECell)) != 0) {
                mismatch_cells++;
            }
        }
    }
    assert(mismatch_cells == 0);

    img_ce_grid_destroy(g_add);
    img_ce_grid_destroy(g_sub);
    img_delta_memory_destroy(mem_add);
    img_delta_memory_destroy(mem_sub);
    PASS("img_subtract_apply_unit matches img_delta_apply byte-for-byte");
}

static void test_saturation_clamps_at_extremes(void) {
    /* Cells at 255 saturate correctly when a positive add is applied
     * (no overflow), and at 0 when a negative add is applied (no
     * underflow). This is the §5.1.2 "경계 처리 자동 0 clamp". */
    ImgCEGrid* g = img_ce_grid_create();
    assert(g);
    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) {
        g->cells[i].core = 255;
        g->cells[i].link = 0;
        g->cells[i].delta = 128;
        g->cells[i].priority = 200;
    }

    ImgSubtractContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    img_subtract_from_grid(&ctx, g);

    ImgConcreteDelta plus50  = {0};
    plus50.add_core     =  50;
    plus50.add_link     =  50;
    plus50.add_delta    =  50;
    plus50.add_priority =  50;
    ImgConcreteDelta minus50 = {0};
    minus50.add_core     = -50;
    minus50.add_link     = -50;
    minus50.add_delta    = -50;
    minus50.add_priority = -50;

    img_subtract_apply_concrete(&ctx, 0, &g->cells[0], &plus50);
    img_subtract_apply_concrete(&ctx, 1, &g->cells[1], &minus50);
    img_subtract_to_grid(&ctx, g);

    assert(g->cells[0].core == 255);        /* 255 + 50 clamps up */
    assert(g->cells[0].link == 50);         /* 0 + 50 fine */
    assert(g->cells[1].core == 205);        /* 255 - 50 fine */
    assert(g->cells[1].link == 0);          /* 0 - 50 clamps down */

    img_ce_grid_destroy(g);
    PASS("subtract-domain apply clamps at 0 and 255 automatically");
}

int main(void) {
    printf("=== test_img_subtract ===\n");
    test_saturating_arith_identity();
    test_grid_round_trip();
    test_apply_unit_equivalence();
    test_saturation_clamps_at_extremes();
    printf("  4/4 passed\n");
    return 0;
}
