# IMG-CANVAS V2

**Dual-modality spatial pattern engine — text and image share one keyframe space, and the engine draws by *stamping* learned deltas, not by denoising noise.** V2 rebuilds the image-side internals around three design axes — **A-region grouping**, **4-level hierarchy**, and **subtract-domain delta apply** — while keeping every public function signature, CLI flag, and file format bit-identical to V1.

![hero](assets/main_hero.png)

```
V1 compatibility guarantee
  ├── Public C API signatures      — unchanged
  ├── CLI flags and defaults       — unchanged
  ├── IMEM / SPAI / NMEM formats   — unchanged on save (v2 NMEM opt-in)
  └── Reference PNG output hashes  — bit-identical under same flags
```

If you run `draw --memory … --noise … --noise-seed 42` on a V1-trained model, you get the same PNG bytes out of V2. Our `test_backcompat` suite pins fingerprints that fail loudly if that ever drifts.

---

## The three axes (V2)

### Axis 1 — Subtract-domain delta apply

Classic `cell_state = base + Σ deltas` becomes `cell_state = 255 - Σ subtracts`. The two are algebraic twins for any saturating byte arithmetic (`test_img_subtract` proves it over the full 256×600 input grid) but the subtract view makes the 0/255 clamps automatic and gives editing / diffusion code a natural footing.

```c
ImgSubtractContext ctx = {0};
img_subtract_from_grid(&ctx, grid);      /* invariant: grid[c] = 255 - acc[c] */
img_subtract_apply_unit(&ctx, i, &grid->cells[i], memory, unit);
img_subtract_to_grid(&ctx, grid);        /* matches img_delta_apply byte-for-byte */
```

### Axis 2 — A-region grouping

One cell is not a concept. A concept lives in the *set* of cells that share an A (priority) band. `img_region_map_extract` runs a 4-connected flood fill keyed off the seed's priority and packs cells into a pooled `ImgRegion` layout with a reverse `cell_to_region` index for O(1) membership.

```c
ImgRegionMap map = {0};
img_region_map_extract(grid, /*a_band_width=*/16, &map);
for (uint32_t r = 0; r < map.region_count; r++) {
    const ImgRegion* region = &map.regions[r];
    /* region->level, region->dominant_depth, region->dominant_flow,
     * region->a_band_lo..hi, region->cell_count */
}
img_region_map_free(&map);
```

`img_drawing_pass` builds the region map at the start of every pass. The raster-order stamp loop is intentionally unchanged so reference hashes stay bit-identical — the map is the foundation later phases (region-wise scheduling, editing maps) build on.

### Axis 3 — Four-level hierarchy

```
Level 3 — 절 / 분위기 (clause / mood)   ← largest zones, rough sweep
Level 2 — 구 / 면     (phrase / area)   ← structural blocks
Level 1 — 단어 / 선   (word / line)     ← oriented mid-detail
Level 0 — 형태소 / 점 (morpheme / point)← fine stamps
```

Every region gets a level via `img_level_infer_from_region(dominant_depth, dominant_flow, band_hi, cell_count)`. NMEM v2 reserves the top bit of `tags0` and `tags1` per sample to persist that level across save/load (`img_noise_memory_save_versioned(nmem, path, IMG_NOISE_VERSION_V2)`) — v1 files load back as level 0, v2 files restore the encoded level. Similarity clustering masks those MSBs so a v2 sample compares identically to its v1 twin.

Default save stays v1, so regenerated NMEM files still hash-match V1 byte-for-byte. V2 is opt-in.

---

## At a glance

| Modality | Input | Storage | Inference / generation |
|---|---|---|---|
| **Text** | line-per-clause UTF-8 | 256 × 256 RGBA grid, keyframe + delta chain, EMA priors, topic-hash bucket | `ai_predict`, `ai_generate_next`, `ai_recluster` |
| **Image** | PNG / JPEG / BMP / TGA / PPM, any dim | 64 × 64 CE grid of interpreted cells; 9 216-entry pre-baked SoA delta tables; multi-scale tier-diverse memory | `img_pipeline_run` (compress), `img_drawing_pass` (stamp), `img_delta_memory_learn_multiscale` |
| **Bimodal** | text label + image | `Keyframe.ce_snapshot` pointer per KF, trailing record `SPAI_TAG_CE_SNAPSHOT = 0x08` | text-match returns paired CE snapshot; CE match returns paired text grid |

