#include "spatial_io.h"
#include "spatial_grid.h"
#include "spatial_canvas.h"
#include "spatial_subtitle.h"
#include "img_ce.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Header struct (on-disk, 32 bytes) ── */
typedef struct {
    char     magic[4];
    uint32_t version;
    uint32_t kf_count;
    uint32_t df_count;
    uint32_t reserved[3];
} SpaiHeader;

const char* spai_status_str(SpaiStatus s) {
    switch (s) {
        case SPAI_OK:          return "OK";
        case SPAI_ERR_OPEN:    return "cannot open file";
        case SPAI_ERR_MAGIC:   return "bad magic (not a SPAI file)";
        case SPAI_ERR_VERSION: return "unsupported file version";
        case SPAI_ERR_READ:    return "read error / truncated file";
        case SPAI_ERR_WRITE:   return "write error";
        case SPAI_ERR_CORRUPT: return "file corrupted";
        case SPAI_ERR_ALLOC:   return "allocation failed";
        case SPAI_ERR_STATE:   return "engine has fewer entries than file";
    }
    return "unknown";
}

/* ── header helpers ── */

static SpaiStatus read_header(FILE* fp, SpaiHeader* h) {
    if (fread(h, sizeof(*h), 1, fp) != 1) return SPAI_ERR_READ;
    if (memcmp(h->magic, SPAI_MAGIC, 4) != 0) return SPAI_ERR_MAGIC;
    /* Accept any version ≤ current; older files just omit sections
     * added in later versions. */
    if (h->version == 0 || h->version > SPAI_VERSION) return SPAI_ERR_VERSION;
    return SPAI_OK;
}

static SpaiStatus write_header(FILE* fp, uint32_t kf_count, uint32_t df_count) {
    SpaiHeader h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, SPAI_MAGIC, 4);
    h.version  = SPAI_VERSION;
    h.kf_count = kf_count;
    h.df_count = df_count;
    if (fwrite(&h, sizeof(h), 1, fp) != 1) return SPAI_ERR_WRITE;
    return SPAI_OK;
}

/* ── Record writers ── */

static SpaiStatus write_keyframe_record(FILE* fp, const Keyframe* kf) {
    uint8_t tag = SPAI_TAG_KEYFRAME;
    if (fwrite(&tag, 1, 1, fp) != 1) return SPAI_ERR_WRITE;

    if (fwrite(&kf->id, sizeof(uint32_t), 1, fp) != 1) return SPAI_ERR_WRITE;
    if (fwrite(kf->label, 1, 64, fp) != 64) return SPAI_ERR_WRITE;
    if (fwrite(&kf->text_byte_count, sizeof(uint32_t), 1, fp) != 1) return SPAI_ERR_WRITE;

    if (fwrite(kf->grid.A, sizeof(uint16_t), GRID_TOTAL, fp) != GRID_TOTAL)
        return SPAI_ERR_WRITE;
    if (fwrite(kf->grid.R, 1, GRID_TOTAL, fp) != GRID_TOTAL) return SPAI_ERR_WRITE;
    if (fwrite(kf->grid.G, 1, GRID_TOTAL, fp) != GRID_TOTAL) return SPAI_ERR_WRITE;
    if (fwrite(kf->grid.B, 1, GRID_TOTAL, fp) != GRID_TOTAL) return SPAI_ERR_WRITE;
    return SPAI_OK;
}

static SpaiStatus write_delta_record(FILE* fp, const DeltaFrame* df) {
    uint8_t tag = SPAI_TAG_DELTA;
    if (fwrite(&tag, 1, 1, fp) != 1) return SPAI_ERR_WRITE;

    if (fwrite(&df->id,        sizeof(uint32_t), 1, fp) != 1) return SPAI_ERR_WRITE;
    if (fwrite(&df->parent_id, sizeof(uint32_t), 1, fp) != 1) return SPAI_ERR_WRITE;
    if (fwrite(df->label,      1, 64, fp)            != 64) return SPAI_ERR_WRITE;
    if (fwrite(&df->count,     sizeof(uint32_t), 1, fp) != 1) return SPAI_ERR_WRITE;
    if (fwrite(&df->change_ratio, sizeof(float), 1, fp) != 1) return SPAI_ERR_WRITE;

    if (df->count > 0) {
        if (fwrite(df->entries, sizeof(DeltaEntry), df->count, fp) != df->count)
            return SPAI_ERR_WRITE;
    }
    return SPAI_OK;
}

