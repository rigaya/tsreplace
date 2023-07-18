#ifndef __RGY_TSUTIL_H__
#define __RGY_TSUTIL_H__

#include <vector>
#include <cstdint>

static const int64_t TIMESTAMP_INVALID_VALUE = ((int64_t)0x8000000000000000);

uint32_t calc_crc32(const uint8_t *data, const int data_size);

static uint16_t read16(const void *ptr) {
    uint16_t x = *(const uint16_t *)ptr;
    return (x >> 8) | (x << 8);
}

struct RGYTSBuffer {
private:
    std::vector<uint8_t> buffer;
    size_t bufLength;
    size_t bufOffset;
    int64_t currentPos;
public:
    RGYTSBuffer();
    ~RGYTSBuffer();

    uint8_t *data() { return buffer.data() + bufOffset; }
    const uint8_t *data() const { return buffer.data() + bufOffset; }
    size_t size() const { return bufLength; }
    int64_t pos() const { return currentPos; }

    void addData(void *ptr, const size_t addSize);
    void removeData(const size_t removeSize);
};


#endif //__RGY_TSUTIL_H__
