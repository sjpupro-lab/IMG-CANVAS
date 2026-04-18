#ifndef IMG_DRAWING_H
#define IMG_DRAWING_H

#include "img_ce.h"
#include "img_delta_memory.h"

/*
 * img_drawing — "print-the-image" operating mode.
 *
 *   This is the counterpart to img_pipeline_run (which ingests an
 *   input image and compresses it to CE state). img_drawing_pass
 *   operates the engine in the other direction: given a CE grid
 *   (empty, seeded from a keyframe, or mid-draw) plus a populated
 *   DeltaMemory, walk the cells and stamp learned deltas onto them.
 *
 *   Selection model — top-G sampling with presence penalty, lifted
 *   directly from language-model token sampling:
 *
 *     for each target cell (raster order):
 *         topg = img_delta_memory_topg(
 *                   cell, G,
 *                   recent_counts, penalty_alpha, ...);
 *         pick  = topg[0];                 (greedy; penalty diversifies)
 *         img_delta_apply(cell, memory, pick);
 *         recent_counts[pick->id] += 1;
 *
 *   Presence penalty is analogous to coverage / presence penalties
 *   in LM sampling: a delta picked recently drops in score so under-
 *   used candidates surface. This is what keeps the drawing from
 *   collapsing into "stamp the same thing everywhere".
 *
 *   The engine's core model of "same architecture, drawing mode":
 *     - No noise-denoising loop.
 *     - Delta memory holds bounded, lossless resume codes.
 *     - Per-cell tier / role / depth already encode how much detail
 *       belongs where; the stamp does not need a global mode
 *       switch.
 *     - Multiple passes over the grid produce underdrawing →
 *       detail layering (earlier passes stamp low-tier deltas,
 *       later passes pick up finer-tier ones as cell state changes).
 */

typedef struct {
    uint32_t top_g;              /* candidate pool per cell (default 3) */
    double   presence_penalty;   /* α; subtracted per recent pick; default 0.5 */
    uint32_t passes;             /* drawing iterations over the grid; default 1 */
    int      skip_zero_cells;    /* 1 = skip cells with core==0 (seed-only focus);
                                  * 0 = stamp every cell (full blank-canvas fill) */
} ImgDrawingOptions;

typedef struct {
    uint32_t stamps_applied;     /* # of successful img_delta_apply calls */
    uint32_t cells_visited;      /* cells we considered stamping (post-filter) */
    uint32_t unique_deltas_used; /* distinct delta ids picked at least once */
    uint32_t max_recent_count;   /* highest value in the recent_counts table */
} ImgDrawingStats;

ImgDrawingOptions img_drawing_default_options(void);

/* Run a drawing pass on `grid` using `memory`. If either is NULL the
 * call is a no-op that reports zero stats. Internally allocates a
 * recent_counts table the size of img_delta_memory_count(memory) for
 * the duration of the call. Runs `opts.passes` iterations; within a
 * single pass, `recent_counts` accumulates (diversifies selection);
 * it is NOT cleared between passes.
 *
 * Returns 1 on success (including the no-op path), 0 on allocation
 * failure. */
int img_drawing_pass(ImgCEGrid* grid,
                     ImgDeltaMemory* memory,
                     const ImgDrawingOptions* opts_or_null,
                     ImgDrawingStats* out_stats_or_null);

#endif /* IMG_DRAWING_H */
