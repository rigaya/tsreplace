#include "rgy_tsutil.h"

uint32_t calc_crc32(const uint8_t *data, const int data_size) {
    uint32_t crc = 0xffffffff;
    for (int i = 0; i < data_size; ++i) {
        uint32_t c = ((crc >> 24) ^ data[i]) << 24;
        for (int j = 0; j < 8; ++j) {
            c = (c << 1) ^ (c & 0x80000000 ? 0x04c11db7 : 0);
        }
        crc = (crc << 8) ^ c;
    }
    return crc;
}

RGYTSBuffer::RGYTSBuffer() :
    buffer(),
    bufLength(),
    bufOffset(),
    currentPos() {

}

RGYTSBuffer::~RGYTSBuffer() {
    
}

void RGYTSBuffer::addData(void *ptr, const size_t addSize) {
    if (buffer.size() < bufLength + addSize) {
        buffer.resize(std::max(bufLength + addSize, buffer.size() * 3 / 2));
    }
    if (buffer.size() < bufLength + bufOffset + addSize) {
        memmove(buffer.data() + bufOffset, buffer.data(), bufLength);
        bufOffset = 0;
    }
    memcpy(buffer.data() + bufLength, ptr, addSize);
    bufLength += addSize;
}

void RGYTSBuffer::removeData(const size_t removeSize) {
    if (removeSize > bufLength) {
        bufLength = 0;
    } else {
        bufLength -= removeSize;
    }
    if (bufLength == 0) {
        bufOffset = 0;
    } else {
        bufOffset += removeSize;
    }
    currentPos += removeSize;
}
