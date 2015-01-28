#define LOG_NDEBUG 0
#define LOG_TAG "HLSReader"

#include <stdint.h>
#include <utils/Log.h>
#include <media/stagefright/MediaDefs.h>

#include "HLSReader.h"

static HLSReader *gHLSReader = NULL;

HLSReader * instance(void)
{
    if (! gHLSReader) {
        gHLSReader = (HLSReader *)malloc(sizeof(HLSReader));
    }

    return gHLSReader;
}

static void _init_(HLSReader *reader)
{
    memset(reader, 0, sizeof(*reader));

    reader->audio_stream_index = -1;
    reader->video_stream_index = -1;

    reader->is_seek = false;
    reader->seek_time = 0;
    reader->is_stop = false;
    reader->is_pause = false;
    reader->is_running = false;
    reader->urIType = NULL;

    queue_init(&reader->videoq);
    queue_init(&reader->audioq);
}

void notify_hls_reader_seek(HLSReader *reader, int64_t seekTimeUs)
{
    AVStream *stream;

    reader->is_seek = true;

    stream = reader->av_format_ctx->streams[reader->video_stream_index];

    if (stream->start_time != AV_NOPTS_VALUE) {
        seekTimeUs += stream->start_time * av_q2d(stream->time_base) * 1000000;
    }

    reader->seek_time = seekTimeUs;
}

void wait_seek_finish(HLSReader *reader)
{
    for (; ;) {
        if (reader->is_seek) {
            usleep(1000);
        } else {
            break;
        }
    }
}

static void set_av_stream_index(HLSReader *reader)
{
    uint32_t i;

    for (i = 0; i < reader->av_format_ctx->nb_streams; i ++) {
        if (reader->av_format_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
            reader->audio_stream_index = i;
        } else if (reader->av_format_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            reader->video_stream_index = i;
        } else {
            ALOGE("unknown codec type: %d", reader->av_format_ctx->streams[i]->codec->codec_type);
        }
    }

    ALOGI("number streams: %d, audio stream index: %d, video stream index: %d", 
            reader->av_format_ctx->nb_streams, reader->audio_stream_index, reader->video_stream_index);
}

static void init_reader(HLSReader *reader)
{
    reader->video_start_time = reader->av_format_ctx->streams[reader->video_stream_index]->start_time;
    reader->audio_start_time = reader->av_format_ctx->streams[reader->audio_stream_index]->start_time;

    reader->video_rational = reader->av_format_ctx->streams[reader->video_stream_index]->time_base;
    reader->audio_rational = reader->av_format_ctx->streams[reader->audio_stream_index]->time_base;
}

static char *get_uri_type(const char *uri)
{
    if (! strncmp(uri, "http://7d.v.iask.com", 20)) {
        return "SINA";
    }

    return NULL;
}

static bool need_rebuild_timestamp(HLSReader *reader)
{
    return reader->urIType != NULL;
}

_status_t hls_reader_init(const char *url, HLSReader *reader)
{
    _init_(reader);

    reader->urIType = get_uri_type(url);
    reader->pIsNeedRebuildTimestamp = need_rebuild_timestamp;

    avcodec_register_all();                                                                                                         
    av_register_all();
    avformat_network_init();

    reader->av_format_ctx = avformat_alloc_context();
    if (! reader->av_format_ctx) {
        ALOGE("avformat alloc context failed.");
        return ERR_INIT;
    }

    if (avformat_open_input(&reader->av_format_ctx, url, NULL, NULL) < 0) {
        ALOGE("avformat open input: %s failed.", url);
        return ERR_INIT;
    }

    if (avformat_find_stream_info(reader->av_format_ctx, NULL) < 0) {
        ALOGE("avformat find stream info failed.");
        return ERR_INIT;
    }

    set_av_stream_index(reader);
    init_reader(reader);

    return ERR_NONE;
}

int width(HLSReader *reader)
{
    ALOGE("video width: %d", 
            reader->av_format_ctx->streams[reader->video_stream_index]->codec->width);
    return reader->av_format_ctx->streams[reader->video_stream_index]->codec->width;
}

int height(HLSReader *reader)
{
    ALOGE("video height: %d", 
            reader->av_format_ctx->streams[reader->video_stream_index]->codec->height);
    return reader->av_format_ctx->streams[reader->video_stream_index]->codec->height;
}

int video_bit_rate(HLSReader *reader)
{
    ALOGE("video bit rate: %d", 
            reader->av_format_ctx->streams[reader->video_stream_index]->codec->bit_rate);
    return reader->av_format_ctx->streams[reader->video_stream_index]->codec->bit_rate;
}

