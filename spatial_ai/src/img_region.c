#include "img_region.h"
#include "img_level.h"

#include <stdlib.h>
#include <string.h>

/* ── Internal growth helpers ────────────────────────────────
 * Cells live in a single flat pool (cell_ids). Regions store
 * offset+count into that pool so iteration over a region costs
 * one dereference per cell. */

static int region_map_push_region(ImgRegionMap* m, const ImgRegion* r) {
    if (m->region_count == m->regions_capacity) {
        uint32_t new_cap = m->regions_capacity ? m->regions_capacity * 2 : 16;
        ImgRegion* nr = (ImgRegion*)realloc(m->regions,
                                            new_cap * sizeof(ImgRegion));
        if (!nr) return 0;
        m->regions = nr;
        m->regions_capacity = new_cap;
    }
    m->regions[m->region_count++] = *r;
    return 1;
}

static int region_map_push_cell(ImgRegionMap* m, uint32_t cell_id) {
    if (m->cell_ids_count == m->cell_ids_capacity) {
        uint32_t new_cap = m->cell_ids_capacity
                             ? m->cell_ids_capacity * 2
                             : IMG_CE_TOTAL;
        uint32_t* nc = (uint32_t*)realloc(m->cell_ids,
                                          new_cap * sizeof(uint32_t));
        if (!nc) return 0;
        m->cell_ids = nc;
        m->cell_ids_capacity = new_cap;
    }
    m->cell_ids[m->cell_ids_count++] = cell_id;
    return 1;
}

/* Majority vote over a uint8 attribute across the cells currently
 * buffered in [offset, offset+count). Used for role/tier dominance.
 * Bucket size is 256 but costs nothing; this runs once per region. */
static uint8_t majority_u8(const ImgCEGrid* grid,
                           const uint32_t* cell_ids,
                           uint32_t offset, uint32_t count,
                           size_t field_offset_in_cell) {
    uint32_t bins[256];
    memset(bins, 0, sizeof(bins));
    for (uint32_t k = 0; k < count; k++) {
        uint32_t ci = cell_ids[offset + k];
        const uint8_t* base = (const uint8_t*)&grid->cells[ci];
        uint8_t v = *(base + field_offset_in_cell);
        bins[v]++;
    }
    uint8_t best_v = 0;
    uint32_t best_n = 0;
    for (int v = 0; v < 256; v++) {
        if (bins[v] > best_n) { best_n = bins[v]; best_v = (uint8_t)v; }
    }
    return best_v;
}

/* Neighbour test: |a - b| <= width, working in unsigned arithmetic. */
static int within_band(uint8_t a, uint8_t b, uint8_t width) {
    int d = (int)a - (int)b;
    if (d < 0) d = -d;
    return d <= (int)width;
}

void img_region_map_free(ImgRegionMap* map) {
    if (!map) return;
    free(map->regions);
    free(map->cell_ids);
    memset(map, 0, sizeof(*map));
}

int img_region_map_extract(const ImgCEGrid* grid,
                           uint8_t a_band_width,
                           ImgRegionMap* out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++)
        out->cell_to_region[i] = IMG_REGION_NONE;
    if (!grid || !grid->cells) return 0;

    const uint32_t W = grid->width, H = grid->height;
    const uint32_t N = W * H;
    if (N == 0 || N > IMG_CE_TOTAL) return 0;

    /* Pre-allocate pool; we know the upper bound. */
    out->cell_ids = (uint32_t*)malloc(N * sizeof(uint32_t));
    if (!out->cell_ids) { img_region_map_free(out); return 0; }
    out->cell_ids_capacity = N;

    /* BFS queue reused across regions. */
    uint32_t* queue = (uint32_t*)malloc(N * sizeof(uint32_t));
    uint8_t*  seen  = (uint8_t*) calloc(N, 1);
    if (!queue || !seen) { free(queue); free(seen);
                           img_region_map_free(out); return 0; }

    const size_t OFF_ROLE = (size_t)&(((ImgCECell*)0)->semantic_role);
    const size_t OFF_DEPTH = (size_t)&(((ImgCECell*)0)->depth_class);
    const size_t OFF_FLOW  = (size_t)&(((ImgCECell*)0)->direction_class);

    for (uint32_t seed = 0; seed < N; seed++) {
        if (seen[seed]) continue;

        const uint8_t seed_a = grid->cells[seed].priority;
        uint8_t band_lo = seed_a, band_hi = seed_a;

        ImgRegion r;
        memset(&r, 0, sizeof(r));
        r.cells_offset = out->cell_ids_count;

        /* BFS flood fill, 4-connected, tolerance a_band_width
         * relative to the seed's A value. Using the seed as the
         * reference (rather than "neighbour's A") keeps regions
         * bounded and prevents long drift across gradients. */
        uint32_t qh = 0, qt = 0;
        queue[qt++] = seed;
        seen[seed] = 1;
        while (qh < qt) {
            uint32_t c = queue[qh++];
            if (!region_map_push_cell(out, c)) goto fail;
            out->cell_to_region[c] = (uint16_t)out->region_count;
            r.cell_count++;

            uint8_t ca = grid->cells[c].priority;
            if (ca < band_lo) band_lo = ca;
            if (ca > band_hi) band_hi = ca;

            uint32_t y = c / W, x = c % W;
            uint32_t nb[4];
            uint32_t nn = 0;
            if (y > 0)     nb[nn++] = c - W;
            if (y + 1 < H) nb[nn++] = c + W;
            if (x > 0)     nb[nn++] = c - 1;
            if (x + 1 < W) nb[nn++] = c + 1;
            for (uint32_t k = 0; k < nn; k++) {
                uint32_t nc = nb[k];
                if (seen[nc]) continue;
                if (!within_band(grid->cells[nc].priority, seed_a,
                                 a_band_width)) continue;
                seen[nc] = 1;
                queue[qt++] = nc;
            }
        }

        r.a_band_lo      = band_lo;
        r.a_band_hi      = band_hi;
        r.dominant_role  = majority_u8(grid, out->cell_ids,
                                       r.cells_offset, r.cell_count,
                                       OFF_ROLE);
        r.dominant_depth = majority_u8(grid, out->cell_ids,
                                       r.cells_offset, r.cell_count,
                                       OFF_DEPTH);
        r.dominant_flow  = majority_u8(grid, out->cell_ids,
                                       r.cells_offset, r.cell_count,
                                       OFF_FLOW);
        r.level          = img_level_infer_from_region(
                               r.dominant_depth, r.dominant_flow,
                               band_hi, r.cell_count);

        if (!region_map_push_region(out, &r)) goto fail;
    }

    free(queue);
    free(seen);
    return 1;

fail:
    free(queue);
    free(seen);
    img_region_map_free(out);
    return 0;
}

void img_region_apply(const ImgRegionMap* map,
                      const ImgRegion* region,
                      ImgCEGrid* grid,
                      ImgRegionCellOp op,
                      void* user) {
    if (!map || !region || !grid || !op) return;
    const uint32_t off = region->cells_offset;
    const uint32_t n   = region->cell_count;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t ci = map->cell_ids[off + k];
        if (ci >= IMG_CE_TOTAL) continue;
        op(&grid->cells[ci], ci, user);
    }
}
