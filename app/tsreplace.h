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
#include <thread>
#include <mutex>
#include "rgy_tsdemux.h"
#include "rgy_avutil.h"
#include "rgy_pipe.h"
#include "rgy_queue.h"

enum class TSRReplaceStartPoint {
    KeyframPts,
    FirstFrame,
    FirstPacket,
};

static const CX_DESC list_startpoint[] = {
    { _T("keyframe"),    (int)TSRReplaceStartPoint::KeyframPts },
    { _T("firstframe"),  (int)TSRReplaceStartPoint::FirstFrame },
    { _T("firstpacket"), (int)TSRReplaceStartPoint::FirstPacket },
    { nullptr, 0 }
};


enum RGYPktFlags : uint32_t {
    RGY_FLAG_PICT_TYPE_I     = 0x0001000,
    RGY_FLAG_PICT_TYPE_P     = 0x0002000,
    RGY_FLAG_PICT_TYPE_B     = 0x0003000,
    RGY_FLAG_PICT_TYPES      = 0x0003000,

    RGY_FLAG_PICSTRUCT_FIELD = 0x0010000,
    RGY_FLAG_PICSTRUCT_TFF   = 0x0020000,
    RGY_FLAG_PICSTRUCT_BFF   = 0x0040000,
    RGY_FLAG_PICSTRUCT_RFF   = 0x0080000,
    RGY_FLAG_PICSTRUCTS      = RGY_FLAG_PICSTRUCT_TFF | RGY_FLAG_PICSTRUCT_BFF | RGY_FLAG_PICSTRUCT_RFF,
};

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

    uint8_t                  *inputBuffer;           //入力バッファ

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

    const AVCodec            *codecDecode;                                        //動画のデコーダ (使用しない場合はnullptr)
    std::unique_ptr<AVCodecContext, RGYAVDeleter<AVCodecContext>> codecCtxDecode; //動画のデコーダ (使用しない場合はnullptr)


    std::unique_ptr<AVCodecParserContext, decltype(&av_parser_close)> parserCtx;            //動画ストリームのParser
    std::unique_ptr<AVCodecContext, RGYAVDeleter<AVCodecContext>> codecCtxParser;       //動画ストリームのParser用

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

    AVDemuxer();
    ~AVDemuxer();
    void close();
};

class TSReplaceVideo {
public:
    TSReplaceVideo(std::shared_ptr<RGYLog> log);
    virtual ~TSReplaceVideo();
    std::vector<int> getAVReaderStreamIndex(AVMediaType type);
    RGY_ERR initAVReader(const tstring& videofile, RGYQueueBuffer *inputQueue, const tstring& inputFormat);
    std::tuple<int, std::unique_ptr<AVPacket, RGYAVDeleter<AVPacket>>> getSample();
    RGYTSStreamType getVideoStreamType() const;

    const uint8_t *getExtraData(int& size) const;
    const AVCodecParameters *getVidCodecPar() const;
    AVCodecID getVidCodecID() const;
    AVRational getVidTimebase() const;
    int64_t getFirstKeyPts() const;
    RGY_ERR fillPackets();
    std::tuple<RGY_ERR, int64_t, int64_t> getFrontPktPtsDts();
    std::tuple<RGY_ERR, std::unique_ptr<AVPacket, RGYAVDeleter<AVPacket>>> getFrontPktAndPop();

    RGY_ERR initDecoder();
    RGY_ERR getFirstPts(int64_t& firstPTSVideoAudioStreams, int64_t& firstPTSFrame, int64_t& firstPTSKeyFrame);

    // custom io
    int readPacket(uint8_t *buf, int buf_size);
    int writePacket(uint8_t *buf, int buf_size);
    int64_t seek(int64_t offset, int whence);
protected:
    void SetExtraData(AVCodecParameters *codecParam, const uint8_t *data, uint32_t size);
    RGY_ERR GetHeader();
    void hevcMp42Annexb(AVPacket *pkt);
    RGY_ERR initVideoBsfs();
    RGY_ERR initVideoParser();

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
    int64_t m_firstPTSFrame;              //最初のフレームのpts
    int64_t m_firstPTSVideoAudioStreams;  //全ストリームの最初のpts
    RGYQueueBuffer *m_inputQueue;
};

