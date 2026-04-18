#include "img_drawing.h"

#include <stdlib.h>
#include <string.h>

ImgDrawingOptions img_drawing_default_options(void) {
    ImgDrawingOptions o;
    o.top_g            = 3;
    o.presence_penalty = 0.5;
    o.passes           = 1;
    o.skip_zero_cells  = 0;
    return o;
}

int img_drawing_pass(ImgCEGrid* grid,
                     ImgDeltaMemory* memory,
                     const ImgDrawingOptions* opts_or_null,
                     ImgDrawingStats* out_stats) {
    ImgDrawingStats local = {0, 0, 0, 0};

    if (!grid || !grid->cells || !memory ||
        img_delta_memory_count(memory) == 0) {
        if (out_stats) *out_stats = local;
        return 1;   /* no-op success */
    }

    ImgDrawingOptions opt = opts_or_null ? *opts_or_null
                                         : img_drawing_default_options();
    if (opt.top_g == 0) opt.top_g = 1;
    if (opt.passes == 0) opt.passes = 1;

    const uint32_t mem_count = img_delta_memory_count(memory);
    uint32_t* recent_counts = (uint32_t*)calloc(mem_count, sizeof(uint32_t));
    uint8_t*  picked_any    = (uint8_t*) calloc(mem_count, 1);
    if (!recent_counts || !picked_any) {
        free(recent_counts); free(picked_any);
        return 0;
    }

    /* Candidate buffer sized at the largest G we accept. 16 keeps us
     * within img_delta_memory_topg's 32-candidate scratch cap. */
    enum { MAX_G = 16 };
    const ImgDeltaUnit* candidates[MAX_G];
    double              scores[MAX_G];
    if (opt.top_g > MAX_G) opt.top_g = MAX_G;

    const uint32_t n_cells = grid->width * grid->height;

    for (uint32_t pass = 0; pass < opt.passes; pass++) {
        for (uint32_t i = 0; i < n_cells; i++) {
            ImgCECell* cell = &grid->cells[i];

            if (opt.skip_zero_cells && cell->core == 0) continue;

            local.cells_visited++;

            int level = -1;
            uint32_t n = img_delta_memory_topg(
                memory, cell, opt.top_g,
                recent_counts, opt.presence_penalty,
                candidates, scores, &level);
            if (n == 0) continue;

            /* Greedy pick — penalty already diversifies the choice. */
            const ImgDeltaUnit* picked = candidates[0];
            img_delta_apply(cell, memory, picked);

            if (picked->id < mem_count) {
                recent_counts[picked->id]++;
                if (!picked_any[picked->id]) {
                    picked_any[picked->id] = 1;
                    local.unique_deltas_used++;
                }
                if (recent_counts[picked->id] > local.max_recent_count) {
                    local.max_recent_count = recent_counts[picked->id];
                }
            }

            local.stamps_applied++;
        }
    }

    free(recent_counts);
    free(picked_any);

    if (out_stats) *out_stats = local;
    return 1;
}
