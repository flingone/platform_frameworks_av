#ifndef FLVMETADATAPARSER_H_
#define FLVMETADATAPARSER_H_

namespace android {

#define MAKE_UI8(x)     ((*x))
#define MAKE_UI16(x)    (((*x) << 8) | (*(x + 1)))
#define MAKE_UI32(x)    (((*x) << 24) | (*(x + 1) << 16) | ((*(x + 2)) << 8) | (*(x + 3)))

class SimpleBuffer {
private:
    uint8_t *buff;
    uint32_t size;
    uint32_t used;
public:
    SimpleBuffer(uint8_t *buff, uint32_t size);
    ~SimpleBuffer();

    //read num bytes to buff
    int readBytes(void *buff, uint32_t num);

    //change Big endian to little endian.
    static void * toLittleEndian(void *buff, uint32_t len);

    bool bufferEnd(void);
    int skipByte(uint32_t num);
};

struct FLVMetaData
{
public:
    bool hasMetadata;
    off_t dataSize;
    int64_t duration;
    off_t fileSize;
    int framerate;

    bool hasAudio;
    struct {
        int audioCodecID;
        bool stereo;
        int audioBitRate;
        int audioSampleRate;
        int audioSampleSize;
        off_t audioSize;
    } audio;

    bool hasVideo;
    struct {
        int videoCodecID;
        int width;
        int height;
        off_t videoSize;
        int videoDataRate;
    } video;

    int64_t lastTimeStamp;
    int64_t lastKeyFrameTimeStamp;
    off_t lastKeyFrameLocation;

    bool hasKeyFrames;
    struct {
        uint32_t keyFrameTimeNum;
        int64_t *keyFrameTimes;

        uint32_t keyFramePosNum;
        off_t *keyFramePos;
    } keyFrame;

public:
    void freeMemoryForKeyFrameTimes(void);
    void freeMemoryForKeyFramePos(void);
};

extern FLVMetaData *flvMetaData;

class flvMetaDataParser {
private:
    SimpleBuffer *mediaBuffer;
public:
    //FLVMetaData *flvMetaData;

private:
    int parserAMFType_2(void);
    int parserAMFType_8(void);
    void dumpFLVMetaData(void);
public:
    flvMetaDataParser(uint8_t *buff, uint32_t size);
    ~flvMetaDataParser();

    int start(void);
    int stop(void);
};
}
#endif /* FLVMETADATAPARSER_H_ */
