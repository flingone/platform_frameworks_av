/*
 * Copyright (c) 2012, Marvell International Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "MOVExtractor"
#include <utils/Log.h>

#include "MOVExtractor.h"
#include "include/SampleTable.h"
#include "include/ESDS.h"

#include <arpa/inet.h>

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <media/stagefright/foundation/ABitReader.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaBufferGroup.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/Utils.h>
#include <utils/String8.h>

namespace android {

class MOVSource : public MediaSource {
public:
    // Caller retains ownership of both "dataSource" and "sampleTable".
    MOVSource(const sp<MetaData> &format,
                const sp<DataSource> &dataSource,
                int32_t timeScale,
                const sp<SampleTable> &sampleTable);

    virtual status_t start(MetaData *params = NULL);
    virtual status_t stop();

    virtual sp<MetaData> getFormat();

    virtual status_t read(
            MediaBuffer **buffer, const ReadOptions *options = NULL);

protected:
    virtual ~MOVSource();

private:
    Mutex mLock;

    sp<MetaData> mFormat;
    sp<DataSource> mDataSource;
    int32_t mTimescale;
    sp<SampleTable> mSampleTable;
    uint32_t mCurrentSampleIndex;

    bool mIsAVC;
    size_t mNALLengthSize;

    bool mStarted;

    MediaBufferGroup *mGroup;

    MediaBuffer *mBuffer;

    bool mWantsNALFragments;

    uint8_t *mSrcBuffer;

    size_t parseNALSize(const uint8_t *data) const;

    MOVSource(const MOVSource &);
    MOVSource &operator=(const MOVSource &);
};

// This custom data source wraps an existing one and satisfies requests
// falling entirely within a cached range from the cache while forwarding
// all remaining requests to the wrapped datasource.
// This is used to cache the full sampletable metadata for a single track,
// possibly wrapping multiple times to cover all tracks, i.e.
// Each MOVDataSource caches the sampletable metadata for a single track.

struct MOVDataSource : public DataSource {
    MOVDataSource(const sp<DataSource> &source);

    virtual status_t initCheck() const;
    virtual ssize_t readAt(off64_t offset, void *data, size_t size);
    virtual status_t getSize(off64_t *size);
    virtual uint32_t flags();

    status_t setCachedRange(off64_t offset, size_t size);

protected:
    virtual ~MOVDataSource();

private:
    Mutex mLock;

    sp<DataSource> mSource;
    off64_t mCachedOffset;
    size_t mCachedSize;
    uint8_t *mCache;

    void clearCache();

    MOVDataSource(const MOVDataSource &);
    MOVDataSource &operator=(const MOVDataSource &);
};

MOVDataSource::MOVDataSource(const sp<DataSource> &source)
    : mSource(source),
      mCachedOffset(0),
      mCachedSize(0),
      mCache(NULL) {
}

MOVDataSource::~MOVDataSource() {
    clearCache();
}

void MOVDataSource::clearCache() {
    if (mCache) {
        free(mCache);
        mCache = NULL;
    }

    mCachedOffset = 0;
    mCachedSize = 0;
}

status_t MOVDataSource::initCheck() const {
    return mSource->initCheck();
}

ssize_t MOVDataSource::readAt(off64_t offset, void *data, size_t size) {
    Mutex::Autolock autoLock(mLock);

    if (offset >= mCachedOffset
            && offset + size <= mCachedOffset + mCachedSize) {
        memcpy(data, &mCache[offset - mCachedOffset], size);
        return size;
    }

    return mSource->readAt(offset, data, size);
}

status_t MOVDataSource::getSize(off64_t *size) {
    return mSource->getSize(size);
}

uint32_t MOVDataSource::flags() {
    return mSource->flags();
}

status_t MOVDataSource::setCachedRange(off64_t offset, size_t size) {
    Mutex::Autolock autoLock(mLock);

    clearCache();

    mCache = (uint8_t *)malloc(size);

    if (mCache == NULL) {
        return -ENOMEM;
    }

    mCachedOffset = offset;
    mCachedSize = size;

    ssize_t err = mSource->readAt(mCachedOffset, mCache, mCachedSize);

    if (err < (ssize_t)size) {
        clearCache();

        return ERROR_IO;
    }

    return OK;
}

////////////////////////////////////////////////////////////////////////////////

static void hexdump(const void *_data, size_t size) {
    const uint8_t *data = (const uint8_t *)_data;
    size_t offset = 0;
    while (offset < size) {
        printf("0x%04x  ", offset);

        size_t n = size - offset;
        if (n > 16) {
            n = 16;
        }

        for (size_t i = 0; i < 16; ++i) {
            if (i == 8) {
                printf(" ");
            }

            if (offset + i < size) {
                printf("%02x ", data[offset + i]);
            } else {
                printf("   ");
            }
        }

        printf(" ");

        for (size_t i = 0; i < n; ++i) {
            if (isprint(data[offset + i])) {
                printf("%c", data[offset + i]);
            } else {
                printf(".");
            }
        }

        printf("\n");

        offset += 16;
    }
}

static const char *FourCC2MIME(uint32_t fourcc) {
    switch (fourcc) {
        case FOURCC('m', 'p', '4', 'a'):
            return MEDIA_MIMETYPE_AUDIO_AAC;

        case FOURCC('s', 'a', 'm', 'r'):
            return MEDIA_MIMETYPE_AUDIO_AMR_NB;

        case FOURCC('s', 'a', 'w', 'b'):
            return MEDIA_MIMETYPE_AUDIO_AMR_WB;

        case FOURCC('.', 'm', 'p', '3'):
            return MEDIA_MIMETYPE_AUDIO_MPEG;

        case FOURCC('.', 'm', 'p', '2'):
            return MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_II;

        case FOURCC('h', 'd', 'v', '1'):
        case FOURCC('m', '2', 'v', '1'):
            return MEDIA_MIMETYPE_VIDEO_MPEG2;

        case FOURCC('D', 'X', '5', '0'):
        case FOURCC('m', 'p', '4', 'v'):
        case FOURCC('X', 'V', 'I', 'D'):
        case FOURCC('D', 'I', 'V', 'X'):
            return MEDIA_MIMETYPE_VIDEO_MPEG4;

        case FOURCC('m', 's', 0, 'U'):
            return MEDIA_MIMETYPE_AUDIO_MPEG;

        case FOURCC('s', '2', '6', '3'):
        case FOURCC('h', '2', '6', '3'):
        case FOURCC('H', '2', '6', '3'):
            return MEDIA_MIMETYPE_VIDEO_H263;

        case FOURCC('S', 'V', 'Q', '3'):
            return MEDIA_MIMETYPE_VIDEO_SVQ3;

        case FOURCC('a', 'v', 'c', '1'):
            return MEDIA_MIMETYPE_VIDEO_AVC;

        case FOURCC('W', 'M', 'V', '1'):
            return MEDIA_MIMETYPE_VIDEO_WMV1;

        case FOURCC('W', 'M', 'V', '2'):
            return MEDIA_MIMETYPE_VIDEO_WMV2;

        case FOURCC('a', 'c', '-', '3'):
            return MEDIA_MIMETYPE_AUDIO_AC3;

        case FOURCC('e', 'c', '-', '3'):
            return MEDIA_MIMETYPE_AUDIO_EAC3;

        case FOURCC('s', 'o', 'w', 't'):
        case FOURCC('t', 'w', 'o', 's'):
        case FOURCC('f', 'l', '3', '2'):
            return MEDIA_MIMETYPE_AUDIO_RAW;

        case FOURCC('W', 'M', 'A', '2'):
            return MEDIA_MIMETYPE_AUDIO_WMAV2;

        case FOURCC('m', 's', 'V', 'o'):
            return MEDIA_MIMETYPE_AUDIO_VORBIS;

        default:
            CHECK(!"should not be here.");
            return NULL;
    }
}

static size_t GetSizeWidth(size_t x) {
    size_t n = 1;
    while (x > 127) {
        ++n;
        x >>= 7;
    }
    return n;
}

static uint8_t *EncodeSize(uint8_t *dst, size_t x) {
    while (x > 127) {
        *dst++ = (x & 0x7f) | 0x80;
        x >>= 7;
    }
    *dst++ = x;
    return dst;
}

static sp<ABuffer> MakeMPEG4CodecSpecificData(const sp<ABuffer> &config) {
    size_t len1 = config->size() + GetSizeWidth(config->size()) + 1;
    size_t len2 = len1 + GetSizeWidth(len1) + 1 + 13;
    size_t len3 = len2 + GetSizeWidth(len2) + 1 + 3;

    sp<ABuffer> csd = new ABuffer(len3);
    uint8_t *dst = csd->data();
    *dst++ = 0x03;
    dst = EncodeSize(dst, len2 + 3);
    *dst++ = 0x00;  // ES_ID
    *dst++ = 0x00;
    *dst++ = 0x00;  // streamDependenceFlag, URL_Flag, OCRstreamFlag

    *dst++ = 0x04;
    dst = EncodeSize(dst, len1 + 13);
    *dst++ = 0x01;  // Video ISO/IEC 14496-2 Simple Profile
    for (size_t i = 0; i < 12; ++i) {
        *dst++ = 0x00;
    }

    *dst++ = 0x05;
    dst = EncodeSize(dst, config->size());
    memcpy(dst, config->data(), config->size());
    dst += config->size();

    // hexdump(csd->data(), csd->size());

    return csd;
}


MOVExtractor::MOVExtractor(const sp<DataSource> &source)
    : mGlobalHeader(NULL),
      mDataSource(source),
      mInitCheck(NO_INIT),
      mHasVideo(false),
      mIsLittleEndian(false),
      mFirstTrack(NULL),
      mLastTrack(NULL),
      mFileMetaData(new MetaData),
      mFirstSINF(NULL),
      mIsDrm(false) {
}

MOVExtractor::~MOVExtractor() {
    Track *track = mFirstTrack;
    while (track) {
        Track *next = track->next;

        delete track;
        track = next;
    }
    mFirstTrack = mLastTrack = NULL;

    SINF *sinf = mFirstSINF;
    while (sinf) {
        SINF *next = sinf->next;
        delete sinf->IPMPData;
        delete sinf;
        sinf = next;
    }
    mFirstSINF = NULL;
}

sp<MetaData> MOVExtractor::getMetaData() {
    status_t err;
    if ((err = readMetaData()) != OK) {
        return new MetaData;
    }

    return mFileMetaData;
}

size_t MOVExtractor::countTracks() {
    status_t err;
    if ((err = readMetaData()) != OK) {
        return 0;
    }

    size_t n = 0;
    Track *track = mFirstTrack;
    while (track) {
        ++n;
        track = track->next;
    }

    return n;
}

sp<MetaData> MOVExtractor::getTrackMetaData(
        size_t index, uint32_t flags) {
    status_t err;
    if ((err = readMetaData()) != OK) {
        return NULL;
    }

    Track *track = mFirstTrack;
    while (index > 0) {
        if (track == NULL) {
            return NULL;
        }

        track = track->next;
        --index;
    }

    if (track == NULL) {
        return NULL;
    }

    if ((flags & kIncludeExtensiveMetaData)
            && !track->includes_expensive_metadata) {
        track->includes_expensive_metadata = true;

        const char *mime;
        CHECK(track->meta->findCString(kKeyMIMEType, &mime));
        if (!strncasecmp("video/", mime, 6)) {
            uint32_t sampleIndex;
            uint32_t sampleTime;
            if (track->sampleTable->findThumbnailSample(&sampleIndex) == OK
                    && track->sampleTable->getMetaDataForSample(
                        sampleIndex, NULL /* offset */, NULL /* size */,
                        &sampleTime) == OK) {
                track->meta->setInt64(
                        kKeyThumbnailTime,
                        ((int64_t)sampleTime * 1000000) / track->timescale);
            }
        }
    }

    return track->meta;
}

