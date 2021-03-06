/* Copyright (C) 2001-2012 Artifex Software, Inc.
   All Rights Reserved.

   This software is provided AS-IS with no warranty, either express or
   implied.

   This software is distributed under license and may not be copied,
   modified or distributed except as expressly authorized under the terms
   of the license contained in the file LICENSE in this distribution.

   Refer to licensing information at http://www.artifex.com or contact
   Artifex Software, Inc.,  7 Mt. Lassen Drive - Suite A-134, San Rafael,
   CA  94903, U.S.A., +1(415)492-9861, for further information.
*/


/* Interpolated image procedures */
#include "gx.h"
#include "math_.h"
#include "memory_.h"
#include "stdint_.h"
#include "gpcheck.h"
#include "gserrors.h"
#include "gxfixed.h"
#include "gxfrac.h"
#include "gxarith.h"
#include "gxmatrix.h"
#include "gsccolor.h"
#include "gspaint.h"
#include "gxdevice.h"
#include "gxcmap.h"
#include "gxdcolor.h"
#include "gxistate.h"
#include "gxdevmem.h"
#include "gxcpath.h"
#include "gximage.h"
#include "stream.h"             /* for s_alloc_state */
#include "siinterp.h"           /* for spatial interpolation */
#include "siscale.h"            /* for Mitchell filtering */
#include "sidscale.h"           /* for special case downscale filter */
#include "vdtrace.h"
#include "gscindex.h"           /* included for proper handling of index color spaces
                                and keeping data in source color space */
#include "gxcolor2.h"           /* define of float_color_to_byte_color */
#include "gscspace.h"           /* Needed for checking is space is CIE */
#include "gsicc_cache.h"
#include "gsicc_manage.h"
#include "gsicc.h"

static void
decode_sample_frac_to_float(gx_image_enum *penum, frac sample_value, gs_client_color *cc, int i);

/*
 * Define whether we are using Mitchell filtering or spatial
 * interpolation to implement Interpolate.  (The latter doesn't work yet.)
 */
#define USE_MITCHELL_FILTER

/* ------ Strategy procedure ------ */

/* Check the prototype. */
iclass_proc(gs_image_class_0_interpolate);

/* If we're interpolating, use special logic.
   This function just gets interpolation stucture
   initialized and allocates buffer space if needed */
static irender_proc(image_render_interpolate);
static irender_proc(image_render_interpolate_icc);

