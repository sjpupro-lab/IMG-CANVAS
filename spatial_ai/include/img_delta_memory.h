#ifndef IMG_DELTA_MEMORY_H
#define IMG_DELTA_MEMORY_H

#include "img_ce.h"

/*
 * img_delta_memory — incremental-rule store that drives CE generation.
 *
 *   "현재 상태를 다음 상태로 이어주는 증분 규칙의 기억 저장소이며,
 *    CE 생성은 이 delta 규칙을 누적하여 상태를 확장하는 과정이다."
 *
 * Each entry is:
 *
 *   pre_key (StateKey) ──► payload (symbolic) ──► post_hint (StateKey)
 *
 * Important: the payload is *not* a literal numeric diff. It is a
 * compressed symbolic instruction along a small set of axes
 * ("intensify by +1 step", "promote depth by +1 step"…). The actual
 * numeric change is produced at apply time by `img_delta_interpret`,
 * which reads the current cell's tags (tone / role / depth …) and
 * expands the symbol into a context-appropriate concrete change.
 *
 * That gives us:
 *
 *   - tiny memory footprint per delta (a handful of int8 steps)
 *   - the same symbolic delta produces *different* concrete changes
 *     depending on what the current cell already represents
 *   - DeltaMemory becomes a vocabulary of moves, not a frozen lookup
 *     table of literal substitutions
 */

/* ── StateKey ────────────────────────────────────────────────
 * 6 fields packed into one uint64. Lookup is exact match on this
 * key, with a fallback chain that progressively zeros bytes from
 * least to most discriminative when no match exists. */
typedef uint64_t ImgStateKey;

/* Quantize the CE link channel (0..255) into one of 8 buckets. */
uint8_t      img_link_bucket(uint8_t link);

ImgStateKey  img_state_key_make(uint8_t semantic_role,
                                uint8_t tone_class,
                                uint8_t direction_class,
                                uint8_t depth_class,
                                uint8_t link_bucket,
                                uint8_t delta_sign);

ImgStateKey  img_state_key_from_cell(const ImgCECell* cell);

uint8_t      img_state_key_semantic_role  (ImgStateKey k);
uint8_t      img_state_key_tone_class     (ImgStateKey k);
uint8_t      img_state_key_direction_class(ImgStateKey k);
uint8_t      img_state_key_depth_class    (ImgStateKey k);
uint8_t      img_state_key_link_bucket    (ImgStateKey k);
uint8_t      img_state_key_delta_sign     (ImgStateKey k);

/* ── DeltaPayload (symbolic, compressed) ─────────────────────
 * Each axis carries a signed step in roughly [-3, +3]. Interpret
 * decides what each step means for the *current* cell. */
typedef enum {
    IMG_AXIS_INTENSITY = 0,  /* expands to ±core    */
    IMG_AXIS_LINK      = 1,  /* expands to ±link    */
    IMG_AXIS_DIRECTION = 2,  /* rotates direction_class one step (sign = direction) */
    IMG_AXIS_DEPTH     = 3,  /* nudges depth_class up/down by one step */
    IMG_AXIS_MOOD      = 4,  /* expands to ±delta channel + tone shift */
    IMG_AXIS_ROLE      = 5,  /* role-promotion intent (positive = promote, negative = demote) */
    IMG_AXIS_PRIORITY  = 6,  /* expands to ±priority, scaled by current depth */
    IMG_AXIS_COUNT     = 7
} ImgDeltaAxis;

typedef struct {
    int8_t step[IMG_AXIS_COUNT];

    /* Optional explicit role override (used when AXIS_ROLE step != 0
     * and the engine wants to assert a specific target). 0 = ignore. */
    uint8_t role_target;
    uint8_t role_target_on;
} ImgDeltaPayload;

/* The concrete change produced by interpretation. This is what
 * actually gets applied to the cell. It mirrors the existing
 * ImgDeltaCoating from img_ce.h but lives here so the memory module
 * doesn't depend on coating helpers being public — they are still
 * available, but we want a clear "interpret → concrete" boundary. */
typedef struct {
    int16_t add_core;
    int16_t add_link;
    int16_t add_delta;
    int16_t add_priority;

    uint8_t semantic_override;
    uint8_t semantic_override_on;

    uint8_t depth_override;
    uint8_t depth_override_on;

    uint8_t direction_override;
    uint8_t direction_override_on;

    uint8_t delta_sign_override;
    uint8_t delta_sign_override_on;
} ImgConcreteDelta;

