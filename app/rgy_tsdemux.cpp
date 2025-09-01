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

#include <initializer_list>
#include "rgy_tsdemux.h"

static const uint8_t TS_SYNC_BYTE = 0x47;

template<int unit_size>
int findsync_unit_size(const uint8_t *data, const int data_size, const bool unit_size_fixed) {
    const int check_count = 32;
    int check_ret[unit_size] = { 0 };
    const auto checksize = std::min(data_size, unit_size * check_count);
    const auto fin = std::min(data_size, unit_size);
    for (int pos = 0; pos < fin; pos++) {
        bool check = true;
        int i = pos;
        for (; i < checksize; i += unit_size) {
            check = (data[i] == TS_SYNC_BYTE);
            if (!check) break;
        }
        check_ret[pos] = i;
        if (i >= checksize) {
            return pos;
        }
    }
    if (unit_size_fixed) {
        // うまくいかなかった時、先頭が一番うまくいっている限りそれを選択する
        bool check = true;
        for (int pos = 1; pos < fin; pos++) {
            check = (check_ret[pos] + unit_size < check_ret[0]);
            if (!check) break;
        }
        if (check) {
            return 0;
        }
    }
    return -1;
}

int findsync(const uint8_t *data, const int data_size, int *unit_size) {
    int pos = data_size;
    switch (*unit_size) {
    case 188: pos = findsync_unit_size<188>(data, data_size, true); break;
    case 192: pos = findsync_unit_size<192>(data, data_size, true); break;
    case 204: pos = findsync_unit_size<204>(data, data_size, true); break;
    default: break;
    }
    if (pos != data_size) {
        return pos;
    }
    int orig_unit_size = *unit_size;
    for (const auto test_unit_size : { 188, 192, 204 }) {
        if (test_unit_size != orig_unit_size) {
            *unit_size = test_unit_size;
            switch (*unit_size) {
            case 188: pos = findsync_unit_size<188>(data, data_size, false); break;
            case 192: pos = findsync_unit_size<192>(data, data_size, false); break;
            case 204: pos = findsync_unit_size<204>(data, data_size, false); break;
            default:  pos = -1; break;
            }
            if (pos >= 0) {
                return pos;
            }
        }
    }
    return -1;
}

RGYTSDemuxResult::RGYTSDemuxResult() :
    type(RGYTSPacketType::UNKNOWN),
    stream(),
    programNumber(-1),
    pts(TIMESTAMP_INVALID_VALUE),
    dts(TIMESTAMP_INVALID_VALUE),
    pcr(TIMESTAMP_INVALID_VALUE),
    pesHeader(),
    psi() {

}

RGYTSDemuxProgram::RGYTSDemuxProgram() :
    pmt_pid(),
    service(),
    pmtPsi() {

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
    m_programs(),
    m_targetService(nullptr),
    m_pcr(-1) {

}

RGYTSDemuxer::~RGYTSDemuxer() {
    
}

RGY_ERR RGYTSDemuxer::init(std::shared_ptr<RGYLog> log, int selectService) {
    m_log = log;
    m_selectServiceID = selectService;
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

                AddMessage(RGY_LOG_TRACE, _T("  program_number(service_id) %6d, pmt_pid 0x%04x (%d)\n"),
                    pmt.program_number, pmt.pmt_pid, pmt.pmt_pid);

                pat->pmt.push_back(pmt);
                ptr += 4;
            }
            if (!m_pat && pat) {
                for (const auto& p : pat->pmt) {
                    AddMessage(RGY_LOG_INFO, _T("program_number(service_id) %6d, pmt_pid 0x%04x (%d)\n"),
                        p.program_number, p.pmt_pid, p.pmt_pid);
                }
            }
            return pat;
        }
    }
    return nullptr;
}