irender_proc_t
gs_image_class_0_interpolate(gx_image_enum * penum)
{
    gs_memory_t *mem = penum->memory;
    stream_image_scale_params_t iss;
    stream_image_scale_state *pss;
    const stream_template *templat;
    byte *line;
    const gs_color_space *pcs = penum->pcs;
    uint in_size;
    bool use_icc = false;
    int num_des_comps;
    cmm_dev_profile_t *dev_profile;
    int code;

    if (!penum->interpolate)
        return 0;
    if (penum->use_mask_color || penum->posture != image_portrait ||
        penum->masked || penum->alpha) {
        /* We can't handle these cases yet.  Punt. */
        penum->interpolate = false;
        return 0;
    }
    if ( pcs->cmm_icc_profile_data != NULL ) {
        use_icc = true;
    }
    if ( pcs->type->index == gs_color_space_index_Indexed) {
        if ( pcs->base_space->cmm_icc_profile_data != NULL) {
            use_icc = true;
        }
    }
    if (!(penum->bps <= 8 || penum->bps == 16)) {
        use_icc = false;
    }
    /* Do not allow mismatch in devices component output with the
       profile output size.  For example sep device with CMYK profile should
       not go through the fast method */
    code = dev_proc(penum->dev, get_profile)(penum->dev, &dev_profile);
    num_des_comps = gsicc_get_device_profile_comps(dev_profile);
    if (num_des_comps != penum->dev->color_info.num_components) {
        use_icc = false;
    }
    /* If the device has some unique color mapping procs due to its color space,
       then we will need to use those and go through pixel by pixel instead
       of blasting through buffers.  This is true for example with many of
       the color spaces for CUPs */
    if(!gx_device_uses_std_cmap_procs(penum->dev, penum->pis)) {
        use_icc = false;
    }
/*
 * USE_CONSERVATIVE_INTERPOLATION_RULES is normally NOT defined since
 * the MITCHELL digital filter seems OK as long as we are going out to
 * a device that can produce > 15 shades.
 */
#if defined(USE_MITCHELL_FILTER) && defined(USE_CONSERVATIVE_INTERPOLATION_RULES)
    /*
     * We interpolate using a digital filter, rather than Adobe's
     * spatial interpolation algorithm: this produces very bad-looking
     * results if the input resolution is close to the output resolution,
     * especially if the input has low color resolution, so we resort to
     * some hack tests on the input color resolution and scale to suppress
     * interpolation if we think the result would look especially bad.
     * If we used Adobe's spatial interpolation approach, we wouldn't need
     * to do this, but the spatial interpolation filter doesn't work yet.
     */
    if (penum->bps < 4 || penum->bps * penum->spp < 8 ||
        (fabs(penum->matrix.xx) <= 5 && fabs(penum->matrix.yy <= 5))
        ) {
        penum->interpolate = false;
        return 0;
    }
#endif
    if (use_icc) {
        iss.BitsPerComponentOut = 16;
        iss.MaxValueOut = 0xffff;
    } else {
        iss.BitsPerComponentOut = sizeof(frac) * 8;
        iss.MaxValueOut = frac_1;
    }
    iss.WidthOut = fixed2int_pixround_perfect((fixed)((int64_t)(penum->rect.x + penum->rect.w) *
                                                      penum->dst_width / penum->Width))
                 - fixed2int_pixround_perfect((fixed)((int64_t)penum->rect.x *
                                                      penum->dst_width / penum->Width));
    iss.WidthOut = any_abs(iss.WidthOut);
    iss.HeightOut = fixed2int_pixround_perfect((fixed)((int64_t)(penum->rect.y + penum->rect.h) *
                                                       penum->dst_height / penum->Height))
                  - fixed2int_pixround_perfect((fixed)((int64_t)penum->rect.y *
                                                       penum->dst_height / penum->Height));
    iss.HeightOut = any_abs(iss.HeightOut);
    iss.WidthIn = penum->rect.w;
    iss.HeightIn = penum->rect.h;
    iss.PatchWidthOut = fixed2int_pixround_perfect((fixed)((int64_t)(penum->rrect.x + penum->rrect.w) *
                                                           penum->dst_width / penum->Width))
                      - fixed2int_pixround_perfect((fixed)((int64_t)penum->rrect.x *
                                                           penum->dst_width / penum->Width));
    iss.PatchWidthOut = any_abs(iss.PatchWidthOut);
    iss.PatchHeightOut = fixed2int_pixround_perfect((fixed)((int64_t)(penum->rrect.y + penum->rrect.h) *
                                                            penum->dst_height / penum->Height))
                       - fixed2int_pixround_perfect((fixed)((int64_t)penum->rrect.y *
                                                            penum->dst_height / penum->Height));
    iss.PatchHeightOut = any_abs(iss.PatchHeightOut);
    iss.PatchWidthIn = penum->rrect.w;
    iss.PatchHeightIn = penum->rrect.h;
    iss.LeftMarginIn = penum->rrect.x - penum->rect.x;
    iss.LeftMarginOut = fixed2int_pixround_perfect((fixed)((int64_t)iss.LeftMarginIn *
                                                penum->dst_width / penum->Width));
    iss.TopMargin = penum->rrect.y - penum->rect.y;
    iss.src_y_offset = penum->rect.y;
    iss.EntireWidthIn = penum->Width;
    iss.EntireHeightIn = penum->Height;
    iss.EntireWidthOut = fixed2int_pixround(any_abs(penum->dst_width));
    iss.EntireHeightOut = fixed2int_pixround(any_abs(penum->dst_height));
    /* For interpolator cores that don't set Active, have us always active */
    iss.Active = 1;
    if (iss.EntireWidthOut == 0 || iss.EntireHeightOut == 0)
    {
        penum->interpolate = false;
        return 0;
    }
    /* If we are in an indexed space then we need to use the number of components
       in the base space.  Otherwise we use the number of components in the source space */
    if (pcs->type->index == gs_color_space_index_Indexed) {
        /* Use the number of colors in the base space */
        iss.spp_decode = cs_num_components(pcs->base_space);
    } else {
        /* Use the number of colors that exist in the source space
        as this is where we are doing our interpolation */
        iss.spp_decode = cs_num_components(pcs);
    }
    if (iss.HeightOut > iss.EntireHeightIn && use_icc) {
        iss.early_cm = true;
        iss.spp_interp = num_des_comps;
    } else {
        iss.early_cm = false;
        iss.spp_interp = iss.spp_decode;
    }
    if (penum->bps <= 8 ) {
       /* If the input is ICC or other device independent format, go ahead
          and do the interpolation in that space.
          If we have more than 8 bits per channel then we will need to
          handle that in a slightly different manner so
          that the interpolation algorithm handles it properly.
          The interpolation will still be in the source
          color space.  Note that if image data was less the 8 bps
          It is handed here to us in 8 bit form already decoded. */
        iss.BitsPerComponentIn = 8;
        iss.MaxValueIn = 0xff;
        /* If it is an index color space we will need to allocate for
           the decoded data */
       if (pcs->type->index == gs_color_space_index_Indexed) {
           in_size = iss.WidthIn * iss.spp_decode;
       } else {
           /* Non indexed case, we either use the data as
           is, or allocate space if it is reversed in X */
            in_size =
                (penum->matrix.xx < 0 ?
                 /* We need a buffer for reversing each scan line. */
                 iss.WidthIn * iss.spp_decode : 0);
            /* If it is not reversed, and we have 8 bit/color channel data then
            no need to allocate extra as we will use the source directly */
            /* However, if we have a nonstandard encoding and are in
                a device color space we will need to allocate
               in that case also. We will maintain 8 bits but
               do the decode and then interpolate.  This is OK
               for the linear decode */
            if (!penum->device_color && !gs_color_space_is_CIE(pcs)){
                in_size = iss.WidthIn * iss.spp_decode;
            }
        }
    } else {
        /* If it has more than 8 bits per color channel then we will go to frac
           for the interpolation to mantain precision or 16 bit for icc  */
        if (use_icc) {
            iss.BitsPerComponentIn = 16;
            iss.MaxValueIn = 0xffff;
        } else {
            iss.BitsPerComponentIn = sizeof(frac) * 8;
            iss.MaxValueIn = frac_1;
        }
        in_size = round_up(iss.WidthIn * iss.spp_decode * sizeof(frac),
                           align_bitmap_mod);
        /* Size to allocate space to store the input as frac type */
    }
#ifdef USE_MITCHELL_FILTER
    templat = &s_IScale_template;
#else
    templat = &s_IIEncode_template;
#endif
    /* RJW: This is defeated by the presence of pdf14. Use a devspecop. */
    if (((penum->dev->color_info.num_components == 1 &&
          penum->dev->color_info.max_gray < 15) ||
         (penum->dev->color_info.num_components > 1 &&
          penum->dev->color_info.max_color < 15))
        ) {
        /* halftone device -- restrict interpolation */
        if ((iss.WidthOut < iss.WidthIn * 4) &&
            (iss.HeightOut < iss.HeightIn * 4)) {
            if ((iss.WidthOut < iss.WidthIn) &&
                (iss.HeightOut < iss.HeightIn) &&       /* downsampling */
                (penum->dev->color_info.polarity != GX_CINFO_POLARITY_UNKNOWN)) {
                /* Special case handling for when we are downsampling
                   to a dithered device.  The point of this non-linear
                   downsampling is to preserve dark pixels from the source
                   image to avoid dropout. The color polarity is used for this  */
                templat = &s_ISpecialDownScale_template;
            } else {
                penum->interpolate = false;
                return 0;       /* no interpolation / downsampling */
            }
        }
        /* else, continue with the Mitchell filter (for upscaling of at least 4:1) */
    }
    /* The SpecialDownScale filter needs polarity, either ADDITIVE or SUBTRACTIVE */
    /* UNKNOWN case (such as for palette colors) has been handled above */
    iss.ColorPolarityAdditive =
        penum->dev->color_info.polarity == GX_CINFO_POLARITY_ADDITIVE;
    /* Allocate a buffer for one source/destination line. */
    {
        uint out_size =
            iss.WidthOut * max(iss.spp_interp * (iss.BitsPerComponentOut / 8),
                               arch_sizeof_color_index);
        /* Allocate based upon frac size (as BitsPerComponentOut=16) output scan
           line input plus output. The outsize may have an adjustment for
           word boundary on it. Need to account for that now */
        out_size += align_bitmap_mod;
        line = gs_alloc_bytes(mem, in_size + out_size,
                              "image scale src+dst line");
    }
    pss = (stream_image_scale_state *)
        s_alloc_state(mem, templat->stype, "image scale state");
    if (line == 0 || pss == 0 ||
        (pss->params = iss, pss->templat = templat,
         (*pss->templat->init) ((stream_state *) pss) < 0)
        ) {
        gs_free_object(mem, pss, "image scale state");
        gs_free_object(mem, line, "image scale src+dst line");
        /* Try again without interpolation. */
        penum->interpolate = false;
        return 0;
    }
    penum->line = line;  /* Set to the input and output buffer */
    penum->scaler = pss;
    penum->line_xy = 0;
    {
        gx_dda_fixed x0;
        x0 = penum->dda.pixel0.x;
        if (penum->matrix.xx < 0)
            dda_advance(x0, penum->rect.w);
        penum->xyi.x = fixed2int_pixround(dda_current(x0)) + pss->params.LeftMarginOut;
    }
    penum->xyi.y = penum->yi0 + fixed2int_pixround_perfect((fixed)((int64_t)penum->rect.y
                                    * penum->dst_height / penum->Height));
    if_debug0('b', "[b]render=interpolate\n");
    if (use_icc) {
        /* Set up the link now */
        const gs_color_space *pcs;
        gsicc_rendering_param_t rendering_params;
        int k;
        int src_num_comp = cs_num_components(penum->pcs);

        penum->icc_setup.need_decode = false;
        /* Check if we need to do any decoding.  If yes, then that will slow us down */
        for (k = 0; k < src_num_comp; k++) {
            if ( penum->map[k].decoding != sd_none ) {
                penum->icc_setup.need_decode = true;
                break;
            }
        }
        /* Define the rendering intents */
        rendering_params.black_point_comp = BP_ON;
        rendering_params.graphics_type_tag = GS_IMAGE_TAG;
        rendering_params.rendering_intent = penum->pis->renderingintent;
        if (gs_color_space_is_PSCIE(penum->pcs) && penum->pcs->icc_equivalent != NULL) {
            pcs = penum->pcs->icc_equivalent;
        } else {
            /* Look for indexed space */
            if ( penum->pcs->type->index == gs_color_space_index_Indexed) {
                pcs = penum->pcs->base_space;
            } else {
                pcs = penum->pcs;
            }
        }
        penum->icc_setup.is_lab = pcs->cmm_icc_profile_data->islab;
        if (penum->icc_setup.is_lab) penum->icc_setup.need_decode = false;
        penum->icc_setup.must_halftone = gx_device_must_halftone(penum->dev);
        penum->icc_setup.has_transfer =
            gx_has_transfer(penum->pis, num_des_comps);
        if (penum->icc_link == NULL) {
            penum->icc_link = gsicc_get_link(penum->pis, penum->dev, pcs, NULL,
                &rendering_params, penum->memory);
        }
        /* We need to make sure that we do the proper unpacking proc if we
           are doing 16 bit */
        if (penum->bps == 16) {
            penum->unpack = sample_unpackicc_16_proc;
        }
        return &image_render_interpolate_icc;
    } else {
        return &image_render_interpolate;
    }
}

