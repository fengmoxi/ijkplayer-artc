/* Copyright 2020, Alibaba
 */

// Created @2020/06/20 Sat.

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "libavformat/url.h"
#include "libavformat/avformat.h"
#include "libavformat/internal.h"
#include "libavutil/log.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"
#include <assert.h>
#include <stdbool.h>

// structure to store subscribed stream info
// use ioctl(..., "get_stream_info", ...) to fetch
struct rts_worker_demux_info
{
    // audio part
    int audio_flag; // 1 - following audio info is valid; 0 - invalid
    int audio_channels; // 1
    int audio_sample_rate; // 48000

    // video part
    int video_flag; // 1 - following video info is valid; 0 - invalid
    int video_codec;      // 1 - h264  2 - hevc
    int video_width;
    int video_height;
    int video_profile;
    int video_level;

    unsigned char spspps[10 * 1024]; // large enough
    int spspps_len; // actual bytes used in spspps
};

struct pusher_delay{
    long long cap_time_ms;  //current utc time when capture the video frame
    long long enc_time_ms;  //current utc time when the videoframe send to encoder
};

struct player_delay{
    long long decoder_time_ms;  //current utc time when the video frame send to decoder
    long long render_time_ms;   //current utc time when the video frame send to render
    unsigned long long pts;     //the video frame's pts
};

struct rts_worker_demux_info2
{
    unsigned int uid; // 0: unspecified

    // audio part
    int audio_flag; // 1 - following audio info is valid; 0 - invalid
    int audio_channels; // 1
    int audio_sample_rate; // 48000

    // video part
    int video_flag; // 1 - following video info is valid; 0 - invalid
    int video_codec;      // 1 - h264
    int video_width;
    int video_height;
    int video_profile;
    int video_level;

    unsigned char spspps[10 * 1024]; // large enough
    int spspps_len; // actual bytes used in spspps
};

// structure to store subscribed stream info
// use ioctl(..., "get_pub_info", ...) to fetch
// not implemented yet
struct rts_worker_mux_info {
    // audio part
    int audio_flag; // 1 - following audio info is valid; 0 - invalid

    // video part
    int video_flag; // 1 - following video info is valid; 0 - invalid
};

struct rts_frame {
    void *buf;              // where frame data is stored
    int size;               // size of frame data in bytes
    int is_audio;           // 1 for audio frame, 0 for video frame
    unsigned long long pts; // presentation time stamp, in ms
    unsigned long long dts; // decoding time stamp, in ms
    int flag;               // for video frame (is_audio == 0)
                            //     bit 0: key frame;
                            //     bit 1: corruption
                            //     bit 2: sps
                            //     bit 3: sps change
    int duration;           // in ms

    // use this function to free rts_frame object
    void (*free_ptr)(struct rts_frame *);

    unsigned int uid; // reserved. which user this stream attached
    struct pusher_delay delay;
};


static void free_frame(struct rts_frame *f)
{
    if(f == NULL)
        return;
    if(f->buf != NULL)
        free(f->buf);
    free(f);
}

static struct rts_frame *alloc_frame(const void *data, int len, int is_audio, unsigned int uid)
{
    struct rts_frame *f = (struct rts_frame *)malloc(sizeof(struct rts_frame));
    memset(f, 0, sizeof(struct rts_frame));
    f->size = len;
    f->buf  = malloc(f->size);
    if(data != NULL)
        memcpy(f->buf, data, len);
    f->free_ptr = free_frame;
    f->is_audio = is_audio;
    f->flag = 0;
    f->uid = uid;
    return f;
}


#define API_VERSION 2

// ffrtsglue provided function collection
struct rts_glue_funcs {
    int api_version; // validation. must be 2

    /* configure globaly, before open is called
     * do not change values during an instance is running
     */
    void (* preconfig)(const char *key, const char *val);

    /* open a stream specified by url
     * url:   stream url. artc stream supported for now
     * mode:  "r" for subscribe, "w" for publish
     * return value: handle to the stream. NULL if open failed
     */
    void *(* open)(const char *url, const char *mode);

    /* close the stream
     * handle: returned by open
     */
    void (* close)(void *handle);

