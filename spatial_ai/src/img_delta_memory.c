#include "img_delta_memory.h"

#include <stdlib.h>
#include <string.h>

/* ── small helpers ──────────────────────────────────────── */

static inline int clamp_i(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline uint8_t sat_add_u8(uint8_t a, int delta) {
    int v = (int)a + delta;
    if (v < 0)   v = 0;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

/* ── DeltaState packing (SPEC §3.1, §3.3) ───────────────── */

#define DS_TIER_SHIFT            0
#define DS_SCALE_SHIFT           2
#define DS_PRECISION_SHIFT       5
#define DS_SIGN_SHIFT            7
#define DS_TICK_SHIFT            9
#define DS_MODE_SHIFT           13
#define DS_CHANNEL_LAYOUT_SHIFT 16
#define DS_SLOT_SHAPE_SHIFT     19

#define DS_TIER_MASK            (0x3u   << DS_TIER_SHIFT)            /* 2 bits */
#define DS_SCALE_MASK           (0x7u   << DS_SCALE_SHIFT)           /* 3 bits */
#define DS_PRECISION_MASK       (0x3u   << DS_PRECISION_SHIFT)       /* 2 bits */
#define DS_SIGN_MASK            (0x3u   << DS_SIGN_SHIFT)            /* 2 bits */
#define DS_TICK_MASK            (0xFu   << DS_TICK_SHIFT)            /* 4 bits */
#define DS_MODE_MASK            (0x7u   << DS_MODE_SHIFT)            /* 3 bits */
#define DS_CHANNEL_LAYOUT_MASK  (0x7u   << DS_CHANNEL_LAYOUT_SHIFT)  /* 3 bits */
#define DS_SLOT_SHAPE_MASK      (0xFu   << DS_SLOT_SHAPE_SHIFT)      /* 4 bits */

static inline uint32_t ds_clamp(uint32_t v, uint32_t max) {
    return (v < max) ? v : (max - 1);
}

ImgDeltaState img_delta_state_make(uint8_t tier, uint8_t scale,
                                   uint8_t precision, uint8_t sign,
                                   uint8_t tick, uint8_t mode,
                                   uint8_t channel_layout,
                                   uint8_t slot_shape) {
    return ((uint32_t)ds_clamp(tier,           IMG_TIER_MAX)           << DS_TIER_SHIFT)
         | ((uint32_t)ds_clamp(scale,          IMG_SCALE_MAX)          << DS_SCALE_SHIFT)
         | ((uint32_t)ds_clamp(precision,      IMG_PRECISION_MAX)      << DS_PRECISION_SHIFT)
         | ((uint32_t)ds_clamp(sign,           IMG_SIGN_MAX)           << DS_SIGN_SHIFT)
         | ((uint32_t)ds_clamp(tick,           IMG_TICK_MAX)           << DS_TICK_SHIFT)
         | ((uint32_t)ds_clamp(mode,           IMG_MODE_MAX)           << DS_MODE_SHIFT)
         | ((uint32_t)ds_clamp(channel_layout, IMG_CHANNEL_LAYOUT_MAX) << DS_CHANNEL_LAYOUT_SHIFT)
         | ((uint32_t)ds_clamp(slot_shape,     IMG_SLOT_SHAPE_MAX)     << DS_SLOT_SHAPE_SHIFT);
}

ImgDeltaState img_delta_state_simple(uint8_t tier, uint8_t scale,
                                     uint8_t sign, uint8_t mode) {
    return img_delta_state_make(tier, scale, /*precision=*/0, sign,
                                /*tick=*/0, mode,
                                /*channel_layout=*/0,
                                /*slot_shape=*/0);
}

uint8_t img_delta_state_tier          (ImgDeltaState s) { return (uint8_t)((s & DS_TIER_MASK)           >> DS_TIER_SHIFT); }
uint8_t img_delta_state_scale         (ImgDeltaState s) { return (uint8_t)((s & DS_SCALE_MASK)          >> DS_SCALE_SHIFT); }
uint8_t img_delta_state_precision     (ImgDeltaState s) { return (uint8_t)((s & DS_PRECISION_MASK)      >> DS_PRECISION_SHIFT); }
uint8_t img_delta_state_sign          (ImgDeltaState s) { return (uint8_t)((s & DS_SIGN_MASK)           >> DS_SIGN_SHIFT); }
uint8_t img_delta_state_tick          (ImgDeltaState s) { return (uint8_t)((s & DS_TICK_MASK)           >> DS_TICK_SHIFT); }
uint8_t img_delta_state_mode          (ImgDeltaState s) { return (uint8_t)((s & DS_MODE_MASK)           >> DS_MODE_SHIFT); }
uint8_t img_delta_state_channel_layout(ImgDeltaState s) { return (uint8_t)((s & DS_CHANNEL_LAYOUT_MASK) >> DS_CHANNEL_LAYOUT_SHIFT); }
uint8_t img_delta_state_slot_shape    (ImgDeltaState s) { return (uint8_t)((s & DS_SLOT_SHAPE_MASK)     >> DS_SLOT_SHAPE_SHIFT); }

int img_delta_state_is_valid(ImgDeltaState s) {
    if (img_delta_state_tier          (s) >= IMG_TIER_MAX)           return 0;
    if (img_delta_state_scale         (s) >= IMG_SCALE_MAX)          return 0;
    if (img_delta_state_precision     (s) >= IMG_PRECISION_MAX)      return 0;
    if (img_delta_state_sign          (s) >= IMG_SIGN_MAX)           return 0;
    if (img_delta_state_tick          (s) >= IMG_TICK_MAX)           return 0;
    if (img_delta_state_mode          (s) >= IMG_MODE_MAX)           return 0;
    if (img_delta_state_channel_layout(s) >= IMG_CHANNEL_LAYOUT_MAX) return 0;
    if (img_delta_state_slot_shape    (s) >= IMG_SLOT_SHAPE_MAX)     return 0;
    return 1;
}

/* ── StateKey packing ───────────────────────────────────── */

#define KEY_DELTA_SIGN_SHIFT       0
#define KEY_LINK_BUCKET_SHIFT      8
#define KEY_DEPTH_CLASS_SHIFT     16
#define KEY_DIRECTION_CLASS_SHIFT 24
#define KEY_TONE_CLASS_SHIFT      32
#define KEY_SEMANTIC_ROLE_SHIFT   40

#define KEY_BYTE_MASK             0xFFULL

#define KEY_DELTA_SIGN_MASK       (KEY_BYTE_MASK << KEY_DELTA_SIGN_SHIFT)
#define KEY_LINK_BUCKET_MASK      (KEY_BYTE_MASK << KEY_LINK_BUCKET_SHIFT)
#define KEY_DEPTH_CLASS_MASK      (KEY_BYTE_MASK << KEY_DEPTH_CLASS_SHIFT)
#define KEY_DIRECTION_CLASS_MASK  (KEY_BYTE_MASK << KEY_DIRECTION_CLASS_SHIFT)
#define KEY_TONE_CLASS_MASK       (KEY_BYTE_MASK << KEY_TONE_CLASS_SHIFT)
#define KEY_SEMANTIC_ROLE_MASK    (KEY_BYTE_MASK << KEY_SEMANTIC_ROLE_SHIFT)

uint8_t img_link_bucket(uint8_t link) {
    return (uint8_t)(link >> 5);
}

ImgStateKey img_state_key_make(uint8_t role, uint8_t tone, uint8_t dir,
                               uint8_t depth, uint8_t link_bucket,
                               uint8_t sign) {
    return ((ImgStateKey)role  << KEY_SEMANTIC_ROLE_SHIFT)
         | ((ImgStateKey)tone  << KEY_TONE_CLASS_SHIFT)
         | ((ImgStateKey)dir   << KEY_DIRECTION_CLASS_SHIFT)
         | ((ImgStateKey)depth << KEY_DEPTH_CLASS_SHIFT)
         | ((ImgStateKey)link_bucket << KEY_LINK_BUCKET_SHIFT)
         | ((ImgStateKey)sign  << KEY_DELTA_SIGN_SHIFT);
}

ImgStateKey img_state_key_from_cell(const ImgCECell* cell) {
    if (!cell) return 0;
    return img_state_key_make(cell->semantic_role,
                              cell->tone_class,
                              cell->direction_class,
                              cell->depth_class,
                              img_link_bucket(cell->link),
                              cell->delta_sign);
}

uint8_t img_state_key_semantic_role  (ImgStateKey k) { return (uint8_t)((k >> KEY_SEMANTIC_ROLE_SHIFT)   & KEY_BYTE_MASK); }
uint8_t img_state_key_tone_class     (ImgStateKey k) { return (uint8_t)((k >> KEY_TONE_CLASS_SHIFT)      & KEY_BYTE_MASK); }
uint8_t img_state_key_direction_class(ImgStateKey k) { return (uint8_t)((k >> KEY_DIRECTION_CLASS_SHIFT) & KEY_BYTE_MASK); }
uint8_t img_state_key_depth_class    (ImgStateKey k) { return (uint8_t)((k >> KEY_DEPTH_CLASS_SHIFT)     & KEY_BYTE_MASK); }
uint8_t img_state_key_link_bucket    (ImgStateKey k) { return (uint8_t)((k >> KEY_LINK_BUCKET_SHIFT)     & KEY_BYTE_MASK); }
uint8_t img_state_key_delta_sign     (ImgStateKey k) { return (uint8_t)((k >> KEY_DELTA_SIGN_SHIFT)      & KEY_BYTE_MASK); }

/* Fallback chain (SPEC-aligned widening strategy). L0..L6. */
#define FALLBACK_LEVELS 7
static const ImgStateKey FALLBACK_MASKS[FALLBACK_LEVELS] = {
    /* L0 */ KEY_SEMANTIC_ROLE_MASK | KEY_TONE_CLASS_MASK | KEY_DIRECTION_CLASS_MASK |
             KEY_DEPTH_CLASS_MASK   | KEY_LINK_BUCKET_MASK | KEY_DELTA_SIGN_MASK,
    /* L1 */ KEY_SEMANTIC_ROLE_MASK | KEY_TONE_CLASS_MASK | KEY_DIRECTION_CLASS_MASK |
             KEY_DEPTH_CLASS_MASK   | KEY_DELTA_SIGN_MASK,
    /* L2 */ KEY_SEMANTIC_ROLE_MASK | KEY_TONE_CLASS_MASK | KEY_DIRECTION_CLASS_MASK |
             KEY_DEPTH_CLASS_MASK,
    /* L3 */ KEY_SEMANTIC_ROLE_MASK | KEY_DIRECTION_CLASS_MASK | KEY_DEPTH_CLASS_MASK,
    /* L4 */ KEY_SEMANTIC_ROLE_MASK | KEY_DEPTH_CLASS_MASK,
    /* L5 */ KEY_SEMANTIC_ROLE_MASK,
    /* L6 */ 0
};

/* ── Interpretation: pure SoA lookup (SPEC §13.2) ──────────
 *
 * One bounded DeltaState, expanded via precomputed tables keyed by
 *   (mode, tier, scale, sign, tone, depth)
 * into channel values and a packed pattern byte that drives the
 * tag-override flags. Direction / depth step-clamps come from tiny
 * side tables indexed by (current_class, sign).
 *
 * Each channel table is a flat int16 array of TABLE_N entries. The
 * pattern table is uint8 per entry, storing which overrides fire
 * and in what direction:
 *   bits 0..1  direction_sign   (0 none, 1 POS, 2 NEG)
 *   bits 2..3  depth_sign       (same)
 *   bits 4..5  mood_sign_fire   (0 none, 1 POS, 2 NEG)
 *   bits 6..7  role_flag        (0 none, 1 promote UNKNOWN→OBJECT,
 *                                2 demote → UNKNOWN)
 *
 * Layout index (LSB→MSB order in the flat array):
 *   idx = mode
 *       + tier  × MODE_MAX
 *       + scale × MODE_MAX × TIER_MAX
 *       + sign  × MODE_MAX × TIER_MAX × SCALE_MAX
 *       + tone  × …
 *       + depth × …
 */

#define IMG_TONE_BUCKETS   3
#define IMG_DEPTH_BUCKETS  3

#define IMG_DELTA_TABLE_N  (IMG_MODE_MAX * IMG_TIER_MAX * IMG_SCALE_MAX * \
                            IMG_SIGN_MAX * IMG_TONE_BUCKETS *            \
                            IMG_DEPTH_BUCKETS)

static int16_t  g_core_table    [IMG_DELTA_TABLE_N];
static int16_t  g_link_table    [IMG_DELTA_TABLE_N];
static int16_t  g_delta_table   [IMG_DELTA_TABLE_N];
static int16_t  g_priority_table[IMG_DELTA_TABLE_N];
static uint8_t  g_pattern_table [IMG_DELTA_TABLE_N];

/* Side tables for ±1 clamped steps. Size is small enough to sit
 * comfortably in L1. */
#define IMG_FLOW_BUCKETS   5   /* FLOW_NONE..DIAGONAL_DOWN */
static uint8_t g_direction_step[IMG_FLOW_BUCKETS][IMG_SIGN_MAX];
static uint8_t g_depth_step    [IMG_DEPTH_BUCKETS][IMG_SIGN_MAX];

static int g_tables_ready = 0;

static inline size_t img_delta_table_idx(uint8_t mode, uint8_t tier,
                                         uint8_t scale, uint8_t sign,
                                         uint8_t tone, uint8_t depth) {
    return (size_t)mode
         + (size_t)tier  * IMG_MODE_MAX
         + (size_t)scale * (IMG_MODE_MAX * IMG_TIER_MAX)
         + (size_t)sign  * (IMG_MODE_MAX * IMG_TIER_MAX * IMG_SCALE_MAX)
         + (size_t)tone  * (IMG_MODE_MAX * IMG_TIER_MAX * IMG_SCALE_MAX * IMG_SIGN_MAX)
         + (size_t)depth * (IMG_MODE_MAX * IMG_TIER_MAX * IMG_SCALE_MAX * IMG_SIGN_MAX * IMG_TONE_BUCKETS);
}

/* Tier base magnitudes (SPEC §10: T1 fine / T2 mid / T3 structure). */
static const int TIER_MAGNITUDE[IMG_TIER_MAX] = { 0, 4, 12, 24 };
/* Per-bucket multipliers used when mode + bucket interact. */
static const int TONE_MULT_INTENSITY [IMG_TONE_BUCKETS ] = { 12,  6,  3 };
static const int DEPTH_MULT_PRIORITY [IMG_DEPTH_BUCKETS] = {  3,  6, 10 };

static void build_step_tables(void) {
    for (int d = 0; d < IMG_FLOW_BUCKETS; d++) {
        for (int s = 0; s < IMG_SIGN_MAX; s++) {
            int up = d + 1, down = d - 1;
            if (up   > IMG_FLOW_DIAGONAL_DOWN) up   = IMG_FLOW_DIAGONAL_DOWN;
            if (down < 0)                      down = 0;
            if (s == IMG_SIGN_POS)       g_direction_step[d][s] = (uint8_t)up;
            else if (s == IMG_SIGN_NEG)  g_direction_step[d][s] = (uint8_t)down;
            else                         g_direction_step[d][s] = (uint8_t)d;
        }
    }
    for (int d = 0; d < IMG_DEPTH_BUCKETS; d++) {
        for (int s = 0; s < IMG_SIGN_MAX; s++) {
            int up = d + 1, down = d - 1;
            if (up   > IMG_DEPTH_FOREGROUND) up   = IMG_DEPTH_FOREGROUND;
            if (down < 0)                    down = 0;
            if (s == IMG_SIGN_POS)       g_depth_step[d][s] = (uint8_t)up;
            else if (s == IMG_SIGN_NEG)  g_depth_step[d][s] = (uint8_t)down;
            else                         g_depth_step[d][s] = (uint8_t)d;
        }
    }
}

static void build_main_tables(void) {
    for (uint8_t mode = 0; mode < IMG_MODE_MAX; mode++) {
        for (uint8_t tier = 0; tier < IMG_TIER_MAX; tier++) {
            for (uint8_t scale = 0; scale < IMG_SCALE_MAX; scale++) {
                for (uint8_t sign = 0; sign < IMG_SIGN_MAX; sign++) {
                    for (uint8_t tone = 0; tone < IMG_TONE_BUCKETS; tone++) {
                        for (uint8_t depth = 0; depth < IMG_DEPTH_BUCKETS; depth++) {
                            size_t idx = img_delta_table_idx(mode, tier, scale,
                                                             sign, tone, depth);

                            int16_t core = 0, link = 0, dch = 0, prio = 0;
                            uint8_t pat  = 0;

                            int sgn = (sign == IMG_SIGN_POS) ? +1
                                    : (sign == IMG_SIGN_NEG) ? -1 : 0;

                            if (tier != 0 && sgn != 0 && mode != IMG_MODE_NONE) {
                                int base = TIER_MAGNITUDE[tier] * (2 + scale) / 2;

                                switch (mode) {
                                    case IMG_MODE_INTENSITY:
                                        core = (int16_t)clamp_i(
                                            sgn * base * TONE_MULT_INTENSITY[tone] / 4,
                                            -200, 200);
                                        break;
                                    case IMG_MODE_LINK:
                                        link = (int16_t)clamp_i(sgn * base, -120, 120);
                                        break;
                                    case IMG_MODE_PRIORITY:
                                        prio = (int16_t)clamp_i(
                                            sgn * base * DEPTH_MULT_PRIORITY[depth] / 4,
                                            -200, 200);
                                        break;
                                    case IMG_MODE_MOOD:
                                        dch = (int16_t)clamp_i(sgn * base, -120, 120);
                                        /* mood_sign_fire bits 4..5 */
                                        pat |= (uint8_t)((sgn > 0 ? 1u : 2u) << 4);
                                        break;
                                    case IMG_MODE_DIRECTION:
                                        /* direction_sign bits 0..1 */
                                        pat |= (uint8_t)(sgn > 0 ? 1u : 2u);
                                        break;
                                    case IMG_MODE_DEPTH:
                                        /* depth_sign bits 2..3 */
                                        pat |= (uint8_t)((sgn > 0 ? 1u : 2u) << 2);
                                        break;
                                    case IMG_MODE_ROLE:
                                        /* role_flag bits 6..7 */
                                        pat |= (uint8_t)((sgn > 0 ? 1u : 2u) << 6);
                                        break;
                                    default:
                                        break;
                                }
                            }

                            g_core_table    [idx] = core;
                            g_link_table    [idx] = link;
                            g_delta_table   [idx] = dch;
                            g_priority_table[idx] = prio;
                            g_pattern_table [idx] = pat;
                        }
                    }
                }
            }
        }
    }
}

void img_delta_tables_init(void) {
    if (g_tables_ready) return;
    build_step_tables();
    build_main_tables();
    g_tables_ready = 1;
}

uint32_t img_delta_tables_entry_count(void) {
    return (uint32_t)IMG_DELTA_TABLE_N;
}

uint32_t img_delta_tables_memory_bytes(void) {
    return (uint32_t)(sizeof(g_core_table)     + sizeof(g_link_table)
                    + sizeof(g_delta_table)    + sizeof(g_priority_table)
                    + sizeof(g_pattern_table)  + sizeof(g_direction_step)
                    + sizeof(g_depth_step));
}

void img_delta_interpret(const ImgCECell* cell,
                         const ImgDeltaPayload* payload,
                         ImgConcreteDelta* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!cell || !payload) return;
    img_delta_tables_init();

    const ImgDeltaState s = payload->state;
    const uint8_t mode  = img_delta_state_mode(s);
    const uint8_t tier  = img_delta_state_tier(s);
    const uint8_t scale = img_delta_state_scale(s);
    const uint8_t sign  = img_delta_state_sign(s);

    uint8_t tone  = (cell->tone_class  < IMG_TONE_BUCKETS)  ? cell->tone_class  : IMG_TONE_MID;
    uint8_t depth = (cell->depth_class < IMG_DEPTH_BUCKETS) ? cell->depth_class : IMG_DEPTH_MIDGROUND;

    const size_t idx = img_delta_table_idx(mode, tier, scale, sign, tone, depth);

    out->add_core     = g_core_table    [idx];
    out->add_link     = g_link_table    [idx];
    out->add_delta    = g_delta_table   [idx];
    out->add_priority = g_priority_table[idx];

    const uint8_t pat = g_pattern_table[idx];
    const uint8_t dir_sign  = (uint8_t)( pat       & 0x3);
    const uint8_t dep_sign  = (uint8_t)((pat >> 2) & 0x3);
    const uint8_t mood_sign = (uint8_t)((pat >> 4) & 0x3);
    const uint8_t role_flag = (uint8_t)((pat >> 6) & 0x3);

    if (dir_sign) {
        uint8_t cur_dir = (cell->direction_class < IMG_FLOW_BUCKETS)
                        ? cell->direction_class : 0;
        out->direction_override    = g_direction_step[cur_dir][dir_sign];
        out->direction_override_on = 1;
    }

    if (dep_sign) {
        out->depth_override    = g_depth_step[depth][dep_sign];
        out->depth_override_on = 1;
    }

    if (mood_sign) {
        out->delta_sign_override    = (mood_sign == 1) ? IMG_DELTA_POSITIVE
                                                       : IMG_DELTA_NEGATIVE;
        out->delta_sign_override_on = 1;
    }

    /* role_flag=1 promotes UNKNOWN→OBJECT; role_flag=2 demotes any→UNKNOWN.
     * The apply step additionally gates this unless role_target_on is set. */
    if (role_flag == 1 && cell->semantic_role == IMG_ROLE_UNKNOWN) {
        out->semantic_override    = IMG_ROLE_OBJECT;
        out->semantic_override_on = 1;
    } else if (role_flag == 2) {
        out->semantic_override    = IMG_ROLE_UNKNOWN;
        out->semantic_override_on = 1;
    }

    /* Explicit role target bypasses the table's role_flag logic and
     * lets the apply step honour it even on non-UNKNOWN cells. */
    if (payload->role_target_on) {
        out->semantic_override    = payload->role_target;
        out->semantic_override_on = 1;
    }
}

