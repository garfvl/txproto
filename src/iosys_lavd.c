#include <stdatomic.h>
#include <libavdevice/avdevice.h>
#include <libavutil/avstring.h>
#include <libavutil/time.h>
#include <libavutil/crc.h>

#include "iosys_common.h"
#include "utils.h"

#include "../config.h"

typedef struct LavdCtx {
    AVClass *class;
    int log_lvl_offset;

    SPBufferList *events;
    SPBufferList *entries;

    atomic_int quit;
    pthread_t source_update;
} LavdCtx;

typedef struct LavdCaptureCtx {
    LavdCtx *main;
    AVBufferRef *main_ref;

    char *src_name;
    AVInputFormat *src;
    AVFormatContext *avf;
    AVCodecContext *avctx;

    int64_t delay;
    int64_t epoch;

    atomic_bool quit;
    pthread_t pull_thread;
} LavdCaptureCtx;

static void *lavd_thread(void *s)
{
    int64_t pts;
    int err = 0, flushed = 0;
    IOSysEntry *entry = s;
    LavdCaptureCtx *priv = entry->io_priv;

    sp_set_thread_name_self(priv->avf->iformat->name);

    while (!flushed) {
        AVPacket *pkt = NULL;
        if (atomic_load(&priv->quit))
            goto send;

        pkt = av_packet_alloc();

        err = av_read_frame(priv->avf, pkt);
        if (err) {
            av_log(entry, AV_LOG_ERROR, "Unable to read frame: %s!\n", av_err2str(err));
            goto end;
        }

        if (!priv->delay)
            priv->delay = av_gettime_relative() - priv->epoch - pkt->pts;
        pkt->pts += priv->delay;

send:
        pts = pkt ? pkt->pts : AV_NOPTS_VALUE;

        /* Send frame for decoding */
        err = avcodec_send_packet(priv->avctx, pkt);
        av_packet_free(&pkt);
        if (err == AVERROR_EOF) {
            av_log(entry, AV_LOG_INFO, "Decoder flushed!\n");
            err = 0;
            break; /* decoder flushed */
        } else if (err && (err != AVERROR(EAGAIN))) {
            av_log(entry, AV_LOG_ERROR, "Unable to decode frame: %s!\n", av_err2str(err));
            goto end;
        }

        AVFrame *frame = av_frame_alloc();
        err = avcodec_receive_frame(priv->avctx, frame);
        if (err == AVERROR_EOF) {
            av_log(entry, AV_LOG_INFO, "Decoder flushed!\n");
            av_frame_free(&frame);
            err = 0;
            break;
        } else if (err && (err != AVERROR(EAGAIN))) {
            av_log(entry, AV_LOG_ERROR, "Unable to get decoded frame: %s!\n", av_err2str(err));
            av_frame_free(&frame);
            goto end;
        }

        if (frame) {
            /* avcodec_open2 changes the timebase */
            frame->pts = av_rescale_q(pts, priv->avf->streams[0]->time_base, priv->avctx->time_base);

            frame->opaque_ref = av_buffer_allocz(sizeof(FormatExtraData));

            FormatExtraData *fe = (FormatExtraData *)frame->opaque_ref->data;
            fe->time_base       = priv->avctx->time_base;
            fe->avg_frame_rate  = priv->avctx->framerate;

            sp_frame_fifo_push(entry->frames, frame);
            av_frame_free(&frame);
        }
    }

end:
    return NULL;
}

typedef struct LavdIOCtrlCtx {
    enum SPEventType ctrl;
    AVDictionary *opts;
    atomic_int_fast64_t *epoch;
} LavdIOCtrlCtx;

static int lavd_ioctx_ctrl_cb(AVBufferRef *opaque, void *src_ctx)
{
    LavdIOCtrlCtx *event = (LavdIOCtrlCtx *)opaque->data;

    IOSysEntry *entry = src_ctx;
    LavdCaptureCtx *priv = entry->io_priv;

    if (event->ctrl & SP_EVENT_CTRL_START) {
        priv->epoch = atomic_load(event->epoch);
        pthread_create(&priv->pull_thread, NULL, lavd_thread, entry);
        return 0;
    } else if (event->ctrl & SP_EVENT_CTRL_STOP) {
        atomic_store(&priv->quit, 1);
        pthread_join(priv->pull_thread, NULL);
        return 0;
    } else {
        return AVERROR(ENOTSUP);
    }
}