int vextradata_size(HLSReader *reader)
{
    ALOGE("Video extra data size: %d",
            reader->av_format_ctx->streams[reader->video_stream_index]->codec->extradata_size);
    return reader->av_format_ctx->streams[reader->video_stream_index]->codec->extradata_size;
}

uint8_t *vextradata(HLSReader *reader)
{
    return reader->av_format_ctx->streams[reader->video_stream_index]->codec->extradata;
}

int64_t audio_duration(HLSReader *reader)
{
    int64_t duration;
    AVStream *stream;

    stream = reader->av_format_ctx->streams[reader->audio_stream_index];

    if (stream->duration != AV_NOPTS_VALUE) {
        duration = stream->duration * av_q2d(stream->time_base) * 1000000;
        ALOGD("audio duration: %lld", stream->duration);
    } else {
        duration = ::duration(reader);
        ALOGD("No audio stream duration, use file's: %lld", duration);
    }

    return duration;
}


int64_t video_duration(HLSReader *reader)
{
    int64_t duration;
    AVStream *stream;

    stream = reader->av_format_ctx->streams[reader->video_stream_index];

    if (stream->duration != AV_NOPTS_VALUE) {
        duration = stream->duration * av_q2d(stream->time_base) * 1000000;
        ALOGE("video duration: %lld", stream->duration);
    } else {
        duration = ::duration(reader);
        ALOGD("No video stream duration, use file's: %lld", duration);
    }

    return duration;
}

int64_t duration(HLSReader *reader)
{
    if (reader->av_format_ctx->duration < 0) {
        //live show do not have valid duration.
        return 0;
    }

    return reader->av_format_ctx->duration;
}

AVRational video_time_base(HLSReader *reader)
{
    return reader->video_rational;
}

AVRational audio_time_base(HLSReader *reader)
{
    return reader->audio_rational;
}

const char * video_mime_type(HLSReader *reader)
{
    enum AVCodecID codec_id;

    codec_id = 
        reader->av_format_ctx->streams[reader->video_stream_index]->codec->codec_id;

    switch(codec_id) {
        case AV_CODEC_ID_H264:
            return android::MEDIA_MIMETYPE_VIDEO_AVC;
            break;
        default:
            ALOGE("un-supported video codec id: %d", codec_id);
    }

    return NULL;
}

int audio_bit_rate(HLSReader *reader)
{
    ALOGE("audio bit rate: %d", 
            reader->av_format_ctx->streams[reader->audio_stream_index]->codec->bit_rate);
    return reader->av_format_ctx->streams[reader->audio_stream_index]->codec->bit_rate;
}

int audio_sample_rate(HLSReader *reader)
{
    ALOGE("audio sample rate: %d", 
            reader->av_format_ctx->streams[reader->audio_stream_index]->codec->sample_rate);
    return reader->av_format_ctx->streams[reader->audio_stream_index]->codec->sample_rate;
}

int audio_channels(HLSReader *reader)
{
    ALOGE("audio channels: %d", 
            reader->av_format_ctx->streams[reader->audio_stream_index]->codec->channels);
    return reader->av_format_ctx->streams[reader->audio_stream_index]->codec->channels;
}

const char * audio_mime_type(HLSReader *reader)
{
    enum AVCodecID codec_id;

    codec_id = 
        reader->av_format_ctx->streams[reader->audio_stream_index]->codec->codec_id;

    switch(codec_id) {
        case AV_CODEC_ID_AAC:
            return android::MEDIA_MIMETYPE_AUDIO_AAC;
            break;
        default:
            ALOGE("un-supported audio codec id: %d", codec_id);
    }

    return NULL;
}

int aextradata_size(HLSReader *reader)
{
    ALOGE("Audio extra data size: %d",
            reader->av_format_ctx->streams[reader->audio_stream_index]->codec->extradata_size);
    return reader->av_format_ctx->streams[reader->audio_stream_index]->codec->extradata_size;
}

uint8_t *aextradata(HLSReader *reader)
{
    return reader->av_format_ctx->streams[reader->audio_stream_index]->codec->extradata;
}

int64_t video_start_time(HLSReader *reader)
{
    return reader->video_start_time;
}

int64_t audio_start_time(HLSReader *reader)
{
    return reader->audio_start_time;
}

int audio_profile(HLSReader *reader)
{
    ALOGE("Audio profile: %d",
            reader->av_format_ctx->streams[reader->audio_stream_index]->codec->profile);
    return reader->av_format_ctx->streams[reader->audio_stream_index]->codec->profile;
}

