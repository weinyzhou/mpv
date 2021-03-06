/*
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sys/types.h>

#include <libswscale/swscale.h>

#include "config.h"
#include "common/msg.h"
#include "options/options.h"

#include "video/img_format.h"
#include "video/mp_image.h"
#include "vf.h"
#include "video/fmt-conversion.h"

#include "video/sws_utils.h"

#include "video/csputils.h"
#include "video/out/vo.h"

#include "options/m_option.h"

static struct vf_priv_s {
    int w, h;
    int cfg_w, cfg_h;
    int v_chr_drop;
    double param[2];
    struct mp_sws_context *sws;
    int noup;
    int accurate_rnd;
} const vf_priv_dflt = {
    0, 0,
    -1, -1,
    0,
    {SWS_PARAM_DEFAULT, SWS_PARAM_DEFAULT},
};

//===========================================================================//

static const unsigned int outfmt_list[] = {
// YUV:
    IMGFMT_444P,
    IMGFMT_444P16,
    IMGFMT_444P14,
    IMGFMT_444P12,
    IMGFMT_444P10,
    IMGFMT_444P9,
    IMGFMT_422P,
    IMGFMT_422P16,
    IMGFMT_422P14,
    IMGFMT_422P12,
    IMGFMT_422P10,
    IMGFMT_422P9,
    IMGFMT_420P,
    IMGFMT_420P16,
    IMGFMT_420P14,
    IMGFMT_420P12,
    IMGFMT_420P10,
    IMGFMT_420P9,
    IMGFMT_420AP,
    IMGFMT_410P,
    IMGFMT_411P,
    IMGFMT_NV12,
    IMGFMT_NV21,
    IMGFMT_YUYV,
    IMGFMT_UYVY,
    IMGFMT_440P,
// RGB and grayscale (Y8 and Y800):
    IMGFMT_BGR32,
    IMGFMT_RGB32,
    IMGFMT_ABGR,
    IMGFMT_ARGB,
    IMGFMT_BGRA,
    IMGFMT_RGBA,
    IMGFMT_BGR24,
    IMGFMT_RGB24,
    IMGFMT_GBRP,
    IMGFMT_RGB48,
    IMGFMT_BGR565,
    IMGFMT_RGB565,
    IMGFMT_BGR555,
    IMGFMT_RGB555,
    IMGFMT_BGR444,
    IMGFMT_RGB444,
    IMGFMT_Y8,
    IMGFMT_BGR8,
    IMGFMT_RGB8,
    IMGFMT_BGR4,
    IMGFMT_RGB4,
    IMGFMT_RGB4_BYTE,
    IMGFMT_BGR4_BYTE,
    IMGFMT_MONO,
    IMGFMT_MONO_W,
    0
};

/**
 * A list of preferred conversions, in order of preference.
 * This should be used for conversions that e.g. involve no scaling
 * or to stop vf_scale from choosing a conversion that has no
 * fast assembler implementation.
 */
static const int preferred_conversions[][2] = {
    {IMGFMT_YUYV, IMGFMT_UYVY},
    {IMGFMT_YUYV, IMGFMT_422P},
    {IMGFMT_UYVY, IMGFMT_YUYV},
    {IMGFMT_UYVY, IMGFMT_422P},
    {IMGFMT_422P, IMGFMT_YUYV},
    {IMGFMT_422P, IMGFMT_UYVY},
    {IMGFMT_420P10, IMGFMT_420P},
    {IMGFMT_GBRP, IMGFMT_BGR24},
    {IMGFMT_GBRP, IMGFMT_RGB24},
    {IMGFMT_GBRP, IMGFMT_BGR32},
    {IMGFMT_GBRP, IMGFMT_RGB32},
    {IMGFMT_PAL8, IMGFMT_BGR32},
    {IMGFMT_XYZ12, IMGFMT_RGB48},
    {0, 0}
};

static int check_outfmt(vf_instance_t *vf, int outfmt)
{
    enum AVPixelFormat pixfmt = imgfmt2pixfmt(outfmt);
    if (pixfmt == AV_PIX_FMT_NONE || sws_isSupportedOutput(pixfmt) < 1)
        return 0;
    return vf_next_query_format(vf, outfmt);
}

