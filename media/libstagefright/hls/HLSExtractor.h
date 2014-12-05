#ifndef _HLS_EXTRACTOR_H_
#define _HLS_EXTRACTOR_H_

#include <media/stagefright/foundation/ABase.h>
#include <media/stagefright/MediaExtractor.h>
#include <utils/KeyedVector.h>
#include <utils/threads.h>

#include "HLSReader.h"

namespace android {

struct AMessage;
struct DataSource;
struct HLSSource;
struct String8;

struct HLSExtractor : public MediaExtractor {
    HLSExtractor(const sp<DataSource> &source);

    virtual size_t countTracks();
    virtual sp<MediaSource> getTrack(size_t index);
    virtual sp<MetaData> getTrackMetaData(size_t index, uint32_t flags);

    virtual sp<MetaData> getMetaData();

    virtual uint32_t flags() const;

private:
    mutable Mutex mLock;

    status_t mInitCheck;
    HLSReader *mHLSReader;
    sp<DataSource> mDataSource;
    KeyedVector<unsigned, sp<HLSSource> > mTracks;

    void init();
    void configureCodecData(sp<MetaData> meta, uint8_t *data, int size);

    DISALLOW_EVIL_CONSTRUCTORS(HLSExtractor);
};

bool SniffHLS(
        const sp<DataSource> &source, String8 *mimeType, float *confidence,
        sp<AMessage> *);

}  // namespace android

#endif  // _HLS_EXTRACTOR_H_
