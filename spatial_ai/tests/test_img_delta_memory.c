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

/* ── helpers ─────────────────────────────────────────────── */

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

static ImgDeltaPayload payload_simple(uint8_t tier, uint8_t scale,
                                      uint8_t sign, uint8_t mode) {
    ImgDeltaPayload p;
    memset(&p, 0, sizeof(p));
    p.state = img_delta_state_simple(tier, scale, sign, mode);
    return p;
}

/* ── DeltaState pack / bounds ────────────────────────────── */

static void test_delta_state_pack(void) {
    TEST("DeltaState pack/unpack + validity bounds");

    ImgDeltaState s = img_delta_state_make(
        /*tier=*/           IMG_TIER_T2,
        /*scale=*/          5,
        /*precision=*/      2,
        /*sign=*/           IMG_SIGN_POS,
        /*tick=*/           9,
        /*mode=*/           IMG_MODE_INTENSITY,
        /*channel_layout=*/ 3,
        /*slot_shape=*/     12);

    assert(img_delta_state_tier          (s) == IMG_TIER_T2);
    assert(img_delta_state_scale         (s) == 5);
    assert(img_delta_state_precision     (s) == 2);
    assert(img_delta_state_sign          (s) == IMG_SIGN_POS);
    assert(img_delta_state_tick          (s) == 9);
    assert(img_delta_state_mode          (s) == IMG_MODE_INTENSITY);
    assert(img_delta_state_channel_layout(s) == 3);
    assert(img_delta_state_slot_shape    (s) == 12);
    assert(img_delta_state_is_valid(s));

    /* Out-of-range values get clamped to (MAX-1). */
    ImgDeltaState clamped = img_delta_state_make(99, 99, 99, 99,
                                                 99, 99, 99, 99);
    assert(img_delta_state_tier          (clamped) == IMG_TIER_MAX - 1);
    assert(img_delta_state_scale         (clamped) == IMG_SCALE_MAX - 1);
    assert(img_delta_state_precision     (clamped) == IMG_PRECISION_MAX - 1);
    assert(img_delta_state_sign          (clamped) == IMG_SIGN_MAX - 1);
    assert(img_delta_state_tick          (clamped) == IMG_TICK_MAX - 1);
    assert(img_delta_state_mode          (clamped) == IMG_MODE_MAX - 1);
    assert(img_delta_state_channel_layout(clamped) == IMG_CHANNEL_LAYOUT_MAX - 1);
    assert(img_delta_state_slot_shape    (clamped) == IMG_SLOT_SHAPE_MAX - 1);

    /* SPEC §3.1: every axis must be bounded. Entire u32 state must fit
     * in the declared cartesian product. */
    const uint64_t space =
        (uint64_t)IMG_TIER_MAX * IMG_SCALE_MAX * IMG_PRECISION_MAX *
        IMG_SIGN_MAX * IMG_TICK_MAX * IMG_MODE_MAX *
        IMG_CHANNEL_LAYOUT_MAX * IMG_SLOT_SHAPE_MAX;
    assert(space <= (1ULL << 23));   /* 23 bits used */

    PASS();
}

/* ── StateKey pack/unpack ────────────────────────────────── */

static void test_state_key_roundtrip(void) {
    TEST("StateKey pack/unpack roundtrip + link bucketing");

    ImgStateKey k = img_state_key_make(
        IMG_ROLE_PERSON,
        IMG_TONE_DARK,
        IMG_FLOW_DIAGONAL_UP,
        IMG_DEPTH_FOREGROUND,
        5,
        IMG_DELTA_POSITIVE);

    assert(img_state_key_semantic_role  (k) == IMG_ROLE_PERSON);
    assert(img_state_key_tone_class     (k) == IMG_TONE_DARK);
    assert(img_state_key_direction_class(k) == IMG_FLOW_DIAGONAL_UP);
    assert(img_state_key_depth_class    (k) == IMG_DEPTH_FOREGROUND);
    assert(img_state_key_link_bucket    (k) == 5);
    assert(img_state_key_delta_sign     (k) == IMG_DELTA_POSITIVE);

    assert(img_link_bucket(0)   == 0);
    assert(img_link_bucket(31)  == 0);
    assert(img_link_bucket(32)  == 1);
    assert(img_link_bucket(255) == 7);

    ImgCECell c;
    make_cell(&c, IMG_ROLE_SKY, IMG_TONE_BRIGHT, IMG_FLOW_HORIZONTAL,
              IMG_DEPTH_BACKGROUND, /*link=*/200, IMG_DELTA_NONE);
    ImgStateKey k2 = img_state_key_from_cell(&c);
    assert(img_state_key_link_bucket(k2) == img_link_bucket(200));

    PASS();
}