    /* runtime control (e.g. get/set parameters)
     * negative return value for error
     */
    int (* ioctl)(void *handle, const char *cmd, void *arg);

    /* read one frame
     * caller need free the returned frame
     * return value: 1 for one frame read into '*frame'; 0 for try
     *               later; -1 for EOF; or other negative value for
     *               fatal error
     */
    int (* read)(struct rts_frame **frame, void *handle);

     /* write one frame. callee free the frame
     * return value: 1 for ok; 0 for try
     *               later; -1 for EOF; or other negative value for
     *               fatal error
      */
    int (* write)(struct rts_frame **frame, void *handle);
};

static const struct rts_glue_funcs *__rts_funcs = NULL;

void av_set_rts_muxer_funcs(const struct rts_glue_funcs *funcs);

void av_set_rts_muxer_funcs(const struct rts_glue_funcs *funcs)
{
    __rts_funcs = funcs;
}

typedef struct tagRtsEncContext {
    AVClass  *av_class;     // first element
    void     *rts_worker;  // instance of rts worker, created by open()

} RtsEncContext;

#define RECV_MEDIA_INFO_TIMEOUT 15000
#define MEDIA_INFO_QUERY_INTERVAL 10

static int rtc_init_avstream_info(AVFormatContext *s, struct rts_worker_mux_info *stream_info)
{
    // dump
    printf("DDDD: nb_streams=%u\n", s->nb_streams);
    for(unsigned int i=0; i<s->nb_streams; i++) {
        if(s->streams[i] == NULL)
            continue;
        printf("%d: codec_type=%d codec_id=%d\n",
               i,
               s->streams[i]->codecpar->codec_type,
               s->streams[i]->codecpar->codec_id);
    }
    return AVERROR(0);
}

// this function relays messages from artc to app
// app call av_format_set_control_message_cb to set callback
// to receive artc messages
static int format_control_message(struct AVFormatContext *s,
                                  int type,
                                  void *data, // temp data, do not cache it for later use!
                                  size_t data_size)
{
    if (s->control_message_cb == NULL)
        return AVERROR(ENOSYS);

    return s->control_message_cb(s, type, data, data_size);
}

static int output_log(struct AVFormatContext *s, int level, const char *fmt, va_list args)
{
    int fflevel;

    switch (level) {
        case 0: // LOG_ERROR
            fflevel = AV_LOG_ERROR;
            break;

        case 1: // LOG_WARN
            fflevel = AV_LOG_WARNING;
            break;

        case 2: // LOG_INFO
            fflevel = AV_LOG_INFO;
            break;

        default:
            fflevel = AV_LOG_DEBUG;
            break;
    }

    av_vlog(s, fflevel, fmt, args);
    return 0;
}

static int artc_write_close(AVFormatContext *s);

static char *int_to_string(int v, char buf[64])
{
    sprintf(buf, "%d", v);
    return buf;
}
static char *addr_to_string(const void *v, char buf[64])
{
    sprintf(buf, "%llu", (unsigned long long)v);
    return buf;
}

//-----------------------------------------------------------------------------------------

