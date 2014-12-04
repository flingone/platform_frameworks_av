#ifndef _FLVEXTRACTOR_H_
#define _FLVEXTRACTOR_H_

#include <media/stagefright/MediaExtractor.h>
#include <utils/Vector.h>

#include "FLVMetaDataParser.h"
#include "FLVParser.h"

namespace android {

struct AMessage;
class DataSource;
class SampleTable;
class String8;

typedef struct{
    uint8_t amf01_type;
    uint8_t amf02_type;

    uint32_t array_num;

    uint16_t len;
    uint8_t *data;
} flv_script_tag_data_t;

typedef enum {
    AMF_DATA_TYPE_NUMBER      = 0x00,
    AMF_DATA_TYPE_BOOL        = 0x01,
    AMF_DATA_TYPE_STRING      = 0x02,
    AMF_DATA_TYPE_OBJECT      = 0x03,
    AMF_DATA_TYPE_NULL        = 0x05,
    AMF_DATA_TYPE_UNDEFINED   = 0x06,
    AMF_DATA_TYPE_REFERENCE   = 0x07,
    AMF_DATA_TYPE_MIXEDARRAY  = 0x08,
    AMF_DATA_TYPE_OBJECT_END  = 0x09,
    AMF_DATA_TYPE_ARRAY       = 0x0a,
    AMF_DATA_TYPE_DATE        = 0x0b,
    AMF_DATA_TYPE_LONG_STRING = 0x0c,
    AMF_DATA_TYPE_UNSUPPORTED = 0x0d,
} AMFDataType;

class FLVExtractor : public MediaExtractor {
public:
    // Extractor assumes ownership of "source".
    FLVExtractor(const sp<DataSource> &source);

    virtual size_t countTracks();
    virtual sp<MediaSource> getTrack(size_t index);
    virtual sp<MetaData> getTrackMetaData(size_t index, uint32_t flags);
    virtual sp<MetaData> getMetaData();

protected:
    virtual ~FLVExtractor();

private:
    FLVExtractor(const FLVExtractor &);
    FLVExtractor &operator=(const FLVExtractor &);

    sp<DataSource> mDataSource;
    sp<FLVParser> mParser;
};

bool SniffFLV(const sp<DataSource> &source, String8 *mimeType, float *confidence, sp<AMessage> *);

}  // namespace android

#endif  // _FLVEXTRACTOR_H_
