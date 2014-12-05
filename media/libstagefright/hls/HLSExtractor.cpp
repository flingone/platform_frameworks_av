#define LOG_NDEBUG 0
#define LOG_TAG "HLSExtractor"
#include <utils/Log.h>

#include "HLSReader.h"
#include "HLSExtractor.h"
#include "include/avc_utils.h"

#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MetaData.h>
#include <utils/threads.h>
#include <utils/String8.h>

namespace android {

struct HLSSource : public MediaSource {
    HLSSource(
            const sp<HLSExtractor> &extractor, sp<MetaData> meta, 
            bool isVideo, bool withStartCode);

    virtual status_t start(MetaData *params = NULL);
    virtual status_t stop();
    virtual sp<MetaData> getFormat();

    virtual status_t read(
            MediaBuffer **buffer, const ReadOptions *options = NULL);

private:
    sp<HLSExtractor> mExtractor;
    sp<MetaData> mMetaData;
    bool mIsVideo;
    bool mWithStartCode;
    HLSReader *mHLSReader;
    mutable Mutex mLock;

    void addADTSHeader(uint8_t *adts_header, int frame_length, int rate_idx, int channels);
    DISALLOW_EVIL_CONSTRUCTORS(HLSSource);
};

HLSSource::HLSSource(
        const sp<HLSExtractor> &extractor, sp<MetaData> meta, 
        bool isVideo, bool withStartCode)
    : mExtractor(extractor),
    mMetaData(meta),
    mIsVideo(isVideo),
    mWithStartCode(withStartCode),
    mHLSReader(instance()) {
}

status_t HLSSource::start(MetaData *params) {
    Mutex::Autolock autoLock(mLock);
    start_hls_stream_reader_thread(mHLSReader);
    return OK;
}

status_t HLSSource::stop() {
    Mutex::Autolock autoLock(mLock);
    stop_hls_stream_reader_thread(mHLSReader);
    return OK;
}

sp<MetaData> HLSSource::getFormat() {
    Mutex::Autolock autoLock(mLock);
    return mMetaData;
}

status_t HLSSource::read(
        MediaBuffer **out, const ReadOptions *options) {

    Mutex::Autolock autoLock(mLock);

    *out = NULL;
    
    AVPacket *pkt;
    int64_t timeUs;
    int64_t startTime;
    int64_t seekTimeUs;
    ReadOptions::SeekMode mode;
    int64_t pktTS = AV_NOPTS_VALUE;

    MediaBuffer *mediaBuffer;

    if (options && options->getSeekTo(&seekTimeUs, &mode)) {
        ALOGD("before seek time us: %lld", seekTimeUs);
        if (mIsVideo) {
            notify_hls_reader_seek(mHLSReader, seekTimeUs);
        }
        wait_seek_finish(mHLSReader);
        ALOGD("after seek time us: %lld", seekTimeUs);
    }

    if (mIsVideo) {
        pkt = queue_dequeue(&mHLSReader->videoq);
    } else {
        pkt = queue_dequeue(&mHLSReader->audioq);
    }
    
    if (pkt->size == 0 && pkt->data == NULL) {
        return ERROR_END_OF_STREAM;
    }

    if (mIsVideo && ! mWithStartCode) {
        uint8_t *data = pkt->data;
        int size = 0, offset = 0;

        while(size < pkt->size) {
           offset = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
           data[size] = 0;
           data[size + 1] = 0;
           data[size + 2] = 0;
           data[size + 3] = 1;

           size += 4 + offset;
           data += 4 + offset;
        }
    }

    mediaBuffer = new MediaBuffer(pkt->size);
    mediaBuffer->meta_data()->clear();
    mediaBuffer->set_range(0, pkt->size);
        
    memcpy((uint8_t *)mediaBuffer->data(), pkt->data, pkt->size);

    pktTS = pkt->pts;

    if (pktTS == AV_NOPTS_VALUE) {
        pktTS = pkt->dts;
    }

    if (mIsVideo) {
        startTime = video_start_time(mHLSReader);
        if (startTime == AV_NOPTS_VALUE) {
            startTime = 0;
        }
    } else {
        startTime = audio_start_time(mHLSReader);
        if (startTime == AV_NOPTS_VALUE) {
            startTime = 0;
        }
    }

    if (mIsVideo) {
        timeUs = (int64_t)((pktTS - startTime) * av_q2d(video_time_base(mHLSReader)) * 1000000);
    } else {
        timeUs = (int64_t)((pktTS - startTime) * av_q2d(audio_time_base(mHLSReader)) * 1000000);
    }

    mediaBuffer->meta_data()->setInt64(kKeyTime, timeUs);                                                                               
    
    //ALOGD("isVideo: %d timeUS: %lld", mIsVideo, timeUs);

    *out = mediaBuffer;

    av_free_packet(pkt); free(pkt);

    return OK;
}

void HLSSource::addADTSHeader(uint8_t *adts_header, int frame_length, int rate_idx, int channels)
{
       unsigned int obj_type = 0;    
       unsigned int num_data_block = frame_length / 1024; 
       // include the header length also     
       frame_length += 7;   
       /* We want the same metadata */    
       /* Generate ADTS header */    
       if(adts_header == NULL) return;    
       /* Sync point over a full byte */    
       adts_header[0] = 0xFF;    
       /* Sync point continued over first 4 bits + static 4 bits    
      * (ID, layer, protection)*/    
       adts_header[1] = 0xF9;    
       /* Object type over first 2 bits */    
       adts_header[2] = obj_type << 6;//    
       /* rate index over next 4 bits */    
       adts_header[2] |= (rate_idx << 2);    
       /* channels over last 2 bits */    
       adts_header[2] |= (channels & 0x4) >> 2;   
       /* channels continued over next 2 bits + 4 bits at zero */    
       adts_header[3] = (channels & 0x3) << 6;    
       /* frame size over last 2 bits */    
       adts_header[3] |= (frame_length & 0x1800) >> 11;    
       /* frame size continued over full byte */    
       adts_header[4] = (frame_length & 0x1FF8) >> 3;    
       /* frame size continued first 3 bits */    
       adts_header[5] = (frame_length & 0x7) << 5;    
       /* buffer fullness (0x7FF for VBR) over 5 last bits*/    
       adts_header[5] |= 0x1F;    
       /* buffer fullness (0x7FF for VBR) continued over 6 first bits + 2 zeros     
       * number of raw data blocks */
       adts_header[6] = 0xFC;//>
       adts_header[6] |= num_data_block & 0x03; //Set raw Data blocks.
}
////////////////////////////////////////////////////////////////////////////////

void HLSExtractor::init() {
   _status_t st = hls_reader_init(mDataSource->getPath(), mHLSReader);
   CHECK(st == ERR_NONE);

   /* Set audio meta data. */
   sp<MetaData> ameta;

   uint8_t profile = 0, sf_index = 0, channel = 0;

   if (aextradata_size(mHLSReader) <= 0) {
       decodeAudioSpecificInfo(mHLSReader, &profile, &sf_index, &channel);
   } else {
       decodeAudioExtradata(mHLSReader, &profile, &sf_index, &channel);
   }

   ameta = MakeAACCodecSpecificData(profile, sf_index, channel);

   if (isADTS(mHLSReader)) {
       ameta->setInt32(kKeyIsADTS, true);
   }
   ameta->setInt32(kKeyBitRate, audio_bit_rate(mHLSReader));
   ameta->setInt64(kKeyDuration, audio_duration(mHLSReader));
   mTracks.add(0, new HLSSource(this, ameta, false, false));
   ALOGD("create a audio track, track size: %d", mTracks.size());

   /* Set video meta data. */
   sp<MetaData> vmeta;
   bool withStartCode = false;

   if (vextradata_size(mHLSReader) > 0) {
       uint8_t *data = vextradata(mHLSReader);
       int size = vextradata_size(mHLSReader);

        if (data[0] == 1) {
            vmeta = new MetaData;
            withStartCode = false;
            ALOGV("AVC, H.264 bitstream without start codes.");
            vmeta->setData(kKeyAVCC, kTypeAVCC, data, size);
        } else {
            withStartCode = true;
            ALOGV("H264, H.264 bitstream with start codes.");
            sp<ABuffer> buffer = new ABuffer(size);
            memcpy(buffer->data(), data, size);
            vmeta = MakeAVCCodecSpecificData(buffer);
        }
   }

   vmeta->setInt32(kKeyWidth, width(mHLSReader));                                                                                
   vmeta->setInt32(kKeyHeight, height(mHLSReader));
   if (video_bit_rate(mHLSReader) > 0) {
        vmeta->setInt32(kKeyBitRate, video_bit_rate(mHLSReader));
   }

   vmeta->setInt64(kKeyDuration, video_duration(mHLSReader));
   vmeta->setCString(kKeyMIMEType, video_mime_type(mHLSReader));

   mTracks.add(1, new HLSSource(this, vmeta, true, withStartCode));
   ALOGD("create a video track, track size: %d", mTracks.size());


   mInitCheck = OK;
}

HLSExtractor::HLSExtractor(const sp<DataSource> &source)
    : mDataSource(source),
    mInitCheck(NO_INIT),
    mHLSReader(instance()) {
    
    init();
}

size_t HLSExtractor::countTracks() {
    ALOGE("countTracks(): %d", mTracks.size());
    return mInitCheck == OK ? mTracks.size() : 0;
}

sp<MediaSource> HLSExtractor::getTrack(size_t index) {
     ALOGE("HLSExtractor::getTrack[%d]", index);
     
     if (mInitCheck != OK) {
         ALOGE("init check failed, can not getTrack");
         return NULL;
     }
     
     if (index >= mTracks.size()) {
         ALOGE("index out of bound");
         return NULL;
     }

     return mTracks.valueAt(index);
}

sp<MetaData> HLSExtractor::getTrackMetaData(
        size_t index, uint32_t flags) {
    
    if (mInitCheck != OK) {
        ALOGE("init check failed, can not getTrackMetaData");
        return NULL;
    }

    if (index >= mTracks.size()) {
        return NULL;
    }
    
    return mTracks.valueAt(index)->getFormat();
}

sp<MetaData> HLSExtractor::getMetaData() {
    ALOGV("HLSExtractor::getMetaData");                                                                                             
    
    if (mInitCheck != OK) {
        ALOGE("init check failed, can not getMetaData");
        return NULL;
    }
    
    sp<MetaData> meta = new MetaData;
    
    meta->setInt64(kKeyDuration, duration(mHLSReader));
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_CONTAINER_HLS);
    return meta;
}

uint32_t HLSExtractor::flags() const {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return 0;
    }

    uint32_t flags = CAN_PAUSE;
    
    if (duration(mHLSReader) != AV_NOPTS_VALUE) {
        flags |= (CAN_SEEK_FORWARD | CAN_SEEK_BACKWARD | CAN_SEEK);
    }
    return flags;
}

////////////////////////////////////////////////////////////////////////////////

bool SniffHLS(
        const sp<DataSource> &source, String8 *mimeType, float *confidence,
        sp<AMessage> *) {

        ALOGV("SniffHLS");

        const char *uri = NULL;

        uri = source->getPath();
        if (! uri) {
            ALOGE("Can not getPath for the giving source.");
            return false;
        }

        ALOGD("uri: %s", uri);

        if (! strncmp("http://", uri, 7)) {

            if (strcasestr(uri, ".m3u8")) {
                *confidence = 0.99f;
                mimeType->setTo(MEDIA_MIMETYPE_CONTAINER_HLS);
                return true;
            }
        }

        return false;
}

}  // namespace android