/* ── add + count + get ───────────────────────────────────── */

static void test_add_and_count(void) {
    TEST("add + count + get (DeltaState-backed payload)");

    ImgDeltaMemory* m = img_delta_memory_create();
    assert(m);
    assert(img_delta_memory_count(m) == 0);

    ImgDeltaPayload p = payload_simple(IMG_TIER_T1, 2, IMG_SIGN_POS,
                                       IMG_MODE_INTENSITY);

    ImgStateKey k = img_state_key_make(IMG_ROLE_PERSON, IMG_TONE_DARK,
                                       IMG_FLOW_NONE, IMG_DEPTH_FOREGROUND,
                                       2, IMG_DELTA_NONE);
    uint32_t id0 = img_delta_memory_add(m, k, p);
    uint32_t id1 = img_delta_memory_add(m, k, p);
    assert(id0 == 0 && id1 == 1);
    assert(img_delta_memory_count(m) == 2);

    const ImgDeltaUnit* u = img_delta_memory_get(m, 1);
    assert(u && u->id == 1 && u->pre_key == k);
    assert(img_delta_state_mode(u->payload.state) == IMG_MODE_INTENSITY);
    assert(img_delta_state_tier(u->payload.state) == IMG_TIER_T1);

    for (int i = 0; i < 40; i++) img_delta_memory_add(m, k, p);
    assert(img_delta_memory_count(m) == 42);

    img_delta_memory_destroy(m);
    PASS();
}

/* ── fallback chain ──────────────────────────────────────── */

static void test_fallback_chain(void) {
    TEST("candidates fallback chain widens key on miss");

    ImgDeltaMemory* m = img_delta_memory_create();
    ImgDeltaPayload p = payload_simple(IMG_TIER_T2, 3, IMG_SIGN_POS,
                                       IMG_MODE_INTENSITY);

    ImgStateKey stored = img_state_key_make(IMG_ROLE_PERSON, IMG_TONE_DARK,
                                            IMG_FLOW_NONE, IMG_DEPTH_FOREGROUND,
                                            2, IMG_DELTA_POSITIVE);
    (void)img_delta_memory_add(m, stored, p);

    {
        const ImgDeltaUnit* out[8]; int level = -2;
        uint32_t n = img_delta_memory_candidates(m, stored, out, 8, &level);
        assert(n == 1 && level == 0);
    }

    {
        ImgStateKey q = img_state_key_make(IMG_ROLE_PERSON, IMG_TONE_DARK,
                                           IMG_FLOW_NONE, IMG_DEPTH_FOREGROUND,
                                           7, IMG_DELTA_POSITIVE);
        const ImgDeltaUnit* out[8]; int level = -2;
        uint32_t n = img_delta_memory_candidates(m, q, out, 8, &level);
        assert(n == 1 && level == 1);
    }

    {
        ImgStateKey q = img_state_key_make(IMG_ROLE_PERSON, IMG_TONE_DARK,
                                           IMG_FLOW_NONE, IMG_DEPTH_FOREGROUND,
                                           7, IMG_DELTA_NEGATIVE);
        const ImgDeltaUnit* out[8]; int level = -2;
        uint32_t n = img_delta_memory_candidates(m, q, out, 8, &level);
        assert(n == 1 && level == 2);
    }

    {
        ImgStateKey q = img_state_key_make(IMG_ROLE_SKY, IMG_TONE_BRIGHT,
                                           IMG_FLOW_HORIZONTAL,
                                           IMG_DEPTH_BACKGROUND,
                                           5, IMG_DELTA_NONE);
        const ImgDeltaUnit* out[8]; int level = -2;
        uint32_t n = img_delta_memory_candidates(m, q, out, 8, &level);
        assert(n == 1 && level == 6);
    }

    img_delta_memory_destroy(m);
    PASS();
}