static unsigned int find_best_out(vf_instance_t *vf, int in_format)
{
    unsigned int best = 0;
    int i = -1;
    int j = -1;
    int format = 0;

    // find the best outfmt:
    while (1) {
        int ret;
        if (j < 0) {
            format = in_format;
            j = 0;
        } else if (i < 0) {
            while (preferred_conversions[j][0] &&
                   preferred_conversions[j][0] != in_format)
                j++;
            format = preferred_conversions[j++][1];
            // switch to standard list
            if (!format)
                i = 0;
        }
        if (i >= 0)
            format = outfmt_list[i++];
        if (!format)
            break;

        ret = check_outfmt(vf, format);

        MP_DBG(vf, "scale: query(%s) -> %d\n", vo_format_name(format), ret & 3);
        if (ret & VFCAP_CSP_SUPPORTED_BY_HW) {
            best = format; // no conversion -> bingo!
            break;
        }
        if (ret & VFCAP_CSP_SUPPORTED && !best)
            best = format;  // best with conversion
    }
    if (!best) {
        // Try anything else. outfmt_list is just a list of preferred formats.
        for (int cur = IMGFMT_START; cur < IMGFMT_END; cur++) {
            int ret = check_outfmt(vf, cur);

            if (ret & VFCAP_CSP_SUPPORTED_BY_HW) {
                best = cur; // no conversion -> bingo!
                break;
            }
            if (ret & VFCAP_CSP_SUPPORTED && !best)
                best = cur;  // best with conversion
        }
    }
    return best;
}

static int reconfig(struct vf_instance *vf, struct mp_image_params *in,
                    struct mp_image_params *out)
{
    int width = in->w, height = in->h, d_width = in->d_w, d_height = in->d_h;
    unsigned int outfmt = in->imgfmt;
    unsigned int best = find_best_out(vf, outfmt);
    int round_w = 0, round_h = 0;

    if (!best) {
        MP_WARN(vf, "SwScale: no supported outfmt found :(\n");
        return -1;
    }

    vf->next->query_format(vf->next, best);

    vf->priv->w = vf->priv->cfg_w;
    vf->priv->h = vf->priv->cfg_h;

    if (vf->priv->w <= -8) {
        vf->priv->w += 8;
        round_w = 1;
    }
    if (vf->priv->h <= -8) {
        vf->priv->h += 8;
        round_h = 1;
    }

    if (vf->priv->w < -3 || vf->priv->h < -3 ||
        (vf->priv->w < -1 && vf->priv->h < -1))
    {
        // TODO: establish a direct connection to the user's brain
        // and find out what the heck he thinks MPlayer should do
        // with this nonsense.
        MP_ERR(vf, "SwScale: EUSERBROKEN Check your parameters, they make no sense!\n");
        return -1;
    }

    if (vf->priv->w == -1)
        vf->priv->w = width;
    if (vf->priv->w == 0)
        vf->priv->w = d_width;

    if (vf->priv->h == -1)
        vf->priv->h = height;
    if (vf->priv->h == 0)
        vf->priv->h = d_height;

    if (vf->priv->w == -3)
        vf->priv->w = vf->priv->h * width / height;
    if (vf->priv->w == -2)
        vf->priv->w = vf->priv->h * d_width / d_height;

    if (vf->priv->h == -3)
        vf->priv->h = vf->priv->w * height / width;
    if (vf->priv->h == -2)
        vf->priv->h = vf->priv->w * d_height / d_width;

    if (round_w)
        vf->priv->w = ((vf->priv->w + 8) / 16) * 16;
    if (round_h)
        vf->priv->h = ((vf->priv->h + 8) / 16) * 16;

    // check for upscaling, now that all parameters had been applied
    if (vf->priv->noup) {
        if ((vf->priv->w > width) + (vf->priv->h > height) >= vf->priv->noup) {
            vf->priv->w = width;
            vf->priv->h = height;
        }
    }

    MP_DBG(vf, "SwScale: scaling %dx%d %s to %dx%d %s  \n",
           width, height, vo_format_name(outfmt), vf->priv->w, vf->priv->h,
           vo_format_name(best));

    // Compute new d_width and d_height, preserving aspect
    // while ensuring that both are >= output size in pixels.
    if (vf->priv->h * d_width > vf->priv->w * d_height) {
        d_width = vf->priv->h * d_width / d_height;
        d_height = vf->priv->h;
    } else {
        d_height = vf->priv->w * d_height / d_width;
        d_width = vf->priv->w;
    }

    *out = *in;
    out->w = vf->priv->w;
    out->h = vf->priv->h;
    out->d_w = d_width;
    out->d_h = d_height;
    out->imgfmt = best;

