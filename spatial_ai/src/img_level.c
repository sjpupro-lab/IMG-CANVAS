#include "img_level.h"
#include "img_ce.h"   /* IMG_DEPTH_FOREGROUND, IMG_FLOW_NONE */

/* A clause-sized region blankets a quarter of the grid or more
 * (IMG_CE_TOTAL is 4096; 512 ≈ 12.5 %). */
const uint8_t IMG_LEVEL_DEFAULT_PASSES[IMG_LEVEL_COUNT] = {
    2,   /* point  — detail sweep, more passes keep top-K diverse */
    3,   /* line   — oriented mid-detail dominates visual quality */
    2,   /* area   — fills in blocks */
    1,   /* clause — coarse mood, one pass is enough */
};

/* The priority band above which a region is treated as a structural
 * "면" (area) even when its depth vote didn't land on FOREGROUND. */
#define IMG_LEVEL_AREA_PRIORITY_THRESHOLD  180u

uint8_t img_level_infer_from_region(uint8_t dominant_depth,
                                    uint8_t dominant_flow,
                                    uint8_t band_hi,
                                    uint32_t cell_count) {
    if (cell_count >= 512) return IMG_LEVEL_CLAUSE;
    if (band_hi >= IMG_LEVEL_AREA_PRIORITY_THRESHOLD ||
        dominant_depth == IMG_DEPTH_FOREGROUND)
        return IMG_LEVEL_AREA;
    if (dominant_flow != IMG_FLOW_NONE)
        return IMG_LEVEL_LINE;
    return IMG_LEVEL_POINT;
}