/* ------ Rendering for interpolated images ------ */

/* This does some initial required decoding of index spaces and general
   decoding of odd scaled image data needed prior to interpolation or
   application of color management. */
static void
initial_decode(gx_image_enum * penum, const byte * buffer, int data_x, int h,
               bool need_decode, stream_cursor_read *stream_r, bool is_icc)
{
    stream_image_scale_state *pss = penum->scaler;
    //const gs_imager_state *pis = penum->pis;
    const gs_color_space *pcs = penum->pcs;
    //gs_logical_operation_t lop = penum->log_op;
    int spp_decode = pss->params.spp_decode;
    //stream_cursor_write w;
    unsigned char index_space;
    byte *out = penum->line;

    if (h != 0) {
        /* Convert the unpacked data to concrete values in the source buffer. */
        int sizeofPixelIn = pss->params.BitsPerComponentIn / 8;
        uint row_size = pss->params.WidthIn * spp_decode * sizeofPixelIn;
         /* raw input data */
        const unsigned char *bdata = buffer + data_x * spp_decode * sizeofPixelIn;
        index_space = 0;
        /* We have the following cases to worry about
          1) Device 8 bit color but not indexed (e.g. ICC).
             Apply CMM after interpolation if needed.
             Also if ICC CIELAB do not do a decode operation
          2) Indexed 8 bit color.  Get to the base space. We will then be in
             the same state as 1.
          3) 16 bit not indexed.  Remap after interpolation.
          4) Indexed 16bit color.   Get to base space in 16bit form. We
             will then be in same state as 3.
         */
        if (sizeofPixelIn == 1) {
            if (pcs->type->index != gs_color_space_index_Indexed) {
                /* An issue here is that we may not be "device color" due to
                   how the data is encoded.  Need to check for that case here */
                /* Decide here if we need to decode or not. Essentially, as
                 * far as I can gather, we use the top case if we DON'T need
                 * to decode. This is fairly obviously conditional on
                 * need_decode being set to 0. The major exception to this is
                 * that if the colorspace is CIE, we interpolate, THEN decode,
                 * so the decode is done later in the pipeline, so we needn't
                 * decode here (see Bugs 692225 and 692331). */
                if (!need_decode) {
                    /* 8-bit color values, possibly device  indep. or device
                       depend., not indexed. Decode range was [0 1] */
                    if (penum->matrix.xx >= 0) {
                        /* Use the input data directly. sets up data in the
                           stream buffer structure */
                        stream_r->ptr = bdata - 1;
                    } else {
                        /* Mirror the data in X. */
                        const byte *p = bdata + row_size - spp_decode;
                        byte *q = out;
                        int i;

                        for (i = 0; i < pss->params.WidthIn;
                            p -= spp_decode, q += spp_decode, ++i)
                            memcpy(q, p, spp_decode);
                        stream_r->ptr = out - 1;
                        out += round_up(pss->params.WidthIn *
                                        spp_decode, align_bitmap_mod);
                    }
                } else {
                    /* We need to do some decoding. Data will remain in 8 bits
                       This does not occur if color space was CIE encoded.
                       Then we do the decode during concretization which occurs
                       after interpolation */
                    int dc = penum->spp;
                    const byte *pdata = bdata;
                    byte *psrc = (byte *) penum->line;
                    int i, j;
                    int dpd = dc;
                    gs_client_color cc;

                    /* Go backwards through the data */
                    if (penum->matrix.xx < 0) {
                        pdata += (pss->params.WidthIn - 1) * dpd;
                        dpd = - dpd;
                    }
                    stream_r->ptr = (byte *) psrc - 1;
                    for (i = 0; i < pss->params.WidthIn; i++, psrc += spp_decode) {
                        /* Do the decode but remain in 8 bits */
                        for (j = 0; j < dc;  ++j) {
                            decode_sample(pdata[j], cc, j);
                            psrc[j] = float_color_to_byte_color(cc.paint.values[j]);
                        }
                        pdata += dpd;
                    }
                    out += round_up(pss->params.WidthIn * spp_decode,
                                    align_bitmap_mod);
                }
            } else {
                /* indexed 8 bit color values, possibly a device indep. or
                   device depend. base space. We need to get out of the indexed
                   space and into the base color space. Note that we need to
                   worry about the decode function for the index values. */
                int bps = penum->bps;
                int dc = penum->spp;
                const byte *pdata = bdata; /* Input buffer */
                unsigned char *psrc = (unsigned char *) penum->line;  /* Output */
                int i;
                int dpd = dc * (bps <= 8 ? 1 : sizeof(frac));
                float max_range;

                /* Get max of decode range */
                max_range = (penum->map[0].decode_factor < 0 ?
                    penum->map[0].decode_base :
                penum->map[0].decode_base + 255.0 * penum->map[0].decode_factor);
                index_space = 1;
                /* flip the horizontal direction if indicated by the matrix value */
                if (penum->matrix.xx < 0) {
                    pdata += (pss->params.WidthIn - 1) * dpd;
                    dpd = - dpd;
                }
                stream_r->ptr = (byte *) psrc - 1;

                for (i = 0; i < pss->params.WidthIn; i++, psrc += spp_decode) {
                    /* Let's get directly to a decoded byte type loaded into
                       psrc, and do the interpolation in the source space. Then
                       we will do the appropriate remap function after
                       interpolation. */
                    /* First we need to get the properly decoded value. */
                    float decode_value;
                    switch ( penum->map[0].decoding )
                    {
                        case sd_none:
                         /* while our indexin is going to be 0 to 255.0 due to
                            what is getting handed to us, the range of our
                            original data may not have been as such and we may
                            need to rescale, to properly lookup at the correct
                            location (or do the proc correctly) during the index
                            look-up.  This occurs even if decoding was set to
                            sd_none.  */
                            decode_value = (float) pdata[0] * (float)max_range / 255.0;
                            break;
                        case sd_lookup:
                            decode_value =
                              (float) penum->map[0].decode_lookup[pdata[0] >> 4];
                            break;
                        case sd_compute:
                            decode_value =
                              penum->map[0].decode_base +
                              ((float) pdata[0]) * penum->map[0].decode_factor;
                            break;
                        default:
                            decode_value = 0; /* Quiet gcc warning. */
                    }
                    gs_cspace_indexed_lookup_bytes(pcs, decode_value,psrc);
                    pdata += dpd;    /* Can't have just ++
                                        since we could be going backwards */
                }
                /* We need to set the output to the end of the input buffer
                   moving it to the next desired word boundary.  This must
                   be accounted for in the memory allocation of
                   gs_image_class_0_interpolate */
                out += round_up(pss->params.WidthIn * spp_decode,
                                align_bitmap_mod);
            }
        } else {
            /* More than 8-bits/color values */
            /* Even in this case we need to worry about an indexed color space.
               We need to get to the base color space for the interpolation and
               then if necessary do the remap to the device space */
            if (pcs->type->index != gs_color_space_index_Indexed) {
                int bps = penum->bps;
                int dc = penum->spp;
                const byte *pdata = bdata;
                frac *psrc = (frac *) penum->line;
                int i, j;
                int dpd = dc * (bps <= 8 ? 1 : sizeof(frac));

                if (penum->matrix.xx < 0) {
                    pdata += (pss->params.WidthIn - 1) * dpd;
                    dpd = - dpd;
                }
                stream_r->ptr = (byte *) psrc - 1;
                if_debug0('B', "[B]Remap row:\n[B]");
                if (is_icc) {
                    stream_r->ptr = (byte *) pdata - 1;
                } else {
                    for (i = 0; i < pss->params.WidthIn; i++,
                         psrc += spp_decode) {
                        /* Lets get directly to a frac type loaded into psrc,
                           and do the interpolation in the source space. Then we
                           will do the appropriate remap function after
                           interpolation. */
                        for (j = 0; j < dc; ++j) {
                            DECODE_FRAC_FRAC(((const frac *)pdata)[j], psrc[j], j);
                        }
                        pdata += dpd;
    #ifdef DEBUG
                        if (gs_debug_c('B')) {
                            int ci;

                            for (ci = 0; ci < spp_decode; ++ci)
                                dprintf2("%c%04x", (ci == 0 ? ' ' : ','), psrc[ci]);
                        }
    #endif
                    }
                }
                out += round_up(pss->params.WidthIn * spp_decode * sizeof(frac),
                                align_bitmap_mod);
                if_debug0('B', "\n");
            } else {
                /* indexed and more than 8bps.  Need to get to the base space */
                int bps = penum->bps;
                int dc = penum->spp;
                const byte *pdata = bdata; /* Input buffer */
                frac *psrc = (frac *) penum->line;    /* Output buffer */
                int i;
                int dpd = dc * (bps <= 8 ? 1 : sizeof(frac));
                float decode_value;

                index_space = 1;
                /* flip the horizontal direction if indicated by the matrix value */
                if (penum->matrix.xx < 0) {
                    pdata += (pss->params.WidthIn - 1) * dpd;
                    dpd = - dpd;
                }
                stream_r->ptr = (byte *) psrc - 1;
                for (i = 0; i < pss->params.WidthIn; i++, psrc += spp_decode) {
                    /* Lets get the decoded value. Then we need to do the lookup
                       of this */
                    decode_value = penum->map[i].decode_base +
                        (((const frac *)pdata)[0]) * penum->map[i].decode_factor;
                    /* Now we need to do the lookup of this value, and stick it
                       in psrc as a frac, which is what the interpolator is
                       expecting, since we had more than 8 bits of original
                       image data */
                    gs_cspace_indexed_lookup_frac(pcs, decode_value,psrc);
                    pdata += dpd;
                }
                /* We need to set the output to the end of the input buffer
                   moving it to the next desired word boundary.  This must
                   be accounted for in the memory allocation of
                   gs_image_class_0_interpolate */
                out += round_up(pss->params.WidthIn * spp_decode,
                                align_bitmap_mod);
            } /* end of else on indexed */
        }  /* end of else on more than 8 bps */
        stream_r->limit = stream_r->ptr + row_size;
    } else {                    /* h == 0 */
        stream_r->ptr = 0, stream_r->limit = 0;
        index_space = 0;
    }
}