static void enqueue_empty_packet(HLSReader *reader)
{
    AVPacket *pkt1, *pkt2;

    pkt1 = (AVPacket *)malloc(sizeof(*pkt1));
    pkt2 = (AVPacket *)malloc(sizeof(*pkt2));
    
    pkt1->size = pkt2->size = 0;
    pkt1->data = pkt2->data = NULL;

    queue_enqueue(&reader->videoq, pkt1);
    queue_enqueue(&reader->audioq, pkt2);
}

static void calc_time_stamp(HLSReader *reader)
{
    /* find and drop some time stamp*/

    AVPacket *pkt;
    int64_t pkt_ts;
    int64_t video_key_frame_ts;
    int64_t audio_frame_ts;
    AVStream *as, *vs;

    as = reader->av_format_ctx->streams[reader->audio_stream_index];
    vs = reader->av_format_ctx->streams[reader->video_stream_index];

    pkt = queue_head(&reader->videoq);
    if (! pkt) {
        return;
    }

    pkt_ts = ((pkt->pts == AV_NOPTS_VALUE) ? pkt->dts : pkt->pts);
    video_key_frame_ts = (int64_t)((pkt_ts - vs->start_time) * av_q2d(vs->time_base) * 1000000);

    for (; ;) {
       pkt = queue_head(&reader->audioq); 
       if (! pkt) {
           ALOGE("NO more audio packet, break.");
           break;
       }

       pkt_ts = ((pkt->pts == AV_NOPTS_VALUE) ? pkt->dts : pkt->pts);
       audio_frame_ts = (int64_t)((pkt_ts - as->start_time) * av_q2d(as->time_base) * 1000000);

       ALOGD("video TS: %lld audio TS: %lld", video_key_frame_ts, audio_frame_ts);

       if (llabs(video_key_frame_ts - audio_frame_ts) <= 50 * 1000) {
           break;
       }

       queue_dequeue_head(&reader->audioq);
       av_free_packet(pkt); free(pkt);
    }
}

static void find_video_keyframe(HLSReader *reader)
{
    bool key = false;
    AVPacket *pkt;

    for ( ; ; ) {
        pkt = (AVPacket *)malloc(sizeof(AVPacket));

        av_init_packet(pkt);

        if (av_read_frame(reader->av_format_ctx, pkt) < 0) {
            ALOGE("find key frame failed, reached EOF.");
            break;
        }

        if (pkt->stream_index == reader->video_stream_index) {
            key = pkt->flags & AV_PKT_FLAG_KEY ? 1 : 0;

            if (key) {
                ALOGD("before found video key frame.");
                queue_enqueue(&reader->videoq, pkt);
                ALOGD("after found video key frame.");
                break;
            } else {
                av_free_packet(pkt); free(pkt);
            }
        } else if (pkt->stream_index == reader->audio_stream_index) {
            queue_enqueue(&reader->audioq, pkt);
            //av_free_packet(pkt); free(pkt);
        } else {
            ALOGE("unknown stream index: %d", pkt->stream_index);
        }
    }

}

static void * do_hls_stream_reader_thread(void *args)
{
    AVPacket *pkt;
    HLSReader *reader;

    /* set seek pos to start time. ?*/

    reader = (HLSReader *)args;

    while(! reader->is_stop) {

        if (reader->is_pause) {
            usleep(10 * 1000);
            continue;
        }

        if (queue_size(&reader->videoq) > 128 &&
                queue_size(&reader->audioq) > 128 &&
                (! reader->is_seek)) {
            usleep(1000 * 10);
            continue;
        }

        if (reader->is_seek) {
            ALOGD("start seek, seek time: %lld.", reader->seek_time);
           if (avformat_seek_file(reader->av_format_ctx, -1, INT64_MIN, reader->seek_time, INT64_MAX, 0) < 0) {
               ALOGE("seek to %lld failed.", reader->seek_time);
           } else {
               ALOGE("seek success, flush queue.");
               queue_flush(&reader->videoq); 
               queue_flush(&reader->audioq); 
               find_video_keyframe(reader);
               calc_time_stamp(reader);
           }
           reader->is_seek = false;
           reader->seek_time = 0;
        }

        pkt = (AVPacket *) malloc(sizeof(*pkt));
        av_init_packet(pkt);

        if (av_read_frame(reader->av_format_ctx, pkt) < 0) {
            ALOGE("end of stream.");
            enqueue_empty_packet(reader);
            break;
        }
#if 0
        ALOGE("read a packet, size: %d index: %d pts: %lld dts: %lld", 
                pkt->size, pkt->stream_index, pkt->pts, pkt->dts);
#endif

        if (pkt->pts < 0 && pkt->dts < 0) {
            ALOGE("found a invalid ts packet...");
            
            if (pkt->stream_index == reader->video_stream_index) {
                pkt->pts = pkt->dts = reader->v_curr_ts + (40 * av_q2d(video_time_base(reader)) * 1000000);
            } else if (pkt->stream_index == reader->audio_stream_index) {
                pkt->pts = pkt->dts = reader->a_curr_ts + (40 * av_q2d(audio_time_base(reader)) * 1000000);
            }
        }
        
        if (pkt->stream_index == reader->video_stream_index) {
            reader->v_curr_ts = pkt->pts;
            queue_enqueue(&reader->videoq, pkt);
        } else if (pkt->stream_index == reader->audio_stream_index) {
            reader->a_curr_ts = pkt->pts;
            queue_enqueue(&reader->audioq, pkt);
        } else {
            ALOGE("unknown packet type: %d", pkt->stream_index);
            av_free_packet(pkt); free(pkt);
        }
    }

    return NULL;
}