/* ── Record readers ── */

/* Read a keyframe record body (tag already consumed). Allocates grid channels. */
static SpaiStatus read_keyframe_body(FILE* fp, Keyframe* kf) {
    memset(kf, 0, sizeof(*kf));

    if (fread(&kf->id, sizeof(uint32_t), 1, fp) != 1) return SPAI_ERR_READ;
    if (fread(kf->label, 1, 64, fp) != 64) return SPAI_ERR_READ;
    if (fread(&kf->text_byte_count, sizeof(uint32_t), 1, fp) != 1) return SPAI_ERR_READ;

    kf->grid.A = (uint16_t*)malloc(GRID_TOTAL * sizeof(uint16_t));
    kf->grid.R = (uint8_t*) malloc(GRID_TOTAL);
    kf->grid.G = (uint8_t*) malloc(GRID_TOTAL);
    kf->grid.B = (uint8_t*) malloc(GRID_TOTAL);
    if (!kf->grid.A || !kf->grid.R || !kf->grid.G || !kf->grid.B) {
        free(kf->grid.A); free(kf->grid.R); free(kf->grid.G); free(kf->grid.B);
        memset(&kf->grid, 0, sizeof(kf->grid));
        return SPAI_ERR_ALLOC;
    }

    if (fread(kf->grid.A, sizeof(uint16_t), GRID_TOTAL, fp) != GRID_TOTAL)
        return SPAI_ERR_READ;
    if (fread(kf->grid.R, 1, GRID_TOTAL, fp) != GRID_TOTAL) return SPAI_ERR_READ;
    if (fread(kf->grid.G, 1, GRID_TOTAL, fp) != GRID_TOTAL) return SPAI_ERR_READ;
    if (fread(kf->grid.B, 1, GRID_TOTAL, fp) != GRID_TOTAL) return SPAI_ERR_READ;
    return SPAI_OK;
}

static SpaiStatus read_delta_body(FILE* fp, DeltaFrame* df) {
    memset(df, 0, sizeof(*df));

    if (fread(&df->id,        sizeof(uint32_t), 1, fp) != 1) return SPAI_ERR_READ;
    if (fread(&df->parent_id, sizeof(uint32_t), 1, fp) != 1) return SPAI_ERR_READ;
    if (fread(df->label,      1, 64, fp)            != 64) return SPAI_ERR_READ;
    if (fread(&df->count,     sizeof(uint32_t), 1, fp) != 1) return SPAI_ERR_READ;
    if (fread(&df->change_ratio, sizeof(float), 1, fp) != 1) return SPAI_ERR_READ;

    df->entries = NULL;
    if (df->count > 0) {
        df->entries = (DeltaEntry*)malloc(df->count * sizeof(DeltaEntry));
        if (!df->entries) return SPAI_ERR_ALLOC;
        if (fread(df->entries, sizeof(DeltaEntry), df->count, fp) != df->count)
            return SPAI_ERR_READ;
    }
    return SPAI_OK;
}

/* ── Capacity helpers ── */

static int grow_kf_cap(SpatialAI* ai, uint32_t needed) {
    if (needed <= ai->kf_capacity) return 1;
    uint32_t cap = ai->kf_capacity ? ai->kf_capacity : 64;
    while (cap < needed) cap *= 2;
    Keyframe* n = (Keyframe*)realloc(ai->keyframes, cap * sizeof(Keyframe));
    if (!n) return 0;
    memset(&n[ai->kf_capacity], 0, (cap - ai->kf_capacity) * sizeof(Keyframe));
    ai->keyframes   = n;
    ai->kf_capacity = cap;
    return 1;
}