static int lavd_ioctx_ctrl(AVBufferRef *entry, enum SPEventType ctrl, void *arg)
{
    IOSysEntry *iosys_entry = (IOSysEntry *)entry->data;

    if (ctrl & SP_EVENT_CTRL_COMMIT) {
        return sp_bufferlist_dispatch_events(iosys_entry->events, iosys_entry, SP_EVENT_ON_COMMIT);
    } else if (ctrl & SP_EVENT_CTRL_DISCARD) {
        sp_bufferlist_discard_new_events(iosys_entry->events);
        return 0;
    } else if (ctrl & SP_EVENT_CTRL_OPTS) {
        AVDictionary *dict = arg;
    } else if (ctrl & ~(SP_EVENT_CTRL_START | SP_EVENT_CTRL_STOP)) {
        return AVERROR(ENOTSUP);
    }

    SP_EVENT_BUFFER_CTX_ALLOC(LavdIOCtrlCtx, ctrl_ctx, av_buffer_default_free, NULL)

    ctrl_ctx->ctrl = ctrl;
    if (ctrl & SP_EVENT_CTRL_OPTS)
        av_dict_copy(&ctrl_ctx->opts, arg, 0);
    if (ctrl & SP_EVENT_CTRL_START)
        ctrl_ctx->epoch = arg;

    if (ctrl & SP_EVENT_FLAG_IMMEDIATE) {
        int ret = lavd_ioctx_ctrl_cb(ctrl_ctx_ref, iosys_entry);
        av_buffer_unref(&ctrl_ctx_ref);
        return ret;
    }

    enum SPEventType flags = SP_EVENT_FLAG_ONESHOT | SP_EVENT_ON_COMMIT | ctrl;
    AVBufferRef *ctrl_event = sp_event_create(lavd_ioctx_ctrl_cb, NULL,
                                              flags, ctrl_ctx_ref,
                                              sp_event_gen_identifier(iosys_entry, NULL, flags));

    char *fstr = sp_event_flags_to_str_buf(ctrl_event);
    av_log(iosys_entry, AV_LOG_DEBUG, "Registering new event (%s)!\n", fstr);
    av_free(fstr);

    int err = sp_bufferlist_append_event(iosys_entry->events, ctrl_event);
    av_buffer_unref(&ctrl_event);
    if (err < 0)
        return err;

    return 0;
}

static int lavd_init_io(AVBufferRef *ctx_ref, AVBufferRef *entry,
                        AVDictionary *opts)
{
    int err = 0;
    LavdCtx *ctx = (LavdCtx *)ctx_ref->data;

    IOSysEntry *iosys_entry = (IOSysEntry *)entry->data;

    LavdCaptureCtx *priv = av_mallocz(sizeof(*priv));
    if (!priv)
        return AVERROR(ENOMEM);

    priv->main = ctx;
    priv->quit = ATOMIC_VAR_INIT(0);
    priv->src = (AVInputFormat *)iosys_entry->api_priv;
    priv->src_name = av_strdup(priv->src->name);

    err = avformat_open_input(&priv->avf, iosys_entry->name, priv->src, &opts);
    if (err) {
        av_log(ctx, AV_LOG_ERROR, "Unable to open context for source \"%s\": %s\n",
               priv->src->name, av_err2str(err));
        return err;
    }

    err = avformat_find_stream_info(priv->avf, NULL);
    if (err) {
        av_log(ctx, AV_LOG_ERROR, "Unable to get stream info for source \"%s\": %s\n",
               priv->src->name, av_err2str(err));
        return err;
    }

    AVCodecParameters *codecpar = priv->avf->streams[0]->codecpar;

    AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);

    priv->avctx = avcodec_alloc_context3(codec);

    avcodec_parameters_to_context(priv->avctx, codecpar);
    priv->avctx->time_base = priv->avf->streams[0]->time_base;
    priv->avctx->framerate = priv->avf->streams[0]->avg_frame_rate;

    err = avcodec_open2(priv->avctx, codec, NULL);
	if (err) {
		av_log(ctx, AV_LOG_ERROR, "Cannot open encoder: %s!\n", av_err2str(err));
		return err;
	}

    iosys_entry->io_priv = priv;
    iosys_entry->frames = sp_frame_fifo_create(iosys_entry, 0, 0);
    iosys_entry->ctrl = lavd_ioctx_ctrl;
    iosys_entry->events = sp_bufferlist_new();
    priv->main_ref = av_buffer_ref(ctx_ref);
    priv->main = (LavdCtx *)priv->main_ref->data;

    return 0;
}

