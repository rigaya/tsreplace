#include "rgy_tsstruct.h"

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