status_t MOVExtractor::readMetaData() {
    if (mInitCheck != NO_INIT) {
        return mInitCheck;
    }

    off64_t offset = 0;
    status_t err;
    while ((err = parseChunk(&offset, 0)) == OK) {
    }

    if (mInitCheck == OK) {
        if (mHasVideo) {
            mFileMetaData->setCString(
                    kKeyMIMEType, MEDIA_MIMETYPE_CONTAINER_MOV);
        } else {
            mFileMetaData->setCString(kKeyMIMEType, "audio/mp4");
        }

        mInitCheck = OK;
    } else {
        mInitCheck = err;
    }

    CHECK_NE(err, (status_t)NO_INIT);
    return mInitCheck;
}

char* MOVExtractor::getDrmTrackInfo(size_t trackID, int *len) {
    if (mFirstSINF == NULL) {
        return NULL;
    }

    SINF *sinf = mFirstSINF;
    while (sinf && (trackID != sinf->trackID)) {
        sinf = sinf->next;
    }

    if (sinf == NULL) {
        return NULL;
    }

    *len = sinf->len;
    return sinf->IPMPData;
}

// Reads an encoded integer 7 bits at a time until it encounters the high bit clear.
int32_t readSize(off64_t offset,
        const sp<DataSource> DataSource, uint8_t *numOfBytes) {
    uint32_t size = 0;
    uint8_t data;
    bool moreData = true;
    *numOfBytes = 0;

    while (moreData) {
        if (DataSource->readAt(offset, &data, 1) < 1) {
            return -1;
        }
        offset ++;
        moreData = (data >= 128) ? true : false;
        size = (size << 7) | (data & 0x7f); // Take last 7 bits
        (*numOfBytes) ++;
    }

    return size;
}

status_t MOVExtractor::parseDrmSINF(off64_t *offset, off64_t data_offset) {
    uint8_t updateIdTag;
    if (mDataSource->readAt(data_offset, &updateIdTag, 1) < 1) {
        return ERROR_IO;
    }
    data_offset ++;

    if (0x01/*OBJECT_DESCRIPTOR_UPDATE_ID_TAG*/ != updateIdTag) {
        return ERROR_MALFORMED;
    }

    uint8_t numOfBytes;
    int32_t size = readSize(data_offset, mDataSource, &numOfBytes);
    if (size < 0) {
        return ERROR_IO;
    }
    int32_t classSize = size;
    data_offset += numOfBytes;

    while(size >= 11 ) {
        uint8_t descriptorTag;
        if (mDataSource->readAt(data_offset, &descriptorTag, 1) < 1) {
            return ERROR_IO;
        }
        data_offset ++;

        if (0x11/*OBJECT_DESCRIPTOR_ID_TAG*/ != descriptorTag) {
            return ERROR_MALFORMED;
        }

        uint8_t buffer[8];
        //ObjectDescriptorID and ObjectDescriptor url flag
        if (mDataSource->readAt(data_offset, buffer, 2) < 2) {
            return ERROR_IO;
        }
        data_offset += 2;

        if ((buffer[1] >> 5) & 0x0001) { //url flag is set
            return ERROR_MALFORMED;
        }

        if (mDataSource->readAt(data_offset, buffer, 8) < 8) {
            return ERROR_IO;
        }
        data_offset += 8;

        if ((0x0F/*ES_ID_REF_TAG*/ != buffer[1])
                || ( 0x0A/*IPMP_DESCRIPTOR_POINTER_ID_TAG*/ != buffer[5])) {
            return ERROR_MALFORMED;
        }

        SINF *sinf = new SINF;
        sinf->trackID = U16_AT(&buffer[3]);
        sinf->IPMPDescriptorID = buffer[7];
        sinf->next = mFirstSINF;
        mFirstSINF = sinf;

        size -= (8 + 2 + 1);
    }

    if (size != 0) {
        return ERROR_MALFORMED;
    }

    if (mDataSource->readAt(data_offset, &updateIdTag, 1) < 1) {
        return ERROR_IO;
    }
    data_offset ++;

    if(0x05/*IPMP_DESCRIPTOR_UPDATE_ID_TAG*/ != updateIdTag) {
        return ERROR_MALFORMED;
    }

    size = readSize(data_offset, mDataSource, &numOfBytes);
    if (size < 0) {
        return ERROR_IO;
    }
    classSize = size;
    data_offset += numOfBytes;

    while (size > 0) {
        uint8_t tag;
        int32_t dataLen;
        if (mDataSource->readAt(data_offset, &tag, 1) < 1) {
            return ERROR_IO;
        }
        data_offset ++;

        if (0x0B/*IPMP_DESCRIPTOR_ID_TAG*/ == tag) {
            uint8_t id;
            dataLen = readSize(data_offset, mDataSource, &numOfBytes);
            if (dataLen < 0) {
                return ERROR_IO;
            } else if (dataLen < 4) {
                return ERROR_MALFORMED;
            }
            data_offset += numOfBytes;

            if (mDataSource->readAt(data_offset, &id, 1) < 1) {
                return ERROR_IO;
            }
            data_offset ++;

            SINF *sinf = mFirstSINF;
            while (sinf && (sinf->IPMPDescriptorID != id)) {
                sinf = sinf->next;
            }
            if (sinf == NULL) {
                return ERROR_MALFORMED;
            }
            sinf->len = dataLen - 3;
            sinf->IPMPData = new char[sinf->len];

            if (mDataSource->readAt(data_offset + 2, sinf->IPMPData, sinf->len) < sinf->len) {
                return ERROR_IO;
            }
            data_offset += sinf->len;

            size -= (dataLen + numOfBytes + 1);
        }
    }

    if (size != 0) {
        return ERROR_MALFORMED;
    }

    return UNKNOWN_ERROR;  // Return a dummy error.
}

static void MakeFourCCString(uint32_t x, char *s) {
    s[0] = x >> 24;
    s[1] = (x >> 16) & 0xff;
    s[2] = (x >> 8) & 0xff;
    s[3] = x & 0xff;
    s[4] = '\0';
}

struct PathAdder {
    PathAdder(Vector<uint32_t> *path, uint32_t chunkType)
        : mPath(path) {
        mPath->push(chunkType);
    }

    ~PathAdder() {
        mPath->pop();
    }

private:
    Vector<uint32_t> *mPath;

    PathAdder(const PathAdder &);
    PathAdder &operator=(const PathAdder &);
};

static bool underMetaDataPath(const Vector<uint32_t> &path) {
    return path.size() >= 5
        && path[0] == FOURCC('m', 'o', 'o', 'v')
        && path[1] == FOURCC('u', 'd', 't', 'a')
        && path[2] == FOURCC('m', 'e', 't', 'a')
        && path[3] == FOURCC('i', 'l', 's', 't');
}
static bool is3gpUdtaMetaParsePath(const Vector<uint32_t> &path) {
    return path.size() == 3
        && path[0] == FOURCC('m', 'o', 'o', 'v')
        && path[1] == FOURCC('u', 'd', 't', 'a')
        && (   path[2] == FOURCC('t', 'i', 't', 'l')
            || path[2] == FOURCC('d', 's', 'c', 'p')
            || path[2] == FOURCC('c', 'p', 'r', 't')
            || path[2] == FOURCC('p', 'e', 'r', 'f')
            || path[2] == FOURCC('a', 'u', 't', 'h')
            || path[2] == FOURCC('g', 'n', 'r', 'e')
            || path[2] == FOURCC('r', 't', 'n', 'g')
            || path[2] == FOURCC('c', 'l', 's', 'f')
            || path[2] == FOURCC('k', 'y', 'w', 'd')
            || path[2] == FOURCC('l', 'o', 'c', 'i')
            || path[2] == FOURCC('a', 'l', 'b', 'm')
            || path[2] == FOURCC('y', 'r', 'r', 'c') );	//see sec. 8.2 of 3GPP TS 26.244-700, http://www.3gpp.org/ftp/Specs/archive/26_series/26.244/
}

// Given a time in seconds since Jan 1 1904, produce a human-readable string.
static void convertTimeToDate(int64_t time_1904, String8 *s) {
    time_t time_1970 = time_1904 - (((66 * 365 + 17) * 24) * 3600);

    char tmp[32];
    strftime(tmp, sizeof(tmp), "%Y%m%dT%H%M%S.000Z", gmtime(&time_1970));

    s->setTo(tmp);
}