void start_hls_stream_reader_thread(HLSReader *reader)
{
    int err;

    if (reader->is_running) {
        ALOGE("hls streaming reader has already running.");
        return;
    }

    reader->is_running = true;

    err = pthread_create(&reader->ptid, NULL, do_hls_stream_reader_thread, reader);

    if (err != 0) {
        ALOGE("start hls streaming reader thread failed.");
    }
}

void stop_hls_stream_reader_thread(HLSReader *reader)
{
    reader->is_stop = true;
    pthread_join(reader->ptid, NULL);
}

void hls_reader_destroy(HLSReader *reader)
{
    ALOGE("Enter %s", __func__);
    avformat_close_input(&reader->av_format_ctx);
    avformat_network_deinit();

    queue_destroy(&reader->videoq);
    queue_destroy(&reader->audioq);
    av_bitstream_filter_close(reader->bsfc);
    free(gHLSReader); gHLSReader = NULL;
}

//FIXME, drop a audio packet. ?
void decodeAudioSpecificInfo(HLSReader *reader, uint8_t *profile, 
        uint8_t *sf_index, uint8_t *channel)
{
    AVCodecContext *avctx;
    const char *name = "aac_adtstoasc";

    reader->bsfc = av_bitstream_filter_init(name);
    if (! reader->bsfc) {
        ALOGE("av_bitstream_filter_init failed.");
    }

    ALOGD("open %s bsfc success.", name);

    while(true) {
        AVPacket *pkt = (AVPacket *)malloc(sizeof(AVPacket));
        av_read_frame(reader->av_format_ctx, pkt);

        if (pkt->stream_index == reader->audio_stream_index) {
            int     ret; 
            int     outbuf_size;
            uint8_t *outbuf;

            avctx = reader->av_format_ctx->streams[reader->audio_stream_index]->codec;
            ALOGD("read a audio packet, size: %d", pkt->size);

            if (reader->bsfc && pkt && pkt->data) {
                ret = av_bitstream_filter_filter(reader->bsfc, avctx, NULL, &outbuf, 
                        &outbuf_size, pkt->data, pkt->size, pkt->flags & AV_PKT_FLAG_KEY);

                if (ret < 0 ||!outbuf_size) {
                    av_free_packet(pkt);
                    continue;
                }    
                if (outbuf && outbuf != pkt->data) {
                    memmove(pkt->data, outbuf, outbuf_size);
                    pkt->size = outbuf_size;
                }    

                break;
            }    
        } else {
            ALOGD("It's a video packet, enqueue it.");
            queue_enqueue(&reader->videoq, pkt);
        }
    }

    decodeAudioExtradata(reader, profile, sf_index, channel);
}

bool isADTS(HLSReader *reader)
{
    char tagbuf[32] = {0};                                                                            
    AVCodecContext *avctx;

    avctx = reader->av_format_ctx->streams[reader->audio_stream_index]->codec;

    av_get_codec_tag_string(tagbuf, sizeof(tagbuf), avctx->codec_tag);

    return strncmp(tagbuf, "mp4a", 4);
}

void decodeAudioExtradata(HLSReader *reader, uint8_t *profile, 
        uint8_t *sf_index, uint8_t *channel)
{
    AVCodecContext *avctx;
    const uint8_t *header;                                                                                                               

    avctx = reader->av_format_ctx->streams[reader->audio_stream_index]->codec;
    header = avctx->extradata;

    // AudioSpecificInfo follows
    // oooo offf fccc c000
    // o - audioObjectType
    // f - samplingFreqIndex
    // c - channelConfig
    *profile = ((header[0] & 0xf8) >> 3) - 1;
    *sf_index = (header[0] & 0x07) << 1 | (header[1] & 0x80) >> 7;
    *channel = (header[1] >> 3) & 0xf;
    ALOGV("aac profile: %d, sf_index: %d, channel: %d", *profile, *sf_index, *channel);
}