/* ── Laplace smoothing ──────────────────────────────────── */

static void test_laplace_smoothing(void) {
    TEST("Laplace smoothing prevents 1/1 from dominating 50/100");

    ImgDeltaUnit veteran = {0};
    veteran.usage_count   = 100;
    veteran.success_count = 50;

    ImgDeltaUnit newbie  = {0};
    newbie.usage_count   = 1;
    newbie.success_count = 1;

    double rv = img_delta_unit_success_rate(&veteran);
    double rn = img_delta_unit_success_rate(&newbie);

    assert(rn < 1.0);
    assert(rn > rv);

    ImgDeltaUnit fresh = {0};
    assert(img_delta_unit_success_rate(&fresh) == 0.5);

    PASS();
}

/* ── scoring + best selection ────────────────────────────── */

static void test_scoring_and_best(void) {
    TEST("scoring + best selection prefers exact, role-matched, smoothed-success unit");

    ImgDeltaMemory* m = img_delta_memory_create();
    ImgDeltaPayload p = payload_simple(IMG_TIER_T2, 3, IMG_SIGN_POS,
                                       IMG_MODE_INTENSITY);

    ImgStateKey k_full = img_state_key_make(IMG_ROLE_PERSON, IMG_TONE_DARK,
                                            IMG_FLOW_NONE, IMG_DEPTH_FOREGROUND,
                                            2, IMG_DELTA_NONE);
    uint32_t id_v = img_delta_memory_add(m, k_full, p);
    for (int i = 0; i < 100; i++) {
        img_delta_memory_record_usage(m, id_v, (i < 50) ? 1 : 0);
    }

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
    assert(best->id == id_v);
    assert(level == 0);
    assert(score >= 0.35 + 0.20 + 0.20);

    img_delta_memory_destroy(m);
    PASS();
}

/* ── interpret: same symbolic state, different cell context ── */

static void test_interpret_context_dependence(void) {
    TEST("same DeltaState expands differently per cell tags");

    ImgDeltaPayload p_intensity = payload_simple(
        IMG_TIER_T2, 3, IMG_SIGN_POS, IMG_MODE_INTENSITY);
    ImgDeltaPayload p_priority = payload_simple(
        IMG_TIER_T2, 3, IMG_SIGN_POS, IMG_MODE_PRIORITY);

    ImgCECell dark_bg, bright_fg;
    make_cell(&dark_bg,   IMG_ROLE_UNKNOWN, IMG_TONE_DARK,   IMG_FLOW_NONE,
              IMG_DEPTH_BACKGROUND, 0, IMG_DELTA_NONE);
    make_cell(&bright_fg, IMG_ROLE_UNKNOWN, IMG_TONE_BRIGHT, IMG_FLOW_NONE,
              IMG_DEPTH_FOREGROUND, 0, IMG_DELTA_NONE);

    ImgConcreteDelta d_dark_i, d_bright_i;
    img_delta_interpret(&dark_bg,   &p_intensity, &d_dark_i);
    img_delta_interpret(&bright_fg, &p_intensity, &d_bright_i);
    /* Dark cell amplifies INTENSITY more than bright cell. */
    assert(d_dark_i.add_core > d_bright_i.add_core);

    ImgConcreteDelta d_dark_p, d_bright_p;
    img_delta_interpret(&dark_bg,   &p_priority, &d_dark_p);
    img_delta_interpret(&bright_fg, &p_priority, &d_bright_p);
    /* Foreground absorbs PRIORITY more than background. */
    assert(d_bright_p.add_priority > d_dark_p.add_priority);

    PASS();
}

/* ── interpret: zero-state produces no change ───────────── */

