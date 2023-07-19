
#include <initializer_list>
#include "rgy_tsdemux.h"

static const uint8_t TS_SYNC_BYTE = 0x47;

template<int unit_size>
int findsync_unit_size(const uint8_t *data, const int data_size) {
    const auto checksize = std::min(data_size, unit_size * 32);
    const auto fin = std::min(data_size, unit_size);
    for (int pos = 0; pos < fin; pos++) {
        bool check = true;
        int i = pos;
        for (; check && i < checksize; i += unit_size) {
            check = (data[i] == TS_SYNC_BYTE);
        }
        if (i >= checksize) {
            return pos;
        }
    }
    return -1;
}

int findsync(const uint8_t *data, const int data_size, int *unit_size) {
    int pos = data_size;
    switch (*unit_size) {
    case 188: pos = findsync_unit_size<188>(data, data_size); break;
    case 192: pos = findsync_unit_size<192>(data, data_size); break;
    case 204: pos = findsync_unit_size<204>(data, data_size); break;
    default: break;
    }
    if (pos != data_size) {
        return pos;
    }
    int orig_unit_size = *unit_size;
    for (const auto test_unit_size : { 188, 192, 204 }) {
        if (test_unit_size != orig_unit_size) {
            *unit_size = test_unit_size;
            pos = findsync(data, data_size, unit_size);
            if (pos >= 0) {
                return pos;
            }
        }
    }
    return -1;
}

RGYTSDemuxResult::RGYTSDemuxResult() :
    type(RGYTSPacketType::UNKNOWN),
    pts(TIMESTAMP_INVALID_VALUE),
    dts(TIMESTAMP_INVALID_VALUE),
    pesHeader() {

}

RGYTSDemuxer::RGYTSDemuxer() :
    m_selectServiceID(0),
    m_audio0Mode(0),
    m_audio1Mode(0),
    m_captionMode(0),
    m_superimposeMode(0),
    m_log(),
    m_pat(),
    m_patPsi(),
    m_pmtPsi(),
    m_service(),
    m_pcr(-1) {

}

RGYTSDemuxer::~RGYTSDemuxer() {
    
}

RGY_ERR RGYTSDemuxer::init(std::shared_ptr<RGYLog> log) {
    m_log = log;
    return RGY_ERR_NONE;
}


int RGYTSDemuxer::parsePSI(RGYTS_PSI *psi, const uint8_t *payload, const int payloadSize, const int unitStart, const int counter) {
    int copy_pos = 0;
    int copy_size = payloadSize;
    int done = 1;
    if (unitStart) {
        if (payloadSize < 1) {
            psi->continuity_counter = psi->data_count = psi->version_number = 0;
            return 1;
        }
        int pointer = payload[0];
        psi->continuity_counter = (psi->continuity_counter + 1) & 0x2f;
        if (pointer > 0 && psi->continuity_counter == (0x20 | counter)) {
            copy_pos = 1;
            copy_size = pointer;
            // Call the function again
            done = 0;
        } else {
            psi->continuity_counter = 0x20 | counter;
            psi->data_count = psi->version_number = 0;
            copy_pos = 1 + pointer;
            copy_size -= copy_pos;
        }
    } else {
        psi->continuity_counter = (psi->continuity_counter + 1) & 0x2f;
        if (psi->continuity_counter != (0x20 | counter)) {
            psi->continuity_counter = psi->data_count = psi->version_number = 0;
            return 1;
        }
    }
    if (copy_size > 0 && copy_pos + copy_size <= payloadSize) {
        copy_size = std::min(copy_size, (int)(sizeof(psi->data)) - psi->data_count);
        std::copy(payload + copy_pos, payload + copy_pos + copy_size, psi->data + psi->data_count);
        psi->data_count += copy_size;
    }

    // If psi->version_number != 0, these fields are valid.
    if (psi->data_count >= 3) {
        int section_length = ((psi->data[1] & 0x03) << 8) | psi->data[2];
        if (psi->data_count >= 3 + section_length &&
            calc_crc32(psi->data, 3 + section_length) == 0 &&
            section_length >= 3) {
            psi->table_id = psi->data[0];
            psi->section_length = section_length;
            psi->version_number = 0x20 | ((psi->data[5] >> 1) & 0x1f);
            psi->current_next_indicator = psi->data[5] & 0x01;
        }
    }
    return done;
}