static int grow_df_cap(SpatialAI* ai, uint32_t needed) {
    if (needed <= ai->df_capacity) return 1;
    uint32_t cap = ai->df_capacity ? ai->df_capacity : 64;
    while (cap < needed) cap *= 2;
    DeltaFrame* n = (DeltaFrame*)realloc(ai->deltas, cap * sizeof(DeltaFrame));
    if (!n) return 0;
    memset(&n[ai->df_capacity], 0, (cap - ai->df_capacity) * sizeof(DeltaFrame));
    ai->deltas      = n;
    ai->df_capacity = cap;
    return 1;
}

/* ── Public: save ── */

static SpaiStatus write_weights_record(FILE* fp, const ChannelWeight* w) {
    uint8_t tag = SPAI_TAG_WEIGHTS;
    if (fwrite(&tag, 1, 1, fp) != 1) return SPAI_ERR_WRITE;
    if (fwrite(&w->w_A, sizeof(float), 1, fp) != 1) return SPAI_ERR_WRITE;
    if (fwrite(&w->w_R, sizeof(float), 1, fp) != 1) return SPAI_ERR_WRITE;
    if (fwrite(&w->w_G, sizeof(float), 1, fp) != 1) return SPAI_ERR_WRITE;
    if (fwrite(&w->w_B, sizeof(float), 1, fp) != 1) return SPAI_ERR_WRITE;
    return SPAI_OK;
}

static SpaiStatus read_weights_body(FILE* fp, ChannelWeight* w) {
    if (fread(&w->w_A, sizeof(float), 1, fp) != 1) return SPAI_ERR_READ;
    if (fread(&w->w_R, sizeof(float), 1, fp) != 1) return SPAI_ERR_READ;
    if (fread(&w->w_G, sizeof(float), 1, fp) != 1) return SPAI_ERR_READ;
    if (fread(&w->w_B, sizeof(float), 1, fp) != 1) return SPAI_ERR_READ;
    return SPAI_OK;
}

/* ── CE snapshot record: tag 0x06 ──
 * Layout (all little-endian native):
 *   uint32 kf_id           owning keyframe
 *   uint32 width           = IMG_CE_SIZE (sanity)
 *   uint32 height          = IMG_CE_SIZE (sanity)
 *   cell_bytes[9 * IMG_CE_TOTAL]
 *      per-cell packed tuple:
 *        core, link, delta, priority,
 *        tone_class, semantic_role, direction_class,
 *        depth_class, delta_sign
 *      last_delta_id is NOT serialized — it's a runtime resume
 *      pointer and is reset to IMG_DELTA_ID_NONE on load.
 */
static SpaiStatus write_ce_snapshot_record(FILE* fp,
                                           uint32_t kf_id,
                                           const ImgCEGrid* ce) {
    uint8_t tag = SPAI_TAG_CE_SNAPSHOT;
    if (fwrite(&tag, 1, 1, fp) != 1) return SPAI_ERR_WRITE;

    uint32_t w = ce->width;
    uint32_t h = ce->height;
    if (fwrite(&kf_id, 4, 1, fp) != 1) return SPAI_ERR_WRITE;
    if (fwrite(&w,     4, 1, fp) != 1) return SPAI_ERR_WRITE;
    if (fwrite(&h,     4, 1, fp) != 1) return SPAI_ERR_WRITE;

    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) {
        const ImgCECell* c = &ce->cells[i];
        uint8_t buf[9] = {
            c->core, c->link, c->delta, c->priority,
            c->tone_class, c->semantic_role,
            c->direction_class, c->depth_class, c->delta_sign
        };
        if (fwrite(buf, 1, 9, fp) != 9) return SPAI_ERR_WRITE;
    }
    return SPAI_OK;
}