/* ── DeltaUnit success rate ─────────────────────────────── */

double img_delta_unit_success_rate(const ImgDeltaUnit* u) {
    if (!u) return 0.0;
    return (double)(u->success_count + 1) / (double)(u->usage_count + 2);
}

/* ── DeltaMemory storage ────────────────────────────────── */

struct ImgDeltaMemory {
    ImgDeltaUnit* units;
    uint32_t      count;
    uint32_t      capacity;
};

ImgDeltaMemory* img_delta_memory_create(void) {
    ImgDeltaMemory* m = (ImgDeltaMemory*)calloc(1, sizeof(ImgDeltaMemory));
    if (!m) return NULL;
    m->capacity = 16;
    m->units = (ImgDeltaUnit*)calloc(m->capacity, sizeof(ImgDeltaUnit));
    if (!m->units) { free(m); return NULL; }
    return m;
}

void img_delta_memory_destroy(ImgDeltaMemory* m) {
    if (!m) return;
    if (m->units) free(m->units);
    free(m);
}

uint32_t img_delta_memory_count(const ImgDeltaMemory* m) {
    return m ? m->count : 0;
}

static int memory_grow(ImgDeltaMemory* m) {
    uint32_t new_cap = m->capacity * 2;
    ImgDeltaUnit* p = (ImgDeltaUnit*)realloc(m->units,
                                             new_cap * sizeof(ImgDeltaUnit));
    if (!p) return 0;
    memset(p + m->capacity, 0,
           (new_cap - m->capacity) * sizeof(ImgDeltaUnit));
    m->units = p;
    m->capacity = new_cap;
    return 1;
}

