#include "img_tier_table.h"

#include <assert.h>
#include <stdio.h>
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

/* ── canonical defaults ──────────────────────────────────── */

static void test_canonical_table(void) {
    TEST("canonical tier table values match prior inline constants");

    assert(IMG_TIER_TABLE[IMG_TIER_NONE].scale_factor == 0);
    assert(IMG_TIER_TABLE[IMG_TIER_T1  ].scale_factor == 4);
    assert(IMG_TIER_TABLE[IMG_TIER_T2  ].scale_factor == 12);
    assert(IMG_TIER_TABLE[IMG_TIER_T3  ].scale_factor == 24);

    assert(IMG_TIER_TABLE[IMG_TIER_NONE].range_max == 0);
    assert(IMG_TIER_TABLE[IMG_TIER_T1  ].range_max == 8);
    assert(IMG_TIER_TABLE[IMG_TIER_T2  ].range_max == 32);
    assert(IMG_TIER_TABLE[IMG_TIER_T3  ].range_max == 96);

    /* range_max strictly increasing (so classify is well-defined). */
    assert(IMG_TIER_TABLE[IMG_TIER_T1].range_max <
           IMG_TIER_TABLE[IMG_TIER_T2].range_max);
    assert(IMG_TIER_TABLE[IMG_TIER_T2].range_max <
           IMG_TIER_TABLE[IMG_TIER_T3].range_max);

    PASS();
}

/* ── classification against canonical ────────────────────── */

static void test_classify_canonical(void) {
    TEST("img_tier_classify buckets values into tiers 0..4");

    assert(img_tier_classify(0)   == 0);
    assert(img_tier_classify(1)   == 1);
    assert(img_tier_classify(8)   == 1);   /* boundary */
    assert(img_tier_classify(9)   == 2);
    assert(img_tier_classify(32)  == 2);   /* boundary */
    assert(img_tier_classify(33)  == 3);
    assert(img_tier_classify(96)  == 3);   /* boundary */
    assert(img_tier_classify(97)  == 4);   /* saturated */
    assert(img_tier_classify(255) == 4);

    PASS();
}

/* ── classify_with: custom table ─────────────────────────── */

static void test_classify_with_custom(void) {
    TEST("img_tier_classify_with respects a custom range_max");

    ImgTierEntry custom[IMG_TIER_MAX];
    memcpy(custom, IMG_TIER_TABLE, sizeof(custom));
    custom[IMG_TIER_T1].range_max = 20;
    custom[IMG_TIER_T2].range_max = 60;
    custom[IMG_TIER_T3].range_max = 120;

    assert(img_tier_classify_with(15,  custom) == 1);
    assert(img_tier_classify_with(20,  custom) == 1);   /* boundary */
    assert(img_tier_classify_with(21,  custom) == 2);
    assert(img_tier_classify_with(121, custom) == 4);

    PASS();
}

/* ── adaptive tier (quantile) ───────────────────────────── */

static void test_adapt_quantile(void) {
    TEST("img_tier_adapt splits a skewed histogram into monotonic tiers");

    uint32_t hist[256] = {0};

    /* Mass heavily at low values: values 1..50 very common, 51..150
     * moderate, 151..255 rare. Expect t1 < t2 < t3 strictly. */
    for (int v = 1;   v <= 50;  v++) hist[v] = 100;
    for (int v = 51;  v <= 150; v++) hist[v] = 10;
    for (int v = 151; v <= 255; v++) hist[v] = 1;

    ImgTierEntry out[IMG_TIER_MAX];
    img_tier_adapt(hist, out);

    assert(out[IMG_TIER_T1].range_max <  out[IMG_TIER_T2].range_max);
    assert(out[IMG_TIER_T2].range_max <  out[IMG_TIER_T3].range_max);
    assert(out[IMG_TIER_T3].range_max <= 255);

    /* Since mass concentrates at low values, t1 should land inside
     * [1, 50] (the quartile of a left-skewed distribution). */
    assert(out[IMG_TIER_T1].range_max >= 1);
    assert(out[IMG_TIER_T1].range_max <= 50);

    /* scale_factor is preserved from canonical defaults. */
    assert(out[IMG_TIER_T1].scale_factor ==
           IMG_TIER_TABLE[IMG_TIER_T1].scale_factor);
    assert(out[IMG_TIER_T3].scale_factor ==
           IMG_TIER_TABLE[IMG_TIER_T3].scale_factor);

    PASS();
}

/* ── adapt falls back safely on empty histogram ──────────── */

static void test_adapt_empty_histogram(void) {
    TEST("empty histogram → output equals canonical defaults");

    uint32_t hist[256] = {0};
    ImgTierEntry out[IMG_TIER_MAX];
    memset(out, 0xAA, sizeof(out));
    img_tier_adapt(hist, out);

    assert(memcmp(out, IMG_TIER_TABLE, sizeof(out)) == 0);

    PASS();
}

int main(void) {
    printf("=== test_img_tier_table ===\n");

    test_canonical_table();
    test_classify_canonical();
    test_classify_with_custom();
    test_adapt_quantile();
    test_adapt_empty_histogram();

    printf("  %d/%d passed\n\n", tests_passed, tests_total);
    return (tests_passed == tests_total) ? 0 : 1;
}