struct TSRReplaceParams {
    tstring input;
    tstring replacefile;
    tstring replacefileformat;
    tstring output;
    TSRReplaceStartPoint startpoint;
    tstring encoderPath;
    std::vector<tstring> encoderArgs;
    bool addAud;
    bool addHeaders;

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
    void close();
protected:
    RGY_ERR initDemuxer(std::vector<uniqueRGYTSPacket>& tsPackets);
    RGY_ERR initEncoder();
    RGY_ERR readTS(std::vector<uniqueRGYTSPacket>& packetBuffer);
    RGY_ERR writePacket(const RGYTSPacket *pkt);
    RGY_ERR writeReplacedPMT(const RGYTSDemuxResult& result);
    RGY_ERR writeReplacedVideo();
    RGY_ERR writeReplacedVideo(AVPacket *pkt);
    int64_t getOrigPtsOffset();
    void pushPESPTS(std::vector<uint8_t>& buf, const int64_t pts, const uint8_t top4bit);
    bool isFirstNalAud(const bool isHEVC, const uint8_t *ptr, const size_t size);
    uint8_t getvideoDecCtrlEncodeFormat(const int height);
    uint8_t getAudValue(const AVPacket *pkt) const;
    std::tuple<RGY_ERR, bool, bool> checkPacket(const AVPacket *pkt);
    int64_t getStartPointPTS() const;

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
    tstring m_replaceFileFormat; // 置き換え用ファイルのフォーマット
    tstring m_encoderPath; // エンコーダのパス
    std::vector<tstring> m_encoderArgs; // エンコーダの引数
    std::unique_ptr<RGYTSPacketSplitter> m_tsPktSplitter; // ts読み込み時ののpacket分割用
    std::unique_ptr<FILE, fp_deleter> m_fpTSIn;  // 入力tsファイル
    std::unique_ptr<FILE, fp_deleter> m_fpTSOut; // 出力tsファイル
    bool m_inputAbort; // 入力スレッドの終了要求
    std::unique_ptr<std::thread> m_threadInputTS; // オリジナルts読み込みスレッド
    std::unique_ptr<std::thread> m_threadSendEncoder; // tsからエンコーダへの送信スレッド
    std::unique_ptr<RGYQueueBuffer> m_queueInputReplace; // tsreplaceの読み込み用
    std::unique_ptr<RGYQueueBuffer> m_queueInputEncoder; // エンコーダの読み込み用
    std::unique_ptr<RGYQueueBuffer> m_queueInputPreAnalysis; // 事前解析読み込み用
    std::vector<uint8_t> m_bufferTS; // 読み込みtsのファイルバッファ
    bool m_preAnalysisFin; // 事前解析の終了
    uint16_t m_vidPIDReplace;   // 出力tsの動画のPID上書き用
    int64_t m_vidDTSOutMax;     // 動画フレームのDTS最大値(出力制御用)
    int64_t m_vidPTS;           // 直前の動画フレームのPTS
    int64_t m_vidDTS;           // 直前の動画フレームのDTS
    int64_t m_vidFirstFramePTS; // 最初の動画フレームのPTS
    int64_t m_vidFirstFrameDTS; // 最初の動画フレームのDTS
    int64_t m_vidFirstKeyPTS;   // 最初の動画キーフレームのPTS
    TSRReplaceStartPoint m_startPoint; // 起点モード
    int64_t m_vidFirstTimestamp;       // 起点のtimestamp
    int64_t m_vidFirstPacketPTS;       // 最初のパケットのPTS
    std::vector<uint8_t> m_lastPmt; // 直前の出力PMTデータ
    std::unique_ptr<TSReplaceVideo> m_videoReplace; // 置き換え対象の動画の読み込み用
    uint8_t m_pmtCounter; // 出力PMTのカウンタ
    uint8_t m_vidCounter; // 出力映像のカウンタ
    int64_t m_ptswrapOffset; // PCR wrapの加算分
    bool m_addAud; // audの挿入
    bool m_addHeaders; // ヘッダの挿入
    decltype(parse_nal_unit_h264_c) *m_parseNalH264; // H.264用のnal unit分解関数へのポインタ
    decltype(parse_nal_unit_hevc_c) *m_parseNalHevc; // HEVC用のnal unit分解関数へのポインタ

    std::unique_ptr<RGYPipeProcess> m_encoder; // エンコーダプロセス
    ProcessPipe m_encPipe;      // エンコーダのパイプ
    std::thread m_encThreadOut; // エンコーダからの標準出力を読み取り m_videoReplace に転送するスレッド
    std::thread m_encThreadErr; // エンコーダからの標準エラー出力を読み取るスレッド
    std::unique_ptr<RGYQueueBuffer> m_encQueueOut; // エンコーダ → m_videoReplace への転送用
};

#endif //__TSREPLACE_H__
