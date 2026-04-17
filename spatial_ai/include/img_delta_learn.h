#ifndef IMG_DELTA_LEARN_H
#define IMG_DELTA_LEARN_H

#include "img_ce.h"
#include "img_delta_memory.h"

/*
 * img_delta_learn — observe a before/after pair and populate a
 * DeltaMemory with the transitions that explain the change.
 *
 *   before_ce, after_ce  (same dimensions)
 *      │
 *      ▼  for each cell where state_key_before ≠ state_key_after
 *   pre_key  = state_key_from_cell(before)
 *   payload  = derive_delta(before, after)       // one dominant axis
 *   post_hint = state_key_from_cell(after)
 *      │
 *      ▼
 *   memory.add(pre_key, payload, post_hint)
 *
 * derive_delta picks ONE axis to describe the transition, in this
 * priority order:
 *
 *   1. semantic_role change   → MODE_ROLE with role_target_on
 *   2. direction_class change → MODE_DIRECTION (±1 per apply)
 *   3. depth_class change     → MODE_DEPTH     (±1 per apply)
 *   4. largest |Δ| among core/link/delta/priority → matching MODE
 *
 * Numeric deltas below a small noise floor are skipped so image
 * compression jitter doesn't pollute the memory. Tag-level changes
 * are always captured.
 *
 * Result: memory.count grows, and subsequent img_pipeline_run calls
 * will have non-zero `expansions` when the pipeline encounters
 * cells whose state_key matches a stored pre_key (or falls back to
 * a wider key via the existing fallback chain).
 */

/* Walk both grids cell-by-cell. Returns the number of DeltaUnits
 * inserted into `memory`. Requires before and after to have
 * matching dimensions. */
uint32_t img_delta_memory_learn_from_pair(ImgDeltaMemory* memory,
                                          const ImgCEGrid* before,
                                          const ImgCEGrid* after);

/* Convenience: compress two RGB images through SmallCanvas → CE
 * and delegate to learn_from_pair. Image dimensions may differ
 * between before and after (both collapse to the same CE size),
 * but each image must be non-empty. Returns the number of
 * DeltaUnits inserted. */
uint32_t img_delta_memory_learn_from_images(ImgDeltaMemory* memory,
                                            const uint8_t* before_rgb,
                                            uint32_t before_w,
                                            uint32_t before_h,
                                            const uint8_t* after_rgb,
                                            uint32_t after_w,
                                            uint32_t after_h);

#endif /* IMG_DELTA_LEARN_H */