Each side scores independently on a 0..1 scale; callers combine (`joint = α·text + β·ce`) — no mixed state-key space.

---

## Architecture

```
           ┌────────────── text clause ──────────────┐
           │                                         │
           ▼                                         ▼
       layers_encode_clause                    detect_data_type
       (3-layer bitmap, EMA prior)             PROSE/DIALOG/CODE/SHORT
           │                                         │
           ▼                                         ▼
       256 × 256 RGBA grid ◄─── topic_hash + seq_in_topic
           │
           ▼
       Keyframe { id, grid, topic_hash, seq_in_topic,
                  data_kind, ce_snapshot }
           │                                         ▲
           │                                         │
           │    ┌── image (RGB, any dim) ────────────┘
           │    ▼
           │   img_image_to_small_canvas  →  SmallCanvas 256 × 256
           │          (R=intensity · G=flow · B=mood · A=depth)
           │    ▼
           │   img_small_canvas_to_ce     →  CE grid 64 × 64
           │          (R=core · G=link · B=delta · A=priority
           │           + tone / role / direction / depth / delta_sign
           │           + tier / last_delta_id)
           │
           ▼
      ┌─── COMPRESS PATH ───────────────────────────────┐
      │  img_pipeline_run: seed → BFS expand → resolve │
      │  → auto-feedback → .spai / .imem                │
      └────────────────────────────────────────────────┘

      ┌─── GENERATE PATH (V2-enriched) ─────────────────┐
      │  img_drawing_pass(grid, memory, opts):          │
      │    build region map (Axis 2)                    │
      │    for each cell (raster order, bit-identical): │
      │      topg + presence penalty + brush bias →     │
      │      pick → apply → bump recent_counts          │
      │    region.level from img_level_infer (Axis 3)   │
      │    img_subtract_apply equivalent on the side    │
      │      (Axis 1, proven bit-identical)             │
      └────────────────────────────────────────────────┘
```

---

## Quick start

```bash
cd spatial_ai

make              # build engine objects
make test         # every suite: text + image + bimodal + V2
make demo         # image pipeline visualiser
make train        # bimodal training CLI (manifest → .spai + .imem [+ --nmem *.nmem])
make draw         # frame-by-frame image generation CLI
make chat         # interactive text/image REPL
make stream       # text streaming trainer
make gen-tables   # regenerate baked CE delta tables (rare)
```

### Windows (PowerShell, MSYS2 + MinGW-w64)

```powershell
# From the repo root, with mingw32-make + gcc on PATH.
scripts\windows\build.ps1               # builds engine + all tools
scripts\windows\test.ps1                # runs every suite

scripts\windows\train.ps1 `
  -Manifest spatial_ai\data\characters_manifest.tsv `
  -Name characters                      # → out\models\characters.{spai,imem,nmem}

scripts\windows\draw.ps1 `
  -Memory out\models\characters.imem `
  -Model  out\models\characters.spai `
  -SeedKf 0 -Frames 8 -Name kf0         # → out\draw\kf0\final.png (+frames\)
```

---

## V2 source map

```
spatial_ai/
├── include/
│   ├── img_region.h      (Axis 2) — ImgRegionMap, flood-fill extract
│   ├── img_level.h       (Axis 3) — 4-level constants + tag MSB codec
│   ├── img_subtract.h    (Axis 1) — ImgSubtractContext + apply helpers
│   └── img_noise_memory.h         — V2 adds IMG_NOISE_VERSION_V2 + save_versioned
├── src/
│   ├── img_region.c               — 4-connected BFS + majority vote
│   ├── img_level.c                — infer_from_region rule table
│   ├── img_subtract.c             — saturating sub + from/to_grid round-trip
│   ├── img_drawing.c              — calls img_region_map_extract per pass
│   └── img_noise_memory.c         — v1/v2 load, level-masked ns_sample_distance
└── tests/
    ├── test_img_region.c          — 6 tests
    ├── test_img_level.c           — 6 tests
    ├── test_img_subtract.c        — 4 tests (incl. full input sweep)
    └── test_backcompat.c          — 3 tests (pinned V1 fingerprints)
