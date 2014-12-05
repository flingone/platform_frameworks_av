#ifndef _HLS_READER_H_
#define _HLS_READER_H_

#include <stdio.h>
#include <stdbool.h>
#include <pthread.h>

#include "Queue.h"

extern "C" {
#include "libavutil/avstring.h"
#include "libavformat/avformat.h"

typedef enum _status_t {
    ERR_NONE = 0,
    ERR_INIT,
    ERR_OUT_OF_MEM
} _status_t;

typedef struct HLSReader {
    int video_stream_index;
    int audio_stream_index;

    AVFormatContext *av_format_ctx;

    volatile bool is_stop;
    volatile bool is_pause;
    pthread_t ptid;
    volatile bool is_running;

    Queue videoq;
    Queue audioq;
    AVBitStreamFilterContext *bsfc;

    volatile bool is_seek;
    volatile int64_t seek_time;

    AVRational video_rational;
    AVRational audio_rational;

    int64_t video_ts_base;
    int64_t audio_ts_base;

    int64_t last_video_ts;
    int64_t last_audio_ts;

    int64_t video_start_time;
    int64_t audio_start_time;

    bool is_mp4_section;

} HLSReader;

extern HLSReader * instance(void);
extern _status_t hls_reader_init(const char *url, HLSReader *reader);
extern void hls_reader_destroy(HLSReader *reader);
extern void start_hls_stream_reader_thread(HLSReader *reader);
extern void stop_hls_stream_reader_thread(HLSReader *reader);

/* For file stream. */
extern int64_t duration(HLSReader *reader);
extern AVRational video_time_base(HLSReader *reader);
extern AVRational audio_time_base(HLSReader *reader);

/* For video stream. */
extern int64_t video_duration(HLSReader *reader);
extern int width(HLSReader *reader);
extern int height(HLSReader *reader);
extern int video_bit_rate(HLSReader *reader);
const char * video_mime_type(HLSReader *reader);
extern uint8_t *vextradata(HLSReader *reader);
extern int vextradata_size(HLSReader *reader);

/* For audio stream. */
extern bool isADTS(HLSReader *reader);
extern int64_t audio_duration(HLSReader *reader);
extern int audio_bit_rate(HLSReader *reader);
extern int audio_sample_rate(HLSReader *reader);
extern int audio_channels(HLSReader *reader);
extern const char * audio_mime_type(HLSReader *reader);
extern uint8_t *aextradata(HLSReader *reader);
extern int aextradata_size(HLSReader *reader);
extern int audio_profile(HLSReader *reader);
extern int64_t video_start_time(HLSReader *reader);
extern int64_t audio_start_time(HLSReader *reader);

extern void decodeAudioSpecificInfo(HLSReader *reader, uint8_t *profile, uint8_t *sf_index, uint8_t *channel);
extern void decodeAudioExtradata(HLSReader *reader, uint8_t *profile, uint8_t *sf_index, uint8_t *channel);
extern void notify_hls_reader_seek(HLSReader *reader, int64_t seekTimeUs);
extern void wait_seek_finish(HLSReader *reader);
}
#endif //_HLS_READER_H_

