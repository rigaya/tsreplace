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

#ifndef __RGY_TS_DEMUX_H__
#define __RGY_TS_DEMUX_H__

#include <cstdint>
#include <cstdarg>
#include "rgy_tsutil.h"
#include "rgy_tsstruct.h"
#include "rgy_err.h"
#include "rgy_log.h"
#include "rgy_util.h"

int findsync(const uint8_t *data, const int data_size, int *unit_size);

enum class RGYTSDescriptor : uint8_t {
    ConditionalAccess          = 0x09,
    NetworkName                = 0x40,
    ServiceList                = 0x41,
    Stuffing                   = 0x42,
    SatelliteDeliverySystem    = 0x43,
    Service                    = 0x48,
    Linkage                    = 0x4A,
    ShortEvent                 = 0x4D,
    ExtendedEvent              = 0x4E,
    Component                  = 0x50,
    StreamIdentifier           = 0x52,
    Content                    = 0x54,
    ParentalRate               = 0x55,
    LocalTimeOffset            = 0x58,
    HierarchicalTransmission   = 0xC0,
    DigitalCopyControl         = 0xC1,
    AudioComponent             = 0xC4,
    Hyperlink                  = 0xC5,
    TargetRegion               = 0xC6,
    DateContent                = 0xC7,
    VideoDecodeControl         = 0xC8,
    DownloadContent            = 0xC9,
    CA_EMM_TS                  = 0xCA,
    CAContractInformation      = 0xCB,
    CAService                  = 0xCC,
    EmergencyInformation       = 0xFC,
    DataComponent              = 0xFD,
    SystemManagement           = 0xFE,
    EventGroup                 = 0xD6,
    SI_Parameter               = 0xD7,
    BroadcasterName            = 0xD8,
    ComponentGroup             = 0xD9,
    Series                     = 0xD5,
    ContentAvailability        = 0xDE,
};

enum class RGYTSVideoDecCtrlDesc {
    _1080_P,
    _1080_I,
    _720_P,
    _480_P,
    _480_I,
    _240_P,
    _120_P,
    _2160_60_P,
    _180_P,
    _2160_120_P,
    _4320_60_P,
    _4320_120_P,
};

enum class RGYTSPacketType {
    UNKNOWN,
    PAT,
    PMT,
    PCR,
    VID,
    OTHER
};

struct RGYTSDemuxResult {
    RGYTSPacketType type;
    RGYTSStreamInfo stream;
    int programNumber;
    int64_t pts;
    int64_t dts;
    int64_t pcr;
    std::unique_ptr<RGYTSPESHeader> pesHeader;
    std::unique_ptr<RGYTS_PSI> psi;

    RGYTSDemuxResult();
};

struct RGYTSDemuxProgram {
    RGYTS_PMT_PID pmt_pid;
    RGYService service;
    RGYTS_PSI pmtPsi; // PMT抽出用領域

    RGYTSDemuxProgram();
};

class RGYTSDemuxer {
public:
    RGYTSDemuxer();
    virtual ~RGYTSDemuxer();
    RGY_ERR init(std::shared_ptr<RGYLog> log, int selectService);
    std::tuple<RGY_ERR, RGYTSDemuxResult> parse(const RGYTSPacket *pkt);

    const RGYTS_PAT* pat() const { return m_pat.get(); }
    const int64_t pcr() const { return m_pcr; }
    const RGYService *service() const { return m_targetService; }
    const RGYTS_PMT_PID *selectServiceID();
    void resetPCR();
    void resetPSICache();
    bool isPIDTargetService(const int pid) const;
    bool isPIDExists(const int pid) const;
protected:
    void checkPMTList();
    RGYTSDemuxProgram *selectProgramFromPMTPID(const int pmt_pid);
    RGYTSDemuxProgram *selectProgramFromPID(const int pid);
    const RGYTS_PMT_PID *selectServiceID(const int serviceID);
    int parsePSI(RGYTS_PSI *psi, const uint8_t *ptr, const int payloadSize, const int unitStart, const int counter);
    std::unique_ptr<RGYTS_PAT> parsePAT(const uint8_t *ptr, const int payloadSize, const int unitStart, const int counter);

    void parsePMT(RGYTSDemuxProgram *program, const uint8_t *payload, const int payloadSize, const int unitStart, const int counter);
    void parsePMT(RGYTSDemuxProgram *program);
    int64_t parsePCR(const RGYTSPacketHeader& packetHeader, const uint8_t *packet);
    int64_t packetPTS(const uint8_t *packet);
    int64_t parsePESPTS(const uint8_t *ptr);
    RGYTSPESHeader parsePESHeader(const std::vector<uint8_t>& pkt);

    void AddMessage(RGYLogLevel log_level, const tstring &str) {
        if (m_log == nullptr || log_level < m_log->getLogLevel(RGY_LOGT_IN)) {
            return;
        }
        auto lines = split(str, _T("\n"));
        for (const auto &line : lines) {
            if (line[0] != _T('\0')) {
                m_log->write(log_level, RGY_LOGT_IN, (_T("demux: ") + line + _T("\n")).c_str());
            }
        }
    }
    void AddMessage(RGYLogLevel log_level, const TCHAR *format, ...) {
        if (m_log == nullptr || log_level < m_log->getLogLevel(RGY_LOGT_IN)) {
            return;
        }

        va_list args;
        va_start(args, format);
        int len = _vsctprintf(format, args) + 1; // _vscprintf doesn't count terminating '\0'
        tstring buffer;
        buffer.resize(len, _T('\0'));
        _vstprintf_s(&buffer[0], len, format, args);
        va_end(args);
        AddMessage(log_level, buffer);
    }

    int m_selectServiceID;
    int m_audio0Mode;
    int m_audio1Mode;
    int m_captionMode;
    int m_superimposeMode;

    std::shared_ptr<RGYLog> m_log;
    std::unique_ptr<RGYTS_PAT> m_pat;
    RGYTS_PSI m_patPsi; // PAT抽出用領域
    std::vector<std::unique_ptr<RGYTSDemuxProgram>> m_programs;
    RGYService *m_targetService;
    int64_t m_pcr;
};

class RGYTSPacketSplitter {
public:
    RGYTSPacketSplitter();
    virtual ~RGYTSPacketSplitter();
    RGY_ERR init(std::shared_ptr<RGYLog> log);
    std::tuple<RGY_ERR, std::vector<uniqueRGYTSPacket>> split(void *ptr, const size_t addSize);
    int64_t pos() const { return m_readBuf.pos(); }
protected:
    RGYTSPacketHeader parsePacketHeader(const uint8_t *ptr, const int64_t pos);
    void parsePacketHeaderAdaption(RGYTSPacketHeader& pktHeader, const uint8_t *ptr);

    std::shared_ptr<RGYLog> m_log;
    RGYTSBuffer m_readBuf;
    RGYTSPacketContainer m_packetContainer;
    int m_packetSize;
};

#endif //__RGY_TS_DEMUX_H__
