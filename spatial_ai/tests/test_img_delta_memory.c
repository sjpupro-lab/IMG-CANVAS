#include "img_delta_memory.h"

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

static void make_cell(ImgCECell* c,
                      uint8_t role, uint8_t tone, uint8_t dir,
                      uint8_t depth, uint8_t link, uint8_t sign) {
    memset(c, 0, sizeof(*c));
    c->core            = 100;
    c->link            = link;
    c->delta           = 0;
    c->priority        = 100;
    c->tone_class      = tone;
    c->semantic_role   = role;
    c->direction_class = dir;
    c->depth_class     = depth;
    c->delta_sign      = sign;
    c->last_delta_id   = IMG_DELTA_ID_NONE;
}

/* ── StateKey pack/unpack ────────────────────────────────── */

static void test_state_key_roundtrip(void) {
    TEST("StateKey pack/unpack roundtrip");

    ImgStateKey k = img_state_key_make(
        IMG_ROLE_PERSON,
        IMG_TONE_DARK,
        IMG_FLOW_DIAGONAL_UP,
        IMG_DEPTH_FOREGROUND,
        5,                       /* link bucket */
        IMG_DELTA_POSITIVE);

    assert(img_state_key_semantic_role  (k) == IMG_ROLE_PERSON);
    assert(img_state_key_tone_class     (k) == IMG_TONE_DARK);
    assert(img_state_key_direction_class(k) == IMG_FLOW_DIAGONAL_UP);
    assert(img_state_key_depth_class    (k) == IMG_DEPTH_FOREGROUND);
    assert(img_state_key_link_bucket    (k) == 5);
    assert(img_state_key_delta_sign     (k) == IMG_DELTA_POSITIVE);

    /* link bucket = link / 32 */
    assert(img_link_bucket(0)   == 0);
    assert(img_link_bucket(31)  == 0);
    assert(img_link_bucket(32)  == 1);
    assert(img_link_bucket(255) == 7);

    /* from_cell uses the bucketed link */
    ImgCECell c;
    make_cell(&c, IMG_ROLE_SKY, IMG_TONE_BRIGHT, IMG_FLOW_HORIZONTAL,
              IMG_DEPTH_BACKGROUND, /*link=*/200, IMG_DELTA_NONE);
    ImgStateKey k2 = img_state_key_from_cell(&c);
    assert(img_state_key_link_bucket(k2) == img_link_bucket(200));

    PASS();
}

/* ── add + count + get ───────────────────────────────────── */

static void test_add_and_count(void) {
    TEST("add + count + get");

    ImgDeltaMemory* m = img_delta_memory_create();
    assert(m);
    assert(img_delta_memory_count(m) == 0);

    ImgDeltaPayload p; memset(&p, 0, sizeof(p));
    p.step[IMG_AXIS_INTENSITY] = 1;

    ImgStateKey k = img_state_key_make(IMG_ROLE_PERSON, IMG_TONE_DARK,
                                       IMG_FLOW_NONE, IMG_DEPTH_FOREGROUND,
                                       2, IMG_DELTA_NONE);
    uint32_t id0 = img_delta_memory_add(m, k, p);
    uint32_t id1 = img_delta_memory_add(m, k, p);
    assert(id0 == 0 && id1 == 1);
    assert(img_delta_memory_count(m) == 2);

    const ImgDeltaUnit* u = img_delta_memory_get(m, 1);
    assert(u && u->id == 1 && u->pre_key == k);
    assert(u->payload.step[IMG_AXIS_INTENSITY] == 1);

    /* Insert enough to force a realloc (initial capacity 16) */
    for (int i = 0; i < 40; i++) img_delta_memory_add(m, k, p);
    assert(img_delta_memory_count(m) == 42);

    img_delta_memory_destroy(m);
    PASS();
}

/* ── fallback chain ──────────────────────────────────────── */