static int artc_write_header(AVFormatContext *s)
{
    av_log(s, AV_LOG_INFO, "entering artc_write_header ... @%lld\n", (long long)av_gettime() / 1000);
    RtsEncContext *artc = s->priv_data;
    int res = AVERROR(EAGAIN);
    char buf[64];
    int host_unreachable = 1;
    int64_t start_time = 0;

    if (__rts_funcs == NULL) {
        av_log(s, AV_LOG_ERROR, "Please call av_set_rts_muxer_funcs first!\n");
        res = AVERROR(ENXIO);
        goto out;
    }
    else if (__rts_funcs->api_version != API_VERSION) {
        av_log(s, AV_LOG_ERROR, "Bad api version %d, require %d!\n", __rts_funcs->api_version, API_VERSION);
        res = AVERROR(EPROTO);
        goto out;
    }

    // start rts, and register callback 'format_control_message' & 's' to artc
    // stop rts first
    if (artc->rts_worker != NULL) {
        __rts_funcs->close(artc->rts_worker);
        artc->rts_worker = NULL;
    }

    int sample_rate = 48000;
    int channels = 1;
    int audio_timebase_num = 1;
    int audio_timebase_den = 48000;
    int video_timebase_num = 1;
    int video_timebase_den = 25;
    const char *acodec = "opus";
    int i;
    int video_stream_id = 0;
    for (i = 0; i < s->nb_streams; i++) {
        if(s->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            switch(s->streams[i]->codecpar->codec_id) {
            case AV_CODEC_ID_AAC:
                acodec = "aac";
                switch(s->streams[i]->codecpar->profile) {
                case FF_PROFILE_AAC_LOW:
                    av_log(s, AV_LOG_INFO, "aac-lc\n");
                    acodec = "aac-lc"; break;
                case FF_PROFILE_AAC_HE:
                    av_log(s, AV_LOG_INFO, "aac-he\n");
                    acodec = "aac-he"; break;
                case FF_PROFILE_AAC_HE_V2:
                    av_log(s, AV_LOG_INFO, "aac-he2\n");
                    acodec = "aac-he2"; break;
                default:
                    av_log(s, AV_LOG_ERROR, "not supported aac profile %d. use LC\n", s->streams[i]->codecpar->profile);
                    acodec = "aac-lc"; break;
                    break;
                }
                break;
            case AV_CODEC_ID_OPUS:
                acodec = "opus";
                break;
            case AV_CODEC_ID_ADPCM_G722:
                acodec = "g722";
                break;
            default:
                acodec = "unknown";
                break;
            }
            sample_rate = s->streams[i]->codecpar->sample_rate;
            if(s->streams[i]->codecpar->profile == FF_PROFILE_AAC_HE) {
                sample_rate /= 2;
            }
            av_log(s, AV_LOG_INFO, "sample rate set to %d\n", sample_rate);
            channels = s->streams[i]->codecpar->channels;
            break;
        }
        else if(s->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_id = i;
        }
    }

    // make use of support-id-prefix to setup the map between
    // app domain id and rtssdk domain id.
    // e.g. use UserID as support-id-prefix, it will be convenient to
    // query associated log/profiling data to that UserID
    __rts_funcs->preconfig("HelpSupportIDPrefix", "yourInstanceID");
    __rts_funcs->preconfig("LogCallback", addr_to_string(output_log, buf));
    __rts_funcs->preconfig("LogCbParam", addr_to_string(s, buf));
    __rts_funcs->preconfig("MessageCallback", addr_to_string(format_control_message, buf));
    __rts_funcs->preconfig("MessageCbParam", addr_to_string(s, buf));
    __rts_funcs->preconfig("AudioFormat", acodec);
    __rts_funcs->preconfig("AudioSampleRate", int_to_string(sample_rate, buf));
    __rts_funcs->preconfig("AudioChannels", int_to_string(channels, buf));
    if(s->streams[video_stream_id]->codecpar->codec_id == AV_CODEC_ID_HEVC)
        __rts_funcs->preconfig("VideoCodec", "h265");
    artc->rts_worker = __rts_funcs->open(s->url, "w");

    if(artc->rts_worker == NULL) {
        av_log(s, AV_LOG_ERROR, "Failed to open net sdk stack!\n");
        res = AVERROR(EIO);
        goto out;
    }

    host_unreachable = 1;
    start_time = av_gettime();

    // wait until netsdk ready
    while(true) {
        if (ff_check_interrupt(&s->interrupt_callback)) {
            av_log(s, AV_LOG_INFO, "User interrupted\n");
            res = AVERROR_EXIT;
            goto out;
        }

        // get stream info
        struct rts_worker_mux_info pub_info;
        int r = __rts_funcs->ioctl(artc->rts_worker, "get_pub_info", &pub_info);

        if (r == 0) {
            int rc = rtc_init_avstream_info(s, &pub_info);

            if (rc != AVERROR(0)) {

                res = rc;
                goto out;
            } else {
                // everything is ok, return
                res = AVERROR(0);
                goto out;
            }
        } else if (r == - EINVAL) {
            res = AVERROR(EINVAL);
            goto out;
        } else if (r != - ENOTCONN) {
            host_unreachable = 0;
        }

        av_usleep(MEDIA_INFO_QUERY_INTERVAL * 1000);

        if (((av_gettime() - start_time) / 1000) >=  RECV_MEDIA_INFO_TIMEOUT) {
            av_log(s, AV_LOG_ERROR, "Resource timeout time %" PRId64 "start_time %" PRId64 "\n",
                    av_gettime(), start_time);
            if (host_unreachable == 1)
                res = AVERROR(EHOSTUNREACH);
            else
                res = AVERROR(ETIMEDOUT);

            break;
        }
    }

out:

    if (res != AVERROR(0)) {
        artc_write_close(s);
    }

    av_log(s, AV_LOG_INFO, "leaving artc_write_header %d @%lld\n", res, (long long)av_gettime() / 1000);
    return res;
}

static int artc_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    RtsEncContext *artc = s->priv_data;

    if (ff_check_interrupt(&s->interrupt_callback)) {
        av_log(s, AV_LOG_INFO, "User interrupted\n");
        return AVERROR_EXIT;
    }

    // check audio or video
    int is_audio = 1;
    if(s->streams[pkt->stream_index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        is_audio = 0;
    // write one frame and check error
    struct rts_frame *f = NULL;
    f = alloc_frame(pkt->data, pkt->size, is_audio, 0);
    // calc pts, in ms
    {
        AVRational out_tb;
        out_tb.num = 1;
        out_tb.den = 1000; // turn into ms
        pkt->pts = av_rescale_q_rnd(
                pkt->pts,
                s->streams[pkt->stream_index]->time_base,
                out_tb,
                (enum AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
        pkt->dts = av_rescale_q_rnd(
                pkt->dts,
                s->streams[pkt->stream_index]->time_base,
                out_tb,
                (enum AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
        pkt->duration = av_rescale_q_rnd(
                pkt->duration,
                s->streams[pkt->stream_index]->time_base,
                out_tb,
                (enum AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
    }
    f->dts = pkt->dts;
    f->pts = pkt->pts;
    f->duration = pkt->duration;
    if(!is_audio) {
        unsigned int flag = 0;
        if(pkt->flags & AV_PKT_FLAG_KEY)
            flag |= 1;
        if(pkt->flags & AV_PKT_FLAG_CORRUPT)
            flag |= 2;
        f->flag = flag;
    }

    int r = __rts_funcs->write(&f, artc->rts_worker);

    if (r < 0) {
        av_log(s, AV_LOG_ERROR, "Fatal error received and stream stopped (err=%d)\n",
               r);
        f->free_ptr(f);
        return AVERROR(EIO);
    }

    return 0;
}

static int artc_write_close(AVFormatContext *s)
{
    av_log(s, AV_LOG_INFO, "entering artc_write_close ...\n");
    RtsEncContext *artc = s->priv_data;

    // stop rts
    if (artc->rts_worker != NULL) {
        __rts_funcs->close(artc->rts_worker);
        artc->rts_worker = NULL;
    }

    memset(artc, 0, sizeof(RtsEncContext));
    av_log(s, AV_LOG_INFO, "leaving artc_write_close ...\n");
    return 0;
}

static const AVOption ff_rtc_options[] = {
    { NULL },
};

static const AVClass rtc_muxer_class = {
    .class_name     = "rts muxer",
    .item_name      = av_default_item_name,
    .option         = ff_rtc_options,
    .version        = LIBAVUTIL_VERSION_INT,
};

AVOutputFormat ff_rtc_muxer = {
    .name              = "rtc",
    .long_name         = NULL_IF_CONFIG_SMALL("rtc stream output"),
    .priv_data_size    = sizeof(RtsEncContext),
    .audio_codec       = AV_CODEC_ID_OPUS,
    .video_codec       = AV_CODEC_ID_H264,
    .write_header      = artc_write_header,
    .write_packet      = artc_write_packet,
    .write_trailer     = artc_write_close,
    .flags             = AVFMT_NOFILE /*| AVFMT_GLOBALHEADER*/,
    .priv_class        = &rtc_muxer_class,
    .extensions        = "rtc",
};
