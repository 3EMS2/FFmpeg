/*
 * VVC video Decoder
 *
 * Copyright (C) 2012 - 2021 Pierre-Loup Cabarat
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

#include <ovdec.h>
#include <ovdefs.h>
#include <ovunits.h>
#include <ovframe.h>
#include <ovlog.h>

#include "libavutil/attributes.h"
#include "libavutil/opt.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "profiles.h"
#include "h2645_parse.h"
#include "vvc.h"

typedef OVDec OVVCDec;

struct OVDecContext{
     AVClass *c;
     OVDec* libovvc_dec;
     int nal_length_size;
     int is_nalff;
     int64_t log_level;
     int64_t nb_entry_th;
     int64_t nb_frame_th;
     int64_t brightness;
     int64_t pprocess;
};

#define OFFSET(x) offsetof(struct OVDecContext, x)
#define PAR (AV_OPT_FLAG_DECODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)

static const AVOption options[] = {
    { "threads_frame", "Maximum number of frames being decoded in parallel", OFFSET(nb_frame_th),
        AV_OPT_TYPE_INT, {.i64 = 0}, 0, 16, PAR },
    { "threads_tile", "Number of threads to be used on entries", OFFSET(nb_entry_th),
        AV_OPT_TYPE_INT, {.i64 = 0}, 0, 16, PAR },
    { "log_level", "Verbosity of OpenVVC decoder", OFFSET(log_level),
        AV_OPT_TYPE_INT, {.i64 = 1}, 0, 6, PAR , "ll"},
    { "error", "Verbosity of OpenVVC decoder", 0,
        AV_OPT_TYPE_CONST, {.i64 = 1}, 0, 6, PAR , "ll"},
    { "warning", "Verbosity of OpenVVC decoder", 0,
        AV_OPT_TYPE_CONST, {.i64 = 2}, 0, 6, PAR , "ll"},
    { "info", "Verbosity of OpenVVC decoder", 0,
        AV_OPT_TYPE_CONST, {.i64 = 3}, 0, 6, PAR , "ll"},
    { "verbose", "Verbosity of OpenVVC decoder", 0,
        AV_OPT_TYPE_CONST, {.i64 = 4}, 0, 6, PAR , "ll"},
    { "trace", "Verbosity of OpenVVC decoder", 0,
        AV_OPT_TYPE_CONST, {.i64 = 5}, 0, 6, PAR , "ll"},
    { "debug", "Verbosity of OpenVVC decoder", 0,
        AV_OPT_TYPE_CONST, {.i64 = 6}, 0, 6, PAR , "ll"},
    { "none", "Verbosity of OpenVVC decoder", 0,
        AV_OPT_TYPE_CONST, {.i64 = 5}, 0, 6, PAR , "ll"},
    { "brightness", "Verbosity of OpenVVC decoder", OFFSET(brightness),
        AV_OPT_TYPE_INT, {.i64 = 100}, 100, 10000, PAR },
    { "nopostproc", "Verbosity of OpenVVC decoder", OFFSET(pprocess),
        AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, PAR },
    { NULL },
};

static const AVClass libovvc_decoder_class = {
    .class_name = "Open VVC decoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DECODER,
    //.log_level_offset_offset = offsetof(AVCodecContext, log_level_offset),
};

static void release_nalu(struct OVNALUnit **nalu_p)
{
    OVNALUnit *ovnalu = *nalu_p;
    if (ovnalu->rbsp_data) {
        av_freep(&ovnalu->rbsp_data);
    }

    if (ovnalu->epb_pos) {
        av_freep(&ovnalu->epb_pos);
    }
    av_freep(nalu_p);
    //*nalu_p = NULL;
}

static int copy_rpbs_info(OVNALUnit **ovnalu_p, const uint8_t *rbsp_buffer, int raw_size, const int *skipped_bytes_pos, int skipped_bytes) {


    OVNALUnit *ovnalu;
    int ret = ovnalu_create(&ovnalu);
    if (ret < 0) {
	av_log(NULL, AV_LOG_ERROR, "Could not init new OVNALUnit\n");
	return ret;
    } else {
	uint8_t *rbsp_cpy = av_malloc(raw_size + 8);
	if (rbsp_cpy) {

	    memcpy(rbsp_cpy, rbsp_buffer, raw_size);
	    rbsp_cpy[raw_size]     = 0;
	    rbsp_cpy[raw_size + 1] = 0;
	    rbsp_cpy[raw_size + 2] = 0;
	    rbsp_cpy[raw_size + 3] = 0;
	    rbsp_cpy[raw_size + 4] = 0;
	    rbsp_cpy[raw_size + 5] = 0;
	    rbsp_cpy[raw_size + 6] = 0;
	    rbsp_cpy[raw_size + 7] = 0;

	    ovnalu->rbsp_data = rbsp_cpy;
	    ovnalu->rbsp_size = raw_size;

	    if (skipped_bytes) {
		int *epb_cpy = av_malloc(skipped_bytes * sizeof (*ovnalu->epb_pos));
		if (epb_cpy) {

		    memcpy(epb_cpy, skipped_bytes_pos, skipped_bytes * sizeof (*ovnalu->epb_pos));

		    ovnalu->epb_pos = epb_cpy;
		    ovnalu->nb_epb = skipped_bytes;
		} else {
		    ovnalu_unref(&ovnalu);
		    av_freep(rbsp_cpy);
		    ret = AVERROR(ENOMEM);
		}
	    }
            ovnalu->release = release_nalu;
	} else {
            ovnalu_unref(&ovnalu);
	     ret = AVERROR(ENOMEM);
	}
    }

    *ovnalu_p = ovnalu;

    return ret;
}

static int convert_avpkt(OVPictureUnit **ovpu_p, const H2645Packet *pkt) {

    int ret = ovpu_init(ovpu_p, pkt->nb_nals);
    OVPictureUnit *ovpu = *ovpu_p;
    int i;

    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "No NAL Unit in packet.\n");
        return AVERROR(ENOMEM);
    }

    for (i = 0; i < ovpu->nb_nalus; ++i) {
         const H2645NAL *avnalu = &pkt->nals[i];
         OVNALUnit **ovnalu_p = &ovpu->nalus[i];
         int rbsp_size = avnalu->size_bits + 8 >> 3;

         copy_rpbs_info(ovnalu_p, avnalu->data, rbsp_size, avnalu->skipped_bytes_pos, avnalu->skipped_bytes);
         (*ovnalu_p)->type = avnalu->type;
    }

    return 0;
}

static void unref_ovframe(void *opaque, uint8_t *data) {

    OVFrame **frame_p = (OVFrame **)&data;
    ovframe_unref(frame_p);
}

static void convert_ovframe(AVFrame *avframe, const OVFrame *ovframe) {
    avframe->data[0] = ovframe->data[0];
    avframe->data[1] = ovframe->data[1];
    avframe->data[2] = ovframe->data[2];

    avframe->linesize[0] = ovframe->linesize[0];
    avframe->linesize[1] = ovframe->linesize[1];
    avframe->linesize[2] = ovframe->linesize[2];

    avframe->width  = ovframe->width;
    avframe->height = ovframe->height;

    avframe->color_trc       = ovframe->frame_info.color_desc.transfer_characteristics;
    avframe->color_primaries = ovframe->frame_info.color_desc.colour_primaries;
    avframe->colorspace      = ovframe->frame_info.color_desc.matrix_coeffs;

    avframe->pict_type = ovframe->frame_info.chroma_format == OV_YUV_420_P8 ? AV_PIX_FMT_YUV420P
                                                                            : AV_PIX_FMT_YUV420P10;

    avframe->buf[0] = av_buffer_create((uint8_t*)ovframe, sizeof(ovframe), unref_ovframe, NULL, 0);

}

static void
export_frame_properties(const AVFrame *const avframe, AVCodecContext *c)
{
    c->pix_fmt = avframe->pict_type;

    c->width   = avframe->width;
    c->height  = avframe->height;

    c->coded_width   = avframe->width;
    c->coded_height  = avframe->height;
}

static int libovvc_decode_frame(struct AVCodecContext *c, struct AVFrame *outdata, int *outdata_size, struct AVPacket *avpkt) {

    struct OVDecContext *dec_ctx = (struct OVDecContext *)c->priv_data;
    OVDec *libovvc_dec = dec_ctx->libovvc_dec;
    OVFrame *ovframe = NULL;
    OVPictureUnit *ovpu;
    H2645Packet pkt = {0};

    int *nb_pic_out = outdata_size;
    int ret;

    //ovdec_set_option(libovvc_dec, OVDEC_BRIGHTNESS, dec_ctx->brightness);
    //av_log(c, AV_LOG_ERROR, "Bright %ld\n", dec_ctx->brightness);
    ovdec_set_opt(libovvc_dec, "nopostproc", &dec_ctx->pprocess);


    if (!avpkt->size) {

        ret = ovdec_drain_picture(libovvc_dec, &ovframe);

        if (ovframe) {
            av_log(c, AV_LOG_TRACE, "Draining pic with POC: %d\n", ovframe->poc);

            convert_ovframe(outdata, ovframe);
            AVFrameSideData *sd_dst = av_frame_new_side_data(outdata, AV_FRAME_DATA_OPENVVC,
                                            256);
        sd_dst->data = ovframe;

            export_frame_properties(outdata, c);

            *outdata_size = 1;
        } else {
 
            av_log(c, AV_LOG_WARNING, "No pic to drain");
	}

        return 0;
    }

    *nb_pic_out = 0;

    if (avpkt->side_data_elems) {
        av_log(c, AV_LOG_WARNING, "Unsupported side data\n");
    }

    ret = ff_h2645_packet_split(&pkt, avpkt->data, avpkt->size, c, dec_ctx->is_nalff,
                                dec_ctx->nal_length_size, AV_CODEC_ID_VVC, 0, 0);
    if (ret < 0) {
        av_log(c, AV_LOG_ERROR, "Error splitting the input into NAL units.\n");
        return ret;
    }

    ret = convert_avpkt(&ovpu, &pkt);
    if (ret < 0) {
        av_log(c, AV_LOG_ERROR, "Fail to convert AVPacket to OVPictureUnit\n");
    }

    ret = ovdec_submit_picture_unit(libovvc_dec, ovpu);
    if (ret < 0) {
        av_log(c, AV_LOG_ERROR, "Error submitting picture unit\n");
        ovpu_unref(&ovpu);
        return AVERROR_INVALIDDATA;
    }

    ovdec_receive_picture(libovvc_dec, &ovframe);

    if (ovframe) {

        av_log(c, AV_LOG_TRACE, "Received pic with POC: %d\n", ovframe->poc);

        convert_ovframe(outdata, ovframe);

        export_frame_properties(outdata, c);
            AVFrameSideData *sd_dst = av_frame_new_side_data(outdata, AV_FRAME_DATA_OPENVVC,
                                            256);
        sd_dst->data = ovframe;
        *nb_pic_out = 1;
    }

    ovpu_unref(&ovpu);

    ff_h2645_packet_uninit(&pkt);

    return 0;
}

static int ov_log_level;

static void set_libovvc_log_level(int level) {
    ov_log_level = level;
}

static void libovvc_log(void* ctx, int log_level, const char* fmt, va_list vl)
{
     static const uint8_t log_level_lut[6] = {AV_LOG_ERROR, AV_LOG_WARNING, AV_LOG_INFO, AV_LOG_TRACE, AV_LOG_DEBUG, AV_LOG_VERBOSE};
     const AVClass *avcl = &libovvc_decoder_class;
     if (log_level < ov_log_level) {
         av_vlog(&avcl, log_level_lut[log_level], fmt, vl);
     }
}

static av_cold int libovvc_decode_init(AVCodecContext *c) {
    struct OVDecContext *dec_ctx = (struct OVDecContext *)c->priv_data;
    OVDec **libovvc_dec_p = (OVDec**) &dec_ctx->libovvc_dec;
    int ret;
    int nb_frame_th = dec_ctx->nb_frame_th;
    int nb_entry_th = dec_ctx->nb_entry_th;

    if (!dec_ctx->libovvc_dec) {

        set_libovvc_log_level(dec_ctx->log_level);
	c->log_level_offset =3;
        ovlog_set_log_level(dec_ctx->log_level);

        ovdec_set_log_callback(libovvc_log);

        ret = ovdec_init(libovvc_dec_p);
        if (ret < 0) {
            av_log(c, AV_LOG_ERROR, "Could not init Open VVC decoder\n");
            return AVERROR_DECODER_NOT_FOUND;
        }

        ovdec_config_threads(*libovvc_dec_p, nb_entry_th, nb_frame_th);

        ret = ovdec_start(*libovvc_dec_p);

        if (ret < 0) {

            ovdec_close(dec_ctx->libovvc_dec);

            av_log(c, AV_LOG_ERROR, "Could not init Open VVC decoder\n");

            return AVERROR_DECODER_NOT_FOUND;
        }
    }

    dec_ctx->is_nalff        = 0;
    dec_ctx->nal_length_size = 0;

    return 0;
}

static av_cold int libovvc_decode_free(AVCodecContext *c) {

    struct OVDecContext *dec_ctx = (struct OVDecContext *)c->priv_data;

    av_log(c, AV_LOG_ERROR, "Closing\n");

    ovdec_close(dec_ctx->libovvc_dec);

    dec_ctx->libovvc_dec = NULL;

    return 0;
}

static av_cold void libovvc_decode_flush(AVCodecContext *c) {
    struct OVDecContext *dec_ctx = (struct OVDecContext *)c->priv_data;
    OVDec *libovvc_dec = dec_ctx->libovvc_dec;

    av_log(c, AV_LOG_ERROR, "Flushing.\n");

    ovdec_flush(libovvc_dec);

    return;
}

const FFCodec ff_libopenvvc_decoder = {
    .p.name                  = "ovvc",
    CODEC_LONG_NAME("Open VVC(Versatile Video Coding)"),
    .p.type                  = AVMEDIA_TYPE_VIDEO,
    .p.id                    = AV_CODEC_ID_VVC,
    .priv_data_size        = sizeof(struct OVDecContext),
    .p.priv_class            = &libovvc_decoder_class,
    .init                  = libovvc_decode_init,
    .close                 = libovvc_decode_free,
    FF_CODEC_DECODE_CB(libovvc_decode_frame),
    .flush                 = libovvc_decode_flush,
    .p.capabilities          = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_OTHER_THREADS,
    .bsfs                  = "vvc_mp4toannexb",
    .p.wrapper_name          = "OpenVVC",
#if 0
    .caps_internal         = FF_CODEC_CAP_INIT_THREADSAFE | FF_CODEC_CAP_EXPORTS_CROPPING,
#endif
    .p.profiles              = NULL_IF_CONFIG_SMALL(ff_vvc_profiles),
};
