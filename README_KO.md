# IMG-CANVAS V2

**텍스트와 이미지가 하나의 keyframe 공간을 공유하는 이중 양식 공간 패턴 엔진.** 엔진은 noise에서 denoising 하는 것이 아니라 학습된 델타를 "찍어내서" 그립니다. V2는 이미지 측 내부를 세 설계 축 — **A 영역 그룹화**, **4단계 계층**, **빼기 연산 기반 델타 적용** — 으로 재구축하되, 공개 함수 시그니처 / CLI 플래그 / 파일 포맷은 V1과 **완전 동일**하게 유지합니다.

![hero](assets/main_hero.png)

```
V1 호환 보장
  ├── 공개 C API 시그니처          — 불변
  ├── CLI 플래그 및 기본값          — 불변
  ├── IMEM / SPAI / NMEM 포맷       — 기본 저장 시 불변 (NMEM v2는 opt-in)
  └── 동일 플래그 기준 PNG 해시     — 바이트 동일
```

V1으로 학습한 모델에 `draw --memory … --noise … --noise-seed 42`를 돌리면 V2에서도 동일한 PNG 바이트가 나옵니다. `test_backcompat` 스위트가 고정된 fingerprint로 이 불변성을 지킵니다.

---

## 세 축 (V2)

### 축 1 — 빼기 기반 델타 적용

기존 `cell_state = base + Σ deltas` → `cell_state = 255 - Σ subtracts`. 포화 바이트 연산에서 두 표현은 대수적으로 동치입니다(`test_img_subtract`가 256×600 전수 sweep으로 검증). 빼기 표현은 0/255 clamp를 자동으로 보장하고, 편집·diffusion 수학과의 호환 기반이 됩니다.

```c
ImgSubtractContext ctx = {0};
img_subtract_from_grid(&ctx, grid);      /* 불변식: grid[c] = 255 - acc[c] */
img_subtract_apply_unit(&ctx, i, &grid->cells[i], memory, unit);
img_subtract_to_grid(&ctx, grid);        /* img_delta_apply와 바이트 동일 */
```

### 축 2 — A 영역 그룹화

**셀 하나는 개념이 아닙니다.** 개념은 같은 A(priority) 밴드를 공유하는 셀들의 집합에 있습니다. `img_region_map_extract`는 seed 셀의 priority를 기준으로 4-연결 flood fill을 수행하여 `ImgRegion` 풀을 만들고, O(1) 역참조를 위한 `cell_to_region` 인덱스를 동시에 제공합니다.

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

`img_drawing_pass`는 각 패스 시작 시 영역 맵을 구축합니다. raster 순회 stamp 루프는 의도적으로 유지되어 레퍼런스 해시가 바이트 동일하게 유지되며, 영역 맵은 향후 단계(영역 단위 스케줄링, 편집 맵)의 기반입니다.

### 축 3 — 4단계 계층

```
Level 3 — 절 / 분위기  ← 큰 영역, 거친 스윕
Level 2 — 구 / 면      ← 구조적 블록
Level 1 — 단어 / 선    ← 방향성 중간 디테일
Level 0 — 형태소 / 점  ← 미세 스탬프
```

각 영역은 `img_level_infer_from_region(dominant_depth, dominant_flow, band_hi, cell_count)`로 레벨을 획득합니다. NMEM v2는 샘플당 `tags0`, `tags1`의 MSB 2비트를 예약해 레벨을 영속화합니다 (`img_noise_memory_save_versioned(nmem, path, IMG_NOISE_VERSION_V2)`) — v1 파일은 level 0으로 복원, v2 파일은 인코딩된 레벨 복원. `ns_sample_distance`는 MSB를 마스킹하여 v2 샘플이 v1 쌍둥이와 동일하게 클러스터링됩니다.

기본 저장은 v1이므로 재생성한 NMEM 파일도 V1과 바이트 동일. V2는 opt-in.

---

## 한 눈에 보기

| 양식 | 입력 | 저장 | 추론 / 생성 |
|---|---|---|---|
| **텍스트** | 절 단위 UTF-8 | 256 × 256 RGBA 격자, keyframe + 델타 체인, EMA 사전, topic-hash 버킷 | `ai_predict`, `ai_generate_next`, `ai_recluster` |
| **이미지** | PNG / JPEG / BMP / TGA / PPM, 임의 크기 | 64 × 64 CE 격자, 9216-entry pre-baked SoA 델타 테이블, 멀티스케일 tier-diverse 메모리 | `img_pipeline_run` (압축), `img_drawing_pass` (스탬프), `img_delta_memory_learn_multiscale` |
| **이중양식** | 텍스트 라벨 + 이미지 | `Keyframe.ce_snapshot` 포인터, 후행 레코드 `SPAI_TAG_CE_SNAPSHOT = 0x08` | 텍스트 매치 → 페어 CE snapshot; CE 매치 → 페어 텍스트 격자 |