static const char *blacklist[] = {
#ifdef HAVE_PULSEAUDIO
    "pulse",
    "alsa",
    "sndio",
    "oss",
#endif
};

static void destroy_entry(void *opaque, uint8_t *data)
{
    LavdCtx *ctx = (LavdCtx *)opaque;
    IOSysEntry *entry = (IOSysEntry *)data;

    if (entry->io_priv) {
        LavdCaptureCtx *priv = entry->io_priv;

        atomic_store(&priv->quit, 1);
        pthread_join(priv->pull_thread, NULL);

        /* EOS */
        sp_frame_fifo_push(entry->frames, NULL);

        /* Free */
        avcodec_free_context(&priv->avctx);
        avformat_flush(priv->avf);
        avformat_close_input(&priv->avf);
        av_free(priv->src_name);
    }

    sp_bufferlist_free(&entry->events);

    av_free(entry->name);
    av_free(entry->desc);
    sp_free_class(entry);
}

static void mod_device(LavdCtx *ctx, AVInputFormat *cur, AVDeviceInfo *dev_info,
                       AVClassCategory category)
{
    const AVCRC *crc_tab = av_crc_get_table(AV_CRC_32_IEEE);
    uint32_t src_crc = av_crc(crc_tab, UINT32_MAX, (void *)&cur, sizeof(cur));

    src_crc = av_crc(crc_tab, src_crc, (void *)&dev_info, sizeof(dev_info));

    uint32_t idx = sp_iosys_gen_identifier(ctx, src_crc, category);

    IOSysEntry *entry;
    AVBufferRef *entry_ref = sp_bufferlist_ref(ctx->entries,
                                               sp_bufferlist_iosysentry_by_id,
                                               &idx);

    if (!entry_ref) {
        entry = av_mallocz(sizeof(*entry));
        entry_ref = av_buffer_create((uint8_t *)entry, sizeof(*entry),
                                     destroy_entry, ctx, 0);

        if (!dev_info) {
            entry->name = av_strdup(cur->name);
            entry->desc = av_strdup(cur->long_name);
        } else {
            entry->name = av_strdup(dev_info->device_name);
            entry->desc = av_strdup(dev_info->device_description);
        }

        entry->api_priv = cur;
        entry->events = sp_bufferlist_new();
        entry->parent = ctx;
        entry->identifier = idx;

        sp_alloc_class(entry, cur->name, category,
                       &entry->log_lvl_offset, &entry->parent);

        sp_bufferlist_dispatch_events(ctx->events, entry_ref->data,
                                      SP_EVENT_ON_CHANGE | category);
        sp_bufferlist_append(ctx->entries, entry_ref);
    } else {
        IOSysEntry *entry = (IOSysEntry *)entry_ref->data;
        int change = 0;
        if (cur->name && strcmp(entry->name, cur->name))
            change |= 1;
        if (cur->long_name && strcmp(entry->desc, cur->long_name))
            change |= 1;
        if (change)
            sp_bufferlist_dispatch_events(ctx->events, entry_ref->data,
                                          SP_EVENT_ON_CHANGE | category);
    }
}

static void iter_sources(LavdCtx *ctx, AVInputFormat *(*iter)(AVInputFormat *),
                         AVClassCategory category)
{
    AVInputFormat *cur = NULL;

start:
    while ((cur = iter(cur))) {
        AVDeviceInfoList *list = NULL;

        if (cur->priv_class->category != category)
            continue;

        for (int i = 0; i < SP_ARRAY_ELEMS(blacklist); i++)
            if (!strncmp(cur->name, blacklist[i], strlen(blacklist[i])))
                goto start;

        int err = avdevice_list_input_sources(cur, NULL, NULL, &list);
        if ((err && (err != AVERROR(ENOSYS)))) {
            av_log(ctx, AV_LOG_DEBUG, "Unable to retrieve device list for source \"%s\": %s\n",
                   cur->name, av_err2str(err));
            continue;
        }

        if (!list || (err == AVERROR(ENOSYS))) {
            mod_device(ctx, cur, NULL, category);
            continue;
        }

        int nb_devs = list->nb_devices;
        if (!nb_devs) {
            av_log(ctx, AV_LOG_DEBUG, "Device \"%s\" has no entries in its devices list.\n", cur->name);
            avdevice_free_list_devices(&list);
            continue;
        }

        for (int i = 0; i < nb_devs; i++) {
            AVDeviceInfo *info = list->devices[i];
            mod_device(ctx, cur, info, category);
        }

        avdevice_free_list_devices(&list);
    }
}