static void test_interpret_zero_state(void) {
    TEST("zero tier/sign/mode yields empty concrete delta");

    ImgDeltaPayload p;
    memset(&p, 0, sizeof(p));   /* all zero → MODE_NONE / TIER_NONE */

    ImgCECell c;
    make_cell(&c, IMG_ROLE_PERSON, IMG_TONE_DARK, IMG_FLOW_NONE,
              IMG_DEPTH_FOREGROUND, 64, IMG_DELTA_NONE);

    ImgConcreteDelta d;
    img_delta_interpret(&c, &p, &d);

    assert(d.add_core == 0);
    assert(d.add_link == 0);
    assert(d.add_delta == 0);
    assert(d.add_priority == 0);
    assert(d.semantic_override_on == 0);
    assert(d.depth_override_on == 0);
    assert(d.direction_override_on == 0);
    assert(d.delta_sign_override_on == 0);

    PASS();
}

/* ── apply: direction / depth constrained to ±1 ─────────── */

static void test_apply_constraints(void) {
    TEST("apply enforces ±1 dir/depth and writes last_delta_id");

    ImgDeltaMemory* m = img_delta_memory_create();
    /* MODE_DIRECTION + POS + max scale → overrun prevented by ±1 rule */
    ImgDeltaPayload p_dir = payload_simple(IMG_TIER_T3, IMG_SCALE_MAX - 1,
                                           IMG_SIGN_POS, IMG_MODE_DIRECTION);
    ImgStateKey k = img_state_key_make(IMG_ROLE_UNKNOWN, IMG_TONE_MID,
                                       IMG_FLOW_NONE, IMG_DEPTH_BACKGROUND,
                                       0, IMG_DELTA_NONE);
    uint32_t id_dir = img_delta_memory_add(m, k, p_dir);

    /* Then MODE_DEPTH POS */
    ImgDeltaPayload p_depth = payload_simple(IMG_TIER_T3, IMG_SCALE_MAX - 1,
                                             IMG_SIGN_POS, IMG_MODE_DEPTH);
    uint32_t id_depth = img_delta_memory_add(m, k, p_depth);

    ImgCECell cur;
    make_cell(&cur, IMG_ROLE_UNKNOWN, IMG_TONE_MID, IMG_FLOW_NONE,
              IMG_DEPTH_BACKGROUND, 0, IMG_DELTA_NONE);

    img_delta_apply(&cur, m, img_delta_memory_get(m, id_dir));
    assert(cur.direction_class == 1);       /* +1 from FLOW_NONE(0) */
    assert(cur.last_delta_id == id_dir);
    assert(img_delta_memory_get(m, id_dir)->usage_count == 1);

    img_delta_apply(&cur, m, img_delta_memory_get(m, id_depth));
    assert(cur.depth_class == IMG_DEPTH_MIDGROUND);   /* BG → MID */
    assert(cur.last_delta_id == id_depth);

    img_delta_memory_destroy(m);
    PASS();
}

/* ── apply: role override gated ─────────────────────────── */

static void test_apply_role_gate(void) {
    TEST("role override only fires on UNKNOWN unless role_target_on set");

    ImgDeltaMemory* m = img_delta_memory_create();

    /* Implicit role promotion (POS on UNKNOWN → OBJECT). */
    ImgDeltaPayload p_implicit = payload_simple(
        IMG_TIER_T2, 2, IMG_SIGN_POS, IMG_MODE_ROLE);

    ImgStateKey k = img_state_key_make(IMG_ROLE_UNKNOWN, IMG_TONE_MID,
                                       IMG_FLOW_NONE, IMG_DEPTH_MIDGROUND,
                                       0, IMG_DELTA_NONE);
    uint32_t id_imp = img_delta_memory_add(m, k, p_implicit);

    ImgCECell c1;
    make_cell(&c1, IMG_ROLE_UNKNOWN, IMG_TONE_MID, IMG_FLOW_NONE,
              IMG_DEPTH_MIDGROUND, 0, IMG_DELTA_NONE);
    img_delta_apply(&c1, m, img_delta_memory_get(m, id_imp));
    assert(c1.semantic_role == IMG_ROLE_OBJECT);

    ImgCECell c2;
    make_cell(&c2, IMG_ROLE_PERSON, IMG_TONE_MID, IMG_FLOW_NONE,
              IMG_DEPTH_MIDGROUND, 0, IMG_DELTA_NONE);
    img_delta_apply(&c2, m, img_delta_memory_get(m, id_imp));
    /* Non-UNKNOWN role, no role_target_on → override dropped. */
    assert(c2.semantic_role == IMG_ROLE_PERSON);

    /* Explicit role target overrides the gate. */
    ImgDeltaPayload p_target;
    memset(&p_target, 0, sizeof(p_target));
    p_target.state = img_delta_state_simple(IMG_TIER_T2, 2,
                                            IMG_SIGN_POS, IMG_MODE_ROLE);
    p_target.role_target     = IMG_ROLE_FACE;
    p_target.role_target_on  = 1;
    uint32_t id_tgt = img_delta_memory_add(m, k, p_target);

    ImgCECell c3;
    make_cell(&c3, IMG_ROLE_PERSON, IMG_TONE_MID, IMG_FLOW_NONE,
              IMG_DEPTH_MIDGROUND, 0, IMG_DELTA_NONE);
    img_delta_apply(&c3, m, img_delta_memory_get(m, id_tgt));
    assert(c3.semantic_role == IMG_ROLE_FACE);

    img_delta_memory_destroy(m);
    PASS();
}

