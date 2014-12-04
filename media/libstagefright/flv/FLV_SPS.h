#ifndef _PARSER_H264_SPS_H_
#define _PARSER_H264_SPS_H_
typedef struct
{
    uint8_t *bytes;
    size_t length;
    size_t byte;
    short bit;
} bitstream_t;

typedef struct
{
    int width;
    int height;
} h264data_t;

void getH264SPS(uint8_t *data, uint32_t len, h264data_t *h264);

#endif    //_PARSER_H264_SPS_H_
