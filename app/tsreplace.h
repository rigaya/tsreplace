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

#ifndef __TSREPLACE_H__
#define __TSREPLACE_H__

#include <memory>
#include <deque>
#include "rgy_tsdemux.h"
#include "rgy_avutil.h"

enum class RGYHEVCBsf {
    INTERNAL,
    LIBAVCODEC
};

struct AVDemuxFormat {
    std::unique_ptr<AVFormatContext, decltype(&avformat_free_context)> formatCtx; //動画ファイルのformatContext
    double                    analyzeSec;            //動画ファイルを先頭から分析する時間
    bool                      isPipe;                //入力がパイプ
    bool                      lowLatency;            //低遅延モード
    AVDictionary             *formatOptions;         //avformat_open_inputに渡すオプション

    AVDemuxFormat();
    ~AVDemuxFormat();
    void close();
};

struct AVDemuxVideo {
    //動画は音声のみ抽出する場合でも同期のため参照することがあり、
    //pCodecCtxのチェックだけでは読み込むかどうか判定できないので、
    //実際に使用するかどうかはこのフラグをチェックする
    bool                      readVideo;
    const AVStream           *stream;                //動画のStream, 動画を読み込むかどうかの判定には使用しないこと (readVideoを使用)
    int                       index;                 //動画のストリームID
    int64_t                   streamFirstKeyPts;     //動画ファイルの最初のpts
    AVPacket                 *firstPkt;              //動画の最初のpacket
    uint32_t                  streamPtsInvalid;      //動画ファイルのptsが無効 (H.264/ES, 等)
    bool                      gotFirstKeyframe;      //動画の最初のキーフレームを取得済み
    int                       keyFrameOffset;
    std::unique_ptr<AVBSFContext, RGYAVDeleter<AVBSFContext>> bsfcCtx;
    uint8_t                  *extradata;             //動画のヘッダ情報
    int                       extradataSize;         //動画のヘッダサイズ

    RGYHEVCBsf                hevcbsf;               //HEVCのbsfの選択
    bool                      bUseHEVCmp42AnnexB;
    int                       hevcNaluLengthSize;

    AVDemuxVideo();
    ~AVDemuxVideo();
    void close();
};

struct AVDemuxer {
    AVDemuxFormat format;
    AVDemuxVideo  video;

    AVDemuxer() : format(), video() {};
};

class TSReplaceVideo {
public:
    TSReplaceVideo(std::shared_ptr<RGYLog> log);
    virtual ~TSReplaceVideo();
    std::vector<int> getAVReaderStreamIndex(AVMediaType type);
    RGY_ERR initAVReader(const tstring& videofile);
    std::tuple<int, std::unique_ptr<AVPacket, RGYAVDeleter<AVPacket>>> getSample();
    RGYTSStreamType getVideoStreamType() const;

    AVRational getVidTimebase() const;
    int64_t getFirstKeyPts() const;
    std::tuple<RGY_ERR, int64_t, int64_t> getFrontPktPtsDts();
    std::tuple<RGY_ERR, std::unique_ptr<AVPacket, RGYAVDeleter<AVPacket>>> getFrontPktAndPop();

protected:
    RGY_ERR GetHeader();
    void hevcMp42Annexb(AVPacket *pkt);
    RGY_ERR initVideoBsfs();