/* ── feedback round-trip via last_delta_id ───────────────── */

static void test_feedback_roundtrip(void) {
    TEST("resolve-style feedback updates the originating delta");

    ImgDeltaMemory* m = img_delta_memory_create();
    ImgDeltaPayload p = payload_simple(IMG_TIER_T1, 2, IMG_SIGN_POS,
                                       IMG_MODE_INTENSITY);

    ImgStateKey k = img_state_key_make(IMG_ROLE_OBJECT, IMG_TONE_DARK,
                                       IMG_FLOW_NONE, IMG_DEPTH_FOREGROUND,
                                       2, IMG_DELTA_NONE);
    uint32_t id = img_delta_memory_add(m, k, p);

    ImgCECell cur;
    make_cell(&cur, IMG_ROLE_OBJECT, IMG_TONE_DARK, IMG_FLOW_NONE,
              IMG_DEPTH_FOREGROUND, 64, IMG_DELTA_NONE);

    img_delta_apply(&cur, m, img_delta_memory_get(m, id));
    assert(cur.last_delta_id == id);
    assert(img_delta_memory_get(m, id)->usage_count == 1);
    assert(img_delta_memory_get(m, id)->success_count == 0);

    img_delta_memory_record_usage(m, cur.last_delta_id, 1);
    assert(img_delta_memory_get(m, id)->usage_count == 2);
    assert(img_delta_memory_get(m, id)->success_count == 1);

    img_delta_memory_record_usage(m, cur.last_delta_id, 0);
    assert(img_delta_memory_get(m, id)->usage_count == 3);
    assert(img_delta_memory_get(m, id)->success_count == 1);

    img_delta_memory_destroy(m);
    PASS();
}

/* ── Phase B: SoA lookup tables ──────────────────────────── */

static void test_tables_init_idempotent_and_sized(void) {
    TEST("Phase B tables: init idempotent + memory/entry sanity");

    /* Eager init, then a second call must be a no-op. */
    img_delta_tables_init();
    img_delta_tables_init();

    uint32_t entries = img_delta_tables_entry_count();
    uint32_t bytes   = img_delta_tables_memory_bytes();

    /* 8 × 4 × 8 × 4 × 3 × 3 = 9216 entries per SoA array. */
    assert(entries == (uint32_t)(IMG_MODE_MAX * IMG_TIER_MAX *
                                 IMG_SCALE_MAX * IMG_SIGN_MAX * 3 * 3));
    assert(entries == 9216);

    /* 4 × (int16 × N) + 1 × (u8 × N) + small step tables. Should live
     * comfortably in L2 (< 256 KB budget). */
    assert(bytes >= 9 * entries);     /* ≥ 4×2 + 1 bytes per slot */
    assert(bytes < 256u * 1024u);

    PASS();
}