status_t MOVExtractor::parseChunk(off64_t *offset, int depth) {
    ALOGV("entering parseChunk %lld/%d", *offset, depth);
    uint32_t hdr[2];
    if (mDataSource->readAt(*offset, hdr, 8) < 8) {
        return ERROR_IO;
    }
    uint64_t chunk_size = ntohl(hdr[0]);
    uint32_t chunk_type = ntohl(hdr[1]);
    off64_t data_offset = *offset + 8;

    if (chunk_size == 1) {
        if (mDataSource->readAt(*offset + 8, &chunk_size, 8) < 8) {
            return ERROR_IO;
        }
        chunk_size = ntoh64(chunk_size);
        data_offset += 8;

        if (chunk_size < 16) {
            // The smallest valid chunk is 16 bytes long in this case.
            return ERROR_MALFORMED;
        }
    } else if (chunk_size == 0) {
        // Refer to Gstreamer solution.
        // It seems not aligned with QTFF spec, which says, it is allowed only for a top-level atom,
        // designates the last atom in the file and indicates that the atom extends to the end of the file.
        ALOGV("This chunk is EMPTY!");
        *offset = *offset + 4;
        return OK;
    } else if (chunk_size < 8) {
        // The smallest valid chunk is 8 bytes long.
        return ERROR_MALFORMED;
    }

    char chunk[5];
    MakeFourCCString(chunk_type, chunk);
    ALOGV("chunk: %s @ %lld", chunk, *offset);

#if 0
    static const char kWhitespace[] = "                                        ";
    const char *indent = &kWhitespace[sizeof(kWhitespace) - 1 - 2 * depth];
    ALOGI("%sfound chunk '%s' of size %lld\n", indent, chunk, chunk_size);

    char buffer[256];
    size_t n = chunk_size;
    if (n > sizeof(buffer)) {
        n = sizeof(buffer);
    }
    if (mDataSource->readAt(*offset, buffer, n)
            < (ssize_t)n) {
        return ERROR_IO;
    }

    hexdump(buffer, n);
#endif

    PathAdder autoAdder(&mPath, chunk_type);

    off64_t chunk_data_size = *offset + chunk_size - data_offset;

    if (chunk_type != FOURCC('c', 'p', 'r', 't')
            && chunk_type != FOURCC('c', 'o', 'v', 'r')
            && mPath.size() == 5 && underMetaDataPath(mPath)) {
        off64_t stop_offset = *offset + chunk_size;
        *offset = data_offset;
        while (*offset < stop_offset) {
            status_t err = parseChunk(offset, depth + 1);
            if (err != OK) {
                return err;
            }
        }

        if (*offset != stop_offset) {
            return ERROR_MALFORMED;
        }

        return OK;
    }

    switch(chunk_type) {
        case FOURCC('m', 'o', 'o', 'v'):
        case FOURCC('t', 'r', 'a', 'k'):
        case FOURCC('m', 'd', 'i', 'a'):
        case FOURCC('m', 'i', 'n', 'f'):
        case FOURCC('d', 'i', 'n', 'f'):
        case FOURCC('s', 't', 'b', 'l'):
        case FOURCC('m', 'v', 'e', 'x'):
        case FOURCC('m', 'o', 'o', 'f'):
        //case FOURCC('u', 'd', 't', 'a'):
        case FOURCC('t', 'r', 'a', 'f'):
        case FOURCC('m', 'f', 'r', 'a'):
        case FOURCC('i', 'l', 's', 't'):
        {
            if (chunk_type == FOURCC('s', 't', 'b', 'l')) {
                ALOGV("sampleTable chunk is %d bytes long.", (size_t)chunk_size);

                if (mDataSource->flags()
                        & (DataSource::kWantsPrefetching
                            | DataSource::kIsCachingDataSource)) {
                    sp<MOVDataSource> cachedSource =
                        new MOVDataSource(mDataSource);

                    if (cachedSource->setCachedRange(*offset, chunk_size) == OK) {
                        mDataSource = cachedSource;
                    }
                }

                mLastTrack->sampleTable = new SampleTable(mDataSource);
            }

            bool isTrack = false;
            if (chunk_type == FOURCC('t', 'r', 'a', 'k')) {
                isTrack = true;

                Track *track = new Track;
                track->next = NULL;
                if (mLastTrack) {
                    mLastTrack->next = track;
                } else {
                    mFirstTrack = track;
                }
                mLastTrack = track;

                track->meta = new MetaData;
                track->includes_expensive_metadata = false;
                track->skipTrack = false;
                track->timescale = 0;
                track->meta->setCString(kKeyMIMEType, "application/octet-stream");
            }

            off64_t stop_offset = *offset + chunk_size;
            *offset = data_offset;
            while (*offset < stop_offset) {
                status_t err = parseChunk(offset, depth + 1);
                if (err != OK) {
                    return err;
                }
            }

            if (*offset != stop_offset) {
                return ERROR_MALFORMED;
            }

            if (isTrack) {
                if (mLastTrack->skipTrack) {
                    Track *cur = mFirstTrack;

                    if (cur == mLastTrack) {
                        delete cur;
                        mFirstTrack = mLastTrack = NULL;
                    } else {
                        while (cur && cur->next != mLastTrack) {
                            cur = cur->next;
                        }
                        cur->next = NULL;
                        delete mLastTrack;
                        mLastTrack = cur;
                    }

                    return OK;
                }

                status_t err = verifyTrack(mLastTrack);

                if (err != OK) {
                    return err;
                }
            } else if (chunk_type == FOURCC('m', 'o', 'o', 'v')) {
                mInitCheck = OK;

                if (!mIsDrm) {
                    return UNKNOWN_ERROR;  // Return a dummy error.
                } else {
                    return OK;
                }
            } else if (chunk_type == FOURCC('s', 't', 'b', 'l')) {

                //rebuild sample table.
                status_t err = mLastTrack->sampleTable->rebuildSampleTable();

                if (err != OK) {
                    return err;
                }

                size_t max_size;
                err = mLastTrack->sampleTable->getMaxSampleSize(&max_size);

                if (err != OK) {
                    return err;
                }

                if ((max_size <= 0) || (max_size >= 3*1024*1024)) {
                    //if max_size is greather than 1080p or less than 0, we think the max_size is wrong,
                    //set it as default value
                    ALOGW("max_size=%d is wrong, set default value 512k\n", max_size);
                    max_size = 512*1024;
                }
                // Assume that a given buffer only contains at most 10 fragments,
                // each fragment originally prefixed with a 2 byte length will
                // have a 4 byte header (0x00 0x00 0x00 0x01) after conversion,
                // and thus will grow by 2 bytes per fragment.
                mLastTrack->meta->setInt32(kKeyMaxInputSize, max_size + 10 * 2);

                // Calculate average frame rate.
                const char *mime;
                CHECK(mLastTrack->meta->findCString(kKeyMIMEType, &mime));
                if (!strncasecmp("video/", mime, 6)) {
                    size_t nSamples = mLastTrack->sampleTable->countSamples();
                    int64_t durationUs;
                    if (mLastTrack->meta->findInt64(kKeyDuration, &durationUs)) {
                        if (durationUs > 0) {
                            int32_t frameRate = (nSamples * 1000000LL +
                                                    (durationUs >> 1)) / durationUs;
                            mLastTrack->meta->setInt32(kKeyFrameRate, frameRate);
                        }
                    }
                }
            }
            break;
        }

        case FOURCC('t', 'k', 'h', 'd'):
        {
            status_t err;
            if ((err = parseTrackHeader(data_offset, chunk_data_size)) != OK) {
                return err;
            }

            *offset += chunk_size;
            break;
        }

        case FOURCC('m', 'd', 'h', 'd'):
        {
            if (chunk_data_size < 4) {
                return ERROR_MALFORMED;
            }

            uint8_t version;
            if (mDataSource->readAt(
                        data_offset, &version, sizeof(version))
                    < (ssize_t)sizeof(version)) {
                return ERROR_IO;
            }

            off64_t timescale_offset;

            if (version == 1) {
                timescale_offset = data_offset + 4 + 16;
            } else if (version == 0) {
                timescale_offset = data_offset + 4 + 8;
            } else {
                return ERROR_IO;
            }

            uint32_t timescale;
            if (mDataSource->readAt(
                        timescale_offset, &timescale, sizeof(timescale))
                    < (ssize_t)sizeof(timescale)) {
                return ERROR_IO;
            }

            mLastTrack->timescale = ntohl(timescale);

            int64_t duration;
            if (version == 1) {
                if (mDataSource->readAt(
                            timescale_offset + 4, &duration, sizeof(duration))
                        < (ssize_t)sizeof(duration)) {
                    return ERROR_IO;
                }
                duration = ntoh64(duration);
            } else {
                int32_t duration32;
                if (mDataSource->readAt(
                            timescale_offset + 4, &duration32, sizeof(duration32))
                        < (ssize_t)sizeof(duration32)) {
                    return ERROR_IO;
                }
                duration = ntohl(duration32);
            }
            mLastTrack->meta->setInt64(
                    kKeyDuration, (duration * 1000000) / mLastTrack->timescale);

            uint8_t lang[2];
            off64_t lang_offset;
            if (version == 1) {
                lang_offset = timescale_offset + 4 + 8;
            } else if (version == 0) {
                lang_offset = timescale_offset + 4 + 4;
            } else {
                return ERROR_IO;
            }

            if (mDataSource->readAt(lang_offset, &lang, sizeof(lang))
                    < (ssize_t)sizeof(lang)) {
                return ERROR_IO;
            }

            // To get the ISO-639-2/T three character language code
            // 1 bit pad followed by 3 5-bits characters. Each character
            // is packed as the difference between its ASCII value and 0x60.
            char lang_code[4];
            lang_code[0] = ((lang[0] >> 2) & 0x1f) + 0x60;
            lang_code[1] = ((lang[0] & 0x3) << 3 | (lang[1] >> 5)) + 0x60;
            lang_code[2] = (lang[1] & 0x1f) + 0x60;
            lang_code[3] = '\0';

            mLastTrack->meta->setCString(
                    kKeyMediaLanguage, lang_code);

            *offset += chunk_size;
            break;
        }

        case FOURCC('s', 't', 's', 'd'):
        {
            if (chunk_data_size < 8) {
                return ERROR_MALFORMED;
            }

            uint8_t buffer[8];
            if (chunk_data_size < (off64_t)sizeof(buffer)) {
                return ERROR_MALFORMED;
            }

            if (mDataSource->readAt(
                        data_offset, buffer, 8) < 8) {
                return ERROR_IO;
            }

            if (U32_AT(buffer) != 0) {
                // Should be version 0, flags 0.
                return ERROR_MALFORMED;
            }

            uint32_t entry_count = U32_AT(&buffer[4]);

            if (entry_count > 1) {
                // For 3GPP timed text, there could be multiple tx3g boxes contain
                // multiple text display formats. These formats will be used to
                // display the timed text.
                const char *mime;
                CHECK(mLastTrack->meta->findCString(kKeyMIMEType, &mime));
                if (strcasecmp(mime, MEDIA_MIMETYPE_TEXT_3GPP)) {
                    // For now we only support a single type of media per track.
                    mLastTrack->skipTrack = true;
                    *offset += chunk_size;
                    break;
                }
            }

            off64_t stop_offset = *offset + chunk_size;
            *offset = data_offset + 8;
            for (uint32_t i = 0; i < entry_count; ++i) {
                status_t err = parseChunk(offset, depth + 1);
                if (err != OK) {
                    return err;
                }
            }

            if (*offset != stop_offset) {
                return ERROR_MALFORMED;
            }
            break;
        }
        case FOURCC('e', 'n', 'd', 'a'):
        {
            uint8_t buffer[2];
            if (mDataSource->readAt(
                        data_offset, buffer, sizeof(buffer)) < (ssize_t)sizeof(buffer)) {
                return ERROR_IO;
            }
            uint32_t endiantype = U16_AT(&buffer[0]);

            if (endiantype) {
                mIsLittleEndian = true;
            }
            *offset += chunk_size;
            break;
        }
        case FOURCC('m', 'p', '4', 'a'):
        case FOURCC('s', 'a', 'm', 'r'):
        case FOURCC('s', 'a', 'w', 'b'):
        case FOURCC('.', 'm', 'p', '3'):
        case FOURCC('.', 'm', 'p', '2'):
        case FOURCC('m', 's', 0, 'U'):
        case FOURCC('a', 'c', '-', '3'):
        case FOURCC('e', 'c', '-', '3'):
        case FOURCC('s', 'o', 'w', 't'):
        case FOURCC('t', 'w', 'o', 's'):
        case FOURCC('f', 'l', '3', '2'):
        case FOURCC('W', 'M', 'A', '2'):
        case FOURCC('m', 's', 'V', 'o'):
        {
            uint8_t buffer[8 + 20];
            if (chunk_type == FOURCC('m', 's', 0, 'U')) {
                if (chunk_data_size < (ssize_t)sizeof(buffer) + 16) {
                    *offset += chunk_size;
                    break;
                }
            }
            if (chunk_data_size < (ssize_t)sizeof(buffer)) {
                // For mp4a, there are two cases: mp4a atom and mp4a entry in stsd atom.
                // If it is an mp4a atom, just skip it.
                if(chunk_type == FOURCC('m', 'p', '4', 'a')){
                    *offset += chunk_size;
                    ALOGV("Skip the small mp4a atom.");
                    break;
                }
                // Basic AudioSampleEntry size.
                *offset += chunk_size;
                return ERROR_MALFORMED;
            }

            if (mDataSource->readAt(
                        data_offset, buffer, sizeof(buffer)) < (ssize_t)sizeof(buffer)) {
                return ERROR_IO;
            }

            uint16_t data_ref_index = U16_AT(&buffer[6]);
            uint16_t num_channels = U16_AT(&buffer[16]);

            uint16_t sample_size = U16_AT(&buffer[18]);
            uint32_t sample_rate = U32_AT(&buffer[24]) >> 16;

            if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_AMR_NB,
                            FourCC2MIME(chunk_type))) {
                // AMR NB audio is always mono, 8kHz
                num_channels = 1;
                sample_rate = 8000;
            } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_AMR_WB,
                               FourCC2MIME(chunk_type))) {
                // AMR WB audio is always mono, 16kHz
                num_channels = 1;
                sample_rate = 16000;
            }

