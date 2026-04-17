#include "img_delta_learn.h"

#include <stdlib.h>
#include <string.h>

/* Noise floor for numeric channel diffs — below this we don't add a
 * rule, so that small quantization jitter from image compression
 * doesn't pollute the memory. Tag-level changes bypass this. */
#define LEARN_NUMERIC_NOISE_FLOOR 3

static inline int abs_i(int v) { return v < 0 ? -v : v; }

/* Map an unsigned magnitude to a DeltaState tier_idx. Coarse on
 * purpose — the interpret tables drive exact output; we just need
 * to pick a tier that gets us in the ballpark. */
static uint8_t tier_from_magnitude(int abs_diff) {
    if (abs_diff <= 0)  return IMG_TIER_NONE;
    if (abs_diff <= 6)  return IMG_TIER_T1;
    if (abs_diff <= 24) return IMG_TIER_T2;
    return IMG_TIER_T3;
}

/* Pick a scale index in [0, IMG_SCALE_MAX-1] roughly proportional
 * to the magnitude. scale=0..3 covers small diffs, 4..7 covers
 * larger ones. */
static uint8_t scale_from_magnitude(int abs_diff) {
    int s = abs_diff / 8;
    if (s < 0) s = 0;
    if (s > (int)IMG_SCALE_MAX - 1) s = IMG_SCALE_MAX - 1;
    return (uint8_t)s;
}

static uint8_t sign_of(int signed_diff) {
    if (signed_diff > 0) return IMG_SIGN_POS;
    if (signed_diff < 0) return IMG_SIGN_NEG;
    return IMG_SIGN_ZERO;
}

/* Returns 1 if a payload was produced, 0 if before≈after. */
static int derive_payload(const ImgCECell* pre,
                          const ImgCECell* post,
                          ImgDeltaPayload* out) {
    memset(out, 0, sizeof(*out));

    /* Tag-level precedence (discrete, always captured). */
    if (post->semantic_role != pre->semantic_role) {
        out->state = img_delta_state_simple(
            IMG_TIER_T2, /*scale=*/2, IMG_SIGN_POS, IMG_MODE_ROLE);
        out->role_target    = post->semantic_role;
        out->role_target_on = 1;
        return 1;
    }
    if (post->direction_class != pre->direction_class) {
        int diff = (int)post->direction_class - (int)pre->direction_class;
        out->state = img_delta_state_simple(
            IMG_TIER_T2, /*scale=*/0,
            sign_of(diff), IMG_MODE_DIRECTION);
        return 1;
    }
    if (post->depth_class != pre->depth_class) {
        int diff = (int)post->depth_class - (int)pre->depth_class;
        out->state = img_delta_state_simple(
            IMG_TIER_T2, /*scale=*/0,
            sign_of(diff), IMG_MODE_DEPTH);
        return 1;
    }

    /* Numeric channels: pick the axis with the largest |Δ|. */
    const int dcore  = (int)post->core     - (int)pre->core;
    const int dlink  = (int)post->link     - (int)pre->link;
    const int ddelta = (int)post->delta    - (int)pre->delta;
    const int dpri   = (int)post->priority - (int)pre->priority;

    int best_abs    = 0;
    int best_signed = 0;
    uint8_t best_mode = IMG_MODE_NONE;

    if (abs_i(dcore)  > best_abs) { best_abs = abs_i(dcore);
                                    best_signed = dcore;
                                    best_mode = IMG_MODE_INTENSITY; }
    if (abs_i(dlink)  > best_abs) { best_abs = abs_i(dlink);
                                    best_signed = dlink;
                                    best_mode = IMG_MODE_LINK; }
    if (abs_i(ddelta) > best_abs) { best_abs = abs_i(ddelta);
                                    best_signed = ddelta;
                                    best_mode = IMG_MODE_MOOD; }
    if (abs_i(dpri)   > best_abs) { best_abs = abs_i(dpri);
                                    best_signed = dpri;
                                    best_mode = IMG_MODE_PRIORITY; }

    if (best_mode == IMG_MODE_NONE) return 0;
    if (best_abs  <  LEARN_NUMERIC_NOISE_FLOOR) return 0;

    out->state = img_delta_state_simple(
        tier_from_magnitude(best_abs),
        scale_from_magnitude(best_abs),
        sign_of(best_signed),
        best_mode);
    return 1;
}

/* ── public API ─────────────────────────────────────────── */

uint32_t img_delta_memory_learn_from_pair(ImgDeltaMemory* memory,
                                          const ImgCEGrid* before,
                                          const ImgCEGrid* after) {
    if (!memory || !before || !after) return 0;
    if (!before->cells || !after->cells) return 0;
    if (before->width  != after->width)  return 0;
    if (before->height != after->height) return 0;

    const uint32_t n = before->width * before->height;
    uint32_t added = 0;

    for (uint32_t i = 0; i < n; i++) {
        const ImgCECell* pre  = &before->cells[i];
        const ImgCECell* post = &after->cells[i];

        ImgDeltaPayload payload;
        if (!derive_payload(pre, post, &payload)) continue;

        const ImgStateKey pre_key  = img_state_key_from_cell(pre);
        const ImgStateKey post_key = img_state_key_from_cell(post);

        /* Insert a new rule. No dedup in v0 — repeated observations
         * let usage_count / success_count accumulate naturally over
         * time, which scoring and Laplace smoothing consume. */
        (void)img_delta_memory_add_with_hint(memory, pre_key, payload,
                                             post_key);
        added++;
    }
    return added;
}

uint32_t img_delta_memory_learn_from_images(ImgDeltaMemory* memory,
                                            const uint8_t* before_rgb,
                                            uint32_t bw, uint32_t bh,
                                            const uint8_t* after_rgb,
                                            uint32_t aw, uint32_t ah) {
    if (!memory || !before_rgb || !after_rgb) return 0;
    if (bw == 0 || bh == 0 || aw == 0 || ah == 0) return 0;

    ImgSmallCanvas* bsc = img_small_canvas_create();
    ImgSmallCanvas* asc = img_small_canvas_create();
    ImgCEGrid*      bce = img_ce_grid_create();
    ImgCEGrid*      ace = img_ce_grid_create();

    uint32_t added = 0;
    if (bsc && asc && bce && ace) {
        img_image_to_small_canvas(before_rgb, bw, bh, bsc);
        img_image_to_small_canvas(after_rgb,  aw, ah, asc);
        img_small_canvas_to_ce(bsc, bce);
        img_small_canvas_to_ce(asc, ace);
        added = img_delta_memory_learn_from_pair(memory, bce, ace);
    }

    if (bsc) img_small_canvas_destroy(bsc);
    if (asc) img_small_canvas_destroy(asc);
    if (bce) img_ce_grid_destroy(bce);
    if (ace) img_ce_grid_destroy(ace);
    return added;
}
