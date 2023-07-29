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

#include "rgy_tsstruct.h"
#include <cstring>

void RGYTSPacketDeleter::operator()(RGYTSPacket *pkt) {
    if (pkt) {
        m_container->addEmpty(pkt);
    }
};

RGYTSPacketContainer::RGYTSPacketContainer() :
    m_emptyList() {
}

RGYTSPacketContainer::~RGYTSPacketContainer() {
    for (auto pkt : m_emptyList) {
        delete pkt;
    }
    m_emptyList.clear();
}

uniqueRGYTSPacket RGYTSPacketContainer::getEmpty() {
    RGYTSPacket *pkt = nullptr;
    if (m_emptyList.empty()) {
        pkt = new RGYTSPacket();
    } else {
        pkt = m_emptyList.front();
        m_emptyList.pop_front();
    }
    return uniqueRGYTSPacket(pkt, RGYTSPacketDeleter(this));
}

void RGYTSPacketContainer::addEmpty(RGYTSPacket *pkt) {
    m_emptyList.push_back(pkt);
}

RGYTS_PSI::RGYTS_PSI() :
    table_id(0),
    section_length(0),
    version_number(0),
    current_next_indicator(0),
    continuity_counter(0),
    data_count(0),
    data() {
};

void RGYTS_PSI::reset() {
    table_id = 0;
    section_length = 0;
    version_number = 0;
    current_next_indicator = 0;
    continuity_counter = 0;
    data_count = 0;
    memset(data, 0, sizeof(data));
}