void RGYTSDemuxer::parsePMT(RGYTSDemuxProgram *program) {
    auto psi = &program->pmtPsi;
    auto& service = program->service;
    if (psi->section_length < 9) {
        return;
    }
    const uint8_t *table = psi->data;
    service.programNumber = (table[3] << 8) | table[4];
    service.pidPcr = ((table[8] & 0x1f) << 8) | table[9];
    if (service.pidPcr == 0x1fff) {
        m_pcr = -1;
    }
    const int programInfoLength = ((table[10] & 0x03) << 8) | table[11];
    int pos = 3 + 9 + programInfoLength;
    if (psi->section_length < pos) {
        return;
    }

    const int lastAudio1Pid = service.aud0.stream.pid;
    const int lastAudio2Pid = service.aud1.stream.pid;
    service.vid.stream.pid = 0;
    service.aud0.stream.pid = 0;
    service.aud1.stream.pid = 0;
    service.cap.stream.pid = 0;
    service.pidSuperimpose = 0;
    service.aud0.stream.type = RGYTSStreamType::ADTS_TRANSPORT;
    service.aud1.stream.type = RGYTSStreamType::ADTS_TRANSPORT;
    service.pidList.clear();
    service.pidList.push_back({ RGYTSStreamType::UNKNOWN, program->pmt_pid.pmt_pid });
    service.pidList.push_back({ RGYTSStreamType::UNKNOWN, service.pidPcr });
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
        RGYTSStreamInfo streamInfo = { streamType, esPid };
        service.pidList.push_back(streamInfo);
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
                if ((service.vid.stream.pid == 0 && componentTag == 0xff) || componentTag == 0x00 || componentTag == 0x81) {
                    service.vid.stream.pid = esPid;
                    service.vid.stream.type = streamType;
                    videoDescPos = pos;
                    maybeCProfile = componentTag == 0x81;
                }
            } else if (streamType == RGYTSStreamType::ADTS_TRANSPORT) {
                if ((service.aud0.stream.pid == 0 && componentTag == 0xff) || componentTag == 0x10 || componentTag == 0x83 || componentTag == 0x85) {
                    service.aud0.stream.pid = esPid;
                    service.aud0.stream.type = streamType;
                    audio1DescPos = pos;
                    audio1ComponentTagUnknown = componentTag == 0xff;
                } else if (componentTag == 0x11) {
                    if (m_audio1Mode != 2) {
                        service.aud1.stream.pid = esPid;
                        service.aud1.stream.type = streamType;
                        audio2DescPos = pos;
                    }
                }
            } else if (streamType == RGYTSStreamType::MPEG2_AUDIO) {
                if (service.aud0.stream.pid == 0) {
                    service.aud0.stream.pid = esPid;
                    service.aud0.stream.type = streamType;
                    audio1DescPos = pos;
                    audio1ComponentTagUnknown = false;
                } else if (service.aud1.stream.pid == 0) {
                    if (m_audio1Mode != 2) {
                        service.aud1.stream.pid = esPid;
                        service.aud1.stream.type = streamType;
                        audio2DescPos = pos;
                    }
                }
            } else if (streamType == RGYTSStreamType::PES_PRIVATE_DATA) {
                if (componentTag == 0x30 || componentTag == 0x87) {
                    if (m_captionMode != 2) {
                        service.cap.stream.pid = esPid;
                        service.cap.stream.type = streamType;
                        captionDescPos = pos;
                    }
                } else if (componentTag == 0x38 || componentTag == 0x88) {
                    if (m_superimposeMode != 2) {
                        service.pidSuperimpose = esPid;
                        superimposeDescPos = pos;
                    }
                }
            }
        }
        pos += 5 + esInfoLength;
    }

    if (service.aud0.stream.pid != lastAudio1Pid) {
        service.aud0.pts = -1;
        service.aud0.packets.clear();
    }
    if (service.aud1.stream.pid != lastAudio2Pid) {
        service.aud1.pts = -1;
        service.aud1.packets.clear();
    }

    const auto loglevel = (service.versionNumber != psi->version_number) ? RGY_LOG_INFO : RGY_LOG_DEBUG;
    if (service.versionNumber != psi->version_number) {
        AddMessage(RGY_LOG_INFO, _T(" New PMT\n"));
    }
    service.versionNumber = psi->version_number;

    AddMessage(loglevel, _T("  version    %4d\n"),  service.versionNumber);
    AddMessage(loglevel, _T("  program  0x%04x (%d)\n"), service.programNumber, service.programNumber);
    AddMessage(loglevel, _T("  pid pmt  0x%04x (%d)\n"), program->pmt_pid.pmt_pid, program->pmt_pid.pmt_pid);
    AddMessage(loglevel, _T("  pid vid  0x%04x (%d)\n"), service.vid.stream.pid, service.vid.stream.pid);
    AddMessage(loglevel, _T("  pid aud0 0x%04x (%d)\n"), service.aud0.stream.pid, service.aud0.stream.pid);
    AddMessage(loglevel, _T("  pid aud1 0x%04x (%d)\n"), service.aud1.stream.pid, service.aud1.stream.pid);
    AddMessage(loglevel, _T("  pid cap  0x%04x (%d)\n"), service.cap.stream.pid, service.cap.stream.pid);
    AddMessage(loglevel, _T("  pid pcr  0x%04x (%d)\n"), service.pidPcr, service.pidPcr);
    for (size_t i = 0; i < service.pidList.size(); i++) {
        if (service.pidList[i].pid != program->pmt_pid.pmt_pid &&
            service.pidList[i].pid != service.vid.stream.pid &&
            service.pidList[i].pid != service.aud0.stream.pid &&
            service.pidList[i].pid != service.aud1.stream.pid &&
            service.pidList[i].pid != service.cap.stream.pid &&
            service.pidList[i].pid != service.pidPcr) {
            AddMessage(loglevel, _T("  pid      0x%04x (%d) (type=%d)\n"), service.pidList[i].pid, service.pidList[i].pid, service.pidList[i].type);
        }
    }
}