uint32_t img_delta_memory_add_with_hint(ImgDeltaMemory* m,
                                         ImgStateKey pre_key,
                                         ImgDeltaPayload payload,
                                         ImgStateKey post_hint) {
    if (!m) return IMG_DELTA_ID_NONE;
    if (m->count >= m->capacity && !memory_grow(m)) {
        return IMG_DELTA_ID_NONE;
    }
    uint32_t id = m->count++;
    ImgDeltaUnit* u = &m->units[id];
    u->id            = id;
    u->pre_key       = pre_key;
    u->payload       = payload;
    u->post_hint     = post_hint;
    u->has_post_hint = (post_hint != 0) ? 1 : 0;
    u->usage_count   = 0;
    u->success_count = 0;
    return id;
}

uint32_t img_delta_memory_add(ImgDeltaMemory* m,
                              ImgStateKey pre_key,
                              ImgDeltaPayload payload) {
    return img_delta_memory_add_with_hint(m, pre_key, payload, 0);
}

const ImgDeltaUnit* img_delta_memory_get(const ImgDeltaMemory* m,
                                          uint32_t id) {
    if (!m || id >= m->count) return NULL;
    return &m->units[id];
}

uint32_t img_delta_memory_candidates(const ImgDeltaMemory* m,
                                     ImgStateKey key,
                                     const ImgDeltaUnit** out,
                                     uint32_t max_out,
                                     int* out_level) {
    if (out_level) *out_level = -1;
    if (!m || m->count == 0 || !out || max_out == 0) return 0;

    for (int level = 0; level < FALLBACK_LEVELS; level++) {
        ImgStateKey mask   = FALLBACK_MASKS[level];
        ImgStateKey target = key & mask;
        uint32_t found = 0;
        for (uint32_t i = 0; i < m->count && found < max_out; i++) {
            if ((m->units[i].pre_key & mask) == target) {
                out[found++] = &m->units[i];
            }
        }
        if (found > 0) {
            if (out_level) *out_level = level;
            return found;
        }
    }
    return 0;
}

