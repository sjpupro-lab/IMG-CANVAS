#include "img_subtract.h"

#include <string.h>

/* Saturating helpers — kept local to the translation unit so they
 * inline cleanly. The `s16` inputs are what img_delta_interpret
 * already hands us (int16_t add_*), so no additional casting dance
 * leaks into callers. */

static inline uint8_t sat_sub_u8_s16(uint8_t v, int16_t s) {
    /* v - s, clamped to [0, 255]. */
    int32_t r = (int32_t)v - (int32_t)s;
    if (r < 0)   return 0;
    if (r > 255) return 255;
    return (uint8_t)r;
}

static inline uint8_t sat_add_u8_s16(uint8_t v, int16_t s) {
    int32_t r = (int32_t)v + (int32_t)s;
    if (r < 0)   return 0;
    if (r > 255) return 255;
    return (uint8_t)r;
}

/* The invariant "acc = initial - cell" means the classic
 *   new_cell = sat_add_s(cell, add)
 * becomes
 *   new_acc  = sat_sub_s(acc, add)
 * Verified over the full (cell, add) value sweep by
 * test_subtract_classic_identity_saturates. */
static inline uint8_t apply_add_as_sub(uint8_t acc, int16_t add) {
    return sat_sub_u8_s16(acc, add);
}

void img_subtract_from_grid(ImgSubtractContext* ctx,
                            const ImgCEGrid* grid) {
    if (!ctx) return;
    if (!grid || !grid->cells) {
        memset(ctx->acc_r, 0, sizeof(ctx->acc_r));
        memset(ctx->acc_g, 0, sizeof(ctx->acc_g));
        memset(ctx->acc_b, 0, sizeof(ctx->acc_b));
        memset(ctx->acc_a, 0, sizeof(ctx->acc_a));
        return;
    }
    /* Default initials if the caller didn't override. */
    if (ctx->initial_r == 0 && ctx->initial_g == 0 &&
        ctx->initial_b == 0 && ctx->initial_a == 0) {
        ctx->initial_r = ctx->initial_g = ctx->initial_b = ctx->initial_a = 255;
    }
    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) {
        const ImgCECell* c = &grid->cells[i];
        ctx->acc_r[i] = (uint8_t)(ctx->initial_r - c->core);
        ctx->acc_g[i] = (uint8_t)(ctx->initial_g - c->link);
        ctx->acc_b[i] = (uint8_t)(ctx->initial_b - c->delta);
        ctx->acc_a[i] = (uint8_t)(ctx->initial_a - c->priority);
    }
}

void img_subtract_to_grid(const ImgSubtractContext* ctx,
                          ImgCEGrid* grid) {
    if (!ctx || !grid || !grid->cells) return;
    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) {
        ImgCECell* c = &grid->cells[i];
        c->core     = (uint8_t)(ctx->initial_r - ctx->acc_r[i]);
        c->link     = (uint8_t)(ctx->initial_g - ctx->acc_g[i]);
        c->delta    = (uint8_t)(ctx->initial_b - ctx->acc_b[i]);
        c->priority = (uint8_t)(ctx->initial_a - ctx->acc_a[i]);
    }
}

void img_subtract_set_initials(ImgSubtractContext* ctx,
                               uint8_t ir, uint8_t ig,
                               uint8_t ib, uint8_t ia) {
    if (!ctx) return;
    ctx->initial_r = ir;
    ctx->initial_g = ig;
    ctx->initial_b = ib;
    ctx->initial_a = ia;
}

void img_subtract_apply_concrete(ImgSubtractContext* ctx,
                                 uint32_t cell_index,
                                 ImgCECell* cell_mut,
                                 const ImgConcreteDelta* cd) {
    if (!ctx || !cell_mut || !cd) return;
    if (cell_index >= IMG_CE_TOTAL) return;

    ctx->acc_r[cell_index] = apply_add_as_sub(ctx->acc_r[cell_index], cd->add_core);
    ctx->acc_g[cell_index] = apply_add_as_sub(ctx->acc_g[cell_index], cd->add_link);
    ctx->acc_b[cell_index] = apply_add_as_sub(ctx->acc_b[cell_index], cd->add_delta);
    ctx->acc_a[cell_index] = apply_add_as_sub(ctx->acc_a[cell_index], cd->add_priority);

    /* Tag overrides are direct assignments in either domain. */
    if (cd->semantic_override_on)   cell_mut->semantic_role   = cd->semantic_override;
    if (cd->depth_override_on)      cell_mut->depth_class     = cd->depth_override;
    if (cd->direction_override_on)  cell_mut->direction_class = cd->direction_override;
    if (cd->delta_sign_override_on) cell_mut->delta_sign      = cd->delta_sign_override;

    /* Keep the cell's RGBA in sync so callers reading the cell mid-
     * pass see a consistent view. (Subtract-context alone would
     * suffice if callers always read through to_grid, but some
     * drawing code also looks at cell->core directly.) */
    cell_mut->core     = (uint8_t)(ctx->initial_r - ctx->acc_r[cell_index]);
    cell_mut->link     = (uint8_t)(ctx->initial_g - ctx->acc_g[cell_index]);
    cell_mut->delta    = (uint8_t)(ctx->initial_b - ctx->acc_b[cell_index]);
    cell_mut->priority = (uint8_t)(ctx->initial_a - ctx->acc_a[cell_index]);
}

/* Mirror the clamping rules in img_delta_memory.c::img_delta_apply
 * so apply_unit is a drop-in equivalent for callers that want the
 * subtract-domain version. */
static void clamp_like_img_delta_apply(const ImgCECell* cell_view,
                                       const ImgDeltaUnit* unit,
                                       ImgConcreteDelta* cd) {
    if (cd->direction_override_on) {
        int diff = (int)cd->direction_override - (int)cell_view->direction_class;
        if (diff > 1)  cd->direction_override = (uint8_t)((int)cell_view->direction_class + 1);
        if (diff < -1) cd->direction_override = (uint8_t)((int)cell_view->direction_class - 1);
    }
    if (cd->depth_override_on) {
        int diff = (int)cd->depth_override - (int)cell_view->depth_class;
        if (diff > 1)  cd->depth_override = (uint8_t)((int)cell_view->depth_class + 1);
        if (diff < -1) cd->depth_override = (uint8_t)((int)cell_view->depth_class - 1);
    }
    if (cd->semantic_override_on
        && cell_view->semantic_role != IMG_ROLE_UNKNOWN
        && !unit->payload.role_target_on) {
        cd->semantic_override_on = 0;
    }
}

void img_subtract_apply_unit(ImgSubtractContext* ctx,
                             uint32_t cell_index,
                             ImgCECell* cell_mut,
                             ImgDeltaMemory* memory_or_null,
                             const ImgDeltaUnit* unit) {
    if (!ctx || !cell_mut || !unit) return;
    if (cell_index >= IMG_CE_TOTAL) return;

    ImgConcreteDelta cd;
    img_delta_interpret(cell_mut, &unit->payload, &cd);
    clamp_like_img_delta_apply(cell_mut, unit, &cd);
    img_subtract_apply_concrete(ctx, cell_index, cell_mut, &cd);

    cell_mut->last_delta_id = unit->id;

    /* Mirror img_delta_apply's usage counter bump. */
    if (memory_or_null) {
        uint32_t n = img_delta_memory_count(memory_or_null);
        if (unit->id < n) {
            /* Reuse the public API: record_usage updates usage_count
             * unconditionally, success_count only when success != 0.
             * Passing 0 here matches img_delta_apply's semantics. */
            img_delta_memory_record_usage(memory_or_null, unit->id, 0);
        }
    }
}