static SpaiStatus read_ce_snapshot_body(FILE* fp,
                                        uint32_t* out_kf_id,
                                        ImgCEGrid* out_ce) {
    uint32_t kf_id, w, h;
    if (fread(&kf_id, 4, 1, fp) != 1) return SPAI_ERR_READ;
    if (fread(&w,     4, 1, fp) != 1) return SPAI_ERR_READ;
    if (fread(&h,     4, 1, fp) != 1) return SPAI_ERR_READ;
    if (w != IMG_CE_SIZE || h != IMG_CE_SIZE) return SPAI_ERR_CORRUPT;

    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) {
        uint8_t buf[9];
        if (fread(buf, 1, 9, fp) != 9) return SPAI_ERR_READ;
        ImgCECell* c = &out_ce->cells[i];
        c->core            = buf[0];
        c->link            = buf[1];
        c->delta           = buf[2];
        c->priority        = buf[3];
        c->tone_class      = buf[4];
        c->semantic_role   = buf[5];
        c->direction_class = buf[6];
        c->depth_class     = buf[7];
        c->delta_sign      = buf[8];
        c->last_delta_id   = IMG_DELTA_ID_NONE;
    }

    if (out_kf_id) *out_kf_id = kf_id;
    return SPAI_OK;
}

/* ── Canvas record: tag 0x04 ──
 * Layout (all little-endian native):
 *   uint32 slot_count
 *   uint32 canvas_type
 *   SlotMeta meta[CV_SLOTS]   (24 bytes each: 4+4+4+4+4 padded)
 *   uint16 A[CV_TOTAL]
 *   uint8  R[CV_TOTAL]
 *   uint8  G[CV_TOTAL]
 *   uint8  B[CV_TOTAL]
 * Canvas width / height are fixed by CV_WIDTH / CV_HEIGHT. */
static SpaiStatus write_canvas_record(FILE* fp, const SpatialCanvas* c) {
    uint8_t tag = SPAI_TAG_CANVAS;
    if (fwrite(&tag, 1, 1, fp) != 1) return SPAI_ERR_WRITE;

    uint32_t stype = (uint32_t)c->canvas_type;
    if (fwrite(&c->slot_count, sizeof(uint32_t), 1, fp) != 1) return SPAI_ERR_WRITE;
    if (fwrite(&stype,         sizeof(uint32_t), 1, fp) != 1) return SPAI_ERR_WRITE;

    /* I/P metadata (v3.1) */
    uint32_t ft  = (uint32_t)c->frame_type;
    uint32_t pid = c->parent_canvas_id;
    float    cr  = c->changed_ratio;
    uint32_t cls = (uint32_t)c->classified;
    if (fwrite(&ft,  4, 1, fp) != 1) return SPAI_ERR_WRITE;
    if (fwrite(&pid, 4, 1, fp) != 1) return SPAI_ERR_WRITE;
    if (fwrite(&cr,  4, 1, fp) != 1) return SPAI_ERR_WRITE;
    if (fwrite(&cls, 4, 1, fp) != 1) return SPAI_ERR_WRITE;

    /* Serialize SlotMeta as explicit fields to avoid struct padding issues */
    for (uint32_t s = 0; s < CV_SLOTS; s++) {
        uint32_t t = (uint32_t)c->meta[s].type;
        float    bw = c->meta[s].boundary_weight;
        uint32_t bl = c->meta[s].byte_length;
        uint32_t th = c->meta[s].topic_hash;
        uint32_t oc = (uint32_t)c->meta[s].occupied;
        if (fwrite(&t,  4, 1, fp) != 1) return SPAI_ERR_WRITE;
        if (fwrite(&bw, 4, 1, fp) != 1) return SPAI_ERR_WRITE;
        if (fwrite(&bl, 4, 1, fp) != 1) return SPAI_ERR_WRITE;
        if (fwrite(&th, 4, 1, fp) != 1) return SPAI_ERR_WRITE;
        if (fwrite(&oc, 4, 1, fp) != 1) return SPAI_ERR_WRITE;
    }
    if (fwrite(c->A, sizeof(uint16_t), CV_TOTAL, fp) != CV_TOTAL) return SPAI_ERR_WRITE;
    if (fwrite(c->R, 1, CV_TOTAL, fp) != CV_TOTAL) return SPAI_ERR_WRITE;
    if (fwrite(c->G, 1, CV_TOTAL, fp) != CV_TOTAL) return SPAI_ERR_WRITE;
    if (fwrite(c->B, 1, CV_TOTAL, fp) != CV_TOTAL) return SPAI_ERR_WRITE;
    return SPAI_OK;
}

static SpaiStatus read_canvas_body(FILE* fp, SpatialCanvas* c) {
    uint32_t stype = 0;
    if (fread(&c->slot_count, sizeof(uint32_t), 1, fp) != 1) return SPAI_ERR_READ;
    if (fread(&stype,         sizeof(uint32_t), 1, fp) != 1) return SPAI_ERR_READ;
    c->canvas_type = (DataType)stype;

    /* I/P metadata (v3.1) */
    uint32_t ft = 0, pid = UINT32_MAX, cls = 0;
    float    cr = 0.0f;
    if (fread(&ft,  4, 1, fp) != 1) return SPAI_ERR_READ;
    if (fread(&pid, 4, 1, fp) != 1) return SPAI_ERR_READ;
    if (fread(&cr,  4, 1, fp) != 1) return SPAI_ERR_READ;
    if (fread(&cls, 4, 1, fp) != 1) return SPAI_ERR_READ;
    c->frame_type = (CanvasFrameType)ft;
    c->parent_canvas_id = pid;
    c->changed_ratio = cr;
    c->classified = (int)cls;

    for (uint32_t s = 0; s < CV_SLOTS; s++) {
        uint32_t t = 0, bl = 0, th = 0, oc = 0;
        float    bw = 1.0f;
        if (fread(&t,  4, 1, fp) != 1) return SPAI_ERR_READ;
        if (fread(&bw, 4, 1, fp) != 1) return SPAI_ERR_READ;
        if (fread(&bl, 4, 1, fp) != 1) return SPAI_ERR_READ;
        if (fread(&th, 4, 1, fp) != 1) return SPAI_ERR_READ;
        if (fread(&oc, 4, 1, fp) != 1) return SPAI_ERR_READ;
        c->meta[s].type = (DataType)t;
        c->meta[s].boundary_weight = bw;
        c->meta[s].byte_length = bl;
        c->meta[s].topic_hash  = th;
        c->meta[s].occupied    = (int)oc;
    }
    if (fread(c->A, sizeof(uint16_t), CV_TOTAL, fp) != CV_TOTAL) return SPAI_ERR_READ;
    if (fread(c->R, 1, CV_TOTAL, fp) != CV_TOTAL) return SPAI_ERR_READ;
    if (fread(c->G, 1, CV_TOTAL, fp) != CV_TOTAL) return SPAI_ERR_READ;
    if (fread(c->B, 1, CV_TOTAL, fp) != CV_TOTAL) return SPAI_ERR_READ;
    return SPAI_OK;
}

/* ── Subtitle record: tag 0x05 ──
 * Layout:
 *   uint32 count
 *   for each: type(u32) topic_hash(u32) canvas_id(u32) slot_id(u32) byte_length(u32) */
static SpaiStatus write_subtitle_record(FILE* fp, const SubtitleTrack* t) {
    uint8_t tag = SPAI_TAG_SUBTITLE;
    if (fwrite(&tag, 1, 1, fp) != 1) return SPAI_ERR_WRITE;
    if (fwrite(&t->count, sizeof(uint32_t), 1, fp) != 1) return SPAI_ERR_WRITE;
    for (uint32_t i = 0; i < t->count; i++) {
        const SubtitleEntry* e = &t->entries[i];
        uint32_t type = (uint32_t)e->type;
        if (fwrite(&type,           4, 1, fp) != 1) return SPAI_ERR_WRITE;
        if (fwrite(&e->topic_hash,  4, 1, fp) != 1) return SPAI_ERR_WRITE;
        if (fwrite(&e->canvas_id,   4, 1, fp) != 1) return SPAI_ERR_WRITE;
        if (fwrite(&e->slot_id,     4, 1, fp) != 1) return SPAI_ERR_WRITE;
        if (fwrite(&e->byte_length, 4, 1, fp) != 1) return SPAI_ERR_WRITE;
    }
    return SPAI_OK;
}