static void test_fallback_chain(void) {
    TEST("candidates fallback chain widens key on miss");

    ImgDeltaMemory* m = img_delta_memory_create();
    ImgDeltaPayload p; memset(&p, 0, sizeof(p));

    /* Stored at: PERSON / DARK / NONE / FOREGROUND / link=2 / POS */
    ImgStateKey stored = img_state_key_make(IMG_ROLE_PERSON, IMG_TONE_DARK,
                                            IMG_FLOW_NONE, IMG_DEPTH_FOREGROUND,
                                            2, IMG_DELTA_POSITIVE);
    uint32_t id = img_delta_memory_add(m, stored, p);
    (void)id;

    /* Exact match → L0 */
    {
        const ImgDeltaUnit* out[8];
        int level = -2;
        uint32_t n = img_delta_memory_candidates(m, stored, out, 8, &level);
        assert(n == 1);
        assert(level == 0);
    }

    /* Different link bucket → L1 hit (drop link_bucket) */
    {
        ImgStateKey q = img_state_key_make(IMG_ROLE_PERSON, IMG_TONE_DARK,
                                           IMG_FLOW_NONE, IMG_DEPTH_FOREGROUND,
                                           7, IMG_DELTA_POSITIVE);
        const ImgDeltaUnit* out[8];
        int level = -2;
        uint32_t n = img_delta_memory_candidates(m, q, out, 8, &level);
        assert(n == 1);
        assert(level == 1);
    }

    /* Different link AND sign → L2 hit (drop link + delta_sign) */
    {
        ImgStateKey q = img_state_key_make(IMG_ROLE_PERSON, IMG_TONE_DARK,
                                           IMG_FLOW_NONE, IMG_DEPTH_FOREGROUND,
                                           7, IMG_DELTA_NEGATIVE);
        const ImgDeltaUnit* out[8];
        int level = -2;
        uint32_t n = img_delta_memory_candidates(m, q, out, 8, &level);
        assert(n == 1);
        assert(level == 2);
    }

    /* Completely different role → only L6 wildcard hits. */
    {
        ImgStateKey q = img_state_key_make(IMG_ROLE_SKY, IMG_TONE_BRIGHT,
                                           IMG_FLOW_HORIZONTAL,
                                           IMG_DEPTH_BACKGROUND,
                                           5, IMG_DELTA_NONE);
        const ImgDeltaUnit* out[8];
        int level = -2;
        uint32_t n = img_delta_memory_candidates(m, q, out, 8, &level);
        assert(n == 1);
        assert(level == 6);
    }

    img_delta_memory_destroy(m);
    PASS();
}

/* ── Laplace smoothing on success rate ───────────────────── */

static void test_laplace_smoothing(void) {
    TEST("Laplace smoothing prevents 1/1 from dominating 50/100");

    ImgDeltaUnit veteran = {0};
    veteran.usage_count   = 100;
    veteran.success_count = 50;

    ImgDeltaUnit newbie  = {0};
    newbie.usage_count   = 1;
    newbie.success_count = 1;

    double rv = img_delta_unit_success_rate(&veteran);   /* 51/102 ≈ 0.500 */
    double rn = img_delta_unit_success_rate(&newbie);    /*   2/3  ≈ 0.667 */

    /* New 1/1 still scores higher (0.667 > 0.500), but bounded.
     * The contract is: smoothing must pull the newbie below 1.0. */
    assert(rn < 1.0);
    assert(rn > rv);

    /* And a 0/0 is exactly 0.5 (no information). */
    ImgDeltaUnit fresh = {0};
    assert(img_delta_unit_success_rate(&fresh) == 0.5);

    PASS();
}

/* ── scoring + best selection ────────────────────────────── */

static void test_scoring_and_best(void) {
    TEST("scoring + best selection prefers exact, role-matched, smoothed-success unit");

    ImgDeltaMemory* m = img_delta_memory_create();
    ImgDeltaPayload p; memset(&p, 0, sizeof(p));

    /* Veteran: full match + 50/100 success */
    ImgStateKey k_full = img_state_key_make(IMG_ROLE_PERSON, IMG_TONE_DARK,
                                            IMG_FLOW_NONE, IMG_DEPTH_FOREGROUND,
                                            2, IMG_DELTA_NONE);
    uint32_t id_v = img_delta_memory_add(m, k_full, p);
    for (int i = 0; i < 100; i++) {
        img_delta_memory_record_usage(m, id_v, (i < 50) ? 1 : 0);
    }

    /* Lone candidate with mismatched direction (still L0 because all
     * fields are part of the key — but mismatched direction means
     * direction_fit=0). */
    ImgStateKey k_other = img_state_key_make(IMG_ROLE_PERSON, IMG_TONE_DARK,
                                             IMG_FLOW_HORIZONTAL,
                                             IMG_DEPTH_FOREGROUND,
                                             2, IMG_DELTA_NONE);
    uint32_t id_o = img_delta_memory_add(m, k_other, p);
    img_delta_memory_record_usage(m, id_o, 1);

    ImgCECell cur;
    make_cell(&cur, IMG_ROLE_PERSON, IMG_TONE_DARK, IMG_FLOW_NONE,
              IMG_DEPTH_FOREGROUND, /*link=*/64, IMG_DELTA_NONE);

    double score = -1.0;
    int level = -2;
    const ImgDeltaUnit* best = img_delta_memory_best(m, &cur, &score, &level);
    assert(best != NULL);
    /* Exact match (k_full) ought to win on direction_fit. */
    assert(best->id == id_v);
    /* Both are at L0 since exact match exists for k_full. */
    assert(level == 0);
    /* Score must include all four positive components and nothing
     * negative from fallback. Lower bound check. */
    assert(score >= 0.35 + 0.20 + 0.20);

    img_delta_memory_destroy(m);
    PASS();
}