std::unique_ptr<RGYTS_PAT> RGYTSDemuxer::parsePAT(const uint8_t *payload, const int payloadSize, const int unitStart, const int counter) {
    for (int done = 0; !done; ) {
        done = parsePSI(&m_patPsi, payload, payloadSize, unitStart, counter);

        if (   m_patPsi.version_number
            && m_patPsi.current_next_indicator
            && m_patPsi.table_id == 0
            && m_patPsi.section_length >= 5) {
            AddMessage(RGY_LOG_DEBUG, _T("PAT section length %d, version %d\n"), m_patPsi.section_length, m_patPsi.version_number);
            auto pat = std::make_unique<RGYTS_PAT>();
            pat->transport_stream_id = read16(m_patPsi.data + 3);
            pat->version_number = m_patPsi.version_number;

            // Update PMT list
            pat->pmt.clear();
            const uint8_t *ptr     = m_patPsi.data + 3 + 5;
            const uint8_t *ptr_fin = m_patPsi.data + 3 + m_patPsi.section_length - 4/*CRC32*/;
            while (ptr < ptr_fin) {
                RGYTS_PMT_PID pmt;
                pmt.pmt_pid = read16(ptr + 2) & 0x1fff;
                pmt.program_number = read16(ptr);

                AddMessage(RGY_LOG_DEBUG, _T("  program_number(service_id) %6d, pmt_pid 0x%04x\n"),
                    pmt.program_number, pmt.pmt_pid);

                pat->pmt.push_back(pmt);
                ptr += 4;
            }
            return pat;
        }
    }
    return nullptr;
}