static SpaiStatus read_subtitle_body(FILE* fp, SubtitleTrack* t) {
    uint32_t count = 0;
    if (fread(&count, sizeof(uint32_t), 1, fp) != 1) return SPAI_ERR_READ;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t type = 0, topic = 0, cv = 0, sl = 0, bl = 0;
        if (fread(&type,  4, 1, fp) != 1) return SPAI_ERR_READ;
        if (fread(&topic, 4, 1, fp) != 1) return SPAI_ERR_READ;
        if (fread(&cv,    4, 1, fp) != 1) return SPAI_ERR_READ;
        if (fread(&sl,    4, 1, fp) != 1) return SPAI_ERR_READ;
        if (fread(&bl,    4, 1, fp) != 1) return SPAI_ERR_READ;
        subtitle_track_add(t, (DataType)type, topic, cv, sl, bl);
    }
    return SPAI_OK;
}

SpaiStatus ai_save(const SpatialAI* ai, const char* path) {
    if (!ai || !path) return SPAI_ERR_OPEN;

    FILE* fp = fopen(path, "wb");
    if (!fp) return SPAI_ERR_OPEN;

    SpaiStatus s = write_header(fp, ai->kf_count, ai->df_count);
    if (s != SPAI_OK) { fclose(fp); return s; }

    /* Write all keyframes, then all deltas. Order within each type
       follows the in-memory array, which matches insertion order. */
    for (uint32_t i = 0; i < ai->kf_count; i++) {
        s = write_keyframe_record(fp, &ai->keyframes[i]);
        if (s != SPAI_OK) { fclose(fp); return s; }
    }
    for (uint32_t i = 0; i < ai->df_count; i++) {
        s = write_delta_record(fp, &ai->deltas[i]);
        if (s != SPAI_OK) { fclose(fp); return s; }
    }

    /* v2: adaptive channel weights as trailing record */
    s = write_weights_record(fp, &ai->global_weights);
    if (s != SPAI_OK) { fclose(fp); return s; }

    /* v3: canvases (if any) then subtitle track */
    if (ai->canvas_pool) {
        for (uint32_t i = 0; i < ai->canvas_pool->count; i++) {
            SpatialCanvas* c = ai->canvas_pool->canvases[i];
            if (!c) continue;
            s = write_canvas_record(fp, c);
            if (s != SPAI_OK) { fclose(fp); return s; }
        }
        s = write_subtitle_record(fp, &ai->canvas_pool->track);
        if (s != SPAI_OK) { fclose(fp); return s; }
    }

    /* Bimodal: image-side CE snapshots bound to keyframes. Trailing
     * records — older readers stop at unknown tag cleanly. */
    for (uint32_t i = 0; i < ai->kf_count; i++) {
        const Keyframe* kf = &ai->keyframes[i];
        if (!kf->ce_snapshot) continue;
        s = write_ce_snapshot_record(fp, kf->id, kf->ce_snapshot);
        if (s != SPAI_OK) { fclose(fp); return s; }
    }

    if (fclose(fp) != 0) return SPAI_ERR_WRITE;
    return SPAI_OK;
}

/* ── Public: load ── */