double img_delta_score(const ImgDeltaUnit* unit,
                       const ImgCECell* current,
                       int fallback_level) {
    if (!unit || !current) return 0.0;
    double s = 0.0;
    if (img_state_key_semantic_role  (unit->pre_key) == current->semantic_role)   s += 0.35;
    if (img_state_key_direction_class(unit->pre_key) == current->direction_class) s += 0.20;
    if (img_state_key_depth_class    (unit->pre_key) == current->depth_class)     s += 0.20;
    s += 0.25 * img_delta_unit_success_rate(unit);
    if (fallback_level > 0) s -= 0.05 * (double)fallback_level;
    return s;
}

const ImgDeltaUnit* img_delta_memory_best(const ImgDeltaMemory* m,
                                           const ImgCECell* current,
                                           double* out_score,
                                           int* out_level) {
    if (out_score) *out_score = 0.0;
    if (out_level) *out_level = -1;
    if (!m || !current) return NULL;

    ImgStateKey key = img_state_key_from_cell(current);
    enum { MAX_CAND = 16 };
    const ImgDeltaUnit* cand[MAX_CAND];
    int level = -1;
    uint32_t n = img_delta_memory_candidates(m, key, cand, MAX_CAND, &level);
    if (n == 0) return NULL;

    const ImgDeltaUnit* best = NULL;
    double best_s = -1.0;
    for (uint32_t i = 0; i < n; i++) {
        double s = img_delta_score(cand[i], current, level);
        if (s > best_s) { best_s = s; best = cand[i]; }
    }
    if (out_score) *out_score = best_s;
    if (out_level) *out_level = level;
    return best;
}

