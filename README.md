# IMG-CANVAS

**Keyframe-based image generation via a hierarchical CE (Compressed Execution) engine.**

An image is not stored as pixels. It is stored as **bounded-integer resume codes** that, when fed through precomputed lookup tables, reconstruct the image on demand. Learning, generation and visualisation all share the same state-resume kernel.

See [`spatial_ai/SPEC-CE.md`](spatial_ai/SPEC-CE.md) for the full engine contract.

---

## Architecture

```
source image (H × W × RGB)
      │
      ▼                       1st compression (256 × 256)
  SmallCanvas
      │                       R = intensity / presence
      │                       G = flow      / direction
      │                       B = mood      / context
      │                       A = depth     / importance
      ▼                       2nd compression (64 × 64)
   CE grid
      │                       R = core (representative signal)
      │                       G = link (connection slot)
      │                       B = delta (mutability)
      │                       A = priority
      │                       + tone / role / direction / depth / delta_sign tags
      │                       + last_delta_id (resume pointer)
      │
      ▼
   Seed selection            top-K cells by priority (≈3%)
      │
      ▼
   Frontier BFS expansion    pick best delta from memory → apply
      │                       (constrained: direction/depth ±1 only,
      │                        role override gated)
      ▼
   Resolve (sieve + repair)  per-cell outlier / explained / promoted
      │                       masks
      ▼
   Auto-feedback             resolve outcomes bump DeltaMemory
                             success_count; Laplace-smoothed rate
                             drives future scoring
```

Render is a separate read-only layer that projects the CE grid onto raster. Engine state and rendering stay decoupled per the spec: *engine unchanged, only the interpretation differs*.

---

## Key properties

- **Bounded state** — every delta axis has a declared `MAX_*` constant.  The cartesian product of (mode × tier × scale × sign × tone × depth) is 9 216, so all channel contributions are precomputed into **compile-time `const` SoA tables** (`src/img_delta_tables_data.c`). Runtime cost of `img_delta_interpret` is one index calculation + five indexed loads + a packed flag dispatch — no float, no branching on values.
- **Resume-code delta** — a delta is not a numeric diff.  It is a packed `uint32` state `(tier, scale, precision, sign, tick, mode, channel_layout, slot_shape)` that selects a point in the finite state space.  Same symbolic delta expands to *different* concrete changes depending on the cell's current tags.
- **Single source of truth for tiers** — `IMG_TIER_TABLE[]` holds `(scale_factor, range_max)` per tier.  `img_delta_compute` reads `scale_factor`; `img_render` reads `range_max`.  `img_tier_adapt` can quantile-rederive `range_max` per image for adaptive rendering.
- **Set16 / Quad SIMD unit** (SPEC §6) — 4 × 4 = 16 cells, four 2 × 2 quads (PLUS / MINUS / SCALE / PRECISION).  SoA layout: every channel array is exactly 16 bytes — one 128-bit SSE2 / NEON register.
- **Closed learning loop** — `img_delta_learn` reads before/after image pairs and inserts rules into memory.  `img_pipeline_run` applies the best match per cell; `img_ce_resolve` flags outliers; `img_delta_memory_ingest_resolve` credits every applied delta's outcome back into memory (`success_count` only; `usage_count` is already owned by `img_delta_apply`).  Laplace-smoothed success rate (`(s+1)/(u+2)`) keeps fresh 1/1 units from dominating seasoned 50/100 veterans.
- **Weighted learning (hierarchical sieve)** — each stored delta carries a `weight` (baseline 1000).  At learn time, the first insert of a new semantic-role × tone × direction × depth bucket gets 4× baseline, the second gets 2×, the third and beyond settle at baseline.  Weight contributes a bounded `±0.30 / −0.10` nudge to `img_delta_score`, so rare patterns win close ties but never veto a strong-match common delta.  **Weight is a tiebreaker and learning-rate modulator, not a filter** — weak signals still survive.
- **I-frame / P-frame CE codec** — `img_ce_diff_compute(base, target, diff)` produces a sparse per-cell patch (channel deltas + tag replacements).  `img_ce_diff_apply(base, diff, out)` reconstructs the target, optionally in place.  Same keyframe / delta doctrine the text engine already uses (`SPEC.md §D`, `README_KO §4`), now applied to CE grid state: stack-friendly persistence, compact transmission, and a natural unit for future memory serialisation.

---

## Project layout

