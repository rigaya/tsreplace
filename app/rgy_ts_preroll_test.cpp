#include "rgy_ts_preroll.h"
#include <cstdlib>
#include <iostream>

static void check(bool value, const char *message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
static int64_t pcr(const uint8_t *p) {
    return (int64_t(p[6]) << 25) | (int64_t(p[7]) << 17)
        | (int64_t(p[8]) << 9) | (int64_t(p[9]) << 1) | (p[10] >> 7);
}
static void table(std::vector<uint8_t>& data, int pid, int length, int cc) {
    std::vector<uint8_t> section(length, 0);
    section[0] = pid ? 2 : 0;
    section[1] = uint8_t(0xb0 | ((length - 3) >> 8));
    section[2] = (length - 3) & 255;
    size_t pos = 0;
    while (pos < section.size()) {
        std::array<uint8_t, 188> packet;
        packet.fill(0xff);
        packet[0] = 0x47; packet[1] = uint8_t((pos ? 0 : 0x40) | (pid >> 8));
        packet[2] = pid & 255; packet[3] = 0x10 | (cc++ & 15);
        size_t payload = 4;
        if (!pos) packet[payload++] = 0;
        const auto n = std::min(section.size() - pos, 188 - payload);
        std::copy_n(section.data() + pos, n, packet.data() + payload);
        pos += n; data.insert(data.end(), packet.begin(), packet.end());
    }
}
int main() {
    constexpr int64_t mask = (int64_t(1) << 33) - 1;
    for (const auto first : {int64_t(45000), mask - 45000, int64_t(7323214731)}) {
        for (const int duration : {20, 960, 1281, 1600, 60000}) {
            const int repetitions = (duration + 19) / 20;
            std::vector<uint8_t> input;
            table(input, 0, 200, 15); // PATを複数パケットに分割し、CCの周回を確認する。
            table(input, 0x101, 400, 14); // PMTを3パケットに分割する。
            check(tsrMakeStartupPreroll(input, 0x101, 0x100).empty(), "Missing PCR must not emit preroll");
            std::array<uint8_t, 188> clock;
            clock.fill(0xff);
            clock[0]=0x47;clock[1]=1;clock[2]=0;clock[3]=0x30|7;clock[4]=7;clock[5]=0x10;
            clock[6]=uint8_t(first>>25);clock[7]=uint8_t(first>>17);clock[8]=uint8_t(first>>9);clock[9]=uint8_t(first>>1);
            clock[10]=uint8_t(first<<7)|0x7f;clock[11]=0x20; // PCRの拡張部を288にする。
            input.insert(input.end(),clock.begin(),clock.end());
            const auto original = input;
            const auto lead = tsrMakeStartupPreroll(input,0x101,0x100,duration);
            check(input==original, "Original TS must remain untouched");
            check(lead.size()==(repetitions*6+(repetitions>=4?256:0))*188, "Complete tables and delivery padding must be present");
            int previous[2] = {-1,-1};
            int64_t previousPCR = -1;
            int clocks=0, nulls=0;
            for(size_t i=0;i<lead.size();i+=188) {
                const auto *p=lead.data()+i;const int pid=((p[1]&31)<<8)|p[2];
                if(pid==0x100) {
                    check(p[3] == (0x20 | 6), "Adaptation-only PCR must not increment payload CC");
                    check((p[10]&1)==1&&p[11]==0x20, "PCR extension must be preserved");
                    const auto now=pcr(p);
                    if(previousPCR>=0)check(((now-previousPCR)&mask)==1800,"PCR must be monotonic across wrap");
                    else check(((first-now)&mask)==repetitions*1800,"Lead-in must match rounded duration");
                    previousPCR=now;clocks++;
                } else if(pid==0x1fff) {
                    check(clocks==4,"Delivery padding must follow four PCRs");
                    nulls++;
                } else {
                    const int t=pid==0?0:1,cc=p[3]&15;
                    if(previous[t]>=0)check(cc==((previous[t]+1)&15),"PSI CC must stay consecutive");
                    previous[t]=cc;
                }
            }
            check(nulls==(repetitions>=4?256:0),"Delivery padding must exceed TvtPlay send buffer");
            check(clocks==repetitions&&((first-previousPCR)&mask)==1800,"Last PCR must precede original PCR by 20 ms");
            check(((previous[0]+1)&15)==15&&((previous[1]+1)&15)==14,"Original PSI continuity must be preserved");
            check(tsrMakeStartupPreroll(input,0x102,0x100).empty(),"Missing selected PMT must not emit preroll");
        }
    }
    check(tsrMakeStartupPreroll({},0x101,0x100,0).empty(),"Disabled lead-in must be empty");
    check(tsrMakeStartupPreroll({},0x101,0x100,-1).empty(),"Negative duration must be rejected");
    check(tsrMakeStartupPreroll({},0x101,0x100,60001).empty(),"Oversized duration must be rejected");
    std::cout << "Startup preroll tests passed\n";
}