SpatialAI* ai_load(const char* path, SpaiStatus* out_status) {
    if (out_status) *out_status = SPAI_OK;
    if (!path) { if (out_status) *out_status = SPAI_ERR_OPEN; return NULL; }

    FILE* fp = fopen(path, "rb");
    if (!fp) { if (out_status) *out_status = SPAI_ERR_OPEN; return NULL; }

    SpaiHeader h;
    SpaiStatus s = read_header(fp, &h);
    if (s != SPAI_OK) { fclose(fp); if (out_status) *out_status = s; return NULL; }

    SpatialAI* ai = spatial_ai_create();
    if (!ai) { fclose(fp); if (out_status) *out_status = SPAI_ERR_ALLOC; return NULL; }

    if (!grow_kf_cap(ai, h.kf_count) || !grow_df_cap(ai, h.df_count)) {
        fclose(fp); spatial_ai_destroy(ai);
        if (out_status) *out_status = SPAI_ERR_ALLOC;
        return NULL;
    }

    /* Walk records. Read until both KF and Delta counts are satisfied,
     * then check for optional v2+ trailing records (weights). */
    uint32_t kfs_read = 0, dfs_read = 0;
    while (kfs_read < h.kf_count || dfs_read < h.df_count) {
        uint8_t tag;
        size_t got = fread(&tag, 1, 1, fp);
        if (got != 1) {
            fclose(fp); spatial_ai_destroy(ai);
            if (out_status) *out_status = SPAI_ERR_READ;
            return NULL;
        }

        if (tag == SPAI_TAG_KEYFRAME) {
            if (kfs_read >= h.kf_count) {
                fclose(fp); spatial_ai_destroy(ai);
                if (out_status) *out_status = SPAI_ERR_CORRUPT;
                return NULL;
            }
            s = read_keyframe_body(fp, &ai->keyframes[kfs_read]);
            if (s != SPAI_OK) {
                fclose(fp); spatial_ai_destroy(ai);
                if (out_status) *out_status = s;
                return NULL;
            }
            kfs_read++;
        } else if (tag == SPAI_TAG_DELTA) {
            if (dfs_read >= h.df_count) {
                fclose(fp); spatial_ai_destroy(ai);
                if (out_status) *out_status = SPAI_ERR_CORRUPT;
                return NULL;
            }
            s = read_delta_body(fp, &ai->deltas[dfs_read]);
            if (s != SPAI_OK) {
                fclose(fp); spatial_ai_destroy(ai);
                if (out_status) *out_status = s;
                return NULL;
            }
            dfs_read++;
        } else {
            fclose(fp); spatial_ai_destroy(ai);
            if (out_status) *out_status = SPAI_ERR_CORRUPT;
            return NULL;
        }
    }

    ai->kf_count = kfs_read;
    ai->df_count = dfs_read;

    /* Optional trailing records: weights (v2+) and canvas + subtitle (v3+).
     * Keep reading tagged records until EOF or unknown tag. */
    uint8_t tag;
    while (fread(&tag, 1, 1, fp) == 1) {
        if (tag == SPAI_TAG_WEIGHTS) {
            s = read_weights_body(fp, &ai->global_weights);
            if (s != SPAI_OK) {
                fclose(fp); spatial_ai_destroy(ai);
                if (out_status) *out_status = s;
                return NULL;
            }
            weight_normalize(&ai->global_weights);
        } else if (tag == SPAI_TAG_CANVAS) {
            /* Lazy-create pool on first canvas record */
            SpatialCanvasPool* pool = ai_get_canvas_pool(ai);
            if (!pool) {
                fclose(fp); spatial_ai_destroy(ai);
                if (out_status) *out_status = SPAI_ERR_ALLOC;
                return NULL;
            }
            SpatialCanvas* cv = canvas_create();
            if (!cv) {
                fclose(fp); spatial_ai_destroy(ai);
                if (out_status) *out_status = SPAI_ERR_ALLOC;
                return NULL;
            }
            s = read_canvas_body(fp, cv);
            if (s != SPAI_OK) {
                canvas_destroy(cv);
                fclose(fp); spatial_ai_destroy(ai);
                if (out_status) *out_status = s;
                return NULL;
            }
            /* Append to pool.canvases (reuse realloc logic inline) */
            if (pool->count >= pool->capacity) {
                uint32_t nc = pool->capacity ? pool->capacity * 2 : 4;
                SpatialCanvas** arr = (SpatialCanvas**)realloc(pool->canvases,
                                              nc * sizeof(*pool->canvases));
                if (!arr) {
                    canvas_destroy(cv);
                    fclose(fp); spatial_ai_destroy(ai);
                    if (out_status) *out_status = SPAI_ERR_ALLOC;
                    return NULL;
                }
                pool->canvases = arr;
                pool->capacity = nc;
            }
            pool->canvases[pool->count++] = cv;
        } else if (tag == SPAI_TAG_SUBTITLE) {
            SpatialCanvasPool* pool = ai_get_canvas_pool(ai);
            if (!pool) {
                fclose(fp); spatial_ai_destroy(ai);
                if (out_status) *out_status = SPAI_ERR_ALLOC;
                return NULL;
            }
            /* Fresh-load the track (subtitle_track_destroy + init is
             * a no-op if already clean, but be explicit). */
            subtitle_track_destroy(&pool->track);
            subtitle_track_init(&pool->track);
            s = read_subtitle_body(fp, &pool->track);
            if (s != SPAI_OK) {
                fclose(fp); spatial_ai_destroy(ai);
                if (out_status) *out_status = s;
                return NULL;
            }
        } else if (tag == SPAI_TAG_CE_SNAPSHOT) {
            ImgCEGrid* ce = img_ce_grid_create();
            if (!ce) {
                fclose(fp); spatial_ai_destroy(ai);
                if (out_status) *out_status = SPAI_ERR_ALLOC;
                return NULL;
            }
            uint32_t kf_id = 0;
            s = read_ce_snapshot_body(fp, &kf_id, ce);
            if (s != SPAI_OK) {
                img_ce_grid_destroy(ce);
                fclose(fp); spatial_ai_destroy(ai);
                if (out_status) *out_status = s;
                return NULL;
            }
            /* Bind to the owning keyframe if still in range. If the
             * record references an id that was trimmed out, drop the
             * grid silently — the file is consistent but the KF isn't
             * here anymore. */
            if (kf_id < ai->kf_count) {
                if (ai->keyframes[kf_id].ce_snapshot) {
                    img_ce_grid_destroy(ai->keyframes[kf_id].ce_snapshot);
                }
                ai->keyframes[kf_id].ce_snapshot = ce;
            } else {
                img_ce_grid_destroy(ce);
            }
        } else {
            /* Unknown trailing tag — stop cleanly, forward compatible. */
            break;
        }
    }

    fclose(fp);
    if (out_status) *out_status = SPAI_OK;
    return ai;
}