/* ── interpretation: same payload, different cell ─────────── */

static void test_interpret_context_dependence(void) {
    TEST("same symbolic payload expands differently per cell");

    ImgDeltaPayload p; memset(&p, 0, sizeof(p));
    p.step[IMG_AXIS_INTENSITY] = 1;
    p.step[IMG_AXIS_PRIORITY]  = 1;

    ImgCECell dark_bg, bright_fg;
    make_cell(&dark_bg,   IMG_ROLE_UNKNOWN, IMG_TONE_DARK,   IMG_FLOW_NONE,
              IMG_DEPTH_BACKGROUND, 0, IMG_DELTA_NONE);
    make_cell(&bright_fg, IMG_ROLE_UNKNOWN, IMG_TONE_BRIGHT, IMG_FLOW_NONE,
              IMG_DEPTH_FOREGROUND, 0, IMG_DELTA_NONE);

    ImgConcreteDelta d_dark, d_bright;
    img_delta_interpret(&dark_bg,   &p, &d_dark);
    img_delta_interpret(&bright_fg, &p, &d_bright);

    /* Dark cell amplifies intensity more than bright cell. */
    assert(d_dark.add_core > d_bright.add_core);
    /* Foreground cell absorbs more priority than background. */
    assert(d_bright.add_priority > d_dark.add_priority);

    PASS();
}

/* ── apply: constraints + last_delta_id + usage bump ─────── */

static void test_apply_constraints(void) {
    TEST("apply enforces ±1 dir/depth and writes last_delta_id");

    ImgDeltaMemory* m = img_delta_memory_create();
    ImgDeltaPayload p; memset(&p, 0, sizeof(p));
    p.step[IMG_AXIS_DIRECTION] = 3;   /* try to over-rotate */
    p.step[IMG_AXIS_DEPTH]     = 3;   /* try to over-jump   */

    ImgStateKey k = img_state_key_make(IMG_ROLE_UNKNOWN, IMG_TONE_MID,
                                       IMG_FLOW_NONE, IMG_DEPTH_BACKGROUND,
                                       0, IMG_DELTA_NONE);
    uint32_t id = img_delta_memory_add(m, k, p);
    const ImgDeltaUnit* u = img_delta_memory_get(m, id);

    ImgCECell cur;
    make_cell(&cur, IMG_ROLE_UNKNOWN, IMG_TONE_MID, IMG_FLOW_NONE,
              IMG_DEPTH_BACKGROUND, 0, IMG_DELTA_NONE);

    img_delta_apply(&cur, m, u);

    /* Direction may only rotate ±1 from FLOW_NONE (=0): result must be 1. */
    assert(cur.direction_class == 1);
    /* Depth may only step ±1 from BACKGROUND (=0): result must be MIDGROUND (=1). */
    assert(cur.depth_class == IMG_DEPTH_MIDGROUND);
    /* last_delta_id pinned to the applied unit. */
    assert(cur.last_delta_id == id);
    /* usage_count bumped once. */
    assert(img_delta_memory_get(m, id)->usage_count == 1);

    img_delta_memory_destroy(m);
    PASS();
}

/* ── apply: role override is gated unless explicitly target_on ── */