---

## 아키텍처

```
           ┌────────────── 텍스트 절 ─────────────────┐
           │                                         │
           ▼                                         ▼
       layers_encode_clause                    detect_data_type
       (3-layer bitmap, EMA prior)             PROSE/DIALOG/CODE/SHORT
           │                                         │
           ▼                                         ▼
       256 × 256 RGBA ◄─── topic_hash + seq_in_topic
           │
           ▼
       Keyframe { id, grid, topic_hash, seq_in_topic,
                  data_kind, ce_snapshot }
           │                                         ▲
           │                                         │
           │    ┌── 이미지 (RGB, 임의 크기) ─────────┘
           │    ▼
           │   img_image_to_small_canvas  →  SmallCanvas 256 × 256
           │    ▼
           │   img_small_canvas_to_ce     →  CE 격자 64 × 64
           │
           ▼
      ┌─── 압축 경로 ───────────────────────────────────┐
      │  img_pipeline_run: seed → BFS 확장 → resolve    │
      │  → auto-feedback → .spai / .imem                │
      └────────────────────────────────────────────────┘

      ┌─── 생성 경로 (V2 확장) ─────────────────────────┐
      │  img_drawing_pass(grid, memory, opts):          │
      │    영역 맵 구축 (축 2)                          │
      │    raster 순회 셀 스탬프 (바이트 동일 유지):    │
      │      topg + presence penalty + brush bias →     │
      │      pick → apply → bump recent_counts          │
      │    region.level 추론 (축 3)                     │
      │    img_subtract_apply 등가 경로 (축 1)          │
      └────────────────────────────────────────────────┘
```

---

## 빠른 시작

```bash
cd spatial_ai

make              # 엔진 오브젝트 빌드
make test         # 전체 스위트: 텍스트 + 이미지 + 이중양식 + V2
make demo         # 이미지 파이프라인 시각화
make train        # 이중양식 학습 CLI (manifest → .spai + .imem [+ --nmem *.nmem])
make draw         # 프레임별 이미지 생성 CLI
make chat         # 대화형 텍스트/이미지 REPL
make stream       # 텍스트 스트리밍 트레이너
make gen-tables   # CE 델타 테이블 재생성 (드묾)
```

### Windows (PowerShell, MSYS2 + MinGW-w64)

```powershell
scripts\windows\build.ps1               # 엔진 + 모든 툴 빌드
scripts\windows\test.ps1                # 전체 테스트

scripts\windows\train.ps1 `
  -Manifest spatial_ai\data\characters_manifest.tsv `
  -Name characters                      # → out\models\characters.{spai,imem,nmem}

scripts\windows\draw.ps1 `
  -Memory out\models\characters.imem `
  -Model  out\models\characters.spai `
  -SeedKf 0 -Frames 8 -Name kf0         # → out\draw\kf0\final.png (+frames\)
```

---

## V2 소스 맵

```
spatial_ai/
├── include/
│   ├── img_region.h      (축 2) — ImgRegionMap, flood-fill extract
│   ├── img_level.h       (축 3) — 4-level 상수 + tag MSB 코덱
│   ├── img_subtract.h    (축 1) — ImgSubtractContext + apply helpers
│   └── img_noise_memory.h        — V2: IMG_NOISE_VERSION_V2 + save_versioned
├── src/
│   ├── img_region.c              — 4-연결 BFS + majority vote
│   ├── img_level.c               — infer_from_region 규칙 테이블
│   ├── img_subtract.c            — 포화 빼기 + from/to_grid 왕복
│   ├── img_drawing.c             — 패스당 img_region_map_extract 호출
│   └── img_noise_memory.c        — v1/v2 로드, 레벨 마스킹 ns_sample_distance
└── tests/
    ├── test_img_region.c         — 6 테스트
    ├── test_img_level.c          — 6 테스트
    ├── test_img_subtract.c       — 4 테스트 (전수 입력 sweep)
    └── test_backcompat.c         — 3 테스트 (V1 fingerprint 고정)
