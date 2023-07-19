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
