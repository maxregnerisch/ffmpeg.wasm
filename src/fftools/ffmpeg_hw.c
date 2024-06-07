/*
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

#include <string.h>

#include "libavutil/avstring.h"
#include "libavutil/pixdesc.h"
#include "libavfilter/buffersink.h"
#include "libavutil/opt.h"
#include "libavutil/channel_layout.h"

#include "ffmpeg.h"

static int nb_hw_devices;
static HWDevice **hw_devices;

static HWDevice *hw_device_get_by_type(enum AVHWDeviceType type)
{
    HWDevice *found = NULL;
    int i;
    for (i = 0; i < nb_hw_devices; i++) {
        if (hw_devices[i]->type == type) {
            if (found)
                return NULL;
            found = hw_devices[i];
        }
    }
    return found;
}

HWDevice *hw_device_get_by_name(const char *name)
{
    int i;
    for (i = 0; i < nb_hw_devices; i++) {
        if (!strcmp(hw_devices[i]->name, name))
            return hw_devices[i];
    }
    return NULL;
}

static HWDevice *hw_device_add(void)
{
    int err;
    err = av_reallocp_array(&hw_devices, nb_hw_devices + 1,
                            sizeof(*hw_devices));
    if (err) {
        nb_hw_devices = 0;
        return NULL;
    }
    hw_devices[nb_hw_devices] = av_mallocz(sizeof(HWDevice));
    if (!hw_devices[nb_hw_devices])
        return NULL;
    return hw_devices[nb_hw_devices++];
}

static char *hw_device_default_name(enum AVHWDeviceType type)
{
    const char *type_name = av_hwdevice_get_type_name(type);
    char *name;
    size_t index_pos;
    int index, index_limit = 1000;
    index_pos = strlen(type_name);
    name = av_malloc(index_pos + 4);
    if (!name)
        return NULL;
    for (index = 0; index < index_limit; index++) {
        snprintf(name, index_pos + 4, "%s%d", type_name, index);
        if (!hw_device_get_by_name(name))
            break;
    }
    if (index >= index_limit) {
        av_freep(&name);
        return NULL;
    }
    return name;
}

int hw_device_init_from_string(const char *arg, HWDevice **dev_out)
{
    AVDictionary *options = NULL;
    const char *type_name = NULL, *name = NULL, *device = NULL;
    enum AVHWDeviceType type;
    HWDevice *dev, *src;
    AVBufferRef *device_ref = NULL;
    int err;
    const char *errmsg, *p, *q;
    size_t k;

    k = strcspn(arg, ":=@");
    p = arg + k;

    type_name = av_strndup(arg, k);
    if (!type_name) {
        err = AVERROR(ENOMEM);
        goto fail;
    }
    type = av_hwdevice_find_type_by_name(type_name);
    if (type == AV_HWDEVICE_TYPE_NONE) {
        errmsg = "unknown device type";
        goto invalid;
    }

    if (*p == '=') {
        k = strcspn(p + 1, ":@,");

        name = av_strndup(p + 1, k);
        if (!name) {
            err = AVERROR(ENOMEM);
            goto fail;
        }
        if (hw_device_get_by_name(name)) {
            errmsg = "named device already exists";
            goto invalid;
        }

        p += 1 + k;
    } else {
        name = hw_device_default_name(type);
        if (!name) {
            err = AVERROR(ENOMEM);
            goto fail;
        }
    }

    if (!*p) {
        err = av_hwdevice_ctx_create(&device_ref, type,
                                     NULL, NULL, 0);
        if (err < 0)
            goto fail;

    } else if (*p == ':') {
        ++p;
        q = strchr(p, ',');
        if (q) {
            if (q - p > 0) {
                device = av_strndup(p, q - p);
                if (!device) {
                    err = AVERROR(ENOMEM);
                    goto fail;
                }
            }
            err = av_dict_parse_string(&options, q + 1, "=", ",", 0);
            if (err < 0) {
                errmsg = "failed to parse options";
                goto invalid;
            }
        }

        err = av_hwdevice_ctx_create(&device_ref, type,
                                     q ? device : p[0] ? p : NULL,
                                     options, 0);
        if (err < 0)
            goto fail;

    } else if (*p == '@') {
        src = hw_device_get_by_name(p + 1);
        if (!src) {
            errmsg = "invalid source device name";
            goto invalid;
        }

        err = av_hwdevice_ctx_create_derived(&device_ref, type,
                                             src->device_ref, 0);
        if (err < 0)
            goto fail;
    } else if (*p == ',') {
        err = av_dict_parse_string(&options, p + 1, "=", ",", 0);

        if (err < 0) {
            errmsg = "failed to parse options";
            goto invalid;
        }

        err = av_hwdevice_ctx_create(&device_ref, type,
                                     NULL, options, 0);
        if (err < 0)
            goto fail;
    } else {
        errmsg = "parse error";
        goto invalid;
    }

    dev = hw_device_add();
    if (!dev) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    dev->name = name;
    dev->type = type;
    dev->device_ref = device_ref;

    if (dev_out)
        *dev_out = dev;

    name = NULL;
    err = 0;
done:
    av_freep(&type_name);
    av_freep(&name);
    av_freep(&device);
    av_dict_free(&options);
    return err;
invalid:
    av_log(NULL, AV_LOG_ERROR,
           "Invalid device specification \"%s\": %s\n", arg, errmsg);
    err = AVERROR(EINVAL);
    goto done;
fail:
    av_log(NULL, AV_LOG_ERROR,
           "Device creation failed: %d.\n", err);
    av_buffer_unref(&device_ref);
    goto done;
}

static int hw_device_init_from_type(enum AVHWDeviceType type,
                                    const char *device,
                                    HWDevice **dev_out)
{
    AVBufferRef *device_ref = NULL;
    HWDevice *dev;
    char *name;
    int err;

    name = hw_device_default_name(type);
    if (!name) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    err = av_hwdevice_ctx_create(&device_ref, type, device, NULL, 0);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Device creation failed: %d.\n", err);
        goto fail;
    }

    dev = hw_device_add();
    if (!dev) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    dev->name = name;
    dev->type = type;
    dev->device_ref = device_ref;

    if (dev_out)
        *dev_out = dev;

    return 0;

fail:
    av_freep(&name);
    av_buffer_unref(&device_ref);
    return err;
}

void hw_device_free_all(void)
{
    int i;
    for (i = 0; i < nb_hw_devices; i++) {
        av_freep(&hw_devices[i]->name);
        av_buffer_unref(&hw_devices[i]->device_ref);
        av_freep(&hw_devices[i]);
    }
    av_freep(&hw_devices);
    nb_hw_devices = 0;
}

static HWDevice *hw_device_match_by_codec(const AVCodec *codec)
{
    const AVCodecHWConfig *config;
    HWDevice *dev;
    int i;
    for (i = 0;; i++) {
        config = avcodec_get_hw_config(codec, i);
        if (!config)
            return NULL;
        if (!(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
            continue;
        dev = hw_device_get_by_type(config->device_type);
        if (dev)
            return dev;
    }
}

int hw_device_setup_for_decode(InputStream *ist)
{
    const AVCodecHWConfig *config;
    enum AVHWDeviceType type;
    HWDevice *dev = NULL;
    int i, err;

    if (ist->hwaccel_id == HWACCEL_NONE)
        return 0;

    for (i = 0;; i++) {
        config = avcodec_get_hw_config(ist->dec_ctx->codec, i);
        if (!config) {
            av_log(NULL, AV_LOG_ERROR, "Decoder does not support any device type\n");
            return AVERROR(ENOSYS);
        }
        if (!(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
            continue;
        if (config->device_type == ist->hwaccel_device_type) {
            dev = hw_device_get_by_type(config->device_type);
            if (dev)
                break;
        }
    }
    if (!dev) {
        av_log(NULL, AV_LOG_ERROR, "No suitable hwaccel device found\n");
        return AVERROR(EINVAL);
    }

    err = avcodec_set_hw_device_ctx(ist->dec_ctx, dev->device_ref);
    if (err < 0)
        return err;

    ist->hwaccel_device_ref = av_buffer_ref(dev->device_ref);
    if (!ist->hwaccel_device_ref)
        return AVERROR(ENOMEM);

    return 0;
}

static void set_hwframe_ctx(OutputStream *ost, const AVCodec *codec,
                            const AVCodecContext *enc_ctx,
                            HWDevice *dev)
{
    const AVCodecHWConfig *config;
    int i, err;
    for (i = 0;; i++) {
        config = avcodec_get_hw_config(codec, i);
        if (!config)
            return;
        if (!(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX))
            continue;
        if (config->device_type != dev->type)
            continue;
        err = avcodec_get_hw_frames_parameters(enc_ctx, dev->device_ref,
                                               config->pix_fmt,
                                               &ost->hw_frames_ctx);
        if (err < 0)
            return;
        return;
    }
}

static int hw_device_setup_for_encode(OutputStream *ost)
{
    HWDevice *dev;
    int err;

    dev = hw_device_match_by_codec(ost->enc_ctx->codec);
    if (!dev)
        return 0;

    set_hwframe_ctx(ost, ost->enc_ctx->codec, ost->enc_ctx, dev);
    if (!ost->hw_frames_ctx)
        return 0;

    err = avcodec_set_hw_frames_ctx(ost->enc_ctx, ost->hw_frames_ctx);
    if (err < 0) {
        av_buffer_unref(&ost->hw_frames_ctx);
        return err;
    }
    return 0;
}

int hw_device_setup_for_filter(FilterGraph *fg)
{
    AVFilterContext *sink = fg->graph->sink;
    const AVCodec *codec;
    HWDevice *dev;
    int i, err;
    if (sink->nb_inputs == 0)
        return 0;
    codec = avcodec_find_decoder(sink->inputs[0]->type);
    if (!codec)
        return 0;
    dev = hw_device_match_by_codec(codec);
    if (!dev)
        return 0;
    for (i = 0; i < fg->graph->nb_filters; i++) {
        AVFilterContext *filter = fg->graph->filters[i];
        if (!strcmp(filter->filter->name, "hwupload")) {
            err = av_opt_set(filter, "device", dev->name, 0);
            if (err < 0)
                return err;
        }
    }
    return 0;
}

/* New functionality for audio remixing */
static int remix_audio(AVFrame *frame, const int *remix_map, int remix_size)
{
    AVFrame *remix_frame = av_frame_alloc();
    if (!remix_frame) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate remix frame.\n");
        return AVERROR(ENOMEM);
    }

    remix_frame->channel_layout = frame->channel_layout;
    remix_frame->sample_rate = frame->sample_rate;
    remix_frame->format = frame->format;
    remix_frame->nb_samples = frame->nb_samples;

    if (av_frame_get_buffer(remix_frame, 0) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate remix frame buffer.\n");
        av_frame_free(&remix_frame);
        return AVERROR(ENOMEM);
    }

    for (int i = 0; i < remix_size; ++i) {
        memcpy(remix_frame->data[i], frame->data[remix_map[i]], remix_frame->linesize[0]);
    }

    av_frame_copy_props(remix_frame, frame);
    av_frame_unref(frame);
    av_frame_move_ref(frame, remix_frame);

    return 0;
}

int apply_audio_remix(InputStream *ist, const int *remix_map, int remix_size)
{
    int ret;
    AVFrame *frame = NULL;

    while ((ret = av_read_frame(ist->avf_ctx, ist->pkt)) >= 0) {
        if (ist->pkt->stream_index == ist->st->index) {
            ret = avcodec_send_packet(ist->dec_ctx, ist->pkt);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Error sending packet for decoding.\n");
                break;
            }

            while ((ret = avcodec_receive_frame(ist->dec_ctx, frame)) >= 0) {
                ret = remix_audio(frame, remix_map, remix_size);
                if (ret < 0) {
                    av_log(NULL, AV_LOG_ERROR, "Error remixing audio.\n");
                    break;
                }

                // Process the remixed frame (e.g., encode, filter, etc.)
                // ...
            }
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                continue;
            else if (ret < 0)
                break;
        }
        av_packet_unref(ist->pkt);
    }

    av_frame_free(&frame);
    return ret;
}