/* Expand a symbolic payload into a concrete delta given the cell's
 * current tags. Same payload, different cell ⇒ different concrete
 * output. This is the "해석이 붙어야 진짜 값들로 빠르게 대체" step. */
void img_delta_interpret(const ImgCECell* cell,
                         const ImgDeltaPayload* payload,
                         ImgConcreteDelta* out);

/* ── DeltaUnit ───────────────────────────────────────────────
 * One stored entry. usage / success counts feed the Laplace-smoothed
 * success rate used in scoring. */
typedef struct {
    uint32_t        id;
    ImgStateKey     pre_key;
    ImgDeltaPayload payload;
    ImgStateKey     post_hint;     /* 0 if unset */
    uint8_t         has_post_hint;

    uint32_t usage_count;
    uint32_t success_count;
} ImgDeltaUnit;

/* Laplace-smoothed success rate: (s + 1) / (u + 2). Stops a
 * brand-new 1/1 unit from outranking a 50/100 veteran. */
double img_delta_unit_success_rate(const ImgDeltaUnit* unit);

/* ── DeltaMemory ─────────────────────────────────────────────
 * Stores a flat array of units. Lookup uses exact key match first,
 * then a fallback chain that progressively widens the key:
 *
 *   L0 full key
 *   L1 drop link_bucket
 *   L2 + drop delta_sign
 *   L3 + drop tone_class
 *   L4 + drop direction_class
 *   L5 + drop depth_class
 *   L6 anything (semantic_role also dropped)
 *
 * Returns the level that produced a hit so callers can weight
 * candidates by how "tight" the match was. */

typedef struct ImgDeltaMemory ImgDeltaMemory;

ImgDeltaMemory* img_delta_memory_create(void);
void            img_delta_memory_destroy(ImgDeltaMemory* m);

uint32_t        img_delta_memory_count(const ImgDeltaMemory* m);

/* Insert a new delta. Returns the assigned id. */
uint32_t        img_delta_memory_add(ImgDeltaMemory* m,
                                     ImgStateKey pre_key,
                                     ImgDeltaPayload payload);

uint32_t        img_delta_memory_add_with_hint(ImgDeltaMemory* m,
                                               ImgStateKey pre_key,
                                               ImgDeltaPayload payload,
                                               ImgStateKey post_hint);

const ImgDeltaUnit* img_delta_memory_get(const ImgDeltaMemory* m,
                                          uint32_t id);

/* Fill `out` with up to `max_out` candidate units that match `key`
 * (or progressively coarser keys via fallback). Returns the number
 * filled and writes the fallback level used to *out_level. */
uint32_t        img_delta_memory_candidates(const ImgDeltaMemory* m,
                                            ImgStateKey key,
                                            const ImgDeltaUnit** out,
                                            uint32_t max_out,
                                            int* out_level);

/* Score a candidate against the current cell.
 *   semantic_fit  0.35
 *   direction_fit 0.20
 *   depth_fit     0.20
 *   success_rate  0.25 (Laplace-smoothed)
 *   - 0.05 per fallback level
 */
double          img_delta_score(const ImgDeltaUnit* unit,
                                const ImgCECell* current,
                                int fallback_level);

/* Pick the highest-scoring candidate (or NULL if memory is empty for
 * any fallback level). Writes the score and fallback level used. */
const ImgDeltaUnit* img_delta_memory_best(const ImgDeltaMemory* m,
                                           const ImgCECell* current,
                                           double* out_score,
                                           int* out_level);

/* Resolve / quality feedback. `success` is 0 or 1. The cell's
 * last_delta_id is the natural source of `delta_id`. No-op if the
 * id isn't in this memory. */
void            img_delta_memory_record_usage(ImgDeltaMemory* m,
                                              uint32_t delta_id,
                                              int success);

/* Apply a stored delta to a CE cell:
 *    1) interpret payload against the cell's current tags
 *    2) saturating add the channel changes
 *    3) apply tag overrides
 *    4) write cell->last_delta_id = unit->id
 *    5) bump unit->usage_count
 *
 * Constraints:
 *    - direction may only rotate by ±1 step from current
 *    - depth may only change by ±1 step
 *    - role override is allowed only when the cell's role is UNKNOWN
 *      OR the payload's role_target_on is set
 */
void            img_delta_apply(ImgCECell* cell,
                                ImgDeltaMemory* m,
                                const ImgDeltaUnit* unit);

#endif /* IMG_DELTA_MEMORY_H */
