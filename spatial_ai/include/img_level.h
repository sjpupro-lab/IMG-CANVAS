#ifndef IMG_LEVEL_H
#define IMG_LEVEL_H

/*
 * img_level — 4-level hierarchy (Axis 3 of the V2 refactor).
 *
 *   Level 3 — 절 / 분위기 (clause / mood)       — largest scope, rough sweep
 *   Level 2 — 구 / 면     (phrase / area)       — structural blocks
 *   Level 1 — 단어 / 선   (word / line)         — oriented mid-detail
 *   Level 0 — 형태소 / 점 (morpheme / point)    — fine stamps
 *
 *   Phase B uses levels only as metadata: regions gain a `level` tag
 *   (filled during region extraction), and img_drawing_pass gains an
 *   opt-in `use_level_passes` mode that schedules per-level pass
 *   counts. The default (level-unaware) path is bit-identical to
 *   Phase A, so reference hashes are preserved.
 *
 *   NMEM v2 reserves the two MSBs of the per-sample tag bytes
 *   (tags0 bit 7 = level_lo, tags1 bit 7 = level_hi) to persist the
 *   level a sample was observed at. V1 files load as level 0 (point);
 *   v2 files restore the encoded level. Default save stays v1 — the
 *   v2 path is opt-in so NMEM files regenerated on a V2 build stay
 *   byte-identical for hash matching.
 */

#include <stdint.h>

#define IMG_LEVEL_COUNT     4
#define IMG_LEVEL_POINT     0    /* 형태소 / 점          */
#define IMG_LEVEL_LINE      1    /* 단어 / 선            */
#define IMG_LEVEL_AREA      2    /* 구 / 면              */
#define IMG_LEVEL_CLAUSE    3    /* 절 / 분위기           */

/* Default per-level iteration budget for the level-aware drawing
 * scheduler. Used only when ImgDrawingOptions.use_level_passes is
 * set; default drawing ignores this. */
extern const uint8_t IMG_LEVEL_DEFAULT_PASSES[IMG_LEVEL_COUNT];

/* Mask for the two bits we reserve inside tags0 / tags1 for the
 * 2-bit level code (LSB in tags0 MSB, MSB in tags1 MSB). */
#define IMG_LEVEL_TAG0_BIT  0x80u
#define IMG_LEVEL_TAG1_BIT  0x80u
#define IMG_LEVEL_TAG_MASK  (IMG_LEVEL_TAG0_BIT | IMG_LEVEL_TAG1_BIT)

/* Infer the level of a region from the majority vote signals stored
 * on ImgRegion. Combines spec §5.2.3 (count / tier) with §5.3.2
 * (priority + direction) so both "coarse zone" and "oriented line"
 * regions land on the right level.
 *
 *   cell_count ≥ 512            → level 3 (clause / mood)
 *   band_hi ≥ 180 OR
 *     dominant_depth==FOREGROUND → level 2 (area)
 *   dominant_flow != 0           → level 1 (line)
 *   otherwise                    → level 0 (point) */
uint8_t img_level_infer_from_region(uint8_t dominant_depth,
                                    uint8_t dominant_flow,
                                    uint8_t band_hi,
                                    uint32_t cell_count);

/* Encode / decode the 2-bit level code into tags0 / tags1 MSBs.
 * Callers pass the existing tags bytes; the returned byte has the
 * level bit set/cleared while all other bits are preserved. */
static inline uint8_t img_level_tag0_set(uint8_t tags0, uint8_t level) {
    tags0 &= (uint8_t)~IMG_LEVEL_TAG0_BIT;
    if (level & 0x1u) tags0 |= IMG_LEVEL_TAG0_BIT;
    return tags0;
}
static inline uint8_t img_level_tag1_set(uint8_t tags1, uint8_t level) {
    tags1 &= (uint8_t)~IMG_LEVEL_TAG1_BIT;
    if (level & 0x2u) tags1 |= IMG_LEVEL_TAG1_BIT;
    return tags1;
}
static inline uint8_t img_level_decode(uint8_t tags0, uint8_t tags1) {
    uint8_t lo = (tags0 & IMG_LEVEL_TAG0_BIT) ? 1u : 0u;
    uint8_t hi = (tags1 & IMG_LEVEL_TAG1_BIT) ? 1u : 0u;
    return (uint8_t)(lo | (hi << 1));
}

/* Convenience: pass-tag ↔ level when the caller already encodes
 * level into the top 2 bits of a single byte. */
static inline uint8_t img_level_to_pass_tag(uint8_t level) {
    return (uint8_t)((level & 0x3u) << 6);
}
static inline uint8_t img_pass_tag_to_level(uint8_t pass_tag) {
    return (uint8_t)((pass_tag >> 6) & 0x3u);
}

#endif /* IMG_LEVEL_H */