    // Second-guess what libswscale is going to output and what not.
    // It depends what libswscale supports for in/output, and what makes sense.
    struct mp_imgfmt_desc s_fmt = mp_imgfmt_get_desc(in->imgfmt);
    struct mp_imgfmt_desc d_fmt = mp_imgfmt_get_desc(out->imgfmt);
    // keep colorspace settings if the data stays in yuv
    if (!(s_fmt.flags & MP_IMGFLAG_YUV) || !(d_fmt.flags & MP_IMGFLAG_YUV)) {
        out->colorspace = MP_CSP_AUTO;
        out->colorlevels = MP_CSP_LEVELS_AUTO;
    }
    mp_image_params_guess_csp(out);

    mp_sws_set_from_cmdline(vf->priv->sws, vf->chain->opts->vo.sws_opts);
    vf->priv->sws->flags |= vf->priv->v_chr_drop << SWS_SRC_V_CHR_DROP_SHIFT;
    vf->priv->sws->flags |= vf->priv->accurate_rnd * SWS_ACCURATE_RND;
    vf->priv->sws->src = *in;
    vf->priv->sws->dst = *out;

    if (mp_sws_reinit(vf->priv->sws) < 0) {
        // error...
        MP_WARN(vf, "Couldn't init libswscale for this setup\n");
        return -1;
    }
    return 0;
}

static struct mp_image *filter(struct vf_instance *vf, struct mp_image *mpi)
{
    struct mp_image *dmpi = vf_alloc_out_image(vf);
    if (!dmpi)
        return NULL;
    mp_image_copy_attributes(dmpi, mpi);

    mp_sws_scale(vf->priv->sws, dmpi, mpi);

    talloc_free(mpi);
    return dmpi;
}

static int control(struct vf_instance *vf, int request, void *data)
{
    struct mp_sws_context *sws = vf->priv->sws;

    switch (request) {
    case VFCTRL_GET_EQUALIZER:
        if (mp_sws_get_vf_equalizer(sws, data) < 1)
            break;
        return CONTROL_TRUE;
    case VFCTRL_SET_EQUALIZER:
        if (mp_sws_set_vf_equalizer(sws, data) < 1)
            break;
        return CONTROL_TRUE;
    }

    return CONTROL_UNKNOWN;
}

//===========================================================================//

static int query_format(struct vf_instance *vf, unsigned int fmt)
{
    if (!IMGFMT_IS_HWACCEL(fmt) && imgfmt2pixfmt(fmt) != AV_PIX_FMT_NONE) {
        if (sws_isSupportedInput(imgfmt2pixfmt(fmt)) < 1)
            return 0;
        unsigned int best = find_best_out(vf, fmt);
        int flags;
        if (!best)
            return 0;            // no matching out-fmt
        flags = vf_next_query_format(vf, best);
        if (!(flags & (VFCAP_CSP_SUPPORTED | VFCAP_CSP_SUPPORTED_BY_HW)))
            return 0;
        if (fmt != best)
            flags &= ~VFCAP_CSP_SUPPORTED_BY_HW;
        return flags;
    }
    return 0;   // nomatching in-fmt
}

static void uninit(struct vf_instance *vf)
{
}

static int vf_open(vf_instance_t *vf)
{
    vf->reconfig = reconfig;
    vf->filter = filter;
    vf->query_format = query_format;
    vf->control = control;
    vf->uninit = uninit;
    vf->priv->sws = mp_sws_alloc(vf);
    vf->priv->sws->log = vf->log;
    vf->priv->sws->params[0] = vf->priv->param[0];
    vf->priv->sws->params[1] = vf->priv->param[1];

    MP_VERBOSE(vf, "SwScale params: %d x %d (-1=no scaling)\n",
           vf->priv->cfg_w, vf->priv->cfg_h);

    return 1;
}

#define OPT_BASE_STRUCT struct vf_priv_s
static const m_option_t vf_opts_fields[] = {
    OPT_INT("w", cfg_w, M_OPT_MIN, .min = -11),
    OPT_INT("h", cfg_h, M_OPT_MIN, .min = -11),
    OPT_DOUBLE("param", param[0], M_OPT_RANGE, .min = 0.0, .max = 100.0),
    OPT_DOUBLE("param2", param[1], M_OPT_RANGE, .min = 0.0, .max = 100.0),
    OPT_INTRANGE("chr-drop", v_chr_drop, 0, 0, 3),
    OPT_INTRANGE("noup", noup, 0, 0, 2),
    OPT_FLAG("arnd", accurate_rnd, 0),
    {0}
};

const vf_info_t vf_info_scale = {
    .description = "software scaling",
    .name = "scale",
    .open = vf_open,
    .priv_size = sizeof(struct vf_priv_s),
    .priv_defaults = &vf_priv_dflt,
    .options = vf_opts_fields,
};

//===========================================================================//