static void update_entries(LavdCtx *ctx)
{
    iter_sources(ctx, av_input_video_device_next,
                 AV_CLASS_CATEGORY_DEVICE_VIDEO_INPUT);

    iter_sources(ctx, av_input_audio_device_next,
                 AV_CLASS_CATEGORY_DEVICE_AUDIO_INPUT);
}

static void *source_update_thread(void *s)
{
    LavdCtx *ctx = s;

    while (!atomic_load(&ctx->quit)) {
        update_entries(ctx);
        av_usleep(500000);
    }

    return 0;
}

static int lavd_ctrl(AVBufferRef *ctx_ref, enum SPEventType ctrl, void *arg)
{
    int err = 0;
    LavdCtx *ctx = (LavdCtx *)ctx_ref->data;

    if (ctrl & SP_EVENT_CTRL_NEW_EVENT) {
        AVBufferRef *event = arg;
        char *fstr = sp_event_flags_to_str_buf(event);
        av_log(ctx, AV_LOG_DEBUG, "Registering new event (%s)!\n", fstr);
        av_free(fstr);

        if (ctrl & SP_EVENT_FLAG_IMMEDIATE) {
            /* Bring up the new event to speed with current affairs */
            SPBufferList *tmp_event = sp_bufferlist_new();
            sp_bufferlist_append_event(tmp_event, event);

            update_entries(ctx);

            AVBufferRef *obj = NULL;
            while ((obj = sp_bufferlist_iter_ref(ctx->entries))) {
                sp_bufferlist_dispatch_events(tmp_event, obj->data, SP_EVENT_ON_CHANGE | SP_EVENT_TYPE_SOURCE);
                av_buffer_unref(&obj);
            }

            sp_bufferlist_free(&tmp_event);
        }

        /* Add it to the list now to receive events dynamically */
        err = sp_bufferlist_append_event(ctx->events, event);
        if (err < 0)
            return err;
    }

    return 0;
}

static AVBufferRef *lavd_ref_entry(AVBufferRef *ctx_ref, uint32_t identifier)
{
    LavdCtx *ctx = (LavdCtx *)ctx_ref->data;
    return sp_bufferlist_pop(ctx->entries, sp_bufferlist_iosysentry_by_id, &identifier);
}

static void lavd_uninit(void *opaque, uint8_t *data)
{
    LavdCtx *ctx = (LavdCtx *)data;

    atomic_store(&ctx->quit, 1);
    pthread_join(ctx->source_update, NULL);

    sp_free_class(ctx);
    av_free(ctx);
}

static int lavd_init(AVBufferRef **s)
{
    int err = 0, locked = 0;
    LavdCtx *ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return AVERROR(ENOMEM);

    AVBufferRef *ctx_ref = av_buffer_create((uint8_t *)ctx, sizeof(*ctx),
                                            lavd_uninit, NULL, 0);
    if (!ctx_ref) {
        av_free(ctx);
        return AVERROR(ENOMEM);
    }

    ctx->entries = sp_bufferlist_new();
    if (!ctx->entries) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    ctx->events = sp_bufferlist_new();
    if (!ctx->events) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    ctx->class = av_mallocz(sizeof(*ctx->class));
    if (!ctx->class) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    err = sp_alloc_class(ctx, "libavdevice", AV_CLASS_CATEGORY_NA,
                         &ctx->log_lvl_offset, NULL);
    if (err < 0)
        goto fail;

    avdevice_register_all();

    ctx->quit = ATOMIC_VAR_INIT(0);
    pthread_create(&ctx->source_update, NULL, source_update_thread, ctx);

    *s = ctx_ref;

    return 0;

fail:
    av_buffer_unref(&ctx_ref);

    return err;
}

const IOSysAPI src_lavd = {
    .name      = "libavdevice",
    .ctrl      = lavd_ctrl,
    .init_sys  = lavd_init,
    .ref_entry = lavd_ref_entry,
    .init_io   = lavd_init_io,
};