#if 0
            printf("*** coding='%s' %d channels, size %d, rate %d\n",
                   chunk, num_channels, sample_size, sample_rate);
#endif

            mLastTrack->meta->setCString(kKeyMIMEType, FourCC2MIME(chunk_type));
            mLastTrack->meta->setInt32(kKeyChannelCount, num_channels);
            mLastTrack->meta->setInt32(kKeySampleRate, sample_rate);

            off64_t stop_offset = *offset + chunk_size;

            uint16_t version = U16_AT(&buffer[8]);
            switch(version){
                case 0:
                    *offset = data_offset + 28;
                    break;
                case 1:
                    mLastTrack->sampleTable->setSoundDescParams(data_offset, 44);
                    *offset = data_offset + 44;
                    break;
                case 2:
                    *offset = data_offset + 64;
                    break;
                default:
                    ALOGE("unsupported sample description entry version %d, only support 0, 1, 2", version);
                    break;
            }

            while (*offset < stop_offset) {
                status_t err = parseChunk(offset, depth + 1);
                if (err != OK) {
                    return err;
                }
            }

            if (*offset != stop_offset) {
                return ERROR_MALFORMED;
            }

            mLastTrack->sampleTable->setCodecType(chunk_type, mIsLittleEndian);

            break;
        }
        case FOURCC('S', 'V', 'Q', '3'):
        {
            mHasVideo = true;

            uint8_t buffer[4];

            if (mDataSource->readAt(
                data_offset + 24, buffer, sizeof(buffer)) < (ssize_t)sizeof(buffer)) {
                return ERROR_IO;
            }

            uint16_t width = U16_AT(&buffer[0]);
            uint16_t height = U16_AT(&buffer[2]);

            width = (width + 15) & -16;
            height = (height + 15) & -16;

            if (width == 0)  width  = 352;
            if (height == 0) height = 288;

            ALOGV("*** coding='%s' width=%d height=%d\n", chunk, width, height);

            mLastTrack->meta->setCString(kKeyMIMEType, FourCC2MIME(chunk_type));
            mLastTrack->meta->setInt32(kKeyWidth, width);
            mLastTrack->meta->setInt32(kKeyHeight, height);

            *offset += chunk_size;
            break;
        }
        case FOURCC('g', 'l', 'b', 'l'):
        {
            mGlobalHeader = new ABuffer(chunk_data_size);
            if(mDataSource->readAt(
                    data_offset, mGlobalHeader->data(), mGlobalHeader->size()) < chunk_data_size) {
                return ERROR_IO;
            }

            mGlobalHeader->setRange(0, chunk_data_size);

            *offset += chunk_size;
            break;
        }
        case FOURCC('h', 'd', 'v', '1'):
        case FOURCC('m', '2', 'v', '1'):
        case FOURCC('D', 'X', '5', '0'):
        case FOURCC('D', 'I', 'V', 'X'):
        case FOURCC('X', 'V', 'I', 'D'):
        case FOURCC('m', 'p', '4', 'v'):
        case FOURCC('s', '2', '6', '3'):
        case FOURCC('H', '2', '6', '3'):
        case FOURCC('h', '2', '6', '3'):
        case FOURCC('a', 'v', 'c', '1'):
        case FOURCC('W', 'M', 'V', '1'):
        case FOURCC('W', 'M', 'V', '2'):
        {
            mHasVideo = true;

            uint8_t buffer[78];
            if (chunk_data_size < (ssize_t)sizeof(buffer)) {
                // Basic VideoSampleEntry size.
                return ERROR_MALFORMED;
            }

            if (mDataSource->readAt(
                        data_offset, buffer, sizeof(buffer)) < (ssize_t)sizeof(buffer)) {
                return ERROR_IO;
            }

            uint16_t data_ref_index = U16_AT(&buffer[6]);
            uint16_t width = U16_AT(&buffer[6 + 18]);
            uint16_t height = U16_AT(&buffer[6 + 20]);

            // The video sample is not stand-compliant if it has invalid dimension.
            // Use some default width and height value, and
            // let the decoder figure out the actual width and height (and thus
            // be prepared for INFO_FOMRAT_CHANGED event).
            if (width == 0)  width  = 352;
            if (height == 0) height = 288;

            // printf("*** coding='%s' width=%d height=%d\n",
            //        chunk, width, height);

            mLastTrack->meta->setCString(kKeyMIMEType, FourCC2MIME(chunk_type));
            mLastTrack->meta->setInt32(kKeyWidth, width);
            mLastTrack->meta->setInt32(kKeyHeight, height);

            off64_t stop_offset = *offset + chunk_size;
            *offset = data_offset + sizeof(buffer);
            while (*offset < stop_offset) {
                status_t err = parseChunk(offset, depth + 1);
                if (err != OK) {
                    return err;
                }
            }

            if (*offset != stop_offset) {
                return ERROR_MALFORMED;
            }
            break;
        }

        case FOURCC('s', 't', 'c', 'o'):
        case FOURCC('c', 'o', '6', '4'):
        {
            status_t err =
                mLastTrack->sampleTable->setChunkOffsetParams(
                        chunk_type, data_offset, chunk_data_size);

            if (err != OK) {
                return err;
            }

            *offset += chunk_size;
            break;
        }

        case FOURCC('s', 't', 's', 'c'):
        {
            status_t err =
                mLastTrack->sampleTable->setSampleToChunkParams(
                        data_offset, chunk_data_size);

            if (err != OK) {
                return err;
            }

            *offset += chunk_size;
            break;
        }

        case FOURCC('s', 't', 's', 'z'):
        case FOURCC('s', 't', 'z', '2'):
        {
            status_t err =
                mLastTrack->sampleTable->setSampleSizeParams(
                        chunk_type, data_offset, chunk_data_size);

            if (err != OK) {
                return err;
            }
            *offset += chunk_size;
            break;
        }

        case FOURCC('s', 't', 't', 's'):
        {
            status_t err =
                mLastTrack->sampleTable->setTimeToSampleParams(
                        data_offset, chunk_data_size);

            if (err != OK) {
                return err;
            }

            *offset += chunk_size;
            break;
        }

        case FOURCC('c', 't', 't', 's'):
        {
            status_t err =
                mLastTrack->sampleTable->setCompositionTimeToSampleParams(
                        data_offset, chunk_data_size);

            if (err != OK) {
                return err;
            }

            *offset += chunk_size;
            break;
        }

        case FOURCC('s', 't', 's', 's'):
        {
            status_t err =
                mLastTrack->sampleTable->setSyncSampleParams(
                        data_offset, chunk_data_size);

            if (err != OK) {
                return err;
            }

            *offset += chunk_size;
            break;
        }
        case FOURCC('s', 't', 'p', 's'):
        {
            status_t err =
                mLastTrack->sampleTable->addPartialSyncSample(
                        data_offset, chunk_data_size);
            if (err != OK) {
                return err;
            }
            *offset += chunk_size;
            break;
        }
        // @xyz
        case FOURCC('\xA9', 'x', 'y', 'z'):
        {
            // Best case the total data length inside "@xyz" box
            // would be 8, for instance "@xyz" + "\x00\x04\x15\xc7" + "0+0/",
            // where "\x00\x04" is the text string length with value = 4,
            // "\0x15\xc7" is the language code = en, and "0+0" is a
            // location (string) value with longitude = 0 and latitude = 0.
            if (chunk_data_size < 8) {
                return ERROR_MALFORMED;
            }

            // Worst case the location string length would be 18,
            // for instance +90.0000-180.0000, without the trailing "/" and
            // the string length + language code.
            char buffer[18];

            // Substracting 5 from the data size is because the text string length +
            // language code takes 4 bytes, and the trailing slash "/" takes 1 byte.
            off64_t location_length = chunk_data_size - 5;
            if (location_length >= (off64_t) sizeof(buffer)) {
                return ERROR_MALFORMED;
            }

            if (mDataSource->readAt(
                        data_offset + 4, buffer, location_length) < location_length) {
                return ERROR_IO;
            }

            buffer[location_length] = '\0';
            mFileMetaData->setCString(kKeyLocation, buffer);
            *offset += chunk_size;
            break;
        }

        case FOURCC('w', 'a', 'v', 'e'):
        {
            //At least one terminated atom is required to indicated it is ended.
            if (chunk_data_size < 8) {
                return ERROR_MALFORMED;
            }

            off64_t stop_offset = *offset + chunk_size;
            *offset = data_offset;
            while (*offset < stop_offset) {
                status_t err = parseChunk(offset, depth + 1);
                if (err != OK) {
                    return err;
                }
            }

            if (*offset != stop_offset) {
                //For some file, if *offset != stop_offset, here should not be treated as error.
                //return ERROR_MALFORMED;
                *offset = stop_offset;
            }
            break;
        }

        case FOURCC('e', 's', 'd', 's'):
        {
            if (chunk_data_size < 4) {
                return ERROR_MALFORMED;
            }

            //uint8_t buffer[256];
            //if (chunk_data_size > (off64_t)sizeof(buffer)) {
            //    return ERROR_BUFFER_TOO_SMALL;
            //}
            if (chunk_data_size > 8192) {
                ALOGW("ESDS chunk size %lld too large!", chunk_data_size);
                return ERROR_BUFFER_TOO_SMALL;
            }
            uint8_t *buffer = new uint8_t[chunk_data_size];
            if (buffer == NULL) return ERROR_BUFFER_TOO_SMALL;

            if (mDataSource->readAt(
                        data_offset, buffer, chunk_data_size) < chunk_data_size) {
                delete[] buffer;
                buffer = NULL;
                return ERROR_IO;
            }

            if (U32_AT(buffer) != 0) {
                // Should be version 0, flags 0.
                delete[] buffer;
                buffer = NULL;
                return ERROR_MALFORMED;
            }

            mLastTrack->meta->setData(
                    kKeyESDS, kTypeESDS, &buffer[4], chunk_data_size - 4);

            if (mPath.size() >= 2
                    && mPath[mPath.size() - 2] == FOURCC('m', 'p', '4', 'a')) {
                // Information from the ESDS must be relied on for proper
                // setup of sample rate and channel count for MPEG4 Audio.
                // The generic header appears to only contain generic
                // information...

                status_t err = updateAudioTrackInfoFromESDS_MPEG4Audio(
                        &buffer[4], chunk_data_size - 4);

                if (err != OK) {
                    delete[] buffer;
                    buffer = NULL;
                    return err;
                }
            }
            delete[] buffer;
            buffer = NULL;

            *offset += chunk_size;
            break;
        }

        case FOURCC('a', 'v', 'c', 'C'):
        {
            char buffer[256];
            if (chunk_data_size > (off64_t)sizeof(buffer)) {
                return ERROR_BUFFER_TOO_SMALL;
            }
            if (chunk_data_size == 0) {
                //this is one illegal 'avcC' box, ignore it and will NOT set 'avcC' data for decoder node. For zero length avcC, it means that mp4 store h264 as byte-stream format instead of iso 14496-15 format. Set a zero length kKeyAVCC here, and remove it in verifyTrack().
                mLastTrack->meta->setData(kKeyAVCC, kTypeAVCC, buffer, 0);
                *offset += chunk_size;
                break;
            }
            if (chunk_data_size < 7) {//avoid crash caused by CHECK(size>=7) failure in MOVSource construction
                return ERROR_MALFORMED;
            }

            if (mDataSource->readAt(
                        data_offset, buffer, chunk_data_size) < chunk_data_size) {
                return ERROR_IO;
            }

            if (buffer[0] != 1) {//avoid crash caused by CHECK_EQ((unsigned)ptr[0], 1u) failure in MOVSource construction
                return ERROR_MALFORMED;
            }
            mLastTrack->meta->setData(
                    kKeyAVCC, kTypeAVCC, buffer, chunk_data_size);

            *offset += chunk_size;
            break;
        }

        case FOURCC('d', '2', '6', '3'):
        {
            /*
             * d263 contains a fixed 7 bytes part:
             *   vendor - 4 bytes
             *   version - 1 byte
             *   level - 1 byte
             *   profile - 1 byte
             * optionally, "d263" box itself may contain a 16-byte
             * bit rate box (bitr)
             *   average bit rate - 4 bytes
             *   max bit rate - 4 bytes
             */
            char buffer[23];
            if (chunk_data_size != 7 &&
                chunk_data_size != 23) {
                ALOGE("Incorrect D263 box size %lld", chunk_data_size);
                return ERROR_MALFORMED;
            }

            if (mDataSource->readAt(
                    data_offset, buffer, chunk_data_size) < chunk_data_size) {
                return ERROR_IO;
            }

            mLastTrack->meta->setData(kKeyD263, kTypeD263, buffer, chunk_data_size);

            *offset += chunk_size;
            break;
        }

        case FOURCC('m', 'e', 't', 'a'):
        {
            uint8_t buffer[4];
            if (chunk_data_size < (off64_t)sizeof(buffer)) {
                return ERROR_MALFORMED;
            }

            if (mDataSource->readAt(
                        data_offset, buffer, 4) < 4) {
                return ERROR_IO;
            }

            if (U32_AT(buffer) != 0) {
                // Should be version 0, flags 0.

                // If it's not, let's assume this is one of those
                // apparently malformed chunks that don't have flags
                // and completely different semantics than what's
                // in the MPEG4 specs and skip it.
                *offset += chunk_size;
                return OK;
            }

            off64_t stop_offset = *offset + chunk_size;
            *offset = data_offset + sizeof(buffer);
            while (*offset < stop_offset) {
                status_t err = parseChunk(offset, depth + 1);
                if (err != OK) {
                    return err;
                }
            }

            if (*offset != stop_offset) {
                return ERROR_MALFORMED;
            }
            break;
        }

        case FOURCC('m', 'e', 'a', 'n'):
        case FOURCC('n', 'a', 'm', 'e'):
        case FOURCC('d', 'a', 't', 'a'):
        {
            if (mPath.size() == 6 && underMetaDataPath(mPath)) {
                status_t err = parseMetaData(data_offset, chunk_data_size);

                if (err != OK) {
                    return err;
                }
            }

            *offset += chunk_size;
            break;
        }

        case FOURCC('m', 'v', 'h', 'd'):
        {
            if (chunk_data_size < 12) {
                return ERROR_MALFORMED;
            }

            uint8_t header[12];
            if (mDataSource->readAt(
                        data_offset, header, sizeof(header))
                    < (ssize_t)sizeof(header)) {
                return ERROR_IO;
            }

            int64_t creationTime;
            if (header[0] == 1) {
                creationTime = U64_AT(&header[4]);
            } else if (header[0] != 0) {
                return ERROR_MALFORMED;
            } else {
                creationTime = U32_AT(&header[4]);
            }

            String8 s;
            convertTimeToDate(creationTime, &s);

            mFileMetaData->setCString(kKeyDate, s.string());

            *offset += chunk_size;
            break;
        }

        case FOURCC('m', 'd', 'a', 't'):
        {
            if (!mIsDrm) {
                *offset += chunk_size;
                break;
            }

            if (chunk_size < 8) {
                return ERROR_MALFORMED;
            }

            return parseDrmSINF(offset, data_offset);
        }

        case FOURCC('h', 'd', 'l', 'r'):
        {
            uint32_t buffer;
            if (mDataSource->readAt(
                        data_offset + 8, &buffer, 4) < 4) {
                return ERROR_IO;
            }

            uint32_t type = ntohl(buffer);
            // For the 3GPP file format, the handler-type within the 'hdlr' box
            // shall be 'text'. We also want to support 'sbtl' handler type
            // for a practical reason as various MPEG4 containers use it.
            if (type == FOURCC('t', 'e', 'x', 't') || type == FOURCC('s', 'b', 't', 'l')) {
                mLastTrack->meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_TEXT_3GPP);
            }

            *offset += chunk_size;
            break;
        }

        case FOURCC('t', 'x', '3', 'g'):
        {
            uint32_t type;
            const void *data;
            size_t size = 0;
            if (!mLastTrack->meta->findData(
                    kKeyTextFormatData, &type, &data, &size)) {
                size = 0;
            }

            uint8_t *buffer = new uint8_t[size + chunk_size];

            if (size > 0) {
                memcpy(buffer, data, size);
            }

            if ((size_t)(mDataSource->readAt(*offset, buffer + size, chunk_size))
                    < chunk_size) {
                delete[] buffer;
                buffer = NULL;

                return ERROR_IO;
            }

            mLastTrack->meta->setData(
                    kKeyTextFormatData, 0, buffer, size + chunk_size);

            delete[] buffer;

            *offset += chunk_size;
            break;
        }

        case FOURCC('c', 'o', 'v', 'r'):
        {
            if (mFileMetaData != NULL) {
                ALOGV("chunk_data_size = %lld and data_offset = %lld",
                        chunk_data_size, data_offset);
                uint8_t *buffer = new uint8_t[chunk_data_size + 1];
                if (mDataSource->readAt(
                    data_offset, buffer, chunk_data_size) != (ssize_t)chunk_data_size) {
                    delete[] buffer;
                    buffer = NULL;

                    return ERROR_IO;
                }
                const int kSkipBytesOfDataBox = 16;
                mFileMetaData->setData(
                    kKeyAlbumArt, MetaData::TYPE_NONE,
                    buffer + kSkipBytesOfDataBox, chunk_data_size - kSkipBytesOfDataBox);
            }

            *offset += chunk_size;
            break;
        }

        case FOURCC('-', '-', '-', '-'):
        {
            mLastCommentMean.clear();
            mLastCommentName.clear();
            mLastCommentData.clear();
            *offset += chunk_size;
            break;
        }

        default:
        {
            if(is3gpUdtaMetaParsePath(mPath)) {
                parse3gpUdtaMetaData(chunk_type, data_offset, chunk_data_size);
            }
            else{
                char chunk[5];
                MakeFourCCString(chunk_type, chunk);
                ALOGV("Unknown chunk type %s", chunk);
            }
            *offset += chunk_size;
            break;
        }
    }

    return OK;
}