void RGYTSDemuxer::parsePMT(const RGYTS_PSI *psi) {
    if (psi->section_length < 9) {
        return;
    }
    const uint8_t *table = psi->data;
    m_service.programNumber = (table[3] << 8) | table[4];
    m_service.pidPcr = ((table[8] & 0x1f) << 8) | table[9];
    if (m_service.pidPcr == 0x1fff) {
        m_pcr = -1;
    }
    const int programInfoLength = ((table[10] & 0x03) << 8) | table[11];
    int pos = 3 + 9 + programInfoLength;
    if (psi->section_length < pos) {
        return;
    }

    const int lastAudio1Pid = m_service.aud0.pid;
    const int lastAudio2Pid = m_service.aud1.pid;
    m_service.vid.pid = 0;
    m_service.aud0.pid = 0;
    m_service.aud1.pid = 0;
    m_service.cap.pid = 0;
    m_service.pidSuperimpose = 0;
    m_service.aud0.type = RGYTSStreamType::ADTS_TRANSPORT;
    m_service.aud1.type = RGYTSStreamType::ADTS_TRANSPORT;
    int videoDescPos = 0;
    int audio1DescPos = 0;
    int audio2DescPos = 0;
    int captionDescPos = 0;
    int superimposeDescPos = 0;
    bool maybeCProfile = false;
    bool audio1ComponentTagUnknown = true;

    const int tableLen = 3 + psi->section_length - 4/*CRC32*/;
    while (pos + 4 < tableLen) {
        const auto streamType = (RGYTSStreamType)table[pos];
        const int esPid = ((table[pos + 1] & 0x1f) << 8) | table[pos + 2];
        const int esInfoLength = ((table[pos + 3] & 0x03) << 8) | table[pos + 4];
        if (pos + 5 + esInfoLength <= tableLen) {
            int componentTag = 0xff;
            for (int i = pos + 5; i + 2 < pos + 5 + esInfoLength; i += 2 + table[i + 1]) {
                // stream_identifier_descriptor
                if (table[i] == 0x52) {
                    componentTag = table[i + 2];
                    break;
                }
            }
            if (streamType == RGYTSStreamType::H262_VIDEO ||
                streamType == RGYTSStreamType::H264_VIDEO ||
                streamType == RGYTSStreamType::H265_VIDEO) {
                if ((m_service.vid.pid == 0 && componentTag == 0xff) || componentTag == 0x00 || componentTag == 0x81) {
                    m_service.vid.pid = esPid;
                    videoDescPos = pos;
                    maybeCProfile = componentTag == 0x81;
                }
            } else if (streamType == RGYTSStreamType::ADTS_TRANSPORT) {
                if ((m_service.aud0.pid == 0 && componentTag == 0xff) || componentTag == 0x10 || componentTag == 0x83 || componentTag == 0x85) {
                    m_service.aud0.pid = esPid;
                    m_service.aud0.type = streamType;
                    audio1DescPos = pos;
                    audio1ComponentTagUnknown = componentTag == 0xff;
                } else if (componentTag == 0x11) {
                    if (m_audio1Mode != 2) {
                        m_service.aud1.pid = esPid;
                        m_service.aud1.type = streamType;
                        audio2DescPos = pos;
                    }
                }
            } else if (streamType == RGYTSStreamType::MPEG2_AUDIO) {
                if (m_service.aud0.pid == 0) {
                    m_service.aud0.pid = esPid;
                    m_service.aud0.type = streamType;
                    audio1DescPos = pos;
                    audio1ComponentTagUnknown = false;
                } else if (m_service.aud1.pid == 0) {
                    if (m_audio1Mode != 2) {
                        m_service.aud1.pid = esPid;
                        m_service.aud1.type = streamType;
                        audio2DescPos = pos;
                    }
                }
            } else if (streamType == RGYTSStreamType::PES_PRIVATE_DATA) {
                if (componentTag == 0x30 || componentTag == 0x87) {
                    if (m_captionMode != 2) {
                        m_service.cap.pid = esPid;
                        captionDescPos = pos;
                    }
                } else if (componentTag == 0x38 || componentTag == 0x88) {
                    if (m_superimposeMode != 2) {
                        m_service.pidSuperimpose = esPid;
                        superimposeDescPos = pos;
                    }
                }
            }
        }
        pos += 5 + esInfoLength;
    }

    if (m_service.aud0.pid != lastAudio1Pid) {
        m_service.aud0.pts = -1;
        m_service.aud0.packets.clear();
    }
    if (m_service.aud1.pid != lastAudio2Pid) {
        m_service.aud1.pts = -1;
        m_service.aud1.packets.clear();
    }

    const auto loglevel = (m_service.versionNumber != psi->version_number) ? RGY_LOG_INFO : RGY_LOG_DEBUG;
    if (m_service.versionNumber != psi->version_number) {
        AddMessage(RGY_LOG_INFO, _T(" New PMT\n"));
    }
    m_service.versionNumber = psi->version_number;

    AddMessage(loglevel, _T("  version    %4d\n"),  m_service.versionNumber);
    AddMessage(loglevel, _T("  program  0x%04x\n"), m_service.programNumber);
    AddMessage(loglevel, _T("  pid vid  0x%04x\n"), m_service.vid.pid);
    AddMessage(loglevel, _T("  pid aud0 0x%04x\n"), m_service.aud0.pid);
    AddMessage(loglevel, _T("  pid aud1 0x%04x\n"), m_service.aud1.pid);
    AddMessage(loglevel, _T("  pid cap  0x%04x\n"), m_service.cap.pid);
    AddMessage(loglevel, _T("  pid pcr  0x%04x\n"), m_service.pidPcr);
}

void RGYTSDemuxer::parsePMT(const uint8_t *payload, const int payloadSize, const int unitStart, const int counter) {
    for (int done = 0; !done; ) {
        done = parsePSI(&m_pmtPsi, payload, payloadSize, unitStart, counter);
        AddMessage(RGY_LOG_DEBUG, _T("PMT section length %d, version_number %d\n"), m_pmtPsi.section_length, m_pmtPsi.version_number);
        if (m_pmtPsi.version_number && m_pmtPsi.table_id == 2 && m_pmtPsi.current_next_indicator) {
            parsePMT(&m_pmtPsi);
        }
    }
}

int64_t RGYTSDemuxer::parsePCR(const RGYTSPacketHeader& packetHeader, const uint8_t *packet) {
    int64_t pcr = TIMESTAMP_INVALID_VALUE;
    if (packetHeader.adaptation & 2) {
        const int adaptationLength = packet[4];
        if (adaptationLength >= 6 && !!(packet[5] & 0x10)) {
            const int64_t PCR_base =
                  ((int64_t)packet[ 6] << 25)
                | ((int64_t)packet[ 7] << 17)
                | ((int64_t)packet[ 8] <<  9)
                | ((int64_t)packet[ 9] <<  1)
                | ((int64_t)packet[10] >>  7);
            const int64_t PCR_ext =
                ((int64_t)(packet[10] & 0x01) << 8)
                | (int64_t)packet[11];
            pcr = PCR_base + PCR_ext / 300;
            AddMessage(RGY_LOG_TRACE, _T("PCR  %lld\n"), pcr);
        }
    }
    return pcr;
}

