#ifndef IMG_REGION_H
#define IMG_REGION_H

/*
 * img_region — A-value region grouping (Axis 2 of the V2 refactor).
 *
 *   One cell ≠ one concept. A concept lives in the *set* of cells
 *   that share the same A (priority) band. The editing / drawing
 *   primitive is therefore the region, not the individual cell.
 *
 *   An ImgRegionMap partitions an ImgCEGrid into disjoint regions
 *   via 4-connected flood fill over cells whose `priority` values
 *   lie within ±a_band_width of the region's seed cell. Each region
 *   records the index pool of its member cells, a dominant role /
 *   tier (majority vote), and a 0..3 level tag (filled by Phase B).
 *
 *   Phase A uses the map as metadata only — the existing raster-order
 *   stamping loop in img_drawing_pass is preserved to keep reference
 *   hashes bit-identical. Phase B and C lift region-aware behaviour
 *   (level pass ordering, region-level subtract application) off this
 *   foundation.
 */

#include <stdint.h>

#include "img_ce.h"

/* A-band region — a maximal 4-connected group of cells whose A
 * (priority) values lie within ±a_band_width of the seed. */
typedef struct ImgRegion {
    uint8_t  a_band_lo;      /* inclusive lower priority observed */
    uint8_t  a_band_hi;      /* inclusive upper priority observed */
    uint32_t cell_count;     /* number of cells in the region */
    uint32_t cells_offset;   /* index into ImgRegionMap.cell_ids */
    uint8_t  dominant_role;  /* majority-vote ImgSemanticRole */
    uint8_t  dominant_depth; /* majority-vote depth_class */
    uint8_t  dominant_flow;  /* majority-vote direction_class */
    uint8_t  level;          /* 0..3; Phase B fills via img_level_infer */
} ImgRegion;

/* A partition of all IMG_CE_TOTAL cells into disjoint regions. */
typedef struct ImgRegionMap {
    ImgRegion* regions;                       /* region_count entries */
    uint32_t   region_count;
    uint32_t   regions_capacity;              /* internal, for realloc */

    uint32_t*  cell_ids;                      /* pooled cell index list */
    uint32_t   cell_ids_count;                /* sum of region.cell_count */
    uint32_t   cell_ids_capacity;

    /* Reverse index: cell_to_region[i] = region index for cell i, or
     * 0xFFFF if the cell was unreachable (shouldn't happen for a
     * full-grid extraction, reserved for future partial maps). */
    uint16_t   cell_to_region[IMG_CE_TOTAL];
} ImgRegionMap;

#define IMG_REGION_NONE 0xFFFFu

/* Extract an A-band region partition from `grid`.
 *
 *   a_band_width — priority tolerance in both directions from the
 *                  seed cell. Passing 0 means "only exactly-equal
 *                  priorities join"; 16 is the recommended default.
 *
 * On success returns 1 and populates *out (caller owns, must free
 * with img_region_map_free). On failure returns 0 and leaves *out
 * in a zero-initialised, free-safe state.
 *
 * Deterministic: regions are numbered in raster-order of their seed
 * cell; cell_ids within each region are stored in BFS (flood-fill)
 * visit order; cell_to_region is fully populated for every cell. */
int  img_region_map_extract(const ImgCEGrid* grid,
                            uint8_t a_band_width,
                            ImgRegionMap* out);

/* Free internal allocations. Safe to call on a zero-initialised
 * struct or after a failed extract. Resets *map to zero. */
void img_region_map_free(ImgRegionMap* map);

/* Apply a per-cell operation to every cell belonging to `region`.
 * Traversal order matches the flood-fill order used during extraction. */
typedef void (*ImgRegionCellOp)(ImgCECell* cell, uint32_t cell_id, void* user);

void img_region_apply(const ImgRegionMap* map,
                      const ImgRegion* region,
                      ImgCEGrid* grid,
                      ImgRegionCellOp op,
                      void* user);

#endif /* IMG_REGION_H */