static int
image_render_interpolate(gx_image_enum * penum, const byte * buffer,
                         int data_x, uint iw, int h, gx_device * dev)
{
    stream_image_scale_state *pss = penum->scaler;
    const gs_imager_state *pis = penum->pis;
    const gs_color_space *pcs = penum->pcs;
    gs_logical_operation_t lop = penum->log_op;
    int spp_decode = pss->params.spp_decode;
    stream_cursor_read stream_r;
    stream_cursor_write stream_w;
    bool is_index_space;
    byte *out = penum->line;
    bool islab = false;
    bool need_decode;

    if (pcs->cmm_icc_profile_data != NULL) {
        islab = pcs->cmm_icc_profile_data->islab;
    }
    /* Perform any decode procedure if needed */
    need_decode = !(penum->device_color || gs_color_space_is_CIE(pcs) || islab);
    initial_decode(penum, buffer, data_x, h, need_decode, &stream_r, false);
    is_index_space = (pcs->type->index == gs_color_space_index_Indexed);
    /*
     * Process input and/or collect output.  By construction, the pixels are
     * 1-for-1 with the device, but the Y coordinate might be inverted.
     */
    {
        int xo = penum->xyi.x;
        int yo = penum->xyi.y;
        int width = pss->params.WidthOut;
        int sizeofPixelOut = pss->params.BitsPerComponentOut / 8;
        int dy;
        const gs_color_space *pconcs;
        const gs_color_space *pactual_cs;
        int bpp = dev->color_info.depth;
        uint raster = bitmap_raster(width * bpp);
        bool device_color;

        if (penum->matrix.yy > 0)
            dy = 1;
        else
            dy = -1, yo--;
        for (;;) {
            int ry = yo + penum->line_xy * dy;
            int x;
            const frac *psrc;
            gx_device_color devc;
            int status, code;

            DECLARE_LINE_ACCUM_COPY(out, bpp, xo);
            stream_w.limit = out + width *
                max(spp_decode * sizeofPixelOut, arch_sizeof_color_index) - 1;
            stream_w.ptr = stream_w.limit - width * spp_decode * sizeofPixelOut;
            psrc = (const frac *)(stream_w.ptr + 1);
            /* This is where the rescale takes place; this will consume the
             * data from stream_r, and post processed data into stream_w. The
             * data in stream_w may be bogus if we are outside the active
             * region, and this will be indicated by pss->params.Active being
             * set to false. */
            status = (*pss->templat->process)
                ((stream_state *) pss, &stream_r, &stream_w, h == 0);
            if (status < 0 && status != EOFC)
                return_error(gs_error_ioerror);
            if (stream_w.ptr == stream_w.limit) {
                int xe = xo + pss->params.PatchWidthOut;

                /* Are we active? (i.e. in the render rectangle) */
                if (!pss->params.Active)
                    goto inactive;
                if_debug1('B', "[B]Interpolated row %d:\n[B]",
                          penum->line_xy);
                psrc += pss->params.LeftMarginOut * spp_decode;
                for (x = xo; x < xe;) {
#ifdef DEBUG
                    if (gs_debug_c('B')) {
                        int ci;

                        for (ci = 0; ci < spp_decode; ++ci)
                            dprintf2("%c%04x", (ci == 0 ? ' ' : ','),
                                     psrc[ci]);
                    }
#endif
                    /* if we are in a non device space then work
                       from the pcs not from the concrete space
                       also handle index case, where base case was device type */
                    if (pcs->type->index == gs_color_space_index_Indexed) {
                        pactual_cs = pcs->base_space;
                    } else {
                        pactual_cs = pcs;
                    }
                    pconcs = cs_concrete_space(pactual_cs, pis);
                    if (pconcs->cmm_icc_profile_data != NULL) {
                        device_color = false;
                    } else {
                        device_color = (pconcs == pactual_cs);
                    }
                    if (device_color) {
                        /* Use the underlying concrete space remap */
                        code = (*pconcs->type->remap_concrete_color)
                        (psrc, pconcs, &devc, pis, dev, gs_color_select_source);
                    } else {
                        /* if we are device dependent we need to get back to
                           float prior to remap.  This stuff needs to be
                           reworked  as  part of the ICC flow update.
                           In such a flow, we will want the interpolation
                           algorithm output likely to be 8 bit (if the input
                           were 8 bit) and hit that buffer of values directly
                           with the linked transform */
                        gs_client_color cc;
                        int j;
                        int num_components =
                              gs_color_space_num_components(pactual_cs);

                        for (j = 0; j < num_components;  ++j) {
                            /* If we were indexed, dont use the decode procedure
                               for the index values just get to float directly */
                            if (is_index_space || islab) {
                                cc.paint.values[j] = frac2float(psrc[j]);
                            } else {
                                decode_sample_frac_to_float(penum, psrc[j], &cc, j);
                            }
                        }
                        /* If the source colors are LAB then use the mapping
                           that does not rescale the source colors */
                        if (gs_color_space_is_ICC(pactual_cs) &&
                            pactual_cs->cmm_icc_profile_data != NULL &&
                            pactual_cs->cmm_icc_profile_data->islab) {
                            code = gx_remap_ICC_imagelab (&cc, pactual_cs, &devc,
                                                          pis, dev,
                                                          gs_color_select_source);
                        } else {
                            code = (pactual_cs->type->remap_color)
                                    (&cc, pactual_cs, &devc, pis, dev,
                                     gs_color_select_source);
                        }
                    }
                    if (code < 0)
                        return code;
                    if (color_is_pure(&devc)) {
                        /* Just pack colors into a scan line. */
                        gx_color_index color = devc.colors.pure;
                        /* Skip runs quickly for the common cases. */
                        switch (spp_decode) {
                            case 1:
                                do {
                                    LINE_ACCUM(color, bpp);
                                    vd_pixel(int2fixed(x), int2fixed(ry), color);
                                    x++, psrc += 1;
                                } while (x < xe && psrc[-1] == psrc[0]);
                                break;
                            case 3:
                                do {
                                    LINE_ACCUM(color, bpp);
                                    vd_pixel(int2fixed(x), int2fixed(ry), color);
                                    x++, psrc += 3;
                                } while (x < xe &&
                                         psrc[-3] == psrc[0] &&
                                         psrc[-2] == psrc[1] &&
                                         psrc[-1] == psrc[2]);
                                break;
                            case 4:
                                do {
                                    LINE_ACCUM(color, bpp);
                                    x++, psrc += 4;
                                } while (x < xe &&
                                         psrc[-4] == psrc[0] &&
                                         psrc[-3] == psrc[1] &&
                                         psrc[-2] == psrc[2] &&
                                         psrc[-1] == psrc[3]);
                                break;
                            default:
                                LINE_ACCUM(color, bpp);
                                x++, psrc += spp_decode;
                        }
                    } else {
                        int rcode;

                        LINE_ACCUM_COPY(dev, out, bpp, xo, x, raster, ry);
                        rcode = gx_fill_rectangle_device_rop(x, ry, 1, 1,
                                                             &devc, dev, lop);
                        if (rcode < 0)
                            return rcode;
                        LINE_ACCUM_SKIP(bpp);
                        l_xprev = x + 1;
                        x++, psrc += spp_decode;
                    }
                }
                LINE_ACCUM_COPY(dev, out, bpp, xo, x, raster, ry);
                /*if_debug1('w', "[w]Y=%d:\n", ry);*/ /* See siscale.c about 'w'. */
inactive:
                penum->line_xy++;
                if_debug0('B', "\n");
            }
            if ((status == 0 && stream_r.ptr == stream_r.limit) || status == EOFC)
                break;
        }
    }
    return (h == 0 ? 0 : 1);
}