int64_t RGYTSDemuxer::parsePESPTS(const uint8_t *ptr) {
    int64_t pts = 0;
    pts = (int64_t)(((uint32_t)(ptr[0]) & 0xE) >> 1) << 30;
    pts += (uint32_t)ptr[1] << 22;
    pts += (uint32_t)((uint32_t)ptr[2] >> 1) << 15;
    pts += (uint32_t)ptr[3] << 7;
    pts += (uint32_t)ptr[4] >> 1;
    return pts;
}

RGYTSPESHeader RGYTSDemuxer::parsePESHeader(const std::vector<uint8_t>& pkt) {
    // https://www.jushin-s.co.jp/michi/download/97_t43.pdf
    // https://dvd.sourceforge.net/dvdinfo/pes-hdr.html
    RGYTSPESHeader pes = { 0 };
    pes.pts = TIMESTAMP_INVALID_VALUE;
    pes.dts = TIMESTAMP_INVALID_VALUE;
    static uint8_t PES_START_CODE[3] = { 0x00, 0x00, 0x01 };
    const uint8_t *pes_header = nullptr;
    for (int i = 4; i < (int)pkt.size(); i++) {
        if (memcmp(pkt.data() + i, PES_START_CODE, sizeof(PES_START_CODE)) == 0) {
            pes_header = pkt.data() + i;
            break;
        }
    }
    if (!pes_header) {
        return pes;
    }
    const uint8_t *ptr = pes_header;
    pes.stream_id = ptr[3];
    pes.pes_len = read16(ptr + 4);
    if ((ptr[6] & 0xC0) == (0x80)) {
        pes.scramble                  = (ptr[6] & 0x30) >> 8;
        pes.priority                  = (ptr[6] & 0x08) != 0;
        pes.data_align                = (ptr[6] & 0x04) != 0;
        pes.copyright                 = (ptr[6] & 0x02) != 0;
        pes.original                  = (ptr[6] & 0x01) != 0;
        pes.pts_flag                  = (ptr[7] & 0x80) != 0;
        pes.dts_flag                  = (ptr[7] & 0x40) != 0;
        pes.esrc_flag                 = (ptr[7] & 0x20) != 0;
        pes.es_rate_flag              = (ptr[7] & 0x10) != 0;
        pes.dsm_trick_mode            = (ptr[7] & 0x08) != 0;
        pes.additional_copy_info_flag = (ptr[7] & 0x04) != 0;
        pes.crc_flag                  = (ptr[7] & 0x02) != 0;
        pes.ext_flag                  = (ptr[7] & 0x01) != 0;
        pes.pes_header_len            =  ptr[8];
        ptr += 9;
        if (pes.pts_flag) {
            pes.pts = parsePESPTS(ptr);
            ptr += 5;
        }
        if (pes.dts_flag) {
            pes.dts = parsePESPTS(ptr);
            ptr += 5;
        } else {
            pes.dts = pes.pts;
        }
        if (pes.ext_flag) {
            auto pes_ext = *ptr++;
            int skip = (pes_ext >> 4) & 0x0B;
            skip += skip & 0x09;
            ptr += skip;
            if ((pes_ext & 0x41) == 0x01 &&
                (ptr + 2) <= (pes_header + pes.pes_header_len + PES_HEADER_SIZE)) {
                /* PES extension 2 */
                if ((ptr[0] & 0x7f) > 0 && (ptr[1] & 0x80) == 0) {
                    pes.extended_stream_id = ptr[1];
                }
            }
        }
    }
    return pes;
}

void RGYTSDemuxer::resetPCR() {
    m_pcr = TIMESTAMP_INVALID_VALUE;
}

void RGYTSDemuxer::resetPSICache() {
    m_patPsi.reset();
    m_pmtPsi.reset();
}

const RGYTS_PMT_PID *RGYTSDemuxer::selectServiceID() {
    return selectServiceID(m_selectServiceID);
}