static void test_lookup_covers_all_modes(void) {
    TEST("lookup fires the right channel for each mode");

    ImgCECell c;
    make_cell(&c, IMG_ROLE_UNKNOWN, IMG_TONE_DARK, IMG_FLOW_NONE,
              IMG_DEPTH_FOREGROUND, 0, IMG_DELTA_NONE);

    ImgConcreteDelta d;

    /* INTENSITY → add_core nonzero, others zero. */
    ImgDeltaPayload p = payload_simple(IMG_TIER_T2, 3, IMG_SIGN_POS,
                                       IMG_MODE_INTENSITY);
    img_delta_interpret(&c, &p, &d);
    assert(d.add_core != 0);
    assert(d.add_link == 0 && d.add_delta == 0 && d.add_priority == 0);
    assert(!d.direction_override_on && !d.depth_override_on);

    /* LINK → add_link only. */
    p = payload_simple(IMG_TIER_T2, 3, IMG_SIGN_POS, IMG_MODE_LINK);
    img_delta_interpret(&c, &p, &d);
    assert(d.add_link != 0);
    assert(d.add_core == 0 && d.add_delta == 0 && d.add_priority == 0);

    /* PRIORITY → add_priority only. */
    p = payload_simple(IMG_TIER_T2, 3, IMG_SIGN_POS, IMG_MODE_PRIORITY);
    img_delta_interpret(&c, &p, &d);
    assert(d.add_priority != 0);
    assert(d.add_core == 0 && d.add_link == 0 && d.add_delta == 0);

    /* MOOD → add_delta + delta_sign_override. */
    p = payload_simple(IMG_TIER_T2, 3, IMG_SIGN_POS, IMG_MODE_MOOD);
    img_delta_interpret(&c, &p, &d);
    assert(d.add_delta != 0);
    assert(d.delta_sign_override_on && d.delta_sign_override == IMG_DELTA_POSITIVE);

    /* DIRECTION → direction_override_on only, channels untouched. */
    p = payload_simple(IMG_TIER_T2, 3, IMG_SIGN_POS, IMG_MODE_DIRECTION);
    img_delta_interpret(&c, &p, &d);
    assert(d.direction_override_on);
    assert(d.add_core == 0 && d.add_link == 0
           && d.add_delta == 0 && d.add_priority == 0);

    /* DEPTH → depth_override_on only. */
    p = payload_simple(IMG_TIER_T2, 3, IMG_SIGN_POS, IMG_MODE_DEPTH);
    img_delta_interpret(&c, &p, &d);
    assert(d.depth_override_on);

    /* ROLE POS on UNKNOWN → semantic_override_on → OBJECT. */
    p = payload_simple(IMG_TIER_T2, 3, IMG_SIGN_POS, IMG_MODE_ROLE);
    img_delta_interpret(&c, &p, &d);
    assert(d.semantic_override_on && d.semantic_override == IMG_ROLE_OBJECT);

    PASS();
}

static void test_lookup_sign_symmetry(void) {
    TEST("SIGN_POS and SIGN_NEG produce opposite numeric contributions");

    ImgCECell c;
    make_cell(&c, IMG_ROLE_UNKNOWN, IMG_TONE_MID, IMG_FLOW_NONE,
              IMG_DEPTH_MIDGROUND, 0, IMG_DELTA_NONE);

    ImgConcreteDelta dp, dn;
    ImgDeltaPayload p_pos = payload_simple(IMG_TIER_T2, 3, IMG_SIGN_POS,
                                           IMG_MODE_INTENSITY);
    ImgDeltaPayload p_neg = payload_simple(IMG_TIER_T2, 3, IMG_SIGN_NEG,
                                           IMG_MODE_INTENSITY);

    img_delta_interpret(&c, &p_pos, &dp);
    img_delta_interpret(&c, &p_neg, &dn);

    assert(dp.add_core > 0 && dn.add_core < 0);
    assert(dp.add_core == -dn.add_core);

    PASS();
}

int main(void) {
    printf("=== test_img_delta_memory ===\n");

    test_delta_state_pack();
    test_state_key_roundtrip();
    test_add_and_count();
    test_fallback_chain();
    test_laplace_smoothing();
    test_scoring_and_best();
    test_interpret_context_dependence();
    test_interpret_zero_state();
    test_apply_constraints();
    test_apply_role_gate();
    test_feedback_roundtrip();

    /* Phase B */
    test_tables_init_idempotent_and_sized();
    test_lookup_covers_all_modes();
    test_lookup_sign_symmetry();

    printf("  %d/%d passed\n\n", tests_passed, tests_total);
    return (tests_passed == tests_total) ? 0 : 1;
}
