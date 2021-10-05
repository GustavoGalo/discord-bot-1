/*
 * Copyright (c) 2013 Paul B Mahol
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "avfilter.h"
#include "drawutils.h"
#include "formats.h"
#include "internal.h"
#include "video.h"
#include "preserve_color.h"

#define R 0
#define G 1
#define B 2
#define A 3

typedef struct Range {
    double in_min, in_max;
    double out_min, out_max;
} Range;

typedef struct ColorLevelsContext {
    const AVClass *class;
    Range range[4];
    int preserve_color;

    int nb_comp;
    int bpp;
    int step;
    uint8_t rgba_map[4];
    int linesize;

    int (*colorlevels_slice[2])(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs);
} ColorLevelsContext;

#define OFFSET(x) offsetof(ColorLevelsContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_RUNTIME_PARAM
static const AVOption colorlevels_options[] = {
    { "rimin", "set input red black point",    OFFSET(range[R].in_min),  AV_OPT_TYPE_DOUBLE, {.dbl=0}, -1, 1, FLAGS },
    { "gimin", "set input green black point",  OFFSET(range[G].in_min),  AV_OPT_TYPE_DOUBLE, {.dbl=0}, -1, 1, FLAGS },
    { "bimin", "set input blue black point",   OFFSET(range[B].in_min),  AV_OPT_TYPE_DOUBLE, {.dbl=0}, -1, 1, FLAGS },
    { "aimin", "set input alpha black point",  OFFSET(range[A].in_min),  AV_OPT_TYPE_DOUBLE, {.dbl=0}, -1, 1, FLAGS },
    { "rimax", "set input red white point",    OFFSET(range[R].in_max),  AV_OPT_TYPE_DOUBLE, {.dbl=1}, -1, 1, FLAGS },
    { "gimax", "set input green white point",  OFFSET(range[G].in_max),  AV_OPT_TYPE_DOUBLE, {.dbl=1}, -1, 1, FLAGS },
    { "bimax", "set input blue white point",   OFFSET(range[B].in_max),  AV_OPT_TYPE_DOUBLE, {.dbl=1}, -1, 1, FLAGS },
    { "aimax", "set input alpha white point",  OFFSET(range[A].in_max),  AV_OPT_TYPE_DOUBLE, {.dbl=1}, -1, 1, FLAGS },
    { "romin", "set output red black point",   OFFSET(range[R].out_min), AV_OPT_TYPE_DOUBLE, {.dbl=0},  0, 1, FLAGS },
    { "gomin", "set output green black point", OFFSET(range[G].out_min), AV_OPT_TYPE_DOUBLE, {.dbl=0},  0, 1, FLAGS },
    { "bomin", "set output blue black point",  OFFSET(range[B].out_min), AV_OPT_TYPE_DOUBLE, {.dbl=0},  0, 1, FLAGS },
    { "aomin", "set output alpha black point", OFFSET(range[A].out_min), AV_OPT_TYPE_DOUBLE, {.dbl=0},  0, 1, FLAGS },
    { "romax", "set output red white point",   OFFSET(range[R].out_max), AV_OPT_TYPE_DOUBLE, {.dbl=1},  0, 1, FLAGS },
    { "gomax", "set output green white point", OFFSET(range[G].out_max), AV_OPT_TYPE_DOUBLE, {.dbl=1},  0, 1, FLAGS },
    { "bomax", "set output blue white point",  OFFSET(range[B].out_max), AV_OPT_TYPE_DOUBLE, {.dbl=1},  0, 1, FLAGS },
    { "aomax", "set output alpha white point", OFFSET(range[A].out_max), AV_OPT_TYPE_DOUBLE, {.dbl=1},  0, 1, FLAGS },
    { "preserve", "set preserve color mode",   OFFSET(preserve_color),   AV_OPT_TYPE_INT,    {.i64=0},  0, NB_PRESERVE-1, FLAGS, "preserve" },
    { "none",  "disabled",                     0,                        AV_OPT_TYPE_CONST,  {.i64=P_NONE}, 0, 0, FLAGS, "preserve" },
    { "lum",   "luminance",                    0,                        AV_OPT_TYPE_CONST,  {.i64=P_LUM},  0, 0, FLAGS, "preserve" },
    { "max",   "max",                          0,                        AV_OPT_TYPE_CONST,  {.i64=P_MAX},  0, 0, FLAGS, "preserve" },
    { "avg",   "average",                      0,                        AV_OPT_TYPE_CONST,  {.i64=P_AVG},  0, 0, FLAGS, "preserve" },
    { "sum",   "sum",                          0,                        AV_OPT_TYPE_CONST,  {.i64=P_SUM},  0, 0, FLAGS, "preserve" },
    { "nrm",   "norm",                         0,                        AV_OPT_TYPE_CONST,  {.i64=P_NRM},  0, 0, FLAGS, "preserve" },
    { "pwr",   "power",                        0,                        AV_OPT_TYPE_CONST,  {.i64=P_PWR},  0, 0, FLAGS, "preserve" },
    { NULL }
};

AVFILTER_DEFINE_CLASS(colorlevels);

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_0RGB,  AV_PIX_FMT_0BGR,
        AV_PIX_FMT_ARGB,  AV_PIX_FMT_ABGR,
        AV_PIX_FMT_RGB0,  AV_PIX_FMT_BGR0,
        AV_PIX_FMT_RGB24, AV_PIX_FMT_BGR24,
        AV_PIX_FMT_RGB48, AV_PIX_FMT_BGR48,
        AV_PIX_FMT_RGBA64, AV_PIX_FMT_BGRA64,
        AV_PIX_FMT_RGBA,  AV_PIX_FMT_BGRA,
        AV_PIX_FMT_NONE
    };

    return ff_set_common_formats_from_list(ctx, pix_fmts);
}

typedef struct ThreadData {
    const uint8_t *srcrow;
    uint8_t *dstrow;
    int dst_linesize;
    int src_linesize;

    float coeff[4];

    int h;

    int imin[4];
    int omin[4];
} ThreadData;

#define DO_COMMON(type, clip, preserve)                                         \
    ColorLevelsContext *s = ctx->priv;                                          \
    const ThreadData *td = arg;                                                 \
    const int linesize = s->linesize;                                           \
    const int step = s->step;                                                   \
    const int process_h = td->h;                                                \
    const int slice_start = (process_h *  jobnr   ) / nb_jobs;                  \
    const int slice_end   = (process_h * (jobnr+1)) / nb_jobs;                  \
    const int src_linesize = td->src_linesize / sizeof(type);                   \
    const int dst_linesize = td->dst_linesize / sizeof(type);                   \
    const type *srcrow = (const type *)td->srcrow + src_linesize * slice_start; \
    type *dstrow = (type *)td->dstrow + dst_linesize * slice_start;             \
    const uint8_t offset_r = s->rgba_map[R];                                    \
    const uint8_t offset_g = s->rgba_map[G];                                    \
    const uint8_t offset_b = s->rgba_map[B];                                    \
    const uint8_t offset_a = s->rgba_map[A];                                    \
    const int imin_r = td->imin[R];                                             \
    const int imin_g = td->imin[G];                                             \
    const int imin_b = td->imin[B];                                             \
    const int imin_a = td->imin[A];                                             \
    const int omin_r = td->omin[R];                                             \
    const int omin_g = td->omin[G];                                             \
    const int omin_b = td->omin[B];                                             \
    const int omin_a = td->omin[A];                                             \
    const float coeff_r = td->coeff[R];                                         \
    const float coeff_g = td->coeff[G];                                         \
    const float coeff_b = td->coeff[B];                                         \
    const float coeff_a = td->coeff[A];                                         \
    const type *src_r = srcrow + offset_r;                                      \
    const type *src_g = srcrow + offset_g;                                      \
    const type *src_b = srcrow + offset_b;                                      \
    const type *src_a = srcrow + offset_a;                                      \
    type *dst_r = dstrow + offset_r;                                            \
    type *dst_g = dstrow + offset_g;                                            \
    type *dst_b = dstrow + offset_b;                                            \
    type *dst_a = dstrow + offset_a;                                            \
                                                                                \
    for (int y = slice_start; y < slice_end; y++) {                             \
        for (int x = 0; x < linesize; x += step) {                              \
            int ir, ig, ib, or, og, ob;                                         \
            ir = src_r[x];                                                      \
            ig = src_g[x];                                                      \
            ib = src_b[x];                                                      \
            if (preserve) {                                                     \
                float ratio, icolor, ocolor, max = (1<<(8*sizeof(type)))-1;     \
                                                                                \
                or = (ir - imin_r) * coeff_r + omin_r;                          \
                og = (ig - imin_g) * coeff_g + omin_g;                          \
                ob = (ib - imin_b) * coeff_b + omin_b;                          \
                                                                                \
                preserve_color(s->preserve_color, ir, ig, ib, or, og, ob, max,  \
                              &icolor, &ocolor);                                \
                if (ocolor > 0.f) {                                             \
                    ratio = icolor / ocolor;                                    \
                                                                                \
                    or *= ratio;                                                \
                    og *= ratio;                                                \
                    ob *= ratio;                                                \
                }                                                               \
                                                                                \
                dst_r[x] = clip(or);                                            \
                dst_g[x] = clip(og);                                            \
                dst_b[x] = clip(ob);                                            \
            } else {                                                            \
                dst_r[x] = clip((ir - imin_r) * coeff_r + omin_r);              \
                dst_g[x] = clip((ig - imin_g) * coeff_g + omin_g);              \
                dst_b[x] = clip((ib - imin_b) * coeff_b + omin_b);              \
            }                                                                   \
        }                                                                       \
                                                                                \
        for (int x = 0; x < linesize && s->nb_comp == 4; x += step)             \
            dst_a[x] = clip((src_a[x] - imin_a) * coeff_a + omin_a);            \
                                                                                \
        src_r += src_linesize;                                                  \
        src_g += src_linesize;                                                  \
        src_b += src_linesize;                                                  \
        src_a += src_linesize;                                                  \
                                                                                \
        dst_r += dst_linesize;                                                  \
        dst_g += dst_linesize;                                                  \
        dst_b += dst_linesize;                                                  \
        dst_a += dst_linesize;                                                  \
    }

static int colorlevels_slice_8(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    DO_COMMON(uint8_t, av_clip_uint8, 0)

    return 0;
}

static int colorlevels_slice_16(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    DO_COMMON(uint16_t, av_clip_uint16, 0)

    return 0;
}

static int colorlevels_preserve_slice_8(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    DO_COMMON(uint8_t, av_clip_uint8, 1)

    return 0;
}

static int colorlevels_preserve_slice_16(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    DO_COMMON(uint16_t, av_clip_uint16, 1)

    return 0;
}

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    ColorLevelsContext *s = ctx->priv;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);

    s->nb_comp = desc->nb_components;
    s->bpp = desc->comp[0].depth >> 3;
    s->step = av_get_padded_bits_per_pixel(desc) >> (3 + (s->bpp == 2));
    s->linesize = inlink->w * s->step;
    ff_fill_rgba_map(s->rgba_map, inlink->format);

    s->colorlevels_slice[0] = colorlevels_slice_8;
    s->colorlevels_slice[1] = colorlevels_preserve_slice_8;
    if (s->bpp == 2) {
        s->colorlevels_slice[0] = colorlevels_slice_16;
        s->colorlevels_slice[1] = colorlevels_preserve_slice_16;
    }

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    ColorLevelsContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    const int step = s->step;
    ThreadData td;
    AVFrame *out;

    if (av_frame_is_writable(in)) {
        out = in;
    } else {
        out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
        if (!out) {
            av_frame_free(&in);
            return AVERROR(ENOMEM);
        }
        av_frame_copy_props(out, in);
    }

    td.h             = inlink->h;
    td.dst_linesize  = out->linesize[0];
    td.src_linesize  = in->linesize[0];
    td.srcrow        = in->data[0];
    td.dstrow        = out->data[0];

    switch (s->bpp) {
    case 1:
        for (int i = 0; i < s->nb_comp; i++) {
            Range *r = &s->range[i];
            const uint8_t offset = s->rgba_map[i];
            const uint8_t *srcrow = in->data[0];
            int imin = lrint(r->in_min  * UINT8_MAX);
            int imax = lrint(r->in_max  * UINT8_MAX);
            int omin = lrint(r->out_min * UINT8_MAX);
            int omax = lrint(r->out_max * UINT8_MAX);
            float coeff;

            if (imin < 0) {
                imin = UINT8_MAX;
                for (int y = 0; y < inlink->h; y++) {
                    const uint8_t *src = srcrow;

                    for (int x = 0; x < s->linesize; x += step)
                        imin = FFMIN(imin, src[x + offset]);
                    srcrow += in->linesize[0];
                }
            }
            if (imax < 0) {
                srcrow = in->data[0];
                imax = 0;
                for (int y = 0; y < inlink->h; y++) {
                    const uint8_t *src = srcrow;

                    for (int x = 0; x < s->linesize; x += step)
                        imax = FFMAX(imax, src[x + offset]);
                    srcrow += in->linesize[0];
                }
            }

            coeff = (omax - omin) / (double)(imax - imin);

            td.coeff[i] = coeff;
            td.imin[i]  = imin;
            td.omin[i]  = omin;
        }
        break;
    case 2:
        for (int i = 0; i < s->nb_comp; i++) {
            Range *r = &s->range[i];
            const uint8_t offset = s->rgba_map[i];
            const uint8_t *srcrow = in->data[0];
            int imin = lrint(r->in_min  * UINT16_MAX);
            int imax = lrint(r->in_max  * UINT16_MAX);
            int omin = lrint(r->out_min * UINT16_MAX);
            int omax = lrint(r->out_max * UINT16_MAX);
            float coeff;

            if (imin < 0) {
                imin = UINT16_MAX;
                for (int y = 0; y < inlink->h; y++) {
                    const uint16_t *src = (const uint16_t *)srcrow;

                    for (int x = 0; x < s->linesize; x += step)
                        imin = FFMIN(imin, src[x + offset]);
                    srcrow += in->linesize[0];
                }
            }
            if (imax < 0) {
                srcrow = in->data[0];
                imax = 0;
                for (int y = 0; y < inlink->h; y++) {
                    const uint16_t *src = (const uint16_t *)srcrow;

                    for (int x = 0; x < s->linesize; x += step)
                        imax = FFMAX(imax, src[x + offset]);
                    srcrow += in->linesize[0];
                }
            }

            coeff = (omax - omin) / (double)(imax - imin);

            td.coeff[i] = coeff;
            td.imin[i]  = imin;
            td.omin[i]  = omin;
        }
        break;
    }

    ff_filter_execute(ctx, s->colorlevels_slice[s->preserve_color > 0], &td, NULL,
                      FFMIN(inlink->h, ff_filter_get_nb_threads(ctx)));

    if (in != out)
        av_frame_free(&in);
    return ff_filter_frame(outlink, out);
}

static const AVFilterPad colorlevels_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
        .config_props = config_input,
    },
};

static const AVFilterPad colorlevels_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
};

const AVFilter ff_vf_colorlevels = {
    .name          = "colorlevels",
    .description   = NULL_IF_CONFIG_SMALL("Adjust the color levels."),
    .priv_size     = sizeof(ColorLevelsContext),
    .priv_class    = &colorlevels_class,
    .query_formats = query_formats,
    FILTER_INPUTS(colorlevels_inputs),
    FILTER_OUTPUTS(colorlevels_outputs),
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC | AVFILTER_FLAG_SLICE_THREADS,
    .process_command = ff_filter_process_command,
};