    void AddMessage(RGYLogLevel log_level, const tstring &str) {
        if (m_log == nullptr || log_level < m_log->getLogLevel(RGY_LOGT_APP)) {
            return;
        }
        auto lines = split(str, _T("\n"));
        for (const auto &line : lines) {
            if (line[0] != _T('\0')) {
                m_log->write(log_level, RGY_LOGT_APP, (_T("replace: ") + line + _T("\n")).c_str());
            }
        }
    }
    void AddMessage(RGYLogLevel log_level, const TCHAR *format, ...) {
        if (m_log == nullptr || log_level < m_log->getLogLevel(RGY_LOGT_APP)) {
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

    tstring m_filename;
    AVDemuxer m_Demux;
    std::shared_ptr<RGYLog> m_log;
    std::unique_ptr<RGYPoolAVPacket> m_poolPkt;
    std::vector<uint8_t> m_hevcMp42AnnexbBuffer;       //HEVCのmp4->AnnexB簡易変換用バッファ;
    std::deque<std::unique_ptr<AVPacket, RGYAVDeleter<AVPacket>>> m_packets;
};

struct TSRReplaceParams {
    tstring input;
    tstring replacefile;
    tstring output;

    TSRReplaceParams();
};

class TSReplace {
protected:
    static const int TS_TIMEBASE = 90000;
public:
    TSReplace();
    virtual ~TSReplace();

    RGY_ERR init(std::shared_ptr<RGYLog> log, const TSRReplaceParams& prms);
    RGY_ERR restruct();
protected:
    RGY_ERR readTS(std::vector<uniqueRGYTSPacket>& packetBuffer);
    RGY_ERR writePacket(const RGYTSPacket *pkt);
    RGY_ERR writeReplacedPMT(const RGYTSDemuxResult& result);
    RGY_ERR writeReplacedVideo();
    RGY_ERR writeReplacedVideo(AVPacket *pkt);
    int64_t getOrigPtsOffset();
    void pushPESPTS(std::vector<uint8_t>& buf, const int64_t pts, const uint8_t top4bit);

    void AddMessage(RGYLogLevel log_level, const tstring &str) {
        if (m_log == nullptr || log_level < m_log->getLogLevel(RGY_LOGT_APP)) {
            return;
        }
        auto lines = split(str, _T("\n"));
        for (const auto &line : lines) {
            if (line[0] != _T('\0')) {
                m_log->write(log_level, RGY_LOGT_APP, (_T("replace: ") + line + _T("\n")).c_str());
            }
        }
    }
    void AddMessage(RGYLogLevel log_level, const TCHAR *format, ...) {
        if (m_log == nullptr || log_level < m_log->getLogLevel(RGY_LOGT_APP)) {
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

    std::shared_ptr<RGYLog> m_log;
    std::unique_ptr<RGYTSDemuxer> m_demuxer;
    std::chrono::system_clock::time_point m_tsReadUpdate; // 読み込みの進捗表示用
    int m_fileTSBufSize; // 読み込みtsのファイルバッファサイズ
    tstring m_fileTS;    // 入力tsファイル名
    tstring m_fileOut;   // 出力tsファイル名
    std::unique_ptr<RGYTSPacketSplitter> m_tsPktSplitter; // ts読み込み時ののpacket分割用
    std::unique_ptr<FILE, fp_deleter> m_fpTSIn;  // 入力tsファイル
    std::unique_ptr<FILE, fp_deleter> m_fpTSOut; // 出力tsファイル
    std::vector<uint8_t> m_bufferTS; // 読み込みtsのファイルバッファ
    uint16_t m_vidPIDReplace; // 出力tsの動画のPID上書き用
    int64_t m_vidPTSOutMax;   // 動画フレームのPTS最大値(出力制御用)
    int64_t m_vidPTS;         // 直前の動画フレームのPTS
    int64_t m_vidDTS;         // 直前の動画フレームのDTS
    int64_t m_vidFirstPTS;    // 最初の動画フレームのPTS
    int64_t m_vidFirstDTS;    // 最初の動画フレームのDTS
    int64_t m_vidFirstKeyPTS;    // 最初の動画キーフレームのPTS
    std::vector<uint8_t> m_lastPmt; // 直前の出力PMTデータ
    std::unique_ptr<TSReplaceVideo> m_video; // 置き換え対象の動画の読み込み用
    uint8_t m_pmtCounter; // 出力PMTのカウンタ
    uint8_t m_vidCounter; // 出力映像のカウンタ
    int64_t m_ptswrapOffset; // PCR wrapの加算分
};

#endif //__TSREPLACE_H__