status_t MOVExtractor::parseTrackHeader(
        off64_t data_offset, off64_t data_size) {
    if (data_size < 4) {
        return ERROR_MALFORMED;
    }

    uint8_t version;
    if (mDataSource->readAt(data_offset, &version, 1) < 1) {
        return ERROR_IO;
    }

    size_t dynSize = (version == 1) ? 36 : 24;

    uint8_t buffer[36 + 60];

    if (data_size != (off64_t)dynSize + 60) {
        return ERROR_MALFORMED;
    }

    if (mDataSource->readAt(
                data_offset, buffer, data_size) < (ssize_t)data_size) {
        return ERROR_IO;
    }

    uint64_t ctime, mtime, duration;
    int32_t id;

    if (version == 1) {
        ctime = U64_AT(&buffer[4]);
        mtime = U64_AT(&buffer[12]);
        id = U32_AT(&buffer[20]);
        duration = U64_AT(&buffer[28]);
    } else {
        CHECK_EQ((unsigned)version, 0u);

        ctime = U32_AT(&buffer[4]);
        mtime = U32_AT(&buffer[8]);
        id = U32_AT(&buffer[12]);
        duration = U32_AT(&buffer[20]);
    }

    mLastTrack->meta->setInt32(kKeyTrackID, id);

    size_t matrixOffset = dynSize + 16;
    int32_t a00 = U32_AT(&buffer[matrixOffset]);
    int32_t a01 = U32_AT(&buffer[matrixOffset + 4]);
    int32_t dx = U32_AT(&buffer[matrixOffset + 8]);
    int32_t a10 = U32_AT(&buffer[matrixOffset + 12]);
    int32_t a11 = U32_AT(&buffer[matrixOffset + 16]);
    int32_t dy = U32_AT(&buffer[matrixOffset + 20]);

#if 0
    ALOGI("x' = %.2f * x + %.2f * y + %.2f",
         a00 / 65536.0f, a01 / 65536.0f, dx / 65536.0f);
    ALOGI("y' = %.2f * x + %.2f * y + %.2f",
         a10 / 65536.0f, a11 / 65536.0f, dy / 65536.0f);
#endif

    uint32_t rotationDegrees;

    static const int32_t kFixedOne = 0x10000;
    if (a00 == kFixedOne && a01 == 0 && a10 == 0 && a11 == kFixedOne) {
        // Identity, no rotation
        rotationDegrees = 0;
    } else if (a00 == 0 && a01 == kFixedOne && a10 == -kFixedOne && a11 == 0) {
        rotationDegrees = 90;
    } else if (a00 == 0 && a01 == -kFixedOne && a10 == kFixedOne && a11 == 0) {
        rotationDegrees = 270;
    } else if (a00 == -kFixedOne && a01 == 0 && a10 == 0 && a11 == -kFixedOne) {
        rotationDegrees = 180;
    } else {
        ALOGW("We only support 0,90,180,270 degree rotation matrices");
        rotationDegrees = 0;
    }

    if (rotationDegrees != 0) {
        mLastTrack->meta->setInt32(kKeyRotation, rotationDegrees);
    }

    // Handle presentation display size, which could be different
    // from the image size indicated by kKeyWidth and kKeyHeight.
    uint32_t width = U32_AT(&buffer[dynSize + 52]);
    uint32_t height = U32_AT(&buffer[dynSize + 56]);
    mLastTrack->meta->setInt32(kKeyDisplayWidth, width >> 16);
    mLastTrack->meta->setInt32(kKeyDisplayHeight, height >> 16);

    return OK;
}