void img_delta_memory_record_usage(ImgDeltaMemory* m,
                                   uint32_t delta_id, int success) {
    if (!m || delta_id >= m->count) return;
    ImgDeltaUnit* u = &m->units[delta_id];
    u->usage_count++;
    if (success) u->success_count++;
}

/* ── Constrained apply ──────────────────────────────────── */

void img_delta_apply(ImgCECell* cell,
                     ImgDeltaMemory* m,
                     const ImgDeltaUnit* unit) {
    if (!cell || !unit) return;

    ImgConcreteDelta cd;
    img_delta_interpret(cell, &unit->payload, &cd);

    /* Constraint: direction may only rotate ±1 step from current. */
    if (cd.direction_override_on) {
        int diff = (int)cd.direction_override - (int)cell->direction_class;
        if (diff > 1)  cd.direction_override = (uint8_t)((int)cell->direction_class + 1);
        if (diff < -1) cd.direction_override = (uint8_t)((int)cell->direction_class - 1);
    }
    /* Constraint: depth may only change by ±1 step. */
    if (cd.depth_override_on) {
        int diff = (int)cd.depth_override - (int)cell->depth_class;
        if (diff > 1)  cd.depth_override = (uint8_t)((int)cell->depth_class + 1);
        if (diff < -1) cd.depth_override = (uint8_t)((int)cell->depth_class - 1);
    }
    /* Constraint: role override honored only when role_target_on or
     * the current role is UNKNOWN. */
    if (cd.semantic_override_on
        && cell->semantic_role != IMG_ROLE_UNKNOWN
        && !unit->payload.role_target_on) {
        cd.semantic_override_on = 0;
    }

    cell->core     = sat_add_u8(cell->core,     cd.add_core);
    cell->link     = sat_add_u8(cell->link,     cd.add_link);
    cell->delta    = sat_add_u8(cell->delta,    cd.add_delta);
    cell->priority = sat_add_u8(cell->priority, cd.add_priority);

    if (cd.semantic_override_on)   cell->semantic_role   = cd.semantic_override;
    if (cd.depth_override_on)      cell->depth_class     = cd.depth_override;
    if (cd.direction_override_on)  cell->direction_class = cd.direction_override;
    if (cd.delta_sign_override_on) cell->delta_sign      = cd.delta_sign_override;

    cell->last_delta_id = unit->id;

    if (m && unit->id < m->count) {
        m->units[unit->id].usage_count++;
    }
}
