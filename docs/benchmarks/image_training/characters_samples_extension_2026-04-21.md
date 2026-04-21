# Character Training Extension Report (2026-04-21)

## Scope
- Added 4 sample-based training images from `assets/characters/samples` into the main training set:
  - `assets/characters/char_11_ruby_plain.png`
  - `assets/characters/char_12_ruby_masked.png`
  - `assets/characters/char_13_noir_plain.png`
  - `assets/characters/char_14_noir_masked.png`
- Extended `spatial_ai/data/characters_manifest.tsv` from 10-pair ring to 15 training rows.

## Reproduction
```bash
cd spatial_ai
make train
./build/train \
  --model ../out/models/characters_extended.spai \
  --memory ../out/models/characters_extended.imem \
  data/characters_manifest.tsv

make draw
./build/draw \
  --memory ../out/models/characters_extended.imem \
  --model ../out/models/characters_extended.spai \
  --seed-kf 10 \
  --frames 8 \
  --out ../out/draw/characters_extended_kf10
```

## Train Summary
```
manifest rows:    15 (15 ok)
deltas added:     17106
units (before):   0
units (after):    17106
keyframes:        15
ce snapshots:     15
```

## Draw Practical Test (seed-kf 10, frames=8)
```
frame 01/08  stamps=4096  unique=126
frame 02/08  stamps=4096  unique=160
frame 03/08  stamps=4096  unique=160
frame 04/08  stamps=4096  unique=139
frame 05/08  stamps=4096  unique=131
frame 06/08  stamps=4096  unique=130
frame 07/08  stamps=4096  unique=130
frame 08/08  stamps=4096  unique=130

done — 8 frames, 32768 total stamps, max unique per frame 160
output: ../out/draw/characters_extended_kf10/final.png (+frames/)
```

## Validation
- Full regression suite passed after the dataset change:
  - `cd spatial_ai && make test`
  - Result: `=== ALL TESTS PASSED ===`