status_t MOVExtractor::parse3gpUdtaMetaData(uint32_t type, off64_t offset, size_t size) {
    if(size < 6) {
        return ERROR_MALFORMED;
    }
    if(size > 2047) {
       return ERROR_MALFORMED; //if the meta data is too long, think it's corrupt data, not parse it.
    }
    uint8_t* buffer = new uint8_t[size+1];
    if(mDataSource->readAt(offset, buffer, size) != (ssize_t)size) {
        delete[] buffer;
        buffer = NULL;
        return ERROR_IO;
    }
    uint32_t metadataKey = 0;
    switch(type) {
    case FOURCC('t', 'i', 't', 'l'):
        {
            metadataKey = kKeyTitle;
            break;
        }
    case FOURCC('p', 'e', 'r', 'f'):
        {
            metadataKey = kKeyArtist;
            break;
        }
    case FOURCC('a', 'u', 't', 'h'):
        {
            metadataKey = kKeyAuthor;
            break;
        }
    case FOURCC('g', 'n', 'r', 'e'):
        {
            metadataKey = kKeyGenre;
            break;
        }
    case FOURCC('a', 'l', 'b', 'm'):
        {
            if(buffer[size-1] != 0 && buffer[size-2] == 0) {
                char tmp[8] = {0,};
                sprintf(tmp, "%d", buffer[size - 1]);
                mFileMetaData->setCString(kKeyCDTrackNumber, tmp);
            }
            metadataKey = kKeyAlbum;
            break;
        }
    case FOURCC('y', 'r', 'r', 'c'):
        {
            if(size == 6) {
                char tmp[8] = {0,};
                unsigned int a = buffer[4];
                unsigned int b = buffer[5];
                sprintf(tmp, "%d", (a<<8)|b);
                mFileMetaData->setCString(kKeyYear, tmp);
            }
            break;
        }
    default:
        //For other chunk type, hasn't found suitable kKeyXXXX in MetaData.h. Just skip them.
        break;
    }
    if(size > 6 && metadataKey) {
        buffer[size] = '\0';
        int isUTF8 = 0;
        if(!(buffer[6] == 0xFE && buffer[7] == 0xFF)) {
            isUTF8 = 1;
        }
        if(isUTF8 == 1) {//UTF8
            if(buffer[6] != 0) {//not zero length content
                String8 s8((const char *)buffer + 6);
                mFileMetaData->setCString(metadataKey, s8.string());
            }else{
                ALOGD("string content in udta metadata is zero length(sz %d), not set metadata key %d", size, metadataKey);
            }
        }else{//UTF16
            if(size>=10 && (buffer[8]|buffer[9])!=0) {//not zero length content
                mFileMetaData->setCString(metadataKey, (const char *)buffer + 6);
            }else{
                ALOGD("UTF16 string content in udta metadata is zero length(sz %d), not set metadata key %d", size, metadataKey);
            }
        }
    }
    delete[] buffer;
    buffer = NULL;

    return OK;
}

status_t MOVExtractor::parseMetaData(off64_t offset, size_t size) {
    if (size < 4) {
        return ERROR_MALFORMED;
    }

    uint8_t *buffer = new uint8_t[size + 1];
    if (mDataSource->readAt(
                offset, buffer, size) != (ssize_t)size) {
        delete[] buffer;
        buffer = NULL;

        return ERROR_IO;
    }

    uint32_t flags = U32_AT(buffer);

    uint32_t metadataKey = 0;
    char chunk[5];
    MakeFourCCString(mPath[4], chunk);
    ALOGV("meta: %s @ %lld", chunk, offset);
    switch (mPath[4]) {
        case FOURCC(0xa9, 'a', 'l', 'b'):
        {
            metadataKey = kKeyAlbum;
            break;
        }
        case FOURCC(0xa9, 'A', 'R', 'T'):
        {
            metadataKey = kKeyArtist;
            break;
        }
        case FOURCC('a', 'A', 'R', 'T'):
        {
            metadataKey = kKeyAlbumArtist;
            break;
        }
        case FOURCC(0xa9, 'd', 'a', 'y'):
        {
            metadataKey = kKeyYear;
            break;
        }
        case FOURCC(0xa9, 'n', 'a', 'm'):
        {
            metadataKey = kKeyTitle;
            break;
        }
        case FOURCC(0xa9, 'w', 'r', 't'):
        {
            metadataKey = kKeyWriter;
            break;
        }
        case FOURCC('c', 'o', 'v', 'r'):
        {
            metadataKey = kKeyAlbumArt;
            break;
        }
        case FOURCC('g', 'n', 'r', 'e'):
        {
            metadataKey = kKeyGenre;
            break;
        }
        case FOURCC(0xa9, 'g', 'e', 'n'):
        {
            metadataKey = kKeyGenre;
            break;
        }
        case FOURCC('c', 'p', 'i', 'l'):
        {
            if (size == 9 && flags == 21) {
                char tmp[16];
                sprintf(tmp, "%d",
                        (int)buffer[size - 1]);

                mFileMetaData->setCString(kKeyCompilation, tmp);
            }
            break;
        }
        case FOURCC('t', 'r', 'k', 'n'):
        {
            if (size == 16 && flags == 0) {
                char tmp[16];
                sprintf(tmp, "%d/%d",
                        (int)buffer[size - 5], (int)buffer[size - 3]);

                mFileMetaData->setCString(kKeyCDTrackNumber, tmp);
            }
            break;
        }
        case FOURCC('d', 'i', 's', 'k'):
        {
            if (size == 14 && flags == 0) {
                char tmp[16];
                sprintf(tmp, "%d/%d",
                        (int)buffer[size - 3], (int)buffer[size - 1]);

                mFileMetaData->setCString(kKeyDiscNumber, tmp);
            }
            break;
        }
        case FOURCC('-', '-', '-', '-'):
        {
            buffer[size] = '\0';
            switch (mPath[5]) {
                case FOURCC('m', 'e', 'a', 'n'):
                    mLastCommentMean.setTo((const char *)buffer + 4);
                    break;
                case FOURCC('n', 'a', 'm', 'e'):
                    mLastCommentName.setTo((const char *)buffer + 4);
                    break;
                case FOURCC('d', 'a', 't', 'a'):
                    mLastCommentData.setTo((const char *)buffer + 8);
                    break;
            }
            if (mLastCommentMean == "com.apple.iTunes"
                    && mLastCommentName == "iTunSMPB"
                    && mLastCommentData.length() != 0) {
                int32_t delay, padding;
                if (sscanf(mLastCommentData,
                           " %*x %x %x %*x", &delay, &padding) == 2) {
                    mLastTrack->meta->setInt32(kKeyEncoderDelay, delay);
                    mLastTrack->meta->setInt32(kKeyEncoderPadding, padding);
                }
                mLastCommentMean.clear();
                mLastCommentName.clear();
                mLastCommentData.clear();
            }
            break;
        }

        default:
            break;
    }

    if (size >= 8 && metadataKey) {
        if (metadataKey == kKeyAlbumArt) {
            mFileMetaData->setData(
                    kKeyAlbumArt, MetaData::TYPE_NONE,
                    buffer + 8, size - 8);
        } else if (metadataKey == kKeyGenre) {
            if (flags == 0) {
                // uint8_t genre code, iTunes genre codes are
                // the standard id3 codes, except they start
                // at 1 instead of 0 (e.g. Pop is 14, not 13)
                // We use standard id3 numbering, so subtract 1.
                int genrecode = (int)buffer[size - 1];
                genrecode--;
                if (genrecode < 0) {
                    genrecode = 255; // reserved for 'unknown genre'
                }
                char genre[10];
                sprintf(genre, "%d", genrecode);

                mFileMetaData->setCString(metadataKey, genre);
            } else if (flags == 1) {
                // custom genre string
                buffer[size] = '\0';

                mFileMetaData->setCString(
                        metadataKey, (const char *)buffer + 8);
            }
        } else {
            buffer[size] = '\0';

            mFileMetaData->setCString(
                    metadataKey, (const char *)buffer + 8);
        }
    }

    delete[] buffer;
    buffer = NULL;

    return OK;
}

sp<MediaSource> MOVExtractor::getTrack(size_t index) {
    status_t err;
    if ((err = readMetaData()) != OK) {
        return NULL;
    }

    Track *track = mFirstTrack;
    while (index > 0) {
        if (track == NULL) {
            return NULL;
        }

        track = track->next;
        --index;
    }

    if (track == NULL) {
        return NULL;
    }

    return new MOVSource(
            track->meta, mDataSource, track->timescale, track->sampleTable);
}

// static
status_t MOVExtractor::verifyTrack(Track *track) {
    const char *mime;
    CHECK(track->meta->findCString(kKeyMIMEType, &mime));

    uint32_t type;
    const void *data;
    size_t size;
    if (!strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_AVC)) {
        if (!track->meta->findData(kKeyAVCC, &type, &data, &size)
                || type != kTypeAVCC) {
            return ERROR_MALFORMED;
        }
        if (size == 0){//for zero length avcC
            ALOGW("!!! No 'avcC' data is found for mp4 file");
            track->meta->remove(kKeyAVCC);
        }
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_MPEG4)) {
        if ((!track->meta->findData(kKeyESDS, &type, &data, &size)
                || type != kTypeESDS)) {
            if (mGlobalHeader != NULL) {
                sp<ABuffer> csd = MakeMPEG4CodecSpecificData(mGlobalHeader);
                track->meta->setData(
                    kKeyESDS, kTypeESDS, csd->data(), csd->size());
            }
        }
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_AAC)) {
        if (!track->meta->findData(kKeyESDS, &type, &data, &size)
                || type != kTypeESDS) {
            return ERROR_MALFORMED;
        }
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_WMAV2)) {
        if (mGlobalHeader != NULL) {
            track->meta->setData(kFfmpegCodecSpecificData,
                            0, mGlobalHeader->data(), mGlobalHeader->size());
        }
    }else if (!strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_WMV1)
                        || !strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_WMV2)) {
        if (mGlobalHeader != NULL) {
            track->meta->setData(kKeyRawCodecSpecificData,
                            0, mGlobalHeader->data(), mGlobalHeader->size());
        }
    }else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_VORBIS)) {
        if (mGlobalHeader != NULL) {
            uint8_t *privatedata = mGlobalHeader->data();
            size_t len1 = privatedata[1];
            size_t len2 = privatedata[2];

            track->meta->setData(kKeyVorbisInfo, 0, &privatedata[3], len1);
            track->meta->setData(
                    kKeyVorbisBooks, 0, &privatedata[len1 + len2 + 3],
                    mGlobalHeader->size() - len1 - len2 - 3);
        }
    }

    if (!track->sampleTable->isValid()) {
        // Make sure we have all the metadata we need.
        return ERROR_MALFORMED;
    }

    return OK;
}

status_t MOVExtractor::updateAudioTrackInfoFromESDS_MPEG4Audio(
        const void *esds_data, size_t esds_size) {
    ESDS esds(esds_data, esds_size);

    uint8_t objectTypeIndication;
    if (esds.getObjectTypeIndication(&objectTypeIndication) != OK) {
        return ERROR_MALFORMED;
    }

    if (objectTypeIndication == 0xe1) {
        // This isn't MPEG4 audio at all, it's QCELP 14k...
        mLastTrack->meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_QCELP);
        return OK;
    }

    if (objectTypeIndication  == 0x6b) {
        // The media subtype is MP3 audio
        // Our software MP3 audio decoder may not be able to handle
        // packetized MP3 audio; for now, lets just return ERROR_UNSUPPORTED
        //ALOGE("MP3 track in MP4/3GPP file is not supported");
        //return ERROR_UNSUPPORTED;
        //Mrvl 2012.08, support MP3 in .mp4
        mLastTrack->meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_MPEG);
        return OK;
    }

    const uint8_t *csd;
    size_t csd_size;
    if (esds.getCodecSpecificInfo(
                (const void **)&csd, &csd_size) != OK) {
        return ERROR_MALFORMED;
    }