```

V2 신규: **파일 10개, 약 1 500 LOC**. `test_backcompat`가 출력 표류를 소리내어 잡습니다.

---

## 드로잉 모드 — "프린터"

엔진은 noise에서 denoising 하지 않습니다. 현재 CE 상태를 관찰하고, 학습된 델타를 선택해서 찍습니다. 다양성은 temperature가 아니라 presence penalty로 확보합니다 (LM의 coverage / presence penalty와 동일 원리).

```c
ImgDrawingOptions opt = img_drawing_default_options();
opt.top_g            = 4;     /* 셀당 후보 풀 크기 */
opt.presence_penalty = 0.5;   /* α; 최근 픽 개수만큼 점수 차감 */
opt.passes           = 3;     /* underdrawing → detail 레이어링 */

img_drawing_pass(grid, memory, &opt, &stats);
```

### 브러시 — 영역 · tier · role 제어

같은 메모리, 다른 브러시 → 다른 결과:

```c
uint8_t face_mask[IMG_CE_TOTAL];
img_brush_mask_rect(face_mask, 22, 10, 42, 28);

opt.region_mask  = face_mask;
opt.target_tier  = IMG_TIER_T3;
opt.target_role  = IMG_ROLE_FACE;
opt.tier_bonus   = 0.25;
opt.role_bonus   = 0.20;

img_drawing_pass(grid, memory, &opt, &face_stats);
```

---

## 프레임별 드로잉 — CLI

```bash
./build/draw \
    --memory out/models/characters.imem \
    --noise  out/models/characters.nmem \
    --noise-seed 42 \
    --frames 4 \
    --out out/draw/characters
```

시드 (더 구체적인 것이 우선):

- `--seed-image <path>` — 이미지로 `img_pipeline_run`을 돌려 CE 격자로 시작.
- `--seed-kf <id>` — `--model`의 keyframe에서 `ce_snapshot` 복사.
- `--noise <file>` — 학습된 NMEM 사전으로 첫 패스 전에 샘플링 (`--noise-seed`로 결정적).
- 시드 없음 — 빈 CE 격자 (모든 셀이 L6 fallback).

튜너블: `--frames N`, `--top-g N`, `--penalty F`, `--noise-seed U64`, `--noise-temp Q8` (0 = greedy).

---

## NMEM — 학습된 공간 사전

드로잉 루프 전에, 학습 관찰치에서 증류된 셀당 사전을 선택적으로 샘플링합니다. 랜덤 Gaussian noise를 데이터에서 학습된 구조로 대체합니다.

```c
ImgNoiseMemory nm; img_noise_memory_init(&nm);
img_noise_memory_load(&nm, "out/chars.nmem");     /* v1 / v2 모두 수용 */

ImgNoiseSampleOptions so = img_noise_sample_default_options();
so.seed           = 42;
so.temperature_q8 = 256;                          /* 1.0 */
img_noise_memory_sample_grid(&nm, grid, &so);

img_drawing_pass(grid, memory, &draw_opts, &stats);

img_noise_memory_free(&nm);
```

V2 레벨 인지 저장 (opt-in):

```c
img_noise_memory_save_versioned(&nm, "out/chars.nmem", IMG_NOISE_VERSION_V2);
```

---

## 편집 철학

편집 기능은 후속 작업이지만, 세 축 공통으로 불변식은 이미 확정되어 있습니다:

- **모든 apply는 빼기.** 초기값 채널당 255; clamp 자동.
- **편집 단위는 A 영역.** 픽셀(셀)이 아니라 개념(영역)을 편집.
- **세기는 반복 횟수, alpha 스칼라 아님.** top-G 샘플링이 반복마다 다른 델타를 뽑아 자연스러운 coarse-to-fine.
- **레벨이 스케줄을 구동.** 절 영역은 1 sweep, 선 영역은 3 sweep — 디테일은 global mode가 아니라 브러시가 결정.

전체 계약은 `SPEC.md`, `SPEC-CE.md`, `SPEC-ENGINE.md` 참조.

---

## 테스트

```
make clean && make -j4 && make test
```

모든 스위트 green: 텍스트 (12) + 이미지 측 (11) + V2 (4) + 통합. `test_backcompat`가 파이프라인 출력을 조용히 이동시키는 어떤 변경도 구체적 진단과 함께 실패시킵니다. 의도된 변경은 `IMG_BACKCOMPAT_PRINT=1 ./build/test_backcompat`로 새 값을 받아 교체.

CLI 레벨 레퍼런스 검증:

```bash
sha256sum out/draw_reference/frames/*.png > reference_hashes.txt   # V2 이전
# …리팩토링…
sha256sum out/draw_final/frames/*.png     > new_hashes.txt
diff -q reference_hashes.txt new_hashes.txt                         # 차이 없어야 함
```

---

## 라이센스

저장소 루트 `LICENSE` 참조.
