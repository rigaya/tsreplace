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

#ifndef __RGY_TS_STRUCT_H__
#define __RGY_TS_STRUCT_H__

#include <vector>
#include <deque>
#include <memory>
#include <cstdint>

static const int TS_PACKET_HEADER_LEN = 4;

struct RGYTSPacketAdaption {
    bool discontinuity;
    bool random_access;
    bool es_priority;
    bool pcr_flag;
    bool opcr_flag;
    bool splicing_point;
    bool transport_private_data;
    bool adaption_filed_ext;
    int8_t splicing_countdown;
};

struct RGYTSPacketHeader {
    uint8_t    Sync;
    uint8_t    TsErr;
    uint8_t    PayloadStartFlag;
    uint8_t    Priority;
    uint16_t   PID;
    uint8_t    Scramble;
    uint8_t    adaptation;
    uint8_t    Counter;
    int64_t    pos;
    int        adaptionLength;
    int        payloadSize;
    RGYTSPacketAdaption adapt;
};

struct RGYTSPacket {
    RGYTSPacketHeader header;
    std::vector<uint8_t> packet;
    const uint8_t *data() const {
        return packet.data();
    }
    const size_t datasize() const {
        return packet.size();
    }
    const uint8_t *payload() const {
        return (header.payloadSize > 0) ? packet.data() + packet.size() - header.payloadSize : nullptr;
    }
};

class RGYTSPacketContainer;

struct RGYTSPacketDeleter {
    RGYTSPacketDeleter(RGYTSPacketContainer *container) : m_container(container) {};
    void operator()(RGYTSPacket* pkt);
    RGYTSPacketContainer *m_container;
};

using uniqueRGYTSPacket = std::unique_ptr<RGYTSPacket, RGYTSPacketDeleter>;

class RGYTSPacketContainer {
public:
    RGYTSPacketContainer();
    ~RGYTSPacketContainer();

    uniqueRGYTSPacket getEmpty();
    void addEmpty(RGYTSPacket *pkt);
private:
    std::deque<RGYTSPacket *> m_emptyList;
};

struct RGYTS_PSI {
    int table_id;
    int section_length;
    int version_number;
    int current_next_indicator;
    int continuity_counter;
    int data_count;
    uint8_t data[1024];
    RGYTS_PSI();
    void reset();
};

static const int PES_START_SIZE = 6;
static const int PES_HEADER_SIZE = 9;

struct RGYTSPESHeader {
    int stream_id;
    int pes_len; // PES_START_SIZEを含まない残りの長さ

    //optional
    int scramble;
    bool priority;
    bool data_align;
    bool copyright;
    bool original;
    bool pts_flag;
    bool dts_flag;
    bool esrc_flag;
    bool es_rate_flag;
    bool dsm_trick_mode;
    bool additional_copy_info_flag;
    bool crc_flag;
    bool ext_flag;
    uint8_t pes_header_len; // PES_HEADER_SIZEを含まない残りの長さ
    int extended_stream_id;

    int64_t pts;
    int64_t dts;
};

struct RGYTS_PMT_PID {
    uint16_t pmt_pid;
    uint16_t program_number; // service_id
};
static_assert(sizeof(RGYTS_PMT_PID) == 4);

struct RGYTS_PAT {
    int transport_stream_id;
    int version_number;
    std::vector<RGYTS_PMT_PID> pmt;
    RGYTS_PSI psi;
};

enum class RGYTSStreamType : uint8_t {
    UNKNOWN          = 0x00,
    H262_VIDEO       = 0x02,
    H264_VIDEO       = 0x1b,
    H265_VIDEO       = 0x24,
    MPEG2_AUDIO      = 0x04,
    ADTS_TRANSPORT   = 0x0f,
    PES_PRIVATE_DATA = 0x06,
    TYPE_D           = 0x0D,
};


struct RGYTSStreamInfo {
    RGYTSStreamType type;
    int pid;
};

struct RGYTSStream {
    RGYTSStreamInfo stream;
    int64_t pts;
    std::vector<uint8_t> packets;
};

struct RGYService {
    int versionNumber;
    int programNumber;
    int pidPcr;
    RGYTSStream vid;
    RGYTSStream aud0;
    RGYTSStream aud1;
    RGYTSStream cap;
    std::vector<RGYTSStreamInfo> pidList;
    int pidSuperimpose;
};

#endif //__RGY_TS_STRUCT_H__