void RGYTSDemuxer::parsePMT(RGYTSDemuxProgram *program, const uint8_t *payload, const int payloadSize, const int unitStart, const int counter) {
    for (int done = 0; !done; ) {
        done = parsePSI(&program->pmtPsi, payload, payloadSize, unitStart, counter);
        AddMessage(RGY_LOG_DEBUG, _T("PMT section length %d, version_number %d\n"), program->pmtPsi.section_length, program->pmtPsi.version_number);
        if (program->pmtPsi.version_number && program->pmtPsi.table_id == 2 && program->pmtPsi.current_next_indicator) {
            parsePMT(program);
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
    for (auto& program : m_programs) {
        program->pmtPsi.reset();
    }
}

bool RGYTSDemuxer::isPIDTargetService(const int pid) const {
    if (!m_targetService) {
        return false;
    }
    return std::find_if(m_targetService->pidList.begin(), m_targetService->pidList.end(), [pid](const auto& p) {
        return p.pid == pid;
    }) != m_targetService->pidList.end();
}

bool RGYTSDemuxer::isPIDExists(const int pid) const {
    for (const auto& program : m_programs) {
        if (program->pmt_pid.pmt_pid == pid) {
            return true;
        }
        for (const auto& stream : program->service.pidList) {
            if (stream.pid == pid) {
                return true;
            }
        }
    }
    return false;
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
    if (serviceID < 0) {
        const int selectIndex = -serviceID;
        int idx = 0;
        for (const auto& pmt : m_pat->pmt) {
            if (pmt.program_number != 0) {
                idx++;
                if (idx == selectIndex) {
                    return &pmt;
                }
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

RGYTSDemuxProgram *RGYTSDemuxer::selectProgramFromPMTPID(const int pmt_pid) {
    auto it = std::find_if(m_programs.begin(), m_programs.end(), [pmt_pid](const auto& p) {
        return p->pmt_pid.pmt_pid == pmt_pid;
        });
    return (it != m_programs.end()) ? it->get() : nullptr;
}

RGYTSDemuxProgram *RGYTSDemuxer::selectProgramFromPID(const int pid) {
    for (const auto& program : m_programs) {
        if (program->pmt_pid.pmt_pid == pid) {
            return program.get();
        }
        for (const auto& stream : program->service.pidList) {
            if (stream.pid == pid) {
                return program.get();
            }
        }
    }
    return nullptr;
}

void RGYTSDemuxer::checkPMTList() {
    if (m_pat->pmt.size() == m_programs.size()) {
        // m_pat->pmt の並びとm_programsの並びが一致しているかを確認する
        bool match_ok = true;
        for (size_t i = 0; i < m_pat->pmt.size(); i++) {
            const auto& program = m_programs[i];
            if (m_programs[i]->pmt_pid.pmt_pid != m_pat->pmt[i].pmt_pid
                || m_programs[i]->pmt_pid.program_number != m_pat->pmt[i].program_number) {
                match_ok = false;
                break;
            }
        }
        if (match_ok) {
            return; // 一致していれば終了
        }
    }
    // 一致していない場合、m_pat->pmtの並びに合わせてm_programsを再構築する
    auto program_tmp = std::move(m_programs);
    m_programs.resize(m_pat->pmt.size());

    for (size_t i = 0; i < m_pat->pmt.size(); i++) {
        bool found = false;
        for (auto& program : program_tmp) {
            if (program
                && program->pmt_pid.pmt_pid == m_pat->pmt[i].pmt_pid
                && program->pmt_pid.program_number == m_pat->pmt[i].program_number) {
                m_programs[i] = std::move(program);
                found = true;
                break;
            }
        }
        if (!found) {
            m_programs[i] = std::make_unique<RGYTSDemuxProgram>();
            m_programs[i]->pmt_pid = m_pat->pmt[i];
        }
    }
}

std::tuple<RGY_ERR, RGYTSDemuxResult> RGYTSDemuxer::parse(const RGYTSPacket *pkt) {
    RGYTSDemuxResult result;
    const auto& packetHeader = pkt->header;

    // PAT
    if (packetHeader.PID == 0x00) {
        m_pat = parsePAT(pkt->payload(), packetHeader.payloadSize, packetHeader.PayloadStartFlag, packetHeader.Counter);
        checkPMTList();
        result.type = RGYTSPacketType::PAT;
        return { RGY_ERR_NONE, std::move(result) };
    }
    // PMT
    if (m_pat) {
        const RGYTS_PMT_PID *pmt_pid = nullptr;
        for (const auto& pmt : m_pat->pmt) {
            if (packetHeader.PID == pmt.pmt_pid) {
                pmt_pid = &pmt;
                break;
            }
        }
        if (pmt_pid) {
            auto program = selectProgramFromPMTPID(packetHeader.PID);
            if (!program) {
                // なければ新しく作って追加する
                m_programs.push_back(std::make_unique<RGYTSDemuxProgram>());
                program = m_programs.back().get();
                program->pmt_pid = *pmt_pid;
            }
            parsePMT(program, pkt->payload(), packetHeader.payloadSize, packetHeader.PayloadStartFlag, packetHeader.Counter);
            result.type = RGYTSPacketType::PMT;
            result.psi = std::make_unique<RGYTS_PSI>(program->pmtPsi);
            result.programNumber = program->pmt_pid.program_number;

            // 対象のserviceであるかを確認する
            auto pmt_pid = selectServiceID();
            if (pmt_pid && pmt_pid->pmt_pid == program->pmt_pid.pmt_pid) {
                m_targetService = &program->service;
            }
            return { RGY_ERR_NONE, std::move(result) };
        }
    }

    result.type = RGYTSPacketType::OTHER;
    // PIDからどのserviceか検索する
    auto service = selectProgramFromPID(packetHeader.PID);
    if (service) {
        // 見つかったらprogramNumberを設定する
        result.programNumber = service->pmt_pid.program_number;
        // stream_typeも設定
        for (const auto& st : service->service.pidList) {
            if (packetHeader.PID == st.pid) {
                result.stream = st;
                break;
            }
        }
        if (packetHeader.PID == service->service.pidPcr) {
            const auto pcr = parsePCR(packetHeader, pkt->data());
            if (pcr != TIMESTAMP_INVALID_VALUE) {
                m_pcr = pcr;
            }
            result.type = RGYTSPacketType::PCR;
            result.pcr = pcr;
            // PCRが他のストリームに含まれる場合は、PCRが取得できなくても異常ではない
            const auto pcrMuxedWithOtherStream = 
                   packetHeader.PID == service->service.vid.stream.pid
                || packetHeader.PID == service->service.aud0.stream.pid
                || packetHeader.PID == service->service.aud1.stream.pid
                || packetHeader.PID == service->service.cap.stream.pid
                || packetHeader.PID == service->service.pidSuperimpose;
            AddMessage((pcr != TIMESTAMP_INVALID_VALUE || pcrMuxedWithOtherStream) ? RGY_LOG_TRACE : RGY_LOG_WARN,
                _T("  pid pcr  0x%04x, %lld\n"), service->service.pidPcr, pcr);
        }
        if (packetHeader.PID == service->service.vid.stream.pid) {
            result.type = RGYTSPacketType::VID;
            if (packetHeader.PayloadStartFlag) {
                auto pes = parsePESHeader(pkt->packet);
                AddMessage(RGY_LOG_TRACE, _T("  pid vid  0x%04x, %s, %4d, %lld, %lld\n"),
                    service->service.vid.stream.pid, packetHeader.adapt.random_access ? _T("K") : _T("_"), packetHeader.payloadSize, pes.pts, pes.dts);
                result.pts = pes.pts;
                result.dts = pes.dts;
            }
        } else if (packetHeader.PID == service->service.aud0.stream.pid) {
            if (packetHeader.PayloadStartFlag) {
                auto pes = parsePESHeader(pkt->packet);
                AddMessage(RGY_LOG_TRACE, _T("  pid aud0 0x%04x, %lld\n"), service->service.vid.stream.pid, pes.pts);
                result.pts = pes.pts;
                result.dts = pes.dts;
            }
        } else if (packetHeader.PID == service->service.cap.stream.pid) {
            if (packetHeader.PayloadStartFlag) {
                auto pes = parsePESHeader(pkt->packet);
                AddMessage(RGY_LOG_TRACE, _T("  pid cap  0x%04x, %lld\n"), service->service.vid.stream.pid, pes.pts);
                result.pts = pes.pts;
                result.dts = pes.dts;
            }
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
        pktHeader.adapt.discontinuity          = d5 & 0x80;
        pktHeader.adapt.random_access          = d5 & 0x40;
        pktHeader.adapt.es_priority            = d5 & 0x20;
        pktHeader.adapt.pcr_flag               = d5 & 0x10;
        pktHeader.adapt.opcr_flag              = d5 & 0x08;
        pktHeader.adapt.splicing_point         = d5 & 0x04;
        pktHeader.adapt.transport_private_data = d5 & 0x02;
        pktHeader.adapt.adaption_filed_ext     = d5 & 0x01;
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

    while (m_packetSize == 0 || (int)m_readBuf.size() >= m_packetSize) {
        auto offset = findsync(m_readBuf.data(), (int)m_readBuf.size(), &m_packetSize);
        if (offset < 0) {
            // 見つからなかった場合、読み飛ばしてその先から見つけなおす
            int step = m_packetSize ? m_packetSize : 188;
            for (int dataoffset = step; dataoffset < (int)m_readBuf.size() - step; dataoffset += step) {
                offset = findsync(m_readBuf.data() + dataoffset, (int)m_readBuf.size() - dataoffset, &m_packetSize);
                if (offset >= 0) {
                    offset += dataoffset;
                    m_log->write(RGY_LOG_WARN, RGY_LOGT_IN, _T("Skip %d byte, data might be corrupted.\n"), offset);
                    break;
                }
                step = m_packetSize ? m_packetSize : 188;
            }
        }
        if (offset < 0) {
            return { RGY_ERR_NONE, std::move(packets) };
        }

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
