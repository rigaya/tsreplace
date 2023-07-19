// -----------------------------------------------------------------------------------------
// tsreplace by rigaya
// -----------------------------------------------------------------------------------------
// The MIT License
//
// Copyright (c) 2023 rigaya
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// ------------------------------------------------------------------------------------------

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