/* ── Public: incremental save ── */

SpaiStatus ai_save_incremental(const SpatialAI* ai, const char* path) {
    if (!ai || !path) return SPAI_ERR_OPEN;

    /* 1) Probe for an existing valid file. */
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        /* No existing file — fall back to full save. */
        return ai_save(ai, path);
    }

    SpaiHeader h;
    SpaiStatus s = read_header(fp, &h);
    fclose(fp);
    if (s != SPAI_OK) {
        /* Unreadable / wrong magic / wrong version — overwrite with full save. */
        return ai_save(ai, path);
    }

    /* 2) In-memory engine must not have LESS than what the file recorded. */
    if (ai->kf_count < h.kf_count || ai->df_count < h.df_count) {
        return SPAI_ERR_STATE;
    }

    /* 3) Nothing new? no-op. */
    if (ai->kf_count == h.kf_count && ai->df_count == h.df_count) {
        return SPAI_OK;
    }

    /* 4) Full rewrite: incremental save cannot trivially update a
     *    trailing weights block in place (it sits after records).
     *    For simplicity and correctness we rewrite the whole file;
     *    callers with huge KF counts can still call ai_save_incremental
     *    — it's equivalent to ai_save when weights are present. */
    (void)h;  /* unused now */
    return ai_save(ai, path);
}

/* ── Public: peek header ── */

SpaiStatus ai_peek_header(const char* path,
                          uint32_t* out_kf_count,
                          uint32_t* out_df_count,
                          uint32_t* out_version) {
    if (!path) return SPAI_ERR_OPEN;
    FILE* fp = fopen(path, "rb");
    if (!fp) return SPAI_ERR_OPEN;

    SpaiHeader h;
    SpaiStatus s = read_header(fp, &h);
    fclose(fp);
    if (s != SPAI_OK) return s;

    if (out_kf_count) *out_kf_count = h.kf_count;
    if (out_df_count) *out_df_count = h.df_count;
    if (out_version)  *out_version  = h.version;
    return SPAI_OK;
}
