#ifndef _FLVPARSER_H_
#define _FLVPARSER_H_

#include "FLVMetaDataParser.h"

namespace android{

enum{
    AUDIO_TYPE_MPEG3 = 2,
    AUDIO_TYPE_AAC = 10,
    VIDEO_TYPE_SORENSON_H263 = 2,
    VIDEO_TYPE_VP6 = 4,
    VIDEO_TYPE_AVC = 7,
};

enum{
    FLV_TAG_TYPE_AUDIO = 0x08,
    FLV_TAG_TYPE_VIDEO = 0x09,
    FLV_TAG_TYPE_SCRIPT = 0x12,
};

enum{
    PRE_TAG_SIZE = 4,
    FLV_HEADER_SIZE = 9,
    TAG_HEADER_SIZE = 11,
};

struct trackInfo{
    bool hasVideo;
    bool hasAudio;
};

struct Track {
    Track *next;
    sp<MetaData> meta;
};

class FLVParser : public RefBase {
public:
    FLVParser(const sp<DataSource> &dataSource);

    status_t parseHeader(void);
    status_t readMetaData();
    sp<MetaData> getTrackMetaData(size_t index, uint32_t flags);
    sp<MetaData> getMetaData();
    size_t countTracks();
    Track* getTrackByIndex(size_t index);
    status_t parseScriptTag(void);
    off_t getCurrentAVCPos();
    void parseAmfData(const void *data, size_t size);
    status_t parseAudioVideoDataTag(uint32_t currPos);

    void addESDSFromAudioSpecificInfo(const sp<MetaData> &meta, const void *asi, size_t asiSize);
    status_t updateAudioTrackInfoFromAudioSpecificConfig_MPEG4Audio(
        const void *esds_data, size_t esds_size);


protected:
    virtual ~FLVParser();

private:
    sp<DataSource> mDataSource;
    flvMetaDataParser *mFlvMetaDataParser;
    sp<MetaData> mFileMetaData;
    Track *mFirstTrack, *mLastTrack;

    bool mHasVideo;
    bool mHasAudio;
    bool mHaveMetadata;
    off_t mAVCDataPosition;

    bool is_video_width_ok;
    bool is_video_height_ok;
    bool is_duration_ok;
    bool is_video_mime_type_ok;
    bool is_audio_mime_type_ok;
    bool is_channel_count_ok;
    bool is_video_codec_id_ok;
    bool is_audio_codec_id_ok;

    static double double_big_to_little(double d);
};

}

#endif
