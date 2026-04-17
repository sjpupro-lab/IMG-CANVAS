/*
 * demo_pipeline — run an input image through the full CE pipeline
 * and save both a plain and a mask-overlayed PPM so the result is
 * visually inspectable.
 *
 *   usage: demo_pipeline <input.ppm> [output_prefix]
 *
 *   Reads binary P6 PPM input. To feed PNG/JPEG, convert first:
 *     convert input.png input.ppm    # ImageMagick
 *     ffmpeg -i input.png input.ppm  # ffmpeg
 *
 *   Produces:
 *     <prefix>_plain.ppm   — CE grid rendered with no overlay
 *     <prefix>_masked.ppm  — CE grid + resolve outlier/explained tint
 *                            (cyan = absorbed, red = promoted)
 *
 *   When output_prefix is omitted, "demo_out" is used.
 */

#include "img_pipeline.h"
#include "img_render.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── PPM (P6) loader ─────────────────────────────────────── */

static uint8_t* load_ppm_p6(const char* path,
                            uint32_t* out_w, uint32_t* out_h) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }

    char magic[3] = {0};
    if (fscanf(f, "%2s", magic) != 1 || strcmp(magic, "P6") != 0) {
        fprintf(stderr, "%s: not a P6 PPM\n", path);
        fclose(f); return NULL;
    }

    /* Parse three whitespace-separated integers (w, h, maxval),
     * skipping '#' comment lines. */
    int vals[3];
    int got = 0;
    while (got < 3) {
        int c = fgetc(f);
        if (c == EOF) break;
        if (c == '#') {
            while ((c = fgetc(f)) != EOF && c != '\n') { /* skip */ }
            continue;
        }
        if (isspace(c)) continue;
        ungetc(c, f);
        if (fscanf(f, "%d", &vals[got]) != 1) break;
        got++;
    }
    if (got != 3) {
        fprintf(stderr, "%s: malformed header\n", path);
        fclose(f); return NULL;
    }

    const uint32_t w = (uint32_t)vals[0];
    const uint32_t h = (uint32_t)vals[1];
    const int maxval = vals[2];
    if (w == 0 || h == 0) {
        fprintf(stderr, "%s: zero dimension\n", path);
        fclose(f); return NULL;
    }
    if (maxval != 255) {
        fprintf(stderr, "%s: maxval=%d (only 255 supported)\n", path, maxval);
        fclose(f); return NULL;
    }

    /* Exactly one whitespace separator after maxval, per PPM spec. */
    fgetc(f);

    const size_t n = (size_t)w * h * 3u;
    uint8_t* buf = (uint8_t*)malloc(n);
    if (!buf) { fclose(f); return NULL; }
    const size_t got_bytes = fread(buf, 1, n, f);
    fclose(f);
    if (got_bytes != n) {
        fprintf(stderr, "%s: short read (%zu / %zu bytes)\n",
                path, got_bytes, n);
        free(buf); return NULL;
    }

    *out_w = w;
    *out_h = h;
    return buf;
}

/* ── main ────────────────────────────────────────────────── */

static void print_usage(const char* prog) {
    fprintf(stderr,
        "usage: %s [--adapt] <input.ppm> [output_prefix]\n"
        "\n"
        "   --adapt, -a   Run img_render_options_adapt_to_ce — per-channel\n"
        "                 tier thresholds are re-derived from this image's\n"
        "                 CE histogram before rendering.\n"
        "\n"
        "   Reads binary P6 PPM. Convert other formats externally:\n"
        "     convert input.png input.ppm   (ImageMagick)\n"
        "     ffmpeg -i input.png input.ppm\n"
        "\n"
        "   Outputs:\n"
        "     <prefix>_plain.ppm   plain CE render\n"
        "     <prefix>_masked.ppm  CE render + resolve mask overlay\n",
        prog);
}

int main(int argc, char** argv) {
    int adapt = 0;
    int argi = 1;
    while (argi < argc && argv[argi][0] == '-') {
        if (strcmp(argv[argi], "--adapt") == 0 ||
            strcmp(argv[argi], "-a")      == 0) {
            adapt = 1;
            argi++;
        } else if (strcmp(argv[argi], "--help") == 0 ||
                   strcmp(argv[argi], "-h")     == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown flag: %s\n", argv[argi]);
            print_usage(argv[0]);
            return 2;
        }
    }
    if (argi >= argc) { print_usage(argv[0]); return 2; }

    const char* input  = argv[argi];
    const char* prefix = (argi + 1 < argc) ? argv[argi + 1] : "demo_out";

    uint32_t w = 0, h = 0;
    uint8_t* img = load_ppm_p6(input, &w, &h);
    if (!img) return 1;

    ImgPipelineResult r = {0};
    if (!img_pipeline_run(img, w, h, /*memory=*/NULL, /*opt=*/NULL, &r)) {
        fprintf(stderr, "pipeline failed\n");
        free(img); return 1;
    }

    printf("=== demo_pipeline ===\n");
    printf("  input:              %s (%u x %u)\n", input, w, h);
    printf("  seed_count:         %u\n", r.stats.seed_count);
    printf("  expansions:         %u\n", r.stats.expansions);
    printf("  visited:            %u\n", r.stats.visited);
    printf("  resolve_outliers:   %u\n", r.stats.resolve_outliers);
    printf("  resolve_explained:  %u\n", r.stats.resolve_explained);
    printf("  resolve_promoted:   %u\n", r.stats.resolve_promoted);

    char path[1024];
    int ok_plain = 0, ok_masked = 0;

    ImgRenderOptions ropt = img_render_default_options();
    if (adapt) {
        img_render_options_adapt_to_ce(&ropt, r.ce_grid);
        printf("  adapted tier.core:    {%u, %u, %u}\n",
               ropt.tier_core.t1_max, ropt.tier_core.t2_max,
               ropt.tier_core.t3_max);
        printf("  adapted tier.link:    {%u, %u, %u}\n",
               ropt.tier_link.t1_max, ropt.tier_link.t2_max,
               ropt.tier_link.t3_max);
        printf("  adapted tier.delta:   {%u, %u, %u}\n",
               ropt.tier_delta.t1_max, ropt.tier_delta.t2_max,
               ropt.tier_delta.t3_max);
        printf("  adapted tier.priority:{%u, %u, %u}\n",
               ropt.tier_priority.t1_max, ropt.tier_priority.t2_max,
               ropt.tier_priority.t3_max);
    }

    ImgRenderImage plain = {0};
    if (img_render_ce_grid(r.ce_grid, &ropt, &plain)) {
        snprintf(path, sizeof(path), "%s_plain.ppm", prefix);
        if (img_render_save_ppm(path, &plain)) {
            printf("  wrote %s  (%u x %u)\n", path, plain.width, plain.height);
            ok_plain = 1;
        } else {
            fprintf(stderr, "failed to write %s\n", path);
        }
        img_render_free_image(&plain);
    }

    ImgRenderImage masked = {0};
    ImgRenderMasks masks = { r.outlier_mask, r.explained_mask };
    if (img_render_ce_grid_masked(r.ce_grid, &ropt, &masks, &masked)) {
        snprintf(path, sizeof(path), "%s_masked.ppm", prefix);
        if (img_render_save_ppm(path, &masked)) {
            printf("  wrote %s (%u x %u)\n", path, masked.width, masked.height);
            ok_masked = 1;
        } else {
            fprintf(stderr, "failed to write %s\n", path);
        }
        img_render_free_image(&masked);
    }

    img_pipeline_result_destroy(&r);
    free(img);
    return (ok_plain && ok_masked) ? 0 : 1;
}