#if 0
    printf("ESD of size %d\n", csd_size);
    hexdump(csd, csd_size);
#endif

    if (csd_size == 0) {
        // There's no further information, i.e. no codec specific data
        // Let's assume that the information provided in the mpeg4 headers
        // is accurate and hope for the best.

        return OK;
    }

    if (csd_size < 2) {
        return ERROR_MALFORMED;
    }

    ABitReader br(csd, csd_size);
    uint32_t objectType = br.getBits(5);

    if (objectType == 31) {  // AAC-ELD => additional 6 bits
        objectType = 32 + br.getBits(6);
    }

    uint32_t freqIndex = br.getBits(4);

    int32_t sampleRate = 0;
    int32_t numChannels = 0;
    if (freqIndex == 15) {
        if (csd_size < 5) {
            return ERROR_MALFORMED;
        }
        sampleRate = br.getBits(24);
        numChannels = br.getBits(4);
    } else {
        static uint32_t kSamplingRate[] = {
            96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
            16000, 12000, 11025, 8000, 7350
        };

        if (freqIndex == 13 || freqIndex == 14) {
            return ERROR_MALFORMED;
        }

        sampleRate = kSamplingRate[freqIndex];
        numChannels = br.getBits(4);
    }

    if (numChannels == 0) {
        ALOGW("numChannels in ESDS is 0, ignore the error!");
        //return ERROR_UNSUPPORTED;
    }

    int32_t prevSampleRate;
    CHECK(mLastTrack->meta->findInt32(kKeySampleRate, &prevSampleRate));

    if (prevSampleRate != sampleRate) {
        ALOGV("mpeg4 audio sample rate different from previous setting. "
             "was: %d, now: %d", prevSampleRate, sampleRate);
    }

    mLastTrack->meta->setInt32(kKeySampleRate, sampleRate);

    int32_t prevChannelCount;
    CHECK(mLastTrack->meta->findInt32(kKeyChannelCount, &prevChannelCount));

    if (prevChannelCount != numChannels) {
        ALOGV("mpeg4 audio channel count different from previous setting. "
             "was: %d, now: %d", prevChannelCount, numChannels);
        if (numChannels == 0) numChannels = prevChannelCount;
    }

    mLastTrack->meta->setInt32(kKeyChannelCount, numChannels);

    return OK;
}

////////////////////////////////////////////////////////////////////////////////
static double rint(double x) {
    return x >= 0 ? floor(x + 0.5) : ceil(x - 0.5);
}

static long int lrintf(float x) {
    return (int)(rint(x));
}

static int16_t clip_int16_c(int a) {
    if ((a+0x8000) & ~0xFFFF) return (a>>31) ^ 0x7FFF;
    else return a;
}

static status_t getRawS16LE(void *in, size_t *size, enum CodecId codecid){

    int8_t mbuf[*size];

    if (codecid == CODECID_PCM_S16LE) {
        return OK;
    } else if (codecid == CODECID_PCM_S16BE) {
        int16_t *src = (int16_t *)in;
        int16_t *dst = (int16_t *)mbuf;
        for (size_t i = 0; i < *size/2; i++) {
            *dst++ = U16_AT((uint8_t *)src++);
        }
        memcpy((int8_t *)in, mbuf,*size);
    } else if (codecid == CODECID_PCM_F32LE) {
        // Convert float 32-bit LE to signed 16-bit LE.
        float *src = (float *)in;
        int16_t *dst = (int16_t *)mbuf;
        for (size_t i = 0; i < *size/4; ++i) {
            int16_t x =clip_int16_c(lrintf(src[0] * (1<<15)));
            *dst++ = x;
            src += 1;
        }
        *size /= 2;
        memcpy((int8_t *)in, mbuf,*size);
    } else if (codecid == CODECID_PCM_F32BE) {
        //First, data from *in to mbuf, convert big endian to little endian.
        //Then data from mbuf to *in, convert float 32-bit BE to signed 16-bit LE,
        //and not need memcpy next.
        int32_t *tmpsrc = (int32_t *)in;
        int32_t *tmpdst = (int32_t *)mbuf;
        float *src = (float *)mbuf;
        int16_t *dst = (int16_t *)in;
        for (size_t i = 0; i < *size/4; i++) {
            *tmpdst++ = U32_AT((uint8_t *)tmpsrc++);
            int16_t x =clip_int16_c(lrintf(src[0] * (1<<15)));
            *dst++ = x;
            src += 1;
        }
        *size /= 2;
    }
    return OK;
}

static status_t getMPEGFrame(uint8_t *data, size_t *size){
    int i = 0;
    while (i < *size) {
        if (data[i] == 0xff && (data[i + 1] == 0xfa || data[i + 1] == 0xfb)) {
            if ( i != 0) {
                memmove(data, data + i, i);
                *size -= i;
            }
            break;
        }
        i++;
    }
    if (i == *size) {
        return ERROR_MALFORMED;
    }
    return OK;
}

MOVSource::MOVSource(
        const sp<MetaData> &format,
        const sp<DataSource> &dataSource,
        int32_t timeScale,
        const sp<SampleTable> &sampleTable)
    : mFormat(format),
      mDataSource(dataSource),
      mTimescale(timeScale),
      mSampleTable(sampleTable),
      mCurrentSampleIndex(0),
      mIsAVC(false),
      mNALLengthSize(0),
      mStarted(false),
      mGroup(NULL),
      mBuffer(NULL),
      mWantsNALFragments(false),
      mSrcBuffer(NULL) {
    const char *mime;
    bool success = mFormat->findCString(kKeyMIMEType, &mime);
    CHECK(success);

    mIsAVC = !strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_AVC);

    if (mIsAVC) {
        uint32_t type;
        const void *data;
        size_t size;
        if (format->findData(kKeyAVCC, &type, &data, &size) && size != 0){

        const uint8_t *ptr = (const uint8_t *)data;

        CHECK(size >= 7);
        CHECK_EQ((unsigned)ptr[0], 1u);  // configurationVersion == 1

        // The number of bytes used to encode the length of a NAL unit.
        mNALLengthSize = 1 + (ptr[4] & 3);

        }else {
            mNALLengthSize = 0;//indicate it's zero length avcC mp4 file.
        }
    }
}

MOVSource::~MOVSource() {
    if (mStarted) {
        stop();
    }
}

status_t MOVSource::start(MetaData *params) {
    Mutex::Autolock autoLock(mLock);

    CHECK(!mStarted);

    int32_t val;
    if (params && params->findInt32(kKeyWantsNALFragments, &val)
        && val != 0) {
        mWantsNALFragments = true;
    } else {
        mWantsNALFragments = false;
    }

    mGroup = new MediaBufferGroup;

    int32_t max_size;
    CHECK(mFormat->findInt32(kKeyMaxInputSize, &max_size));

    mGroup->add_buffer(new MediaBuffer(max_size));

    mSrcBuffer = new uint8_t[max_size];

    mStarted = true;

    return OK;
}

status_t MOVSource::stop() {
    Mutex::Autolock autoLock(mLock);

    CHECK(mStarted);

    if (mBuffer != NULL) {
        mBuffer->release();
        mBuffer = NULL;
    }

    delete[] mSrcBuffer;
    mSrcBuffer = NULL;

    delete mGroup;
    mGroup = NULL;

    mStarted = false;
    mCurrentSampleIndex = 0;

    return OK;
}

sp<MetaData> MOVSource::getFormat() {
    Mutex::Autolock autoLock(mLock);

    return mFormat;
}

size_t MOVSource::parseNALSize(const uint8_t *data) const {
    switch (mNALLengthSize) {
        case 1:
            return *data;
        case 2:
            return U16_AT(data);
        case 3:
            return ((size_t)data[0] << 16) | U16_AT(&data[1]);
        case 4:
            return U32_AT(data);
    }

    // This cannot happen, mNALLengthSize springs to life by adding 1 to
    // a 2-bit integer.
    CHECK(!"Should not be here.");

    return 0;
}