```
spatial_ai/
├── SPEC-CE.md                     CE engine contract (SPEC v1.0)
├── include/
│   ├── img_ce.h                   SmallCanvas / CE grid / resolve
│   ├── img_delta_memory.h         StateKey u64 / DeltaUnit / bucket lookup
│   ├── img_delta_compute.h        pure compute_entry (no globals)
│   ├── img_tier_table.h           canonical {scale_factor, range_max}
│   ├── img_set16.h                4×4 Set16 SoA + Quad indexing
│   ├── img_render.h               CE grid → RGB, SlotShape, masks
│   ├── img_pipeline.h             end-to-end run (image → result)
│   ├── img_delta_learn.h          populate memory from image pairs
│   │                              (+ rarity-weighted hierarchical sieve)
│   └── img_ce_diff.h              I-frame / P-frame CE state codec
├── src/
│   ├── img_delta_tables_data.c    AUTO-GENERATED baked tables
│   └── ... (one .c per header)
├── tests/
│   └── test_img_*.c               63 unit tests covering every module
├── tools/
│   ├── gen_delta_tables.c         offline table generator
│   └── demo_pipeline.c            CLI for visual inspection
└── third_party/
    ├── stb_image.h                PNG/JPEG/BMP/TGA reader (public domain)
    └── stb_image_write.h          PNG writer
```

---

## Build & test

```bash
cd spatial_ai
make            # build all objects
make test       # 63 new img_* tests in the CE stack, plus text-engine suites
```

Regenerate the baked delta tables (only needed after changing
`img_delta_compute` or the tier table):

```bash
make gen-tables
```

---

## Demo — visual inspection loop

```bash
cd spatial_ai
make demo

# PNG in → pipeline → PNG + PPM out (plain and mask-overlayed)
./build/demo_pipeline --adapt ../main_hero.png ../out/run1
```

`--adapt` runs `img_render_options_adapt_to_ce` so tier thresholds match the input image's histogram.  Produces four artefacts next to the prefix: `*_plain.{png,ppm}` and `*_masked.{png,ppm}`.  The masked variant tints cyan for resolve-absorbed cells and red for resolve-promoted (unexplained) cells.

### End-to-end learning loop

```bash
./build/demo_pipeline --adapt --learn \
    ../main_hero.png ../visualization_1.png \
    ../main_hero.png ../out/run2
```

- Opens two images, builds a CE grid for each, diffs them into a symbolic delta set, inserts the deltas into memory.
- Runs the full pipeline on the first image with that memory — BFS expansion applies the best-matching delta at every frontier cell.
- Resolve flags outliers; auto-feedback credits success / failure into memory.
- On a fresh run the output shows, per line:
  ```
  expansions:       506
  resolve_promoted: 2
  feedback_success: 504
  feedback_failure: 2      ← matches resolve_promoted exactly
  ```
  That invariant (`feedback_failure == resolve_promoted`) is how the learning loop closes: every cell resolve couldn't absorb is credited as a failure against the delta that produced it.

---

## Testing

```bash
cd spatial_ai && make test
```

Covers (in new CE stack):

| Module | Tests |
|---|---|
| `img_ce` — SmallCanvas / CE grid / resolve | 6 |
| `img_delta_memory` — StateKey pack, fallback chain, Laplace scoring, apply constraints, baked tables regression, auto-feedback, weight (default / weighted add / score nudge / tiebreak-not-filter) | 20 |
| `img_set16` — layout/sizes, quad indices, CE ↔ Set16 roundtrip, edge clipping | 5 |
| `img_render` — default options, empty grid, per-channel SlotShape, tier-4 bleed, PPM save, mask tints | 7 |
| `img_pipeline` — defaults, seed-fraction bounds, memory-driven expansion, pipeline → render, masks flow, feedback ingest | 7 |
| `img_tier_table` — canonical values, classify, adapt quantile, CE histogram, render-options adapt | 7 |
| `img_delta_learn` — identical pairs, single-cell Δcore, tag precedence, noise floor, end-to-end, rarity-weight decay across duplicates | 6 |
| `img_ce_diff` — identical → empty, single-cell roundtrip, tag-only diff, many-cell roundtrip, self-apply, zero-init destroy | 6 |

Pre-existing text-engine tests (`test_grid`, `test_match`, `test_keyframe`, …) remain green.

---

## Related spec

- **Spec:** [`spatial_ai/SPEC-CE.md`](spatial_ai/SPEC-CE.md) — the source of truth for the CE engine (18 sections, from StateKey packing through lookup model, channel semantics, Set16/Quad, slot shape, tier table, resolve and the execution pipeline).
- **Text-side spec:** [`spatial_ai/SPEC.md`](spatial_ai/SPEC.md) and [`README_KO.md`](README_KO.md) describe the sibling 256×256 keyframe engine for Korean clauses, which shares the same I-frame / P-frame doctrine adopted here for image state.

---

## License

See [LICENSE](LICENSE).