const RGYTS_PMT_PID *RGYTSDemuxer::selectServiceID(const int serviceID) {
    if (!m_pat || m_pat->pmt.size() == 0) {
        return nullptr;
    }
    if (serviceID > 0) {
        for (size_t i = 0; i < m_pat->pmt.size(); i++) {
            if (m_pat->pmt[i].program_number == serviceID) {
                return &m_pat->pmt[i];
            }
        }
    }
    for (size_t i = 0; i < m_pat->pmt.size(); i++) {
        if (m_pat->pmt[i].program_number != 0) {
            return &m_pat->pmt[i];
        }
    }
    return nullptr;
}

std::tuple<RGY_ERR, RGYTSDemuxResult> RGYTSDemuxer::parse(const RGYTSPacket *pkt) {
    RGYTSDemuxResult result;
    const auto& packetHeader = pkt->header;

    // PAT
    if (packetHeader.PID == 0x00) {
        m_pat = parsePAT(pkt->payload(), packetHeader.payloadSize, packetHeader.PayloadStartFlag, packetHeader.Counter);
        result.type = RGYTSPacketType::PAT;
        return { RGY_ERR_NONE, std::move(result) };
    }

    auto pmt_pid = selectServiceID();
    if (pmt_pid && packetHeader.PID == pmt_pid->pmt_pid) {
        parsePMT(pkt->payload(), packetHeader.payloadSize, packetHeader.PayloadStartFlag, packetHeader.Counter);
        result.type = RGYTSPacketType::PMT;
        result.psi = std::make_unique<RGYTS_PSI>(m_pmtPsi);
        return { RGY_ERR_NONE, std::move(result) };
    }

    result.type = RGYTSPacketType::OTHER;
    if (packetHeader.PID == m_service.pidPcr) {
        const auto pcr = parsePCR(packetHeader, pkt->data());
        if (pcr != TIMESTAMP_INVALID_VALUE) {
            m_pcr = pcr;
        }
        result.type = RGYTSPacketType::PCR;
        result.pts = pcr;
        AddMessage((pcr != TIMESTAMP_INVALID_VALUE) ? RGY_LOG_TRACE : RGY_LOG_WARN,
            _T("  pid pcr  0x%04x, %lld\n"), m_service.pidPcr, pcr);
    } else if (packetHeader.PID == m_service.vid.pid) {
        result.type = RGYTSPacketType::VID;
        if (packetHeader.PayloadStartFlag) {
            auto pes = parsePESHeader(pkt->packet);
            AddMessage(RGY_LOG_TRACE, _T("  pid vid  0x%04x, %4d, %lld, %lld\n"), m_service.vid.pid, packetHeader.payloadSize, pes.pts, pes.dts);
            result.pts = pes.pts;
            result.dts = pes.dts;
        }
    } else if (packetHeader.PID == m_service.aud0.pid) {
        if (packetHeader.PayloadStartFlag) {
            auto pes = parsePESHeader(pkt->packet);
            AddMessage(RGY_LOG_TRACE, _T("  pid aud0 0x%04x, %lld\n"), m_service.vid.pid, pes.pts);
            result.pts = pes.pts;
            result.dts = pes.dts;
        }
    } else if (packetHeader.PID == m_service.cap.pid) {
        if (packetHeader.PayloadStartFlag) {
            auto pes = parsePESHeader(pkt->packet);
            AddMessage(RGY_LOG_TRACE, _T("  pid cap  0x%04x, %lld\n"), m_service.vid.pid, pes.pts);
            result.pts = pes.pts;
            result.dts = pes.dts;
        }
    }
    return { RGY_ERR_NONE, std::move(result) };
}

RGYTSPacketSplitter::RGYTSPacketSplitter() :
    m_log(),
    m_readBuf(),
    m_packetContainer(),
    m_packetSize(0) {

}

RGYTSPacketSplitter::~RGYTSPacketSplitter() {
    m_log.reset();
}

RGY_ERR RGYTSPacketSplitter::init(std::shared_ptr<RGYLog> log) {
    m_log = log;
    return RGY_ERR_NONE;
}