```

New V2 scope: **10 files, ~1 500 LOC**, covered by a new `test_backcompat` suite that catches any silent output drift.

---

## Drawing mode — the "printer"

The engine doesn't denoise from noise. It observes the current CE state, picks a learned delta, and stamps. Diversity comes from a presence penalty (same idea as LM coverage / presence penalty) instead of temperature.

```c
ImgDrawingOptions opt = img_drawing_default_options();
opt.top_g            = 4;     /* pool size per cell */
opt.presence_penalty = 0.5;   /* α; subtracted per recent pick */
opt.passes           = 3;     /* underdrawing → detail layering */

img_drawing_pass(grid, memory, &opt, &stats);
```

### Brush — region · tier · role control

Single memory, different brushes, different outputs:

```c
uint8_t face_mask[IMG_CE_TOTAL];
img_brush_mask_rect(face_mask, 22, 10, 42, 28);

opt.region_mask  = face_mask;         /* only these cells */
opt.target_tier  = IMG_TIER_T3;       /* structure-level detail */
opt.target_role  = IMG_ROLE_FACE;
opt.tier_bonus   = 0.25;              /* score bonus when payload tier matches */
opt.role_bonus   = 0.20;              /* ditto for role */

img_drawing_pass(grid, memory, &opt, &face_stats);
```

Per-pass stats surface `cells_masked_out`, `brush_bonus_wins`, `unique_deltas_used`, `max_recent_count`.

---

## Frame-by-frame drawing — the CLI

`tools/draw.c` drives the engine as a small "video" generator: one drawing pass per frame, the grid rendered and saved after each pass.

```bash
./build/draw \
    --memory out/models/characters.imem \
    --noise  out/models/characters.nmem \
    --noise-seed 42 \
    --frames 4 \
    --out out/draw/characters
```

Seeds (most-specific wins):

- `--seed-image <path>` — runs `img_pipeline_run` on the image and starts from the resulting CE grid.
- `--seed-kf <id>` — copies `Keyframe.ce_snapshot` from the given keyframe in `--model`.
- `--noise <file>` — samples the learned NMEM prior before the first pass (deterministic in `--noise-seed`).
- no seed — blank CE grid (every cell at the L6 fallback, so output is abstract).

Tunables: `--frames N`, `--top-g N`, `--penalty F` (presence penalty), `--noise-seed U64`, `--noise-temp Q8` (0 = greedy).

---

## NMEM — learned spatial prior

Before the drawing loop, optionally sample a per-cell prior distilled from training observations. That replaces random Gaussian noise with structure learned from the data.

```c
ImgNoiseMemory nm; img_noise_memory_init(&nm);
img_noise_memory_load(&nm, "out/chars.nmem");     /* accepts v1 and v2 */

ImgNoiseSampleOptions so = img_noise_sample_default_options();
so.seed           = 42;
so.temperature_q8 = 256;                          /* 1.0 */
img_noise_memory_sample_grid(&nm, grid, &so);

img_drawing_pass(grid, memory, &draw_opts, &stats);

img_noise_memory_free(&nm);
```

V2 optional level-aware save:

```c
img_noise_memory_save_versioned(&nm, "out/chars.nmem", IMG_NOISE_VERSION_V2);
```

---

## Editing philosophy

Editing is future work but the invariants are already decided, shared across the three axes:

- **Every apply is a subtract.** Initial value is 255 per channel; clamps are automatic.
- **The unit of edit is the A-region.** You edit a concept (region) not a pixel (cell).
- **Strength is iteration count, never an alpha scalar.** Top-G sampling diverges across iterations, producing a natural coarse-to-fine sweep.
- **Levels drive schedule.** Clause regions get one sweep, line regions get three — detail doesn't care about global mode, the brush does.

See `SPEC.md`, `SPEC-CE.md`, `SPEC-ENGINE.md` for the full contract.

---

## Testing

```
make clean && make -j4 && make test
```

Every suite green: text (12) + image-side (11) + V2 (4) + integration. `test_backcompat` carries pinned fingerprints that fail with a specific diagnostic if any change silently shifts the pipeline output — intentional shifts rebase by running `IMG_BACKCOMPAT_PRINT=1 ./build/test_backcompat` and pasting the new values.

CLI-level reference check:

```bash
sha256sum out/draw_reference/frames/*.png > reference_hashes.txt   # pre-V2
# …refactor…
sha256sum out/draw_final/frames/*.png     > new_hashes.txt
diff -q reference_hashes.txt new_hashes.txt                         # must be clean
```

---

## License

See `LICENSE` at the repo root.