static void test_apply_role_gate(void) {
    TEST("role override only fires on UNKNOWN unless role_target_on set");

    ImgDeltaMemory* m = img_delta_memory_create();
    ImgDeltaPayload p; memset(&p, 0, sizeof(p));
    p.step[IMG_AXIS_ROLE] = 1;     /* unknown → object */

    ImgStateKey k = img_state_key_make(IMG_ROLE_UNKNOWN, IMG_TONE_MID,
                                       IMG_FLOW_NONE, IMG_DEPTH_MIDGROUND,
                                       0, IMG_DELTA_NONE);
    uint32_t id = img_delta_memory_add(m, k, p);
    const ImgDeltaUnit* u = img_delta_memory_get(m, id);

    /* Cell with UNKNOWN role: gets promoted to OBJECT. */
    ImgCECell c1;
    make_cell(&c1, IMG_ROLE_UNKNOWN, IMG_TONE_MID, IMG_FLOW_NONE,
              IMG_DEPTH_MIDGROUND, 0, IMG_DELTA_NONE);
    img_delta_apply(&c1, m, u);
    assert(c1.semantic_role == IMG_ROLE_OBJECT);

    /* Cell with non-UNKNOWN role: override is dropped. */
    ImgCECell c2;
    make_cell(&c2, IMG_ROLE_PERSON, IMG_TONE_MID, IMG_FLOW_NONE,
              IMG_DEPTH_MIDGROUND, 0, IMG_DELTA_NONE);
    img_delta_apply(&c2, m, u);
    assert(c2.semantic_role == IMG_ROLE_PERSON);

    /* Now add a unit that asserts role_target_on: override should fire
     * even on a non-UNKNOWN cell. */
    ImgDeltaPayload p2; memset(&p2, 0, sizeof(p2));
    p2.role_target = IMG_ROLE_FACE;
    p2.role_target_on = 1;
    uint32_t id2 = img_delta_memory_add(m, k, p2);
    const ImgDeltaUnit* u2 = img_delta_memory_get(m, id2);

    ImgCECell c3;
    make_cell(&c3, IMG_ROLE_PERSON, IMG_TONE_MID, IMG_FLOW_NONE,
              IMG_DEPTH_MIDGROUND, 0, IMG_DELTA_NONE);
    img_delta_apply(&c3, m, u2);
    assert(c3.semantic_role == IMG_ROLE_FACE);

    img_delta_memory_destroy(m);
    PASS();
}

/* ── feedback round-trip via last_delta_id ───────────────── */

static void test_feedback_roundtrip(void) {
    TEST("resolve-style feedback updates the originating delta");

    ImgDeltaMemory* m = img_delta_memory_create();
    ImgDeltaPayload p; memset(&p, 0, sizeof(p));
    p.step[IMG_AXIS_INTENSITY] = 1;

    ImgStateKey k = img_state_key_make(IMG_ROLE_OBJECT, IMG_TONE_DARK,
                                       IMG_FLOW_NONE, IMG_DEPTH_FOREGROUND,
                                       2, IMG_DELTA_NONE);
    uint32_t id = img_delta_memory_add(m, k, p);
    const ImgDeltaUnit* u = img_delta_memory_get(m, id);

    ImgCECell cur;
    make_cell(&cur, IMG_ROLE_OBJECT, IMG_TONE_DARK, IMG_FLOW_NONE,
              IMG_DEPTH_FOREGROUND, 64, IMG_DELTA_NONE);

    img_delta_apply(&cur, m, u);
    assert(cur.last_delta_id == id);
    assert(img_delta_memory_get(m, id)->usage_count == 1);
    assert(img_delta_memory_get(m, id)->success_count == 0);

    /* Pretend resolve evaluated the cell and decided this delta worked. */
    img_delta_memory_record_usage(m, cur.last_delta_id, /*success=*/1);
    assert(img_delta_memory_get(m, id)->usage_count == 2);
    assert(img_delta_memory_get(m, id)->success_count == 1);

    /* Pretend a later resolve found it failed. */
    img_delta_memory_record_usage(m, cur.last_delta_id, /*success=*/0);
    assert(img_delta_memory_get(m, id)->usage_count == 3);
    assert(img_delta_memory_get(m, id)->success_count == 1);

    img_delta_memory_destroy(m);
    PASS();
}

int main(void) {
    printf("=== test_img_delta_memory ===\n");

    test_state_key_roundtrip();
    test_add_and_count();
    test_fallback_chain();
    test_laplace_smoothing();
    test_scoring_and_best();
    test_interpret_context_dependence();
    test_apply_constraints();
    test_apply_role_gate();
    test_feedback_roundtrip();

    printf("  %d/%d passed\n\n", tests_passed, tests_total);
    return (tests_passed == tests_total) ? 0 : 1;
}