status_t MOVSource::read(
        MediaBuffer **out, const ReadOptions *options) {
    Mutex::Autolock autoLock(mLock);

    CHECK(mStarted);

    *out = NULL;

    int64_t targetSampleTimeUs = -1;

    int64_t seekTimeUs;
    ReadOptions::SeekMode mode;
    if (options && options->getSeekTo(&seekTimeUs, &mode)) {
        uint32_t findFlags = 0;
        switch (mode) {
            case ReadOptions::SEEK_PREVIOUS_SYNC:
                findFlags = SampleTable::kFlagBefore;
                break;
            case ReadOptions::SEEK_NEXT_SYNC:
                findFlags = SampleTable::kFlagAfter;
                break;
            case ReadOptions::SEEK_CLOSEST_SYNC:
            case ReadOptions::SEEK_CLOSEST:
                findFlags = SampleTable::kFlagClosest;
                break;
            default:
                CHECK(!"Should not be here.");
                break;
        }

        uint32_t sampleIndex=0;
        status_t err = mSampleTable->findSampleAtTime(
                seekTimeUs * mTimescale / 1000000,
                &sampleIndex, findFlags);

        if (mode == ReadOptions::SEEK_CLOSEST) {
            // We found the closest sample already, now we want the sync
            // sample preceding it (or the sample itself of course), even
            // if the subsequent sync sample is closer.
            findFlags = SampleTable::kFlagBefore;
        }

        uint32_t syncSampleIndex=mCurrentSampleIndex;
        if (err == OK) {
            err = mSampleTable->findSyncSampleNear(
                    sampleIndex, &syncSampleIndex, findFlags);
        }
        uint32_t sampleTime;
        if (err == OK) {
            err = mSampleTable->getMetaDataForSample(
                    sampleIndex, NULL, NULL, &sampleTime);
        }

        if (err != OK) {
            if (err == ERROR_OUT_OF_RANGE) {
                // An attempt to seek past the end of the stream would
                // normally cause this ERROR_OUT_OF_RANGE error. Propagating
                // this all the way to the MediaPlayer would cause abnormal
                // termination. Legacy behaviour appears to be to behave as if
                // we had seeked to the end of stream, ending normally.
                err = ERROR_END_OF_STREAM;
            }
            return err;
        }

        if (mode == ReadOptions::SEEK_CLOSEST) {
            targetSampleTimeUs = (sampleTime * 1000000ll) / mTimescale;
        }

#if 0
        uint32_t syncSampleTime;
        CHECK_EQ(OK, mSampleTable->getMetaDataForSample(
                    syncSampleIndex, NULL, NULL, &syncSampleTime));

        ALOGI("seek to time %lld us => sample at time %lld us, "
             "sync sample at time %lld us",
             seekTimeUs,
             sampleTime * 1000000ll / mTimescale,
             syncSampleTime * 1000000ll / mTimescale);
#endif

        mCurrentSampleIndex = syncSampleIndex;
        if (mBuffer != NULL) {
            mBuffer->release();
            mBuffer = NULL;
        }

        // fall through
    }

    off64_t offset;
    size_t size;
    uint32_t cts;
    bool isSyncSample;
    bool newBuffer = false;
    if (mBuffer == NULL) {
        newBuffer = true;

        status_t err =
            mSampleTable->getMetaDataForSample(
                    mCurrentSampleIndex, &offset, &size, &cts, &isSyncSample);

        if (err != OK) {
            return err;
        }

        err = mGroup->acquire_buffer(&mBuffer);

        if (err != OK) {
            CHECK(mBuffer == NULL);
            return err;
        }
    }

    if (!mIsAVC || mWantsNALFragments) {
        if (newBuffer) {
            if (mBuffer->size() < size) {
                ALOGE("Actual frame size %d is greater than buffer size %d\n", size, mBuffer->size());
                return ERROR_MALFORMED;
            }
            ssize_t num_bytes_read =
                mDataSource->readAt(offset, (uint8_t *)mBuffer->data(), size);

            if (num_bytes_read < (ssize_t)size) {
                mBuffer->release();
                mBuffer = NULL;

                return ERROR_IO;
            }

            CHECK(mBuffer != NULL);

            const char *mime;
            mFormat->findCString(kKeyMIMEType, &mime);

            if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_MPEG)) {
                status_t err = getMPEGFrame((uint8_t *)mBuffer->data(), &size);
                if (err != OK) {
                    mBuffer->release();
                    mBuffer = NULL;
                    return err;
                }
            }

            if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_RAW)) {
                enum CodecId codecid = mSampleTable->getCodecID();
                status_t err = getRawS16LE(mBuffer->data(), &size, codecid);
                if (err != OK) {
                    mBuffer->release();
                    mBuffer = NULL;
                    return err;
                }
            }

            mBuffer->set_range(0, size);
            mBuffer->meta_data()->clear();
            mBuffer->meta_data()->setInt64(
                    kKeyTime, ((int64_t)cts * 1000000) / mTimescale);

            if (targetSampleTimeUs >= 0) {
                mBuffer->meta_data()->setInt64(
                        kKeyTargetTime, targetSampleTimeUs);
            }

            if (isSyncSample) {
                mBuffer->meta_data()->setInt32(kKeyIsSyncFrame, 1);
            }

            ++mCurrentSampleIndex;
        }

        if (!mIsAVC) {
            *out = mBuffer;
            mBuffer = NULL;

            return OK;
        }

        // Each NAL unit is split up into its constituent fragments and
        // each one of them returned in its own buffer.

        CHECK(mBuffer->range_length() >= mNALLengthSize);

        const uint8_t *src =
            (const uint8_t *)mBuffer->data() + mBuffer->range_offset();

        size_t nal_size;
        if (mNALLengthSize){
            nal_size = parseNALSize(src);
        } else {//it's zero length avcC mp4 case
            nal_size = mBuffer->range_length();
        }
        if (mBuffer->range_length() < mNALLengthSize + nal_size) {
            ALOGE("incomplete NAL unit.");

            mBuffer->release();
            mBuffer = NULL;

            return ERROR_MALFORMED;
        }

        MediaBuffer *clone = mBuffer->clone();
        CHECK(clone != NULL);
        clone->set_range(mBuffer->range_offset() + mNALLengthSize, nal_size);

        CHECK(mBuffer != NULL);
        mBuffer->set_range(
                mBuffer->range_offset() + mNALLengthSize + nal_size,
                mBuffer->range_length() - mNALLengthSize - nal_size);

        if (mBuffer->range_length() == 0) {
            mBuffer->release();
            mBuffer = NULL;
        }

        *out = clone;

        return OK;
    } else {
        // Whole NAL units are returned but each fragment is prefixed by
        // the start code (0x00 00 00 01).
        ssize_t num_bytes_read = 0;
        int32_t drm = 0;
        bool usesDRM = (mFormat->findInt32(kKeyIsDRM, &drm) && drm != 0);
        if (usesDRM) {
            num_bytes_read =
                mDataSource->readAt(offset, (uint8_t*)mBuffer->data(), size);
        } else {
            num_bytes_read = mDataSource->readAt(offset, mSrcBuffer, size);
        }

        if (num_bytes_read < (ssize_t)size) {
            mBuffer->release();
            mBuffer = NULL;

            return ERROR_IO;
        }

        if (usesDRM) {
            CHECK(mBuffer != NULL);
            mBuffer->set_range(0, size);

        } else {
            uint8_t *dstData = (uint8_t *)mBuffer->data();
            size_t srcOffset = 0;
            size_t dstOffset = 0;

            while (srcOffset < size) {
                bool isMalFormed = (srcOffset + mNALLengthSize > size);
                size_t nalLength = 0;
                if (!isMalFormed) {
                    if (mNALLengthSize){
                        nalLength = parseNALSize(&mSrcBuffer[srcOffset]);
                    } else {//it's zero length avcC case
                        nalLength = size;
                    }
                    srcOffset += mNALLengthSize;
                    isMalFormed = srcOffset + nalLength > size;
                }

                if (isMalFormed) {
                    ALOGE("Video is malformed");
                    mBuffer->release();
                    mBuffer = NULL;
                    return ERROR_MALFORMED;
                }

                if (nalLength == 0) {
                    continue;
                }

                CHECK(dstOffset + 4 <= mBuffer->size());
                if (mNALLengthSize){//It's NOT zero length avcC, need to insert startcode. For zero length avcC, h264 video is stored as byte-stream format, already have startcode.
                dstData[dstOffset++] = 0;
                dstData[dstOffset++] = 0;
                dstData[dstOffset++] = 0;
                dstData[dstOffset++] = 1;
                }
                memcpy(&dstData[dstOffset], &mSrcBuffer[srcOffset], nalLength);
                srcOffset += nalLength;
                dstOffset += nalLength;
            }
            CHECK_EQ(srcOffset, size);
            CHECK(mBuffer != NULL);
            mBuffer->set_range(0, dstOffset);
        }

        mBuffer->meta_data()->clear();
        mBuffer->meta_data()->setInt64(
                kKeyTime, ((int64_t)cts * 1000000) / mTimescale);

        if (targetSampleTimeUs >= 0) {
            mBuffer->meta_data()->setInt64(
                    kKeyTargetTime, targetSampleTimeUs);
        }

        if (isSyncSample) {
            mBuffer->meta_data()->setInt32(kKeyIsSyncFrame, 1);
        }

        ++mCurrentSampleIndex;

        *out = mBuffer;
        mBuffer = NULL;

        return OK;
    }
}

MOVExtractor::Track *MOVExtractor::findTrackByMimePrefix(
        const char *mimePrefix) {
    for (Track *track = mFirstTrack; track != NULL; track = track->next) {
        const char *mime;
        if (track->meta != NULL
                && track->meta->findCString(kKeyMIMEType, &mime)
                && !strncasecmp(mime, mimePrefix, strlen(mimePrefix))) {
            return track;
        }
    }

    return NULL;
}

static bool LegacySniffMOV(
        const sp<DataSource> &source, String8 *mimeType, float *confidence) {
    uint8_t header[8];

    ssize_t n = source->readAt(4, header, sizeof(header));
    if (n < (ssize_t)sizeof(header)) {
        return false;
    }

    if (!memcmp(header, "ftypqt  ", 8)) {
        *mimeType = MEDIA_MIMETYPE_CONTAINER_MOV;
        *confidence = 0.7;

        return true;
    }

    return false;
}

static bool isCompatibleBrand(uint32_t fourcc) {
    static const uint32_t kCompatibleBrands[] = {
        // Only support quicktime format.
        FOURCC('q', 't', ' ', ' '),  // Apple's QuickTime
    };

    for (size_t i = 0;
         i < sizeof(kCompatibleBrands) / sizeof(kCompatibleBrands[0]);
         ++i) {
        if (kCompatibleBrands[i] == fourcc) {
            return true;
        }
    }

    return false;
}

// Attempt to actually parse the 'ftyp' atom and determine if a suitable
// compatible brand is present.
// Also try to identify where this file's metadata ends
// (end of the 'moov' atom) and report it to the caller as part of
// the metadata.
static bool BetterSniffMOV(
        const sp<DataSource> &source, String8 *mimeType, float *confidence,
        sp<AMessage> *meta) {
    // We scan up to 128 bytes to identify this file as an MP4.
    static const off64_t kMaxScanOffset = 128ll;

    off64_t offset = 0ll;
    bool foundGoodFileType = false;
    off64_t moovAtomEndOffset = -1ll;
    bool done = false;

    while (!done && offset < kMaxScanOffset) {
        uint32_t hdr[2];
        if (source->readAt(offset, hdr, 8) < 8) {
            return false;
        }

        uint64_t chunkSize = ntohl(hdr[0]);
        uint32_t chunkType = ntohl(hdr[1]);
        off64_t chunkDataOffset = offset + 8;

        if (chunkSize == 1) {
            if (source->readAt(offset + 8, &chunkSize, 8) < 8) {
                return false;
            }

            chunkSize = ntoh64(chunkSize);
            chunkDataOffset += 8;

            if (chunkSize < 16) {
                // The smallest valid chunk is 16 bytes long in this case.
                return false;
            }
        } else if (chunkSize < 8) {
            // The smallest valid chunk is 8 bytes long.
            return false;
        }

        off64_t chunkDataSize = offset + chunkSize - chunkDataOffset;

        switch (chunkType) {
            case FOURCC('f', 't', 'y', 'p'):
            {
                if (chunkDataSize < 8) {
                    return false;
                }

                uint32_t numCompatibleBrands = (chunkDataSize - 8) / 4;
                for (size_t i = 0; i < numCompatibleBrands + 2; ++i) {
                    if (i == 1) {
                        // Skip this index, it refers to the minorVersion,
                        // not a brand.
                        continue;
                    }

                    uint32_t brand;
                    if (source->readAt(
                                chunkDataOffset + 4 * i, &brand, 4) < 4) {
                        return false;
                    }

                    brand = ntohl(brand);

                    if (isCompatibleBrand(brand)) {
                        foundGoodFileType = true;
                        break;
                    }
                }

                if (!foundGoodFileType) {
                    return false;
                }

                break;
            }

            case FOURCC('m', 'o', 'o', 'v'):
            {
                moovAtomEndOffset = offset + chunkSize;

                done = true;
                break;
            }

            default:
                break;
        }

        offset += chunkSize;
    }

    if (!foundGoodFileType) {
        return false;
    }

    *mimeType = MEDIA_MIMETYPE_CONTAINER_MOV;
    *confidence = 0.7f;

    if (moovAtomEndOffset >= 0) {
        *meta = new AMessage;
        (*meta)->setInt64("meta-data-size", moovAtomEndOffset);

        ALOGV("found metadata size: %lld", moovAtomEndOffset);
    }

    return true;
}

bool SniffMOV(
        const sp<DataSource> &source, String8 *mimeType, float *confidence,
        sp<AMessage> *meta) {
    if (BetterSniffMOV(source, mimeType, confidence, meta)) {
        ALOGW("Identified supported MOV through BetterSniffMOV.");
        return true;
    }

    if (LegacySniffMOV(source, mimeType, confidence)) {
        ALOGW("Identified supported MOV through LegacySniffMOV.");
        return true;
    }

    return false;
}

}  // namespace android