void RGYTSPacketSplitter::parsePacketHeaderAdaption(RGYTSPacketHeader& pktHeader, const uint8_t *ptr) {
    if (pktHeader.adaptionLength > 0) {
        uint8_t d5 = ptr[5];
        pktHeader.adapt.discontinuity          = d5 & 0x01;
        pktHeader.adapt.random_access          = d5 & 0x02;
        pktHeader.adapt.es_priority            = d5 & 0x04;
        pktHeader.adapt.pcr_flag               = d5 & 0x08;
        pktHeader.adapt.opcr_flag              = d5 & 0x10;
        pktHeader.adapt.splicing_point         = d5 & 0x20;
        pktHeader.adapt.transport_private_data = d5 & 0x40;
        pktHeader.adapt.adaption_filed_ext     = d5 & 0x80;
        pktHeader.adapt.splicing_countdown     = 0;

        int ipos = 6;
        if (pktHeader.adapt.pcr_flag) {
            ipos += 6;
        }
        if (pktHeader.adapt.opcr_flag) {
            ipos += 6;
        }
        if (pktHeader.adapt.splicing_point) {
            pktHeader.adapt.splicing_countdown = ptr[ipos++];
        }
    }
}

RGYTSPacketHeader RGYTSPacketSplitter::parsePacketHeader(const uint8_t *ptr, const int64_t pos) {
    const uint8_t  d0 = ptr[0];
    const uint16_t d12 = read16(ptr+1);
    const uint8_t  d3  = ptr[3];

    RGYTSPacketHeader pktHeader;
    pktHeader.Sync             =  d0;
    pktHeader.TsErr            = (d12 >> 15) & 0x01;
    pktHeader.PayloadStartFlag = (d12 >> 14) & 0x01;
    pktHeader.Priority         = (d12 >> 13) & 0x01;
    pktHeader.PID              = (d12 & 0x1FFF);
    pktHeader.Scramble         = (d3 & 0xC0) >> 6;
    pktHeader.adaptation       = (d3 & 0x30) >> 4;
    pktHeader.Counter          = (d3 & 0x0F);
    pktHeader.pos              = pos;
    pktHeader.payloadSize      = 0;
    pktHeader.adaptionLength   = 0;

    // 参考: https://vcbook.vtv.co.jp/pages/viewpage.action?pageId=1310821
    if (pktHeader.adaptation & 0x01) { // has_payload
        if (pktHeader.adaptation & 0x02) { // has_adaptation
            pktHeader.adaptionLength = ptr[4];
            if (pktHeader.adaptionLength <= 183) {
                pktHeader.payloadSize = 183 - pktHeader.adaptionLength;
            }
        } else {
            pktHeader.payloadSize = 184;
        }
    } else if (pktHeader.adaptation & 0x02) { // has_adaptation
        pktHeader.adaptionLength = ptr[4];
    }
    parsePacketHeaderAdaption(pktHeader, ptr);

    //AddMessage(RGY_LOG_DEBUG, _T("Packet %16lld, PID=0x%04x, payloadSize %d, adaptionLength %d\n"),
    //    pos, pktHeader.PID, pktHeader.payloadSize, pktHeader.adaptionLength);

    return pktHeader;
}

std::tuple<RGY_ERR, std::vector<uniqueRGYTSPacket>> RGYTSPacketSplitter::split(void *ptr, const size_t addSize) {
    std::vector<uniqueRGYTSPacket> packets;

    m_readBuf.addData(ptr, addSize);

    while (m_packetSize == 0 || m_readBuf.size() >= m_packetSize) {
        auto offset = findsync(m_readBuf.data(), m_readBuf.size(), &m_packetSize);
        if (offset < 0) return { RGY_ERR_NONE, std::move(packets) };

        auto pkt = m_packetContainer.getEmpty();
        pkt->packet.resize(m_packetSize);
        memcpy(pkt->packet.data(), m_readBuf.data() + offset, m_packetSize);
        m_readBuf.removeData(m_packetSize);

        pkt->header = parsePacketHeader(pkt->packet.data(), m_readBuf.pos());
        if (pkt->header.Sync != TS_SYNC_BYTE) {
            return { RGY_ERR_INVALID_BINARY, std::move(packets) };
        }

        if (pkt->header.TsErr) {
            continue;
        }
        packets.push_back(std::move(pkt));
    }
    return { RGY_ERR_NONE, std::move(packets) };
}