/* Interpolation with ICC based source spaces. This is done seperately to
   enable optimization and avoid the multiple tranformations that occur in
   the above code */
static int
image_render_interpolate_icc(gx_image_enum * penum, const byte * buffer,
                         int data_x, uint iw, int h, gx_device * dev)
{
    stream_image_scale_state *pss = penum->scaler;
    const gs_imager_state *pis = penum->pis;
    const gs_color_space *pcs = penum->pcs;
    gs_logical_operation_t lop = penum->log_op;
    //int spp_decode = pss->params.spp_decode;
    byte *out = penum->line;
    bool must_halftone = penum->icc_setup.must_halftone;
    bool has_transfer = penum->icc_setup.has_transfer;
    stream_cursor_read stream_r;
    stream_cursor_write stream_w;
    bool need_decode;

    if (penum->icc_link == NULL) {
        return gs_rethrow(-1, "ICC Link not created duringgs_image_class_0_interpolate");
    }
    /* Go ahead and take apart any indexed color space or do the decode
       so that we can then perform the interpolation or color management */
    need_decode = !(((penum->device_color || penum->icc_setup.is_lab) &&
                     (penum->icc_setup.need_decode == 0)) ||
                     gs_color_space_is_CIE(pcs));
    initial_decode(penum, buffer, data_x, h, need_decode, &stream_r, true);
    /*
     * Process input and/or collect output.  By construction, the pixels are
     * 1-for-1 with the device, but the Y coordinate might be inverted.
     * CM is performed on the entire row.
     */
    {
        int xo = penum->xyi.x;
        int yo = penum->xyi.y;
        int width = pss->params.WidthOut;
        int width_in = pss->params.WidthIn;
        int sizeofPixelOut = pss->params.BitsPerComponentOut / 8;
        int dy;
        int bpp = dev->color_info.depth;
        uint raster = bitmap_raster(width * bpp);
        unsigned short *p_cm_interp;
        byte *p_cm_buff = NULL;
        byte *psrc;
        int spp_decode = pss->params.spp_decode;
        int spp_interp = pss->params.spp_interp;
        int spp_cm;
        gsicc_bufferdesc_t input_buff_desc;
        gsicc_bufferdesc_t output_buff_desc;
        gx_color_index color;
        int code;
        cmm_dev_profile_t *dev_profile;
        int num_bytes_decode = pss->params.BitsPerComponentIn / 8;

        code = dev_proc(dev, get_profile)(dev, &dev_profile);
        spp_cm = gsicc_get_device_profile_comps(dev_profile);
        if (penum->matrix.yy > 0)
            dy = 1;
        else
            dy = -1, yo--;
        /* If it makes sense (if enlarging), do early CM */
        if (pss->params.early_cm && !penum->icc_link->is_identity
            && stream_r.ptr != stream_r.limit) {
            /* Get the buffers set up. */
            p_cm_buff =
                (byte *) gs_alloc_bytes(pis->memory,
                                        num_bytes_decode * width_in * spp_cm,
                                        "image_render_interpolate_icc");
            /* Set up the buffer descriptors. We keep the bytes the same */
            gsicc_init_buffer(&input_buff_desc, spp_decode, num_bytes_decode,
                          false, false, false, 0, width_in * spp_decode,
                          1, width_in);
            gsicc_init_buffer(&output_buff_desc, spp_cm, num_bytes_decode,
                          false, false, false, 0, width_in * spp_cm,
                          1, width_in);
            /* Do the transformation */
            psrc = (byte*) (stream_r.ptr + 1);
            (penum->icc_link->procs.map_buffer)(dev, penum->icc_link, &input_buff_desc,
                                                &output_buff_desc, (void*) psrc,
                                                (void*) p_cm_buff);
            /* Re-set the reading stream to use the cm data */
            stream_r.ptr = p_cm_buff - 1;
            stream_r.limit = stream_r.ptr + num_bytes_decode * width_in * spp_cm;
        } else {
            /* CM after interpolation (or none).  Just set up the buffers
               if needed.  16 bit operations if CM takes place.  */
            if (!penum->icc_link->is_identity) {
                p_cm_buff = (byte *) gs_alloc_bytes(pis->memory,
                    sizeof(unsigned short) * width * spp_cm,
                    "image_render_interpolate_icc");
                /* Set up the buffer descriptors. */
                gsicc_init_buffer(&input_buff_desc, spp_decode, 2,
                              false, false, false, 0, width * spp_decode,
                              1, width);
                gsicc_init_buffer(&output_buff_desc, spp_cm, 2,
                              false, false, false, 0, width * spp_cm,
                              1, width);
            }
        }
        for (;;) {
            int ry = yo + penum->line_xy * dy;
            int x;
            const unsigned short *pinterp;
            gx_device_color devc;
            int status;

            DECLARE_LINE_ACCUM_COPY(out, bpp, xo);
            stream_w.limit = out + width *
                max(spp_interp * sizeofPixelOut, arch_sizeof_color_index) - 1;
            stream_w.ptr = stream_w.limit - width * spp_interp * sizeofPixelOut;
            pinterp = (const unsigned short *)(stream_w.ptr + 1);
            /* This is where the rescale takes place; this will consume the
             * data from stream_r, and post processed data into stream_w. The
             * data in stream_w may be bogus if we are outside the active
             * region, and this will be indiated by pss->params.Active being
             * set to false. */
            status = (*pss->templat->process)
                ((stream_state *) pss, &stream_r, &stream_w, h == 0);
            if (status < 0 && status != EOFC)
                return_error(gs_error_ioerror);
            if (stream_w.ptr == stream_w.limit) {
                int xe = xo + pss->params.PatchWidthOut;

                /* Are we active? (i.e. in the render rectangle) */
                if (!pss->params.Active)
                    goto inactive;
                if_debug1('B', "[B]Interpolated row %d:\n[B]",
                          penum->line_xy);
                /* Take care of CM on the entire interpolated row, if we
                   did not already do CM */
                if (penum->icc_link->is_identity || pss->params.early_cm) {
                    /* Fastest case. No CM needed */
                    p_cm_interp = (unsigned short *) pinterp;
                } else {
                    /* Transform */
                    p_cm_interp = (unsigned short *) p_cm_buff;
                    (penum->icc_link->procs.map_buffer)(dev, penum->icc_link,
                                                        &input_buff_desc,
                                                        &output_buff_desc,
                                                        (void*) pinterp,
                                                        (void*) p_cm_interp);
                }
                p_cm_interp += pss->params.LeftMarginOut * spp_cm;
                for (x = xo; x < xe;) {
#ifdef DEBUG
                    if (gs_debug_c('B')) {
                        int ci;

                        for (ci = 0; ci < spp_cm; ++ci)
                            dprintf2("%c%04x", (ci == 0 ? ' ' : ','),
                                     p_cm_interp[ci]);
                    }
#endif
                    /* Get the device color */
                    /* Now we can do an encoding directly or we have to apply transfer
                       and or halftoning */
                    if (must_halftone || has_transfer) {
                        /* We need to do the tranfer function and/or the halftoning */
                        cmap_transfer_halftone(p_cm_interp, &devc, pis, dev,
                            has_transfer, must_halftone, gs_color_select_source);
                    } else {
                        /* encode as a color index. avoid all the cv to frac to cv
                           conversions */
                        color = dev_proc(dev, encode_color)(dev, p_cm_interp);
                        /* check if the encoding was successful; we presume failure is rare */
                        if (color != gx_no_color_index)
                            color_set_pure(&devc, color);
                    }
                    if (color_is_pure(&devc)) {
                        /* Just pack colors into a scan line. */
                        gx_color_index color = devc.colors.pure;
                        /* Skip runs quickly for the common cases. */
                        switch (spp_cm) {
                            case 1:
                                do {
                                    LINE_ACCUM(color, bpp);
                                    vd_pixel(int2fixed(x), int2fixed(ry), color);
                                    x++, p_cm_interp += 1;
                                } while (x < xe && p_cm_interp[-1] == p_cm_interp[0]);
                                break;
                            case 3:
                                do {
                                    LINE_ACCUM(color, bpp);
                                    vd_pixel(int2fixed(x), int2fixed(ry), color);
                                    x++, p_cm_interp += 3;
                                } while (x < xe && p_cm_interp[-3] == p_cm_interp[0] &&
                                     p_cm_interp[-2] == p_cm_interp[1] &&
                                     p_cm_interp[-1] == p_cm_interp[2]);
                                break;
                            case 4:
                                do {
                                    LINE_ACCUM(color, bpp);
                                    x++, p_cm_interp += 4;
                                } while (x < xe && p_cm_interp[-4] == p_cm_interp[0] &&
                                     p_cm_interp[-3] == p_cm_interp[1] &&
                                     p_cm_interp[-2] == p_cm_interp[2] &&
                                     p_cm_interp[-1] == p_cm_interp[3]);
                                break;
                            default:
                                LINE_ACCUM(color, bpp);
                                x++, p_cm_interp += spp_cm;
                        }
                    } else {
                        int rcode;

                        LINE_ACCUM_COPY(dev, out, bpp, xo, x, raster, ry);
                        rcode = gx_fill_rectangle_device_rop(x, ry,
                                                     1, 1, &devc, dev, lop);
                        if (rcode < 0)
                            return rcode;
                        LINE_ACCUM_SKIP(bpp);
                        l_xprev = x + 1;
                        x++, p_cm_interp += spp_cm;
                    }
                }  /* End on x loop */
                LINE_ACCUM_COPY(dev, out, bpp, xo, x, raster, ry);
                /*if_debug1('w', "[w]Y=%d:\n", ry);*/ /* See siscale.c about 'w'. */
inactive:
                penum->line_xy++;
                if_debug0('B', "\n");
            }
            if ((status == 0 && stream_r.ptr == stream_r.limit) || status == EOFC)
                break;
        }
        /* Free cm buffer, if it was used */
        if (p_cm_buff != NULL) {
            gs_free_object(pis->memory, (byte *)p_cm_buff,
                           "image_render_interpolate_icc");
        }
    }
    return (h == 0 ? 0 : 1);
}

/* Decode a 16-bit sample into a floating point color component.
   This is used for cases where the spatial interpolation function output is 16 bit.
   It is only used here, hence the static declaration for now. */

static void
decode_sample_frac_to_float(gx_image_enum *penum, frac sample_value, gs_client_color *cc, int i)
{
    switch ( penum->map[i].decoding )
    {
        case sd_none:
            cc->paint.values[i] = frac2float(sample_value);
            break;
        case sd_lookup:
            cc->paint.values[i] =
                   penum->map[i].decode_lookup[(frac2byte(sample_value)) >> 4];
            break;
        case sd_compute:
            cc->paint.values[i] =
                   penum->map[i].decode_base + frac2float(sample_value)*255.0 * penum->map[i].decode_factor;
    }
}
