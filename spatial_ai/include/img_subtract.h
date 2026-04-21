#ifndef IMG_SUBTRACT_H
#define IMG_SUBTRACT_H

/*
 * img_subtract — subtract-domain representation of a CE grid
 *                (Axis 1 of the V2 refactor).
 *
 *   Classic add model:
 *       cell_state  = base_state + Σ deltas              (sat to [0,255])
 *
 *   Subtract model (this module):
 *       initial     = 255 per channel
 *       acc_sub[c]  = initial - cell_state[c]            (invariant)
 *       apply(d)    = acc_sub -= d   (saturating at [0,255])
 *
 *   The two models are algebraic twins for any saturating integer
 *   add / sub — every test in test_img_subtract.c confirms they
 *   produce bit-identical ImgCECell bytes.
 *
 *   Phase C keeps img_delta_apply on the classic add path so
 *   reference hashes stay unchanged, but every building block
 *   (ctx from/to grid, apply-in-subtract) lives here so future
 *   phases can flip the hot path — or expose a direct subtract
 *   API — without re-deriving the identities. The subtract view
 *   is also where "경계 처리 자동 0 clamp" and compatibility with
 *   diffusion mathematics become free.
 */

#include <stdint.h>

#include "img_ce.h"
#include "img_delta_memory.h"

typedef struct ImgSubtractContext {
    /* Per-cell accumulated subtractions, one byte per channel.
     * Invariant: grid_cell[c] == initial_channel[c] - acc[c][i].
     * Layout is SoA (channel × cell) to stay SIMD-friendly. */
    uint8_t acc_r[IMG_CE_TOTAL];
    uint8_t acc_g[IMG_CE_TOTAL];
    uint8_t acc_b[IMG_CE_TOTAL];
    uint8_t acc_a[IMG_CE_TOTAL];

    /* Initial per-channel value — defaults to 255. Lower values are
     * reserved for future partial-canvas brushes. */
    uint8_t initial_r, initial_g, initial_b, initial_a;
    uint8_t _pad[4];
} ImgSubtractContext;

/* Initialise `ctx` from `grid`. Default initials are 255; if you
 * pass non-255 values via img_subtract_set_initials, call that first.
 * Cells outside grid bounds (shouldn't happen, grid is fixed size)
 * are zero-filled in acc. */
void img_subtract_from_grid(ImgSubtractContext* ctx,
                            const ImgCEGrid* grid);

/* Commit `ctx` back onto `grid`'s RGBA channels. Tag fields on the
 * grid cells are left untouched (the subtract view only models
 * additive channels — tags are direct-assignment per spec §5.1.1). */
void img_subtract_to_grid(const ImgSubtractContext* ctx,
                          ImgCEGrid* grid);

/* Re-base the initial values. Must be followed by img_subtract_from_grid
 * to re-establish the invariant relative to the new baseline. */
void img_subtract_set_initials(ImgSubtractContext* ctx,
                               uint8_t ir, uint8_t ig, uint8_t ib, uint8_t ia);

/* Apply a concrete delta to the cell at `cell_index`, working in
 * subtract domain for the four channel adds and writing tag /
 * override fields directly onto `cell_mut` (which MUST be the
 * matching cell in the same grid `ctx` was derived from).
 *
 * The RGBA result, after img_subtract_to_grid commits, matches
 * img_delta_apply (which uses sat_add in the classic domain) byte
 * for byte — test_img_subtract.c exhaustively verifies this over a
 * wide range of cell values and delta magnitudes. */
void img_subtract_apply_concrete(ImgSubtractContext* ctx,
                                 uint32_t cell_index,
                                 ImgCECell* cell_mut,
                                 const ImgConcreteDelta* cd);

/* Convenience helper: interpret a delta unit against the current
 * reconstructed cell state, then apply via img_subtract_apply_concrete.
 * Mirrors img_delta_apply's constraint clamps (direction ±1, depth
 * ±1, role gate). `memory` is optional — passing NULL skips the
 * usage_count bump. */
void img_subtract_apply_unit(ImgSubtractContext* ctx,
                             uint32_t cell_index,
                             ImgCECell* cell_mut,
                             ImgDeltaMemory* memory_or_null,
                             const ImgDeltaUnit* unit);

#endif /* IMG_SUBTRACT_H */
