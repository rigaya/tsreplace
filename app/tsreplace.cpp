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

#include <memory>
#include <vector>
#include <thread>
#if defined(_WIN32) || defined(_WIN64)
#include <fcntl.h>
#endif
#include "rgy_osdep.h"
#include "rgy_tchar.h"
#include "rgy_util.h"
#include "rgy_avlog.h"
#include "rgy_bitstream.h"
#include "rgy_filesystem.h"
#include "tsreplace.h"

static const int64_t WRAP_AROUND_VALUE = (1LL << 33);
static const int64_t WRAP_AROUND_CHECK_VALUE = ((1LL << 32) - 1);

static_assert(TIMESTAMP_INVALID_VALUE == AV_NOPTS_VALUE);

static int funcReadPacket(void *opaque, uint8_t *buf, int buf_size) {
    TSReplaceVideo *reader = reinterpret_cast<TSReplaceVideo *>(opaque);
    return reader->readPacket(buf, buf_size);
}
static int funcWritePacket(void *opaque, uint8_t *buf, int buf_size) {
    TSReplaceVideo *reader = reinterpret_cast<TSReplaceVideo *>(opaque);
    return reader->writePacket(buf, buf_size);
}
static int64_t funcSeek(void *opaque, int64_t offset, int whence) {
    TSReplaceVideo *reader = reinterpret_cast<TSReplaceVideo *>(opaque);
    return reader->seek(offset, whence);
}

AVDemuxFormat::AVDemuxFormat() :
    formatCtx(std::unique_ptr<AVFormatContext, decltype(&avformat_free_context)>(nullptr, avformat_free_context)),
    analyzeSec(0.0),
    isPipe(false),
    lowLatency(false),
    formatOptions(nullptr),
    inputBuffer() {

}

AVDemuxFormat::~AVDemuxFormat() {
    close();
}

void AVDemuxFormat::close() {
    formatCtx.reset();
    if (formatOptions) {
        av_dict_free(&formatOptions);
        formatOptions = nullptr;
    }
}

AVDemuxVideo::AVDemuxVideo() :
    readVideo(true),
    stream(nullptr),
    index(-1),
    streamFirstKeyPts(AV_NOPTS_VALUE),
    firstPkt(nullptr),
    streamPtsInvalid(0),
    gotFirstKeyframe(false),
    keyFrameOffset(0),
    bsfcCtx(std::unique_ptr<AVBSFContext, RGYAVDeleter<AVBSFContext>>(nullptr, RGYAVDeleter<AVBSFContext>(av_bsf_free))),
    extradata(nullptr),
    extradataSize(0),
    codecDecode(nullptr),
    codecCtxDecode(std::unique_ptr<AVCodecContext, RGYAVDeleter<AVCodecContext>>(nullptr, RGYAVDeleter<AVCodecContext>(avcodec_free_context))),
    parserCtx(std::unique_ptr<AVCodecParserContext, decltype(&av_parser_close)>(nullptr, av_parser_close)),
    codecCtxParser(std::unique_ptr<AVCodecContext, RGYAVDeleter<AVCodecContext>>(nullptr, RGYAVDeleter<AVCodecContext>(avcodec_free_context))),
    hevcbsf(RGYHEVCBsf::INTERNAL),
    bUseHEVCmp42AnnexB(false),
    hevcNaluLengthSize(0) {

}

AVDemuxVideo::~AVDemuxVideo() {
    close();
}

void AVDemuxVideo::close() {
    parserCtx.reset();
    codecCtxParser.reset();

    codecCtxDecode.reset();
    bsfcCtx.reset();
    if (firstPkt) {
        av_packet_unref(firstPkt);
        firstPkt = nullptr;
    }
    if (extradata) {
        av_free(extradata);
        extradata = nullptr;
    }
}

AVDemuxer::AVDemuxer() : format(), video() {};

AVDemuxer::~AVDemuxer() {
    close();
}
void AVDemuxer::close() {
    video.close();
    format.close();
}

TSRReplaceParams::TSRReplaceParams() :
    input(),
    replacefile(),
    replacefileformat(),
    output(),
    startpoint(TSRReplaceStartPoint::KeyframPts),
    addAud(true),
    addHeaders(true) {
}

TSReplaceVideo::TSReplaceVideo(std::shared_ptr<RGYLog> log) :
    m_filename(),
    m_Demux(),
    m_log(log),
    m_poolPkt(std::make_unique<RGYPoolAVPacket>()),
    m_hevcMp42AnnexbBuffer(),
    m_packets(),
    m_firstPTSFrame(TIMESTAMP_INVALID_VALUE),
    m_firstPTSVideoAudioStreams(TIMESTAMP_INVALID_VALUE),
    m_inputQueue(nullptr) {

}

TSReplaceVideo::~TSReplaceVideo() {

}

void TSReplaceVideo::hevcMp42Annexb(AVPacket *pkt) {
    static const uint8_t SC[] = { 0, 0, 0, 1 };
    if (pkt == NULL) {
        m_hevcMp42AnnexbBuffer.reserve(m_Demux.video.extradataSize + 128);
        const uint8_t *ptr = m_Demux.video.extradata;
        const uint8_t *ptr_fin = ptr + m_Demux.video.extradataSize;
        ptr += 21;
        m_Demux.video.hevcNaluLengthSize = ((*ptr) & 3) + 1; ptr++;
        const int numOfArrays = *ptr; ptr++;
        for (int ia = 0; ia < numOfArrays; ia++) {
            ptr++;
            const int count = readUB16(ptr); ptr += 2;
            for (int i = (std::max)(1, count); i; i--) {
                uint32_t size = readUB16(ptr); ptr += 2;
                m_hevcMp42AnnexbBuffer.insert(m_hevcMp42AnnexbBuffer.end(), SC, SC + 4);
                m_hevcMp42AnnexbBuffer.insert(m_hevcMp42AnnexbBuffer.end(), ptr, ptr + size); ptr += size;
            }
        }
        if (m_Demux.video.extradata) {
            av_free(m_Demux.video.extradata);
        }
        m_Demux.video.extradata = (uint8_t *)av_malloc(m_hevcMp42AnnexbBuffer.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        m_Demux.video.extradataSize = (int)m_hevcMp42AnnexbBuffer.size();
        memcpy(m_Demux.video.extradata, m_hevcMp42AnnexbBuffer.data(), m_hevcMp42AnnexbBuffer.size());
        memset(m_Demux.video.extradata + m_Demux.video.extradataSize, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        if (ptr != ptr_fin) {
            AddMessage(RGY_LOG_WARN, _T("hevcMp42Annexb extradata: data left behind %d bytes"), (int)(ptr_fin - ptr));
        }
    } else {
        bool vps_exist = false;
        bool sps_exist = false;
        bool pps_exist = false;
        bool got_irap = false;
        const int hevcNaluLengthSize = m_Demux.video.hevcNaluLengthSize;
        m_hevcMp42AnnexbBuffer.reserve(pkt->size + 128);
        const uint8_t *ptr = pkt->data;
        const uint8_t *ptr_fin = ptr + pkt->size;
        while (ptr + hevcNaluLengthSize < ptr_fin) {
            uint32_t size = 0;
            for (int i = 0; i < hevcNaluLengthSize; i++) {
                size = (size << 8) | (*ptr); ptr++;
            }
            const int nalu_type = ((*ptr) >> 1) & 0x3f;
            vps_exist |= nalu_type == NALU_HEVC_VPS;
            sps_exist |= nalu_type == NALU_HEVC_SPS;
            pps_exist |= nalu_type == NALU_HEVC_PPS;
            const bool header_exist = vps_exist && sps_exist && pps_exist;
            const bool is_irap = nalu_type >= 16 && nalu_type <= 23;
            // ヘッダーがすでにある場合は、extra dataをつけないようにする (header_existでチェック)
            // 1度つけていたら、もうつけない (got_irapでチェック)
            const bool add_extradata = is_irap && !got_irap && !header_exist;
            got_irap |= is_irap;

            if (add_extradata) {
                m_hevcMp42AnnexbBuffer.insert(m_hevcMp42AnnexbBuffer.end(), m_Demux.video.extradata, m_Demux.video.extradata + m_Demux.video.extradataSize);
            }
            m_hevcMp42AnnexbBuffer.insert(m_hevcMp42AnnexbBuffer.end(), SC, SC + 4);
            m_hevcMp42AnnexbBuffer.insert(m_hevcMp42AnnexbBuffer.end(), ptr, ptr + size); ptr += size;
        }
        if (pkt->buf->size < (int)m_hevcMp42AnnexbBuffer.size() + AV_INPUT_BUFFER_PADDING_SIZE) {
            av_grow_packet(pkt, (int)m_hevcMp42AnnexbBuffer.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        }
        memcpy(pkt->data, m_hevcMp42AnnexbBuffer.data(), m_hevcMp42AnnexbBuffer.size());
        memset(pkt->data + m_hevcMp42AnnexbBuffer.size(), 0, AV_INPUT_BUFFER_PADDING_SIZE);
        pkt->size = (int)m_hevcMp42AnnexbBuffer.size();
        if (ptr != ptr_fin) {
            AddMessage(RGY_LOG_WARN, _T("hevcMp42Annexb: data left behind %d bytes"), (int)(ptr_fin - ptr));
        }
    }
    m_hevcMp42AnnexbBuffer.clear();
}

RGY_ERR TSReplaceVideo::initVideoBsfs() {
    if (m_Demux.video.bsfcCtx) {
        AddMessage(RGY_LOG_DEBUG, _T("initVideoBsfs: Free old bsf...\n"));
        m_Demux.video.bsfcCtx.reset();
        AddMessage(RGY_LOG_DEBUG, _T("initVideoBsfs: Freed old bsf.\n"));
    }
    // NVEnc issue#70でm_Demux.video.bUseHEVCmp42AnnexBを使用することが効果的だあったため、採用したが、
    // NVEnc issue#389ではm_Demux.video.bUseHEVCmp42AnnexBを使用するとエラーとなることがわかった
    // さらに、#389の問題はirapがありヘッダーがない場合の処理の問題と分かった。これを修正し、再度有効に
    if (m_Demux.video.stream->codecpar->codec_id == AV_CODEC_ID_HEVC
        && m_Demux.video.hevcbsf == RGYHEVCBsf::INTERNAL) {
        m_Demux.video.bUseHEVCmp42AnnexB = true;
        AddMessage(RGY_LOG_DEBUG, _T("selected internal hevc bsf filter.\n"));
    } else if (m_Demux.video.stream->codecpar->codec_id == AV_CODEC_ID_H264 ||
        m_Demux.video.stream->codecpar->codec_id == AV_CODEC_ID_HEVC) {
        const char *filtername = nullptr;
        switch (m_Demux.video.stream->codecpar->codec_id) {
        case AV_CODEC_ID_H264: filtername = "h264_mp4toannexb"; break;
        case AV_CODEC_ID_HEVC: filtername = "hevc_mp4toannexb"; break;
        default: break;
        }
        if (filtername == nullptr) {
            AddMessage(RGY_LOG_ERROR, _T("failed to set bitstream filter.\n"));
            return RGY_ERR_NOT_FOUND;
        }
        auto filter = av_bsf_get_by_name(filtername);
        if (filter == nullptr) {
            AddMessage(RGY_LOG_ERROR, _T("failed to find %s.\n"), char_to_tstring(filtername).c_str());
            return RGY_ERR_NOT_FOUND;
        }
        AVBSFContext *ctx = nullptr;
        int ret = av_bsf_alloc(filter, &ctx);
        if (ret < 0) {
            AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory for %s: %s.\n"), char_to_tstring(filter->name).c_str(), qsv_av_err2str(ret).c_str());
            return RGY_ERR_NULL_PTR;
        }
        m_Demux.video.bsfcCtx = std::unique_ptr<AVBSFContext, RGYAVDeleter<AVBSFContext>>(ctx, RGYAVDeleter<AVBSFContext>(av_bsf_free)); ctx = nullptr;
        m_Demux.video.bsfcCtx->time_base_in = av_stream_get_codec_timebase(m_Demux.video.stream);
        if (0 > (ret = avcodec_parameters_copy(m_Demux.video.bsfcCtx->par_in, m_Demux.video.stream->codecpar))) {
            AddMessage(RGY_LOG_ERROR, _T("failed to set parameter for %s: %s.\n"), char_to_tstring(filter->name).c_str(), qsv_av_err2str(ret).c_str());
            return RGY_ERR_NULL_PTR;
        }
        m_Demux.video.bsfcCtx->time_base_in = m_Demux.video.stream->time_base;
        if (0 > (ret = av_bsf_init(m_Demux.video.bsfcCtx.get()))) {
            AddMessage(RGY_LOG_ERROR, _T("failed to init %s: %s.\n"), char_to_tstring(filter->name).c_str(), qsv_av_err2str(ret).c_str());
            return RGY_ERR_NULL_PTR;
        }
        AddMessage(RGY_LOG_DEBUG, _T("initialized %s filter.\n"), char_to_tstring(filter->name).c_str());
    }
    return RGY_ERR_NONE;
}

RGY_ERR TSReplaceVideo::initVideoParser() {
    if (m_Demux.video.parserCtx) {
        AddMessage(RGY_LOG_DEBUG, _T("initVideoParser: Close old parser...\n"));
        m_Demux.video.parserCtx.reset();
        AddMessage(RGY_LOG_DEBUG, _T("initVideoParser: Closed old parser.\n"));
    }
    if (m_Demux.video.stream->codecpar->extradata != nullptr
        && m_Demux.video.extradata == nullptr) {
        return RGY_ERR_MORE_DATA;
    }
    m_Demux.video.parserCtx = std::unique_ptr<AVCodecParserContext, decltype(&av_parser_close)>(av_parser_init(m_Demux.video.stream->codecpar->codec_id), av_parser_close);
    if (m_Demux.video.parserCtx) {
        m_Demux.video.parserCtx->flags |= PARSER_FLAG_COMPLETE_FRAMES;

        auto codec = avcodec_find_decoder(m_Demux.video.stream->codecpar->codec_id);
        if (!codec) {
            AddMessage(RGY_LOG_ERROR, _T("failed to find decoder for %s.\n"), char_to_tstring(avcodec_get_name(m_Demux.video.stream->codecpar->codec_id)).c_str());
            return RGY_ERR_NULL_PTR;
        }
        m_Demux.video.codecCtxParser = std::unique_ptr<AVCodecContext, RGYAVDeleter<AVCodecContext>>(avcodec_alloc_context3(codec), RGYAVDeleter<AVCodecContext>(avcodec_free_context));
        if (!m_Demux.video.codecCtxParser) {
            AddMessage(RGY_LOG_ERROR, _T("failed to allocate context for parser.\n"));
            return RGY_ERR_NULL_PTR;
        }
        unique_ptr_custom<AVCodecParameters> codecParamCopy(avcodec_parameters_alloc(), [](AVCodecParameters *pCodecPar) {
            avcodec_parameters_free(&pCodecPar);
            });
        int ret = 0;
        if (0 > (ret = avcodec_parameters_copy(codecParamCopy.get(), m_Demux.video.stream->codecpar))) {
            AddMessage(RGY_LOG_ERROR, _T("failed to copy codec param to context for parser: %s.\n"), qsv_av_err2str(ret).c_str());
            return RGY_ERR_UNKNOWN;
        }
        if (m_Demux.video.bsfcCtx || m_Demux.video.bUseHEVCmp42AnnexB) {
            SetExtraData(codecParamCopy.get(), m_Demux.video.extradata, m_Demux.video.extradataSize);
        }
        if (0 > (ret = avcodec_parameters_to_context(m_Demux.video.codecCtxParser.get(), codecParamCopy.get()))) {
            AddMessage(RGY_LOG_ERROR, _T("failed to set codec param to context for parser: %s.\n"), qsv_av_err2str(ret).c_str());
            return RGY_ERR_UNKNOWN;
        }
        m_Demux.video.codecCtxParser->time_base = av_stream_get_codec_timebase(m_Demux.video.stream);
        m_Demux.video.codecCtxParser->pkt_timebase = m_Demux.video.stream->time_base;
        AddMessage(RGY_LOG_DEBUG, _T("initialized %s codec context for parser: time_base: %d/%d, pkt_timebase: %d/%d.\n"),
            char_to_tstring(avcodec_get_name(m_Demux.video.stream->codecpar->codec_id)).c_str(),
            m_Demux.video.codecCtxParser->time_base.num, m_Demux.video.codecCtxParser->time_base.den,
            m_Demux.video.codecCtxParser->pkt_timebase.num, m_Demux.video.codecCtxParser->pkt_timebase.den);
    } else {
        AddMessage(RGY_LOG_ERROR, _T("failed to init parser for %s.\n"), char_to_tstring(avcodec_get_name(m_Demux.video.stream->codecpar->codec_id)).c_str());
        return RGY_ERR_NULL_PTR;
    }
    return RGY_ERR_NONE;
}

std::vector<int> TSReplaceVideo::getAVReaderStreamIndex(AVMediaType type) {
    std::vector<int> streams;
    const int n_streams = m_Demux.format.formatCtx->nb_streams;
    for (int i = 0; i < n_streams; i++) {
        const AVStream *stream = m_Demux.format.formatCtx->streams[i];
        if (type == AVMEDIA_TYPE_ATTACHMENT) {
            if (stream->codecpar->codec_type == type || (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0) {
                streams.push_back(i);
            }
        } else if (stream->codecpar->codec_type == type && (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0) {
            if (type == AVMEDIA_TYPE_VIDEO || type == AVMEDIA_TYPE_AUDIO || type == AVMEDIA_TYPE_SUBTITLE) {
                // video, audio, subtitleの場合はCodecIDが必要 (たまにCodecIDのセットされていないものが来てエラーになる)
                if (stream->codecpar->codec_id != AV_CODEC_ID_NONE) {
                    streams.push_back(i);
                }
            } else {
                streams.push_back(i);
            }
        }
    }
    if (type == AVMEDIA_TYPE_VIDEO) {
        std::sort(streams.begin(), streams.end(), [formatCtx = m_Demux.format.formatCtx.get()](int streamIdA, int streamIdB) {
            auto pStreamA = formatCtx->streams[streamIdA];
            auto pStreamB = formatCtx->streams[streamIdB];
            if (pStreamA->codecpar == nullptr) {
                return false;
            }
            if (pStreamB->codecpar == nullptr) {
                return true;
            }
            const int resA = pStreamA->codecpar->width * pStreamA->codecpar->height;
            const int resB = pStreamB->codecpar->width * pStreamB->codecpar->height;
            return (resA > resB);
            });
    }
    return streams;
}

const uint8_t *TSReplaceVideo::getExtraData(int& size) const {
    size = m_Demux.video.extradataSize;
    return m_Demux.video.extradata;
}

const AVCodecParameters *TSReplaceVideo::getVidCodecPar() const {
    if (m_Demux.video.stream) {
        return m_Demux.video.stream->codecpar;
    }
    return nullptr;
}

AVCodecID TSReplaceVideo::getVidCodecID() const {
    if (m_Demux.video.stream) {
        return m_Demux.video.stream->codecpar->codec_id;
    }
    return AV_CODEC_ID_NONE;
}

RGYTSStreamType TSReplaceVideo::getVideoStreamType() const {
    if (m_Demux.video.stream) {
        switch (m_Demux.video.stream->codecpar->codec_id) {
        case AV_CODEC_ID_H264:
            return RGYTSStreamType::H264_VIDEO;
        case AV_CODEC_ID_HEVC:
            return RGYTSStreamType::H265_VIDEO;
        default:
            return RGYTSStreamType::UNKNOWN;
        }
    }
    return RGYTSStreamType::UNKNOWN;
}

RGY_ERR TSReplaceVideo::initAVReader(const tstring& videofile, RGYQueueBuffer *inputQueue, const tstring& inputFormat) {
    if (!check_avcodec_dll()) {
        AddMessage(RGY_LOG_ERROR, error_mes_avcodec_dll_not_found());
        return RGY_ERR_NULL_PTR;
    }

    initAVDevices();

    av_log_set_level((m_log->getLogLevel(RGY_LOGT_IN) == RGY_LOG_DEBUG) ? AV_LOG_DEBUG : RGY_AV_LOG_LEVEL);
    av_qsv_log_set(m_log);

    m_filename = videofile;
    m_inputQueue = inputQueue;
    std::string filename_char;
    if (videofile.length() > 0) {
        if (0 == tchar_to_string(videofile.c_str(), filename_char, CP_UTF8)) {
            AddMessage(RGY_LOG_ERROR, _T("failed to convert filename to utf-8 characters.\n"));
            return RGY_ERR_UNSUPPORTED;
        }

        if (0 == strcmp(filename_char.c_str(), "-")) {
#if defined(_WIN32) || defined(_WIN64)
            if (_setmode(_fileno(stdin), _O_BINARY) < 0) {
                AddMessage(RGY_LOG_ERROR, _T("failed to switch stdin to binary mode.\n"));
                return RGY_ERR_UNDEFINED_BEHAVIOR;
            }
#endif //#if defined(_WIN32) || defined(_WIN64)
            AddMessage(RGY_LOG_DEBUG, _T("input source set to stdin.\n"));
            filename_char = "pipe:0";
        }
    }

    decltype(av_find_input_format(nullptr)) inFormat = nullptr;
    if (inputFormat.length() > 0) {
        if (nullptr == (inFormat = av_find_input_format(tchar_to_string(inputFormat).c_str()))) {
            AddMessage(RGY_LOG_ERROR, _T("Unknown Input format: %s.\n"), inputFormat.c_str());
            return RGY_ERR_INVALID_FORMAT;
        }
    }

    //ts向けの設定
    bool scan_all_pmts_set = false;
    if (!av_dict_get(m_Demux.format.formatOptions, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {
        av_dict_set(&m_Demux.format.formatOptions, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
        scan_all_pmts_set = true;
    }
    //ファイルのオープン
    int ret = 0;
    AVFormatContext *format_ctx = avformat_alloc_context();
    if (m_inputQueue) {
        int bufferSize = 128 * 1024;
        m_Demux.format.inputBuffer = (uint8_t *)av_malloc(bufferSize);
        if (NULL == (format_ctx->pb = avio_alloc_context((unsigned char *)m_Demux.format.inputBuffer, bufferSize, 0, this, &funcReadPacket, nullptr, nullptr))) {
            AddMessage(RGY_LOG_ERROR, _T("failed to alloc avio context.\n"));
            return RGY_ERR_NULL_PTR;
        }
    }
    if ((ret = avformat_open_input(&format_ctx, m_inputQueue ? nullptr : filename_char.c_str(), inFormat, &m_Demux.format.formatOptions)) != 0) {
        AddMessage(RGY_LOG_ERROR, _T("error opening file \"%s\": %s\n"), char_to_tstring(filename_char, CP_UTF8).c_str(), qsv_av_err2str(ret).c_str());
        avformat_free_context(format_ctx);
        return RGY_ERR_FILE_OPEN; // Couldn't open file
    }
    m_Demux.format.formatCtx = std::unique_ptr<AVFormatContext, decltype(&avformat_free_context)>(format_ctx, avformat_free_context); format_ctx = nullptr;
    if (m_inputQueue) {
        AddMessage(RGY_LOG_DEBUG, _T("opened encode queue.\n"));
    } else {
        AddMessage(RGY_LOG_DEBUG, _T("opened file \"%s\".\n"), char_to_tstring(filename_char, CP_UTF8).c_str());
    }

    //不正なオプションを渡していないかチェック
    for (const AVDictionaryEntry *t = NULL; NULL != (t = av_dict_get(m_Demux.format.formatOptions, "", t, AV_DICT_IGNORE_SUFFIX));) {
        if (strcmp(t->key, "scan_all_pmts") != 0) {
            AddMessage(RGY_LOG_WARN, _T("Unknown input option: %s=%s, ignored.\n"),
                char_to_tstring(t->key).c_str(),
                char_to_tstring(t->value).c_str());
        }
    }

    m_Demux.format.formatCtx->flags |= AVFMT_FLAG_NONBLOCK; // ffmpeg_opt.cのopen_input_file()と同様にフラグを立てる
    if (avformat_find_stream_info(m_Demux.format.formatCtx.get(), nullptr) < 0) {
        AddMessage(RGY_LOG_ERROR, _T("error finding stream information.\n"));
        return RGY_ERR_UNKNOWN; // Couldn't find stream information
    }
    AddMessage(RGY_LOG_DEBUG, _T("got stream information.\n"));
    av_dump_format(m_Demux.format.formatCtx.get(), 0, filename_char.c_str(), 0);
    //dump_format(dec.m_Demux.format.formatCtx, 0, argv[1], 0);


    //動画ストリームを探す
    //動画ストリームは動画を処理しなかったとしても同期のため必要
    auto videoStreams = getAVReaderStreamIndex(AVMEDIA_TYPE_VIDEO);
    if (videoStreams.size() == 0) {
        AddMessage(RGY_LOG_ERROR, _T("error finding video stream.\n"));
        return RGY_ERR_INVALID_DATA_TYPE;
    }

    m_Demux.video.index = videoStreams.front();
    m_Demux.video.stream = m_Demux.format.formatCtx->streams[m_Demux.video.index];
    AddMessage(RGY_LOG_INFO, _T("Opened video stream #%d, %s, %dx%d (%s), stream time_base %d/%d, codec_timebase %d/%d.\n"),
        m_Demux.video.stream->index,
        char_to_tstring(avcodec_get_name(m_Demux.video.stream->codecpar->codec_id)).c_str(),
        m_Demux.video.stream->codecpar->width, m_Demux.video.stream->codecpar->height,
        char_to_tstring(av_pix_fmt_desc_get((AVPixelFormat)m_Demux.video.stream->codecpar->format)->name).c_str(),
        m_Demux.video.stream->time_base.num, m_Demux.video.stream->time_base.den,
        av_stream_get_codec_timebase(m_Demux.video.stream).num, av_stream_get_codec_timebase(m_Demux.video.stream).den);

    //必要ならbitstream filterを初期化
    if (m_Demux.video.stream->codecpar->extradata && m_Demux.video.stream->codecpar->extradata[0] == 1) {
        RGY_ERR sts = initVideoBsfs();
        if (sts != RGY_ERR_NONE) {
            AddMessage(RGY_LOG_ERROR, _T("failed to init bsfs.\n"));
            return sts;
        }
    }

    //ヘッダーの取得を確認する
    RGY_ERR sts = GetHeader();
    if (sts != RGY_ERR_NONE) {
        AddMessage(RGY_LOG_ERROR, _T("failed to get header: extradata size = %d.\n"), m_Demux.video.stream->codecpar->extradata_size);
        return sts;
    }

    sts = initVideoParser();
    if (sts != RGY_ERR_NONE) {
        AddMessage(RGY_LOG_ERROR, _T("failed to init parser.\n"));
        return sts;
    }

    m_firstPTSFrame = TIMESTAMP_INVALID_VALUE;
    m_firstPTSVideoAudioStreams = TIMESTAMP_INVALID_VALUE;

    return RGY_ERR_NONE;
}

RGY_ERR TSReplaceVideo::GetHeader() {

    if (m_Demux.video.extradata == nullptr) {
        if (m_Demux.video.stream->codecpar->extradata == nullptr || m_Demux.video.stream->codecpar->extradata_size == 0) {
            return RGY_ERR_NONE;
        }
        m_Demux.video.extradataSize = m_Demux.video.stream->codecpar->extradata_size;
        //ここでav_mallocを使用しないと正常に動作しない
        m_Demux.video.extradata = (uint8_t *)av_malloc(m_Demux.video.stream->codecpar->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        //ヘッダのデータをコピーしておく
        memcpy(m_Demux.video.extradata, m_Demux.video.stream->codecpar->extradata, m_Demux.video.extradataSize);
        memset(m_Demux.video.extradata + m_Demux.video.extradataSize, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        if (m_log != nullptr && RGY_LOG_DEBUG >= m_log->getLogLevel(RGY_LOGT_IN)) {
            tstring header_str;
            for (int i = 0; i < m_Demux.video.extradataSize; i++) {
                header_str += strsprintf(_T("%02x "), m_Demux.video.extradata[i]);
            }
            AddMessage(RGY_LOG_DEBUG, _T("GetHeader extradata(%d): %s\n"), m_Demux.video.extradataSize, header_str.c_str());
        }

        if (m_Demux.video.bUseHEVCmp42AnnexB) {
            hevcMp42Annexb(nullptr);
        } else if (m_Demux.video.bsfcCtx && m_Demux.video.extradata[0] == 1) {
            if (m_Demux.video.extradataSize < m_Demux.video.bsfcCtx->par_out->extradata_size) {
                m_Demux.video.extradata = (uint8_t *)av_realloc(m_Demux.video.extradata, m_Demux.video.bsfcCtx->par_out->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
            }
            memcpy(m_Demux.video.extradata, m_Demux.video.bsfcCtx->par_out->extradata, m_Demux.video.bsfcCtx->par_out->extradata_size);
            AddMessage(RGY_LOG_DEBUG, _T("GetHeader: changed %d bytes -> %d bytes by %s.\n"),
                m_Demux.video.extradataSize, m_Demux.video.bsfcCtx->par_out->extradata_size,
                char_to_tstring(m_Demux.video.bsfcCtx->filter->name).c_str());
            m_Demux.video.extradataSize = m_Demux.video.bsfcCtx->par_out->extradata_size;
            memset(m_Demux.video.extradata + m_Demux.video.extradataSize, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        } else if (m_Demux.video.stream->codecpar->codec_id == AV_CODEC_ID_VC1) {
            //int lengthFix = (0 == strcmp(m_Demux.format.formatCtx->iformat->name, "mpegts")) ? 0 : -1;
            //vc1FixHeader(lengthFix);
        }
        AddMessage(RGY_LOG_DEBUG, _T("GetHeader: %d bytes.\n"), m_Demux.video.extradataSize);
        if (m_Demux.video.extradataSize == 0 && m_Demux.video.extradata != nullptr) {
            av_free(m_Demux.video.extradata);
            m_Demux.video.extradata = nullptr;
            AddMessage(RGY_LOG_DEBUG, _T("Failed to get header: 0 byte."));
            return RGY_ERR_MORE_DATA;
        }
    }
    if (m_Demux.video.stream->codecpar->codec_id == AV_CODEC_ID_AV1
        && m_Demux.video.extradataSize > 0
        && m_Demux.video.firstPkt) {
        const int max_check_len = std::min(8, m_Demux.video.extradataSize - 8);
        if (m_Demux.video.firstPkt->size > m_Demux.video.extradataSize - max_check_len) {
            //mp4に入っているAV1等の場合、先頭に余計なbyteがあることがあるので、最初のパケットと照らし合わせて不要なら取り除く
            for (int i = 1; i <= max_check_len; i++) {
                if (m_Demux.video.extradataSize - i < m_Demux.video.firstPkt->size) {
                    if (memcmp(m_Demux.video.extradata + i, m_Demux.video.firstPkt->data, m_Demux.video.extradataSize - i) == 0) {
                        const int remove_bytes = i;
                        AddMessage(RGY_LOG_DEBUG, _T("GetHeader remove bytes: %d (size: %d -> %d)\n"), remove_bytes, m_Demux.video.extradataSize, m_Demux.video.extradataSize - remove_bytes);
                        m_Demux.video.extradataSize -= remove_bytes;
                        memmove(m_Demux.video.extradata, m_Demux.video.extradata + remove_bytes, m_Demux.video.extradataSize);
                        break;
                    }
                }
            }
        }
    }
    if (m_Demux.video.extradataSize && m_log != nullptr && RGY_LOG_DEBUG >= m_log->getLogLevel(RGY_LOGT_IN)) {
        tstring header_str;
        for (int i = 0; i < m_Demux.video.extradataSize; i++) {
            header_str += strsprintf(_T("%02x "), m_Demux.video.extradata[i]);
        }
        AddMessage(RGY_LOG_DEBUG, _T("GetHeader(%d): %s\n"), m_Demux.video.extradataSize, header_str.c_str());
    }
    return RGY_ERR_NONE;
}

void TSReplaceVideo::SetExtraData(AVCodecParameters *codecParam, const uint8_t *data, uint32_t size) {
    if (data == nullptr || size == 0)
        return;
    if (codecParam->extradata)
        av_free(codecParam->extradata);
    codecParam->extradata_size = size;
    codecParam->extradata = (uint8_t *)av_malloc(codecParam->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(codecParam->extradata, data, size);
    memset(codecParam->extradata + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
};

RGY_ERR TSReplaceVideo::initDecoder() {
    //codec側に格納されている値は、streamからは取得できない
    m_Demux.video.codecDecode = avcodec_find_decoder(m_Demux.video.stream->codecpar->codec_id);
    if (m_Demux.video.codecDecode == nullptr) {
        AddMessage(RGY_LOG_ERROR, errorMesForCodec(_T("Failed to find decoder"), m_Demux.video.stream->codecpar->codec_id).c_str());
        return RGY_ERR_NOT_FOUND;
    }
    AddMessage(RGY_LOG_DEBUG, _T("found decoder for %s.\n"), char_to_tstring(avcodec_get_name(m_Demux.video.stream->codecpar->codec_id)).c_str());

    m_Demux.video.codecCtxDecode = std::unique_ptr<AVCodecContext, RGYAVDeleter<AVCodecContext>>(avcodec_alloc_context3(m_Demux.video.codecDecode), RGYAVDeleter<AVCodecContext>(avcodec_free_context));
    if (!m_Demux.video.codecCtxDecode) {
        AddMessage(RGY_LOG_ERROR, errorMesForCodec(_T("Failed to allocate decoder"), m_Demux.video.stream->codecpar->codec_id).c_str());
        return RGY_ERR_NULL_PTR;
    }
    AddMessage(RGY_LOG_DEBUG, _T("allocated decoder.\n"));

    unique_ptr_custom<AVCodecParameters> codecParamCopy(avcodec_parameters_alloc(), [](AVCodecParameters *pCodecPar) {
        avcodec_parameters_free(&pCodecPar);
        });
    auto ret = avcodec_parameters_copy(codecParamCopy.get(), m_Demux.video.stream->codecpar);
    if (ret < 0) {
        AddMessage(RGY_LOG_ERROR, _T("failed to copy codec param to context for parser: %s.\n"), qsv_av_err2str(ret).c_str());
        return RGY_ERR_UNKNOWN;
    }
    if (m_Demux.video.bsfcCtx || m_Demux.video.bUseHEVCmp42AnnexB) {
        SetExtraData(codecParamCopy.get(), m_Demux.video.extradata, m_Demux.video.extradataSize);
    }
    if (0 > (ret = avcodec_parameters_to_context(m_Demux.video.codecCtxDecode.get(), codecParamCopy.get()))) {
        AddMessage(RGY_LOG_ERROR, _T("failed to set codec param to context for decoder: %s.\n"), qsv_av_err2str(ret).c_str());
        return RGY_ERR_UNKNOWN;
    }

    m_Demux.video.codecCtxDecode->time_base = av_stream_get_codec_timebase(m_Demux.video.stream);
    m_Demux.video.codecCtxDecode->pkt_timebase = m_Demux.video.stream->time_base;
    if (0 > (ret = avcodec_open2(m_Demux.video.codecCtxDecode.get(), m_Demux.video.codecDecode, nullptr))) {
        AddMessage(RGY_LOG_ERROR, _T("Failed to open decoder for %s: %s\n"), char_to_tstring(avcodec_get_name(m_Demux.video.stream->codecpar->codec_id)).c_str(), qsv_av_err2str(ret).c_str());
        return RGY_ERR_UNSUPPORTED;
    }
    AddMessage(RGY_LOG_DEBUG, _T("opened decoder.\n"));
    return RGY_ERR_NONE;
}

RGY_ERR TSReplaceVideo::getFirstPts(int64_t& firstPTSVideoAudioStreams, int64_t& firstPTSFrame, int64_t& firstPTSKeyFrame) {
    firstPTSVideoAudioStreams = TIMESTAMP_INVALID_VALUE;
    firstPTSFrame = TIMESTAMP_INVALID_VALUE;
    firstPTSKeyFrame = TIMESTAMP_INVALID_VALUE;
    auto [ret, avpkt] = getSample();
    if (ret == 0) {
        firstPTSVideoAudioStreams = m_firstPTSVideoAudioStreams;
        firstPTSFrame = m_firstPTSFrame;
        firstPTSKeyFrame = m_Demux.video.streamFirstKeyPts;
        return RGY_ERR_NONE;
    }
    return RGY_ERR_UNKNOWN;
}

#if 0
RGY_ERR TSReplaceVideo::getFirstDecodedPts(int64_t& firstPts, const TSRReplaceStartPoint startpoint) {

    switch (startpoint) {
    case TSRReplaceStartPoint::KeyframPts:
        m_Demux.video.getPktBeforeKey = false;
        break;
    case TSRReplaceStartPoint::LibavDecodeAll:
    case TSRReplaceStartPoint::LibavDecodePts:
    case TSRReplaceStartPoint::FirstPts:
    default:
        m_Demux.video.getPktBeforeKey = true;
        break;
    }

    auto pkt = m_poolPkt->getFree();
    pkt.reset();

    std::unique_ptr<AVFrame, RGYAVDeleter<AVFrame>> frame(av_frame_alloc(), RGYAVDeleter<AVFrame>(av_frame_free));
    int got_frame = 0;
    while (!got_frame) {
        if (!pkt) {
            auto [ret, avpkt] = getSample();
            if (ret == 0) {
                pkt = std::move(avpkt);
            } else if (ret != AVERROR_EOF) {
                return RGY_ERR_UNKNOWN;
            }
        }
        int ret = avcodec_send_packet(m_Demux.video.codecCtxDecode.get(), pkt.get());
        //AVERROR(EAGAIN) -> パケットを送る前に受け取る必要がある
        //パケットが受け取られていないのでpopしない
        if (ret != AVERROR(EAGAIN)) {
            pkt.reset();
        }
        if (ret == AVERROR_EOF) { //これ以上パケットを送れない
            AddMessage(RGY_LOG_DEBUG, _T("failed to send packet to video decoder, already flushed: %s.\n"), qsv_av_err2str(ret).c_str());
        } else if (ret < 0 && ret != AVERROR(EAGAIN)) {
            AddMessage(RGY_LOG_ERROR, _T("failed to send packet to video decoder: %s.\n"), qsv_av_err2str(ret).c_str());
            return RGY_ERR_UNDEFINED_BEHAVIOR;
        }
        ret = avcodec_receive_frame(m_Demux.video.codecCtxDecode.get(), frame.get());
        if (ret == AVERROR(EAGAIN)) { //もっとパケットを送る必要がある
            continue;
        }
        if (ret == AVERROR_EOF) {
            //最後まで読み込んだ
            return RGY_ERR_MORE_DATA;
        }
        if (ret < 0) {
            AddMessage(RGY_LOG_ERROR, _T("failed to receive frame from video decoder: %s.\n"), qsv_av_err2str(ret).c_str());
            return RGY_ERR_UNDEFINED_BEHAVIOR;
        }
        got_frame = TRUE;
        firstPts = frame->pts;
    }
    return RGY_ERR_NONE;
}
#endif

std::tuple<int, std::unique_ptr<AVPacket, RGYAVDeleter<AVPacket>>> TSReplaceVideo::getSample() {
    int i_samples = 0;
    int ret_read_frame = 0;
    auto pkt = m_poolPkt->getFree();
    for (; ((ret_read_frame = av_read_frame(m_Demux.format.formatCtx.get(), pkt.get())) >= 0 || (ret_read_frame == AVERROR(EAGAIN))); // camera等で、av_read_frameがAVERROR(EAGAIN)を返す場合がある
        pkt = m_poolPkt->getFree()) {
        if (ret_read_frame == AVERROR(EAGAIN)) { // camera等で、av_read_frameがAVERROR(EAGAIN)を返す場合がある
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        const auto stream_media_type = m_Demux.format.formatCtx->streams[m_Demux.video.index]->codecpar->codec_type;
        if (m_firstPTSVideoAudioStreams == TIMESTAMP_INVALID_VALUE
            && pkt->pts != AV_NOPTS_VALUE
            && (stream_media_type == AVMEDIA_TYPE_VIDEO || stream_media_type == AVMEDIA_TYPE_AUDIO)) {
            m_firstPTSVideoAudioStreams = pkt->pts;
        }
        if (pkt->stream_index == m_Demux.video.index) {
            if (m_firstPTSFrame == TIMESTAMP_INVALID_VALUE) {
                m_firstPTSFrame = pkt->pts;
            }
            if (pkt->flags & AV_PKT_FLAG_CORRUPT) {
                const auto timestamp = (pkt->pts == AV_NOPTS_VALUE) ? pkt->dts : pkt->pts;
                AddMessage(RGY_LOG_WARN, _T("corrupt packet in video: %lld (%s)\n"), (long long int)timestamp, getTimestampString(timestamp, m_Demux.video.stream->time_base).c_str());
            }
            if (m_Demux.video.bsfcCtx) {
                auto ret = av_bsf_send_packet(m_Demux.video.bsfcCtx.get(), pkt.get());
                if (ret < 0) {
                    pkt.reset();
                    AddMessage(RGY_LOG_ERROR, _T("failed to send packet to %s bitstream filter: %s.\n"), char_to_tstring(m_Demux.video.bsfcCtx->filter->name).c_str(), qsv_av_err2str(ret).c_str());
                    return { 1, nullptr };
                }
                ret = av_bsf_receive_packet(m_Demux.video.bsfcCtx.get(), pkt.get());
                if (ret == AVERROR(EAGAIN)) {
                    continue; //もっとpacketを送らないとダメ
                } else if (ret < 0 && ret != AVERROR_EOF) {
                    AddMessage(RGY_LOG_ERROR, _T("failed to run %s bitstream filter: %s.\n"), char_to_tstring(m_Demux.video.bsfcCtx->filter->name).c_str(), qsv_av_err2str(ret).c_str());
                    pkt.reset();
                    return { 1, nullptr };
                }
            }
            if (m_Demux.video.bUseHEVCmp42AnnexB) {
                hevcMp42Annexb(pkt.get());
            }

            if (m_Demux.video.parserCtx) {
                uint8_t* dummy = nullptr;
                int dummy_size = 0;
                av_parser_parse2(m_Demux.video.parserCtx.get(), m_Demux.video.codecCtxParser.get(), &dummy, &dummy_size, pkt->data, pkt->size, pkt->pts, pkt->dts, pkt->pos);
                const int pict_type = (uint8_t)(std::max)(m_Demux.video.parserCtx->pict_type, 0);
                switch (pict_type) {
                case AV_PICTURE_TYPE_I: pkt->flags |= RGY_FLAG_PICT_TYPE_I | AV_PKT_FLAG_KEY; break;
                case AV_PICTURE_TYPE_P: pkt->flags |= RGY_FLAG_PICT_TYPE_P; break;
                case AV_PICTURE_TYPE_B: pkt->flags |= RGY_FLAG_PICT_TYPE_B; break;
                default: break;
                }
                switch (m_Demux.video.parserCtx->picture_structure) {
                    //フィールドとして符号化されている
                case AV_PICTURE_STRUCTURE_TOP_FIELD:    pkt->flags |= RGY_FLAG_PICSTRUCT_FIELD | RGY_FLAG_PICSTRUCT_TFF; break;
                case AV_PICTURE_STRUCTURE_BOTTOM_FIELD: pkt->flags |= RGY_FLAG_PICSTRUCT_FIELD | RGY_FLAG_PICSTRUCT_BFF; break;
                    //フレームとして符号化されている
                default:
                    switch (m_Demux.video.parserCtx->field_order) {
                    case AV_FIELD_TT:
                    case AV_FIELD_TB: pkt->flags |= RGY_FLAG_PICSTRUCT_TFF; break;
                    case AV_FIELD_BT:
                    case AV_FIELD_BB: pkt->flags |= RGY_FLAG_PICSTRUCT_BFF; break;
                    default: break;
                    }
                }
                if ((uint8_t)m_Demux.video.parserCtx->repeat_pict) {
                    pkt->flags |= RGY_FLAG_PICSTRUCT_RFF;
                }
            }

            const bool keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
            if (!m_Demux.video.gotFirstKeyframe && !keyframe) {
                i_samples++;
                av_packet_unref(pkt.get());
                continue;
            } else if (!m_Demux.video.gotFirstKeyframe) {
                if (pkt->flags & AV_PKT_FLAG_DISCARD) {
                    //timestampが正常に設定されておらず、移乗動作の原因となるので、
                    //AV_PKT_FLAG_DISCARDがついている最初のフレームは無視する
                    continue;
                }
                //ここに入った場合は、必ず最初のキーフレーム
                m_Demux.video.streamFirstKeyPts = (pkt->pts == AV_NOPTS_VALUE) ? pkt->dts : pkt->pts;
                if (m_Demux.video.streamFirstKeyPts == AV_NOPTS_VALUE) {
                    AddMessage(RGY_LOG_WARN, _T("first key frame had timestamp AV_NOPTS_VALUE, this might lead to avsync error.\n"));
                    m_Demux.video.streamFirstKeyPts = 0;
                }
                m_Demux.video.firstPkt = av_packet_clone(pkt.get());
                m_Demux.video.gotFirstKeyframe = true;
                m_Demux.video.keyFrameOffset = i_samples;
                AddMessage(RGY_LOG_DEBUG, _T("found first key frame: timestamp %lld (%s), offset %d\n"),
                    (long long int)m_Demux.video.streamFirstKeyPts, getTimestampString(m_Demux.video.streamFirstKeyPts, m_Demux.video.stream->time_base).c_str(), i_samples);
            }
            return { 0, std::move(pkt) };
        } else {
            pkt.reset();
        }
    }
    if (ret_read_frame != AVERROR_EOF && ret_read_frame < 0) {
        AddMessage(RGY_LOG_ERROR, _T("error while reading file, %s\n"), qsv_av_err2str(ret_read_frame).c_str());
        return { 1, nullptr };
    }
    return { AVERROR_EOF, nullptr };
}

RGY_ERR TSReplaceVideo::fillPackets() {
    {
        auto [err, pkt] = getSample();
        if (err != RGY_ERR_NONE) {
            return (err == AVERROR_EOF) ? RGY_ERR_MORE_DATA : RGY_ERR_UNKNOWN;
        }
        m_packets.push_back(std::move(pkt));
    }

    auto prev_packet = m_packets.back().get();

    //PAFFエンコードで2フレーム連続でフィールドがある場合、
    //2フィールド目にpts/dtsが設定されていない場合がある
    //その場合は、1フィールド目に2フィールド目のデータを連結する
    if (m_Demux.video.stream->codecpar->codec_id == AV_CODEC_ID_H264
        && (prev_packet->flags & RGY_FLAG_PICSTRUCT_FIELD) != 0
        && (prev_packet->flags & (RGY_FLAG_PICSTRUCT_TFF | RGY_FLAG_PICSTRUCT_BFF)) != 0) { // インタレ保持
        auto [err2, pkt2] = getSample();
        if (err2 == 0) {
            if (   (pkt2->flags & RGY_FLAG_PICSTRUCT_FIELD) != 0 // フィールド単位である
                && (pkt2->flags & (RGY_FLAG_PICSTRUCT_TFF | RGY_FLAG_PICSTRUCT_BFF)) != 0 // インタレ保持
                && (pkt2->pts == AV_NOPTS_VALUE || pkt2->dts == AV_NOPTS_VALUE)) {
                const auto orig_pkt_size = prev_packet->size;
                av_grow_packet(prev_packet, pkt2->size);
                memcpy(prev_packet->data + orig_pkt_size, pkt2->data, pkt2->size);
            } else {
                m_packets.push_back(std::move(pkt2));
            }
        }
    }
    return RGY_ERR_NONE;
}

std::tuple<RGY_ERR, int64_t, int64_t> TSReplaceVideo::getFrontPktPtsDts() {
    if (m_packets.empty()) {
        auto err = fillPackets();
        if (err != RGY_ERR_NONE) {
            return { err, AV_NOPTS_VALUE, AV_NOPTS_VALUE };
        }
    }
    auto& fronpkt = m_packets.front();
    return { RGY_ERR_NONE, fronpkt->pts, fronpkt->dts };
}

std::tuple<RGY_ERR, std::unique_ptr<AVPacket, RGYAVDeleter<AVPacket>>> TSReplaceVideo::getFrontPktAndPop() {
    if (m_packets.empty()) {
        auto err = fillPackets();
        if (err != RGY_ERR_NONE) {
            return { err, nullptr };
        }
    }
    auto pkt = std::move(m_packets.front());
    m_packets.pop_front();
    return { RGY_ERR_NONE, std::move(pkt) };
}

AVRational TSReplaceVideo::getVidTimebase() const {
    if (m_Demux.video.stream) {
        return m_Demux.video.stream->time_base;
    }
    return av_make_q(0, 1);
}

int64_t TSReplaceVideo::getFirstKeyPts() const {
    return m_Demux.video.streamFirstKeyPts;
}

int TSReplaceVideo::readPacket(uint8_t *buf, int buf_size) {
    auto ret = m_inputQueue->popDataBlock(buf, buf_size);
    return (ret < 0) ? AVERROR_EOF : (int)ret;
}
int TSReplaceVideo::writePacket(uint8_t *buf, int buf_size) {
    return -1;
}
int64_t TSReplaceVideo::seek(int64_t offset, int whence) {
    return -1;
}

TSReplace::TSReplace() :
    m_log(),
    m_demuxer(),
    m_tsReadUpdate(std::chrono::system_clock::now()),
    m_fileTSBufSize(16 * 1024),
    m_fileTS(),
    m_fileOut(),
    m_encoderPath(),
    m_encoderArgs(),
    m_tsPktSplitter(),
    m_fpTSIn(),
    m_fpTSOut(),
    m_inputAbort(false),
    m_threadInputTS(),
    m_threadSendEncoder(),
    m_queueInputReplace(),
    m_queueInputEncoder(),
    m_queueInputPreAnalysis(),
    m_bufferTS(),
    m_preAnalysisFin(false),
    m_vidPIDReplace(0x0100),
    m_vidDTSOutMax(TIMESTAMP_INVALID_VALUE),
    m_vidPTS(TIMESTAMP_INVALID_VALUE),
    m_vidDTS(TIMESTAMP_INVALID_VALUE),
    m_vidFirstFramePTS(TIMESTAMP_INVALID_VALUE),
    m_vidFirstFrameDTS(TIMESTAMP_INVALID_VALUE),
    m_vidFirstKeyPTS(TIMESTAMP_INVALID_VALUE),
    m_startPoint(TSRReplaceStartPoint::KeyframPts),
    m_vidFirstTimestamp(TIMESTAMP_INVALID_VALUE),
    m_vidFirstPacketPTS(TIMESTAMP_INVALID_VALUE),
    m_lastPmt(),
    m_videoReplace(),
    m_pmtCounter(0),
    m_vidCounter(0),
    m_ptswrapOffset(0),
    m_addAud(true),
    m_addHeaders(true),
    m_parseNalH264(get_parse_nal_unit_h264_func()),
    m_parseNalHevc(get_parse_nal_unit_hevc_func()),
    m_encoder(),
    m_encPipe(),
    m_encThreadOut(),
    m_encThreadErr(),
    m_encQueueOut() {

}
TSReplace::~TSReplace() {
    close();
}

void TSReplace::close() {
    m_inputAbort = true;
    //エンコーダの終了
    if (m_encoder) {
        m_encoder->close();
    }
    if (m_encThreadOut.joinable()) {
        m_encThreadOut.join();
    }
    if (m_encThreadErr.joinable()) {
        m_encThreadErr.join();
    }
    m_encQueueOut.reset();

    if (m_threadSendEncoder) {
        if (m_threadSendEncoder->joinable()) {
            m_threadSendEncoder->join();
        }
        m_threadSendEncoder.reset();
    }
    m_queueInputEncoder.reset();
    m_videoReplace.reset();
    if (m_threadInputTS) {
        if (m_threadInputTS->joinable()) {
            m_threadInputTS->join();
        }
        m_threadInputTS.reset();
    }
    m_queueInputReplace.reset();
}

RGY_ERR TSReplace::init(std::shared_ptr<RGYLog> log, const TSRReplaceParams& prms) {
    m_log = log;
    m_fileTS = prms.input;
    m_fileOut = prms.output;
    m_startPoint = prms.startpoint;
    m_addAud = prms.addAud;
    m_addHeaders = prms.addHeaders;

    AddMessage(RGY_LOG_INFO, _T("Output  file: \"%s\".\n"), prms.output.c_str());
    AddMessage(RGY_LOG_INFO, _T("Input   file: \"%s\".\n"), prms.input.c_str());
    if (prms.replacefile.length() > 0) {
        AddMessage(RGY_LOG_INFO, _T("Replace file: \"%s\"%s.\n"), prms.replacefile.c_str(), (prms.replacefileformat.length() > 0) ? strsprintf(_T(" (%s)"), prms.replacefileformat.c_str()).c_str() : _T(""));
    } else {
        AddMessage(RGY_LOG_INFO, _T("Encoder     : \"%s\"\n"), prms.encoderPath.c_str());
        tstring str;
        for (auto& arg : prms.encoderArgs) {
            str += _T(" ") + arg;
        }
        AddMessage(RGY_LOG_INFO, _T("Encoder Args:%s\n"), str.c_str());
    }
    AddMessage(RGY_LOG_INFO, _T("Start point : %s.\n"), get_cx_desc(list_startpoint, (int)prms.startpoint));
    AddMessage(RGY_LOG_INFO, _T("Add AUD     : %s.\n"), m_addAud ? _T("on") : _T("off"));
    AddMessage(RGY_LOG_INFO, _T("Add Headers : %s.\n"), m_addHeaders ? _T("on") : _T("off"));

    if (_tcscmp(m_fileTS.c_str(), _T("-")) != 0) {
        AddMessage(RGY_LOG_DEBUG, _T("Open input file \"%s\".\n"), m_fileTS.c_str());
        FILE *fptmp = nullptr;
        if (_tfopen_s(&fptmp, m_fileTS.c_str(), _T("rb")) == 0 && fptmp != nullptr) {
            m_fpTSIn = std::unique_ptr<FILE, fp_deleter>(fptmp, fp_deleter());
            m_fileTSBufSize = 1 * 1024 * 1024;
        } else {
            AddMessage(RGY_LOG_ERROR, _T("Failed to open input file \"%s\".\n"), m_fileTS.c_str());
            return RGY_ERR_FILE_OPEN;
        }
    } else {
        AddMessage(RGY_LOG_DEBUG, _T("Open input file stdin.\n"));
        m_fpTSIn = std::unique_ptr<FILE, fp_deleter>(stdin, fp_deleter());
#if defined(_WIN32) || defined(_WIN64)
        if (_setmode(_fileno(stdin), _O_BINARY) < 0) {
            AddMessage(RGY_LOG_ERROR, _T("failed to switch stdin to binary mode.\n"));
            return RGY_ERR_UNDEFINED_BEHAVIOR;
        }
#endif //#if defined(_WIN32) || defined(_WIN64)
    }

    if (_tcscmp(m_fileOut.c_str(), _T("-")) != 0) {
        AddMessage(RGY_LOG_DEBUG, _T("Open output file \"%s\".\n"), m_fileOut.c_str());
        FILE *fptmp = nullptr;
        if (_tfopen_s(&fptmp, m_fileOut.c_str(), _T("wb")) == 0 && fptmp != nullptr) {
            m_fpTSOut = std::unique_ptr<FILE, fp_deleter>(fptmp, fp_deleter());
        } else {
            AddMessage(RGY_LOG_ERROR, _T("Failed to open output file \"%s\".\n"), m_fileOut.c_str());
            return RGY_ERR_FILE_OPEN;
        }
    } else {
        AddMessage(RGY_LOG_DEBUG, _T("Open output file stdout.\n"));
        m_fpTSOut = std::unique_ptr<FILE, fp_deleter>(stdout, fp_deleter());
#if defined(_WIN32) || defined(_WIN64)
        if (_setmode(_fileno(stdout), _O_BINARY) < 0) {
            AddMessage(RGY_LOG_ERROR, _T("failed to switch stdout to binary mode.\n"));
            return RGY_ERR_UNDEFINED_BEHAVIOR;
        }
#endif //#if defined(_WIN32) || defined(_WIN64)
    }

    m_bufferTS.resize(m_fileTSBufSize);

    m_tsPktSplitter = std::make_unique<RGYTSPacketSplitter>();
    m_tsPktSplitter->init(log);

    m_demuxer = std::make_unique<RGYTSDemuxer>();
    m_demuxer->init(log);

    m_replaceFileFormat = prms.replacefileformat;
    if (prms.replacefile.length() > 0) {
        m_videoReplace = std::make_unique<TSReplaceVideo>(log);
        if (auto sts = m_videoReplace->initAVReader(prms.replacefile, nullptr, prms.replacefileformat); sts != RGY_ERR_NONE) {
            return sts;
        }

        const auto streamID = m_videoReplace->getVideoStreamType();
        if (streamID == RGYTSStreamType::UNKNOWN) {
            AddMessage(RGY_LOG_ERROR, _T("Unsupported codec %s.\n"), char_to_tstring(avcodec_get_name(m_videoReplace->getVidCodecID())).c_str());
            return RGY_ERR_INVALID_CODEC;
        }
    } else {
        m_encoderPath = prms.encoderPath;
        m_encoderArgs = prms.encoderArgs;
    }

    m_vidPIDReplace = 0x0100;

    return RGY_ERR_NONE;
}

RGY_ERR TSReplace::readTS(std::vector<uniqueRGYTSPacket>& packetBuffer) {
    if ((m_encoder || m_encoderPath.length() > 0) && !m_queueInputEncoder) {
        m_queueInputEncoder = std::make_unique<RGYQueueBuffer>();
        m_queueInputEncoder->init(4 * 1024 * 1024);
        m_queueInputEncoder->setMaxCapacity(4 * 1024 * 1024);
    }
    if (!m_queueInputReplace) {
        m_queueInputReplace = std::make_unique<RGYQueueBuffer>();
        m_queueInputReplace->init(4 * 1024 * 1024);
        m_queueInputReplace->setMaxCapacity(4 * 1024 * 1024);
    }
    if (!m_queueInputPreAnalysis && !m_preAnalysisFin) {
        m_queueInputPreAnalysis = std::make_unique<RGYQueueBuffer>();
        m_queueInputPreAnalysis->init(4 * 1024 * 1024);
        m_queueInputPreAnalysis->setMaxCapacity(4 * 1024 * 1024);
    }
    // 実際に読み込みを行うスレッドを起動
    if (!m_threadInputTS) {
        m_threadInputTS = std::make_unique<std::thread>([&]() {
            std::vector<uint8_t> readBuffer(m_fileTSBufSize);
            size_t bytes_read = 0;
            while (!m_inputAbort && (bytes_read = _fread_nolock(readBuffer.data(), 1, readBuffer.size(), m_fpTSIn.get())) != 0) {
                if (m_queueInputPreAnalysis && m_preAnalysisFin) {
                    m_queueInputPreAnalysis.reset();
                }

                // キューのリストを作成する
                std::vector<std::pair<bool, RGYQueueBuffer*>> queues;
                if (m_queueInputEncoder)     queues.push_back({ false, m_queueInputEncoder.get() });
                if (m_queueInputReplace)     queues.push_back({ false, m_queueInputReplace.get() });
                if (m_queueInputPreAnalysis) queues.push_back({ false, m_queueInputPreAnalysis.get() });

                size_t queueSent = 0;
                while (queueSent < queues.size()) {
                    bool emptyQueueExists = false;
                    for (auto& queue : queues) {
                        if (queue.first) {
                            emptyQueueExists |= queue.second->size() == 0;
                            continue;
                        }
                        queue.first = queue.second->pushData(readBuffer.data(), bytes_read, 0);
                        queueSent += queue.first ? 1 : 0;
                    }
                    if (emptyQueueExists) {
                        for (auto& queue : queues) {
                            if (!queue.first) {
                                queue.second->setMaxCapacity(m_queueInputEncoder->getMaxCapacity() * 2);
                            }
                        }
                    }
                }
            }
            AddMessage(RGY_LOG_DEBUG, _T("Reached input ts EOF.\n"));
            if (m_queueInputEncoder) m_queueInputEncoder->setEOF();
            if (m_queueInputReplace) m_queueInputReplace->setEOF();
            if (m_queueInputPreAnalysis) m_queueInputPreAnalysis->setEOF();
        });
    }

    m_bufferTS.resize(m_fileTSBufSize);

    auto now = std::chrono::system_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - m_tsReadUpdate).count() > 800) {
        _ftprintf(stderr, _T("Reading %.1f MB.\r"), m_tsPktSplitter->pos() / (double)(1024 * 1024));
        fflush(stderr); //リダイレクトした場合でもすぐ読み取れるようflush
        m_tsReadUpdate = now;
    }
    int64_t bytes_read = 0;
    while ((bytes_read = m_queueInputReplace->popDataBlock(m_bufferTS.data(), m_bufferTS.size())) >= 0) {
        if (bytes_read == 0) {
            continue;
        }
        auto [ret, packets] = m_tsPktSplitter->split(m_bufferTS.data(), bytes_read);
        if (ret != RGY_ERR_NONE) {
            return ret;
        }
        if (packets.size() > 0) {
            int pktNot188Count = 0;
            for (auto& pkt : packets) {
                if (pkt->datasize() == 188) {
                    packetBuffer.push_back(std::move(pkt));
                } else {
                    pktNot188Count++;
                }
            }
            if (pktNot188Count > 0) {
                if (pktNot188Count >= 5) {
                    AddMessage(RGY_LOG_ERROR, _T("Invalid packets (non 188 byte) found.\n"));
                    return RGY_ERR_INVALID_BINARY;
                } else {
                    AddMessage(RGY_LOG_WARN, _T("Invalid %d packet removed.\n"), pktNot188Count);
                }
            }
            return RGY_ERR_NONE;
        }
    }
    return RGY_ERR_MORE_DATA;
}

RGY_ERR TSReplace::writePacket(const RGYTSPacket *pkt) {
    if (fwrite(pkt->data(), 1, pkt->datasize(), m_fpTSOut.get()) != pkt->datasize()) {
        return RGY_ERR_OUT_OF_RESOURCES;
    }
    return RGY_ERR_NONE;
}

uint8_t TSReplace::getvideoDecCtrlEncodeFormat(const int height) {
    switch (height) {
    case 1080: return 0x00;
    case 720:  return 0x02;
    case 480:  return 0x03;
    case 240:  return 0x05;
    case 120:  return 0x06;
    case 2160: return 0x07;
    case 180:  return 0x08;
    case 4320: return 0x0A;
    default:   return 0x00;
    }
}
RGY_ERR TSReplace::writeReplacedPMT(const RGYTSDemuxResult& result) {
    // 参考: https://txqz.net/memo/2012-0916-1729.html
    const auto psi = result.psi.get();
    if (psi->section_length < 9) {
        return RGY_ERR_INVALID_BINARY;
    }
    const uint8_t *const table = psi->data;
    int programInfoLength = ((table[10] & 0x03) << 8) | table[11];
    int pos = 3 + 9 + programInfoLength;
    if (psi->section_length < pos) {
        return RGY_ERR_INVALID_BINARY;
    }

    //const uint8_t video_encode_format = getvideoDecCtrlEncodeFormat(m_videoReplace->getVidCodecPar()->height);

    // Create PMT
    std::vector<uint8_t> buf(1, 0);
    buf.insert(buf.end(), table + 0, table + pos); // descriptor まで
    if (m_demuxer->service()->pidPcr == m_demuxer->service()->vid.pid) {
        buf[1+ 9] = (uint8_t)((m_vidPIDReplace & 0x1fff) >> 8) | (buf[1 + 9] & 0xE0); // PIDの上書き
        buf[1+10] = (uint8_t) (m_vidPIDReplace & 0x00ff);                             // PIDの上書き
    }

    const int tableLen = 3 + psi->section_length - 4/*CRC32*/;
    while (pos + 4 < tableLen) {
        const auto streamType = (RGYTSStreamType)table[pos];
        const int esPid = ((table[pos + 1] & 0x1f) << 8) | table[pos + 2];
        const int esInfoLength = ((table[pos + 3] & 0x03) << 8) | table[pos + 4];
        
        if (streamType == RGYTSStreamType::H262_VIDEO) {
            buf.push_back((uint8_t)m_videoReplace->getVideoStreamType());     // stream typeの上書き
            buf.push_back((uint8_t)((m_vidPIDReplace & 0x1fff) >> 8) | (table[pos + 1] & 0xE0)); // PIDの上書き
            buf.push_back((uint8_t) (m_vidPIDReplace & 0x00ff));                                 // PIDの上書き
            buf.push_back(0xf0);
            buf.push_back(0x03);
            buf.push_back((uint8_t)RGYTSDescriptor::StreamIdentifier);
            buf.push_back(0x01);
            buf.push_back(0x00);
        } else {
            buf.insert(buf.end(), table + pos, table + pos + 5 + esInfoLength);
        }
        pos += 5 + esInfoLength;
    }

    buf[2] = 0xb0 | (uint8_t)((buf.size() + 4 - 4) >> 8);
    buf[3] = (uint8_t)(buf.size() + 4 - 4);

    if (m_lastPmt.size() == buf.size() + 4 &&
        std::equal(buf.begin(), buf.end(), m_lastPmt.begin())) {
        buf.insert(buf.end(), m_lastPmt.end() - 4, m_lastPmt.end()); // copy CRC
    } else {
        const uint32_t crc = calc_crc32(buf.data() + 1, static_cast<int>(buf.size() - 1));
        buf.push_back(crc >> 24);
        buf.push_back((crc >> 16) & 0xff);
        buf.push_back((crc >> 8) & 0xff);
        buf.push_back(crc & 0xff);
        m_lastPmt = buf;
    }

    const auto PMT_PID = m_demuxer->selectServiceID()->pmt_pid;

    // Create TS packets
    RGYTSPacket pkt;
    pkt.packet.reserve(188);
    for (size_t i = 0; i < buf.size(); i += 184) {
        pkt.packet.clear();
        pkt.packet.push_back(0x47);
        pkt.packet.push_back((i == 0 ? 0x40 : 0) | (uint8_t)(PMT_PID >> 8));
        pkt.packet.push_back((uint8_t)(PMT_PID & 0xff));
        m_pmtCounter = (m_pmtCounter + 1) & 0x0f;
        pkt.packet.push_back(0x10 | m_pmtCounter);
        pkt.packet.insert(pkt.packet.end(), buf.begin() + i, buf.begin() + std::min(i + 184, buf.size()));
        pkt.packet.resize(((pkt.packet.size() - 1) / 188 + 1) * 188, 0xff);
        writePacket(&pkt);
    }
    return RGY_ERR_NONE;
}

void TSReplace::pushPESPTS(std::vector<uint8_t>& buf, const int64_t pts, const uint8_t top4bit) {
    const uint16_t a = (((pts >> 15) & 0x7fff) << 1) | 0x01;
    const uint16_t b = ((pts & 0x7fff) << 1) | 0x01;

    buf.push_back(top4bit | (uint8_t)(((pts >> 30) & 0x07) << 1) | 0x01);
    buf.push_back((uint8_t)(a >> 8));
    buf.push_back((uint8_t)(a & 0xff));
    buf.push_back((uint8_t)(b >> 8));
    buf.push_back((uint8_t)(b & 0xff));
}

bool TSReplace::isFirstNalAud(const bool isHEVC, const uint8_t *ptr, const size_t size) {
    static uint8_t NAL_START_CODE_3[3] = { 0x00, 0x00, 0x01 };
    static uint8_t NAL_START_CODE_4[4] = { 0x00, 0x00, 0x00, 0x01 };
    int pos = 0;
    if (memcmp(ptr, NAL_START_CODE_3, sizeof(NAL_START_CODE_3)) == 0) {
        pos = 3;
    } else if (memcmp(ptr, NAL_START_CODE_4, sizeof(NAL_START_CODE_4)) == 0) {
        pos = 4;
    }
    if (pos) {
        if (isHEVC) {
            const uint8_t nal_type = (ptr[pos] & 0x7f) >> 1;
            return nal_type == NALU_HEVC_AUD;
        } else {
            const uint8_t nal_type = (ptr[pos] & 0x1f);
            return nal_type == NALU_H264_AUD;
        }
    }
    return false;
}

uint8_t TSReplace::getAudValue(const AVPacket *pkt) const {
    const auto flags = pkt->flags;
    if (flags & AV_PKT_FLAG_KEY) {
        return 0;
    }
    switch (flags & RGY_FLAG_PICT_TYPES) {
    case RGY_FLAG_PICT_TYPE_I: return 0;
    case RGY_FLAG_PICT_TYPE_P: return 1;
    case RGY_FLAG_PICT_TYPE_B:
    default:                   return 2;
    }
    return 2;
}

std::tuple<RGY_ERR, bool, bool> TSReplace::checkPacket(const AVPacket *pkt) {
    if (m_videoReplace->getVidCodecID() == AV_CODEC_ID_H264) {
        const auto nal_list = m_parseNalH264(pkt->data, pkt->size);
        const auto h264_aud_nal = std::find_if(nal_list.begin(), nal_list.end(), [](nal_info info) { return info.type == NALU_H264_AUD; });
        const auto h264_sps_nal = std::find_if(nal_list.begin(), nal_list.end(), [](nal_info info) { return info.type == NALU_H264_SPS; });
        const auto h264_pps_nal = std::find_if(nal_list.begin(), nal_list.end(), [](nal_info info) { return info.type == NALU_H264_PPS; });
        const bool header_check = (nal_list.end() != h264_sps_nal) && (nal_list.end() != h264_pps_nal);
        return { RGY_ERR_NONE, h264_aud_nal != nal_list.end(), header_check };
    } else if (m_videoReplace->getVidCodecID() == AV_CODEC_ID_HEVC) {
        const auto nal_list = m_parseNalHevc(pkt->data, pkt->size);
        const auto hevc_aud_nal = std::find_if(nal_list.begin(), nal_list.end(), [](nal_info info) { return info.type == NALU_HEVC_AUD; });
        const auto hevc_vps_nal = std::find_if(nal_list.begin(), nal_list.end(), [](nal_info info) { return info.type == NALU_HEVC_VPS; });
        const auto hevc_sps_nal = std::find_if(nal_list.begin(), nal_list.end(), [](nal_info info) { return info.type == NALU_HEVC_SPS; });
        const auto hevc_pps_nal = std::find_if(nal_list.begin(), nal_list.end(), [](nal_info info) { return info.type == NALU_HEVC_PPS; });
        const bool header_check = (nal_list.end() != hevc_vps_nal) && (nal_list.end() != hevc_sps_nal) && (nal_list.end() != hevc_pps_nal);
        return { RGY_ERR_NONE, hevc_aud_nal != nal_list.end(), header_check };
    }
    AddMessage(RGY_LOG_ERROR, _T("Unsupported codec %s!\n"), char_to_tstring(avcodec_get_name(m_videoReplace->getVidCodecID())).c_str());
    return { RGY_ERR_UNSUPPORTED, false, false };
}

RGY_ERR TSReplace::writeReplacedVideo(AVPacket *avpkt) {
    const uint8_t vidStreamID = 0xe0;
    const auto vidPID = m_vidPIDReplace;
    const bool addDts = (avpkt->pts != avpkt->dts);
    const bool isKey = (avpkt->flags & AV_PKT_FLAG_KEY) != 0;
    const auto [err, has_aud, has_header] = checkPacket(avpkt);
    if (err != RGY_ERR_NONE) {
        return err;
    }
    const bool replaceToHEVC = m_videoReplace->getVidCodecID() == AV_CODEC_ID_HEVC;
    const bool addAud = m_addAud && !has_aud;
    const bool addHeader = m_addHeaders && isKey && !has_header;
    const auto pts = av_rescale_q(avpkt->pts - m_videoReplace->getFirstKeyPts(), m_videoReplace->getVidTimebase(), av_make_q(1, TS_TIMEBASE)) + m_vidFirstTimestamp;
    const auto dts = av_rescale_q(avpkt->dts - m_videoReplace->getFirstKeyPts(), m_videoReplace->getVidTimebase(), av_make_q(1, TS_TIMEBASE)) + m_vidFirstTimestamp;

    int add_aud_len = (addAud) ? ((replaceToHEVC) ? 7 : 6) : 0;
    const  uint8_t *header = nullptr;
    int add_header_len = 0;
    if (addHeader) {
        header = m_videoReplace->getExtraData(add_header_len);
        if (!header) {
            AddMessage(RGY_LOG_ERROR, _T("Failed to get header!\n"));
            return RGY_ERR_NULL_PTR;
        }
    }

    RGYTSPacket pkt;
    pkt.packet.reserve(188);
    for (int i = 0; i < avpkt->size; ) {
        const int pes_header_len = (i > 0) ? 0 : (14 + (addDts ? 5 : 0));
        const int min_adaption_len = (false /*無効化*/ && i == 0 && isKey) ? 2 : 0;
        int len = std::min(184 - min_adaption_len, avpkt->size + pes_header_len + add_aud_len + add_header_len - i);
        if (pes_header_len + add_aud_len + add_header_len + min_adaption_len > 184) {
            AddMessage(RGY_LOG_ERROR, _T("Header size %d is too long, unsupported!\n"), add_header_len);
            return RGY_ERR_UNSUPPORTED;
        }
        m_vidCounter = (m_vidCounter + 1) & 0x0f;

        pkt.packet.clear();
        pkt.packet.push_back(0x47);
        pkt.packet.push_back((i == 0 ? 0x40 : 0) | (uint8_t)(vidPID >> 8));
        pkt.packet.push_back((uint8_t)(vidPID & 0xff));
        pkt.packet.push_back((len < 184 ? 0x30 : 0x10) | m_vidCounter);
        if (len < 184) {
            pkt.packet.push_back((uint8_t)(183 - len));
            if (len < 183) {
                pkt.packet.push_back((i == 0 && isKey) ? 0x40 : 0x00);
                pkt.packet.insert(pkt.packet.end(), 182 - len, 0xff);
            }
        }
        if (pes_header_len > 0) {
            static uint8_t PES_START_CODE[3] = { 0x00, 0x00, 0x01 };
            pkt.packet.insert(pkt.packet.end(), PES_START_CODE, PES_START_CODE + sizeof(PES_START_CODE));
            pkt.packet.push_back(vidStreamID); // stream id
            pkt.packet.push_back(0);
            pkt.packet.push_back(0);
            pkt.packet.push_back(0x80);
            pkt.packet.push_back(addDts ? (0x80 | 0x40) : 0x80);
            pkt.packet.push_back(addDts ? 10 : 5); // pes_header_len
            if (addDts) {
                pushPESPTS(pkt.packet, pts, 0x30);
                pushPESPTS(pkt.packet, dts, 0x10);
            } else {
                pushPESPTS(pkt.packet, pts, 0x20);
            }
        }
        if (add_aud_len > 0) {
            static uint8_t NAL_START_CODE[4] = { 0x00, 0x00, 0x00, 0x01 };
            pkt.packet.insert(pkt.packet.end(), NAL_START_CODE, NAL_START_CODE + sizeof(NAL_START_CODE));
            if (replaceToHEVC) {
                pkt.packet.push_back(NALU_HEVC_AUD << 1);
                pkt.packet.push_back(0x01);
            } else {
                pkt.packet.push_back(NALU_H264_AUD);
            }
            pkt.packet.push_back((getAudValue(avpkt) << 5) | 0x10);
        }
        if (add_header_len > 0) {
            pkt.packet.insert(pkt.packet.end(), header, header + add_header_len);
        }

        pkt.packet.insert(pkt.packet.end(), avpkt->data + i, avpkt->data + i + len - pes_header_len - add_aud_len - add_header_len);
        i += (len - pes_header_len - add_aud_len - add_header_len);
        add_aud_len = 0;
        add_header_len = 0;
        writePacket(&pkt);
    }
    return RGY_ERR_NONE;
}

int64_t TSReplace::getOrigPtsOffset() {
    if (m_vidDTS < m_vidDTSOutMax) {
        if (m_vidDTSOutMax - m_vidDTS > WRAP_AROUND_CHECK_VALUE) {
            AddMessage(RGY_LOG_INFO, _T("PTS/DTS wrap!\n"));
            m_ptswrapOffset += WRAP_AROUND_VALUE;
            m_vidDTSOutMax = m_vidDTS;
        }
    } else {
        if (m_vidDTS - m_vidDTSOutMax < WRAP_AROUND_CHECK_VALUE) {
            m_vidDTSOutMax = m_vidDTS;
        }
    }
    // dtsベースで差分を計算するが、起点は最初のPTSとする
    auto offset = m_vidDTSOutMax + m_ptswrapOffset - m_vidFirstTimestamp;
    return offset;
}

RGY_ERR TSReplace::writeReplacedVideo() {
    if (m_vidFirstTimestamp == TIMESTAMP_INVALID_VALUE) {
        return RGY_ERR_NONE;
    }
    const auto dtsOrigOffset = getOrigPtsOffset();
    for (;;) {
        auto [err, pts, dts] = m_videoReplace->getFrontPktPtsDts();
        if (err != RGY_ERR_NONE) {
            return err;
        }
        // dtsベースで差分を計算するが、起点は最初のPTSとする
        const auto dtsVidOffset = av_rescale_q(dts - m_videoReplace->getFirstKeyPts(), m_videoReplace->getVidTimebase(), av_make_q(1, TS_TIMEBASE));

        if (dtsOrigOffset < dtsVidOffset) {
            break;
        }
        auto [err2, pkt] = m_videoReplace->getFrontPktAndPop();
        if (err2 != RGY_ERR_NONE) {
            return err2;
        }
        err = writeReplacedVideo(pkt.get());
        if (err != RGY_ERR_NONE) {
            return err;
        }
    }
    return RGY_ERR_NONE;
}

int64_t TSReplace::getStartPointPTS() const {
    switch (m_startPoint) {
    case TSRReplaceStartPoint::FirstPacket:    return m_vidFirstPacketPTS;
    case TSRReplaceStartPoint::FirstFrame:     return m_vidFirstFramePTS;
    case TSRReplaceStartPoint::KeyframPts:     return m_vidFirstKeyPTS;
    }
    return TIMESTAMP_INVALID_VALUE;
}

RGY_ERR TSReplace::initDemuxer(std::vector<uniqueRGYTSPacket>& tsPackets) {
    const RGYTS_PAT *pat = nullptr;
    const RGYService *service = nullptr;

    // まずPAT/PMTを読み込む
    while (!pat || !service) {
        if (tsPackets.empty() || !pat || !service) {
            auto err = readTS(tsPackets);
            if (err != RGY_ERR_NONE) {
                return err;
            }
        }

        for (auto& tspkt : tsPackets) {
            auto parsed_ret = m_demuxer->parse(tspkt.get());
            const auto err = std::get<0>(parsed_ret);
            if (err != RGY_ERR_NONE) {
                return err;
            }
            const auto packet_type = std::get<1>(parsed_ret).type;
            switch (packet_type) {
            case RGYTSPacketType::PAT:
                pat = m_demuxer->pat();
                break;
            case RGYTSPacketType::PMT:
                service = m_demuxer->service();
                break;
            }
        }
    }

    m_vidPIDReplace = service->vid.pid;
    AddMessage(RGY_LOG_INFO, _T("Output vid pid: 0x%04x.\n"), m_vidPIDReplace);

    // 映像の先頭の時刻を検出
    auto originalTS = std::make_unique<TSReplaceVideo>(m_log);
    auto sts = originalTS->initAVReader(_T(""), m_queueInputPreAnalysis.get(), _T("mpegts"));
    if (sts != RGY_ERR_NONE) {
        return sts;
    }
    sts = originalTS->getFirstPts(m_vidFirstPacketPTS, m_vidFirstFramePTS, m_vidFirstKeyPTS);
    if (sts != RGY_ERR_NONE) {
        return sts;
    }
    // 読み込み側に解析の終了を通知
    m_preAnalysisFin = true;
    originalTS.reset();

    AddMessage(RGY_LOG_INFO, _T("%s First packet PTS: %11lld [%+7.1f ms] [%+7.1f ms]\n"),
        (m_startPoint == TSRReplaceStartPoint::FirstPacket) ? _T("*") : _T(" "),
        m_vidFirstPacketPTS, (m_vidFirstPacketPTS - m_vidFirstPacketPTS) * 1000.0 / (double)TS_TIMEBASE, (m_vidFirstPacketPTS - m_vidFirstFramePTS) * 1000.0 / (double)TS_TIMEBASE);
    AddMessage(RGY_LOG_INFO, _T("%s First frame  PTS: %11lld [%+7.1f ms] [%+7.1f ms]\n"),
        (m_startPoint == TSRReplaceStartPoint::FirstFrame) ? _T("*") : _T(" "),
        m_vidFirstFramePTS, (m_vidFirstFramePTS - m_vidFirstPacketPTS) * 1000.0 / (double)TS_TIMEBASE, (m_vidFirstFramePTS - m_vidFirstFramePTS) * 1000.0 / (double)TS_TIMEBASE);
    AddMessage(RGY_LOG_INFO, _T("%s First key    PTS: %11lld [%+7.1f ms] [%+7.1f ms]\n"),
        (m_startPoint == TSRReplaceStartPoint::KeyframPts) ? _T("*") : _T(" "),
        m_vidFirstKeyPTS, (m_vidFirstKeyPTS - m_vidFirstPacketPTS) * 1000.0 / (double)TS_TIMEBASE, (m_vidFirstKeyPTS - m_vidFirstFramePTS) * 1000.0 / (double)TS_TIMEBASE);
    if (getStartPointPTS() == TIMESTAMP_INVALID_VALUE) {
        AddMessage(RGY_LOG_ERROR, _T("Failed to get first timestamp.\n"));
        return RGY_ERR_UNKNOWN;
    }

    pat = nullptr;
    m_vidFirstTimestamp = TIMESTAMP_INVALID_VALUE;
    m_demuxer->resetPCR();
    m_demuxer->resetPSICache();
    return RGY_ERR_NONE;
}

RGY_ERR TSReplace::initEncoder() {
    m_encoder = createRGYPipeProcess();
    m_encPipe = ProcessPipe{};
    m_encPipe.stdIn.mode = PIPE_MODE_ENABLE;
    m_encPipe.stdOut.mode = PIPE_MODE_ENABLE;
    m_encPipe.stdErr.mode = PIPE_MODE_ENABLE;

    for (auto& arg : m_encoderArgs) {
        arg = str_replace(arg, _T("%{input}"), _T("-"));
        arg = str_replace(arg, _T("%{output}"), _T("-"));
    }

    std::vector<const TCHAR *> args;
    args.push_back(m_encoderPath.c_str());
    for (const auto& arg : m_encoderArgs) {
        args.push_back(arg.c_str());
    }
    args.push_back(nullptr);

    tstring optionstr;
    for (const auto& arg : m_encoderArgs) {
        optionstr += arg;
        optionstr += _T(" ");
    }
    AddMessage(RGY_LOG_INFO, _T("Run encoder: %s %s.\n"), m_encoderPath.c_str(), optionstr.c_str());
    if (m_encoder->run(args, nullptr, &m_encPipe, 0, false, false)) {
        AddMessage(RGY_LOG_ERROR, _T("Failed to run \"%s\".\n"), m_encoderPath.c_str());
        return RGY_ERR_RUN_PROCESS;
#if defined(_WIN32) || defined(_WIN64)
    } else {
        WaitForInputIdle(dynamic_cast<RGYPipeProcessWin *>(m_encoder.get())->getProcessInfo().hProcess, INFINITE);
#endif
    }

    // エンコーダーに転送するスレッドを起動
    m_threadSendEncoder = std::make_unique<std::thread>([&]() {
        std::vector<uint8_t> readBuffer(1 * 1024 * 1024);
        while (!m_inputAbort && m_encoder && m_encoder->processAlive()) {
            auto bytes_read = m_queueInputEncoder->popDataBlock(readBuffer.data(), readBuffer.size());
            if (bytes_read < 0) {
                break;
            }
            const size_t bytes_sent = _fwrite_nolock(readBuffer.data(), 1, bytes_read, m_encPipe.f_stdin);
            if (bytes_sent != bytes_read) {
                AddMessage(RGY_LOG_ERROR, _T("Failed to send bitstream to encoder.\n"));
                break;
            }
            fflush(m_encPipe.f_stdin);
        }
        AddMessage(RGY_LOG_DEBUG, _T("Reached encoder input EOF.\n"));
        fclose(m_encPipe.f_stdin);
    });

    m_encQueueOut = std::make_unique<RGYQueueBuffer>();

    m_encThreadOut = std::thread([&]() {
        std::vector<uint8_t> buffer;
        while (m_encoder->getOneOut(buffer, &m_encPipe) >= 0) {
            if (buffer.size() > 0) {
                m_encQueueOut->pushData(buffer.data(), buffer.size(), std::numeric_limits<int>::max());
                buffer.clear();
            }
        }
        m_encoder->getOneOut(buffer, &m_encPipe);
        if (buffer.size() > 0) {
            m_encQueueOut->pushData(buffer.data(), buffer.size(), std::numeric_limits<int>::max());
            buffer.clear();
        }
        AddMessage(RGY_LOG_DEBUG, _T("Reached encoder stdout EOF.\n"));
        m_encQueueOut->setEOF();
    });

    m_encThreadErr = std::thread([&]() {
        std::vector<uint8_t> buffer;
        while (m_encoder->getOneErr(buffer, &m_encPipe) >= 0) {
            if (buffer.size() > 0) {
                auto str = std::string(buffer.data(), buffer.data() + buffer.size());
                m_log->write(RGY_LOG_INFO, RGY_LOGT_APP, _T("%s"), char_to_tstring(str).c_str());
                buffer.clear();
            }
        }
        m_encoder->getOneErr(buffer, &m_encPipe);
        if (buffer.size() > 0) {
            auto str = std::string(buffer.data(), buffer.data() + buffer.size());
            m_log->write(RGY_LOG_INFO, RGY_LOGT_APP, _T("%s"), char_to_tstring(str).c_str());
            buffer.clear();
        }
        AddMessage(RGY_LOG_DEBUG, _T("Reached encoder stderr EOF.\n"));
    });

    m_videoReplace = std::make_unique<TSReplaceVideo>(m_log);
    if (auto sts = m_videoReplace->initAVReader(_T(""), m_encQueueOut.get(), m_replaceFileFormat); sts != RGY_ERR_NONE) {
        return sts;
    }
    const auto streamID = m_videoReplace->getVideoStreamType();
    if (streamID == RGYTSStreamType::UNKNOWN) {
        AddMessage(RGY_LOG_ERROR, _T("Unsupported codec %s.\n"), char_to_tstring(avcodec_get_name(m_videoReplace->getVidCodecID())).c_str());
        return RGY_ERR_INVALID_CODEC;
    }
    return RGY_ERR_NONE;
}

RGY_ERR TSReplace::restruct() {
    std::vector<uniqueRGYTSPacket> tsPackets;

    {
        auto err = initDemuxer(tsPackets);
        if (err != RGY_ERR_NONE) {
            return err;
        }
    }

    if (m_encoderPath.length() > 0) {
        auto err = initEncoder();
        if (err != RGY_ERR_NONE) {
            return err;
        }
    }

    const RGYTS_PAT *pat = nullptr;
    const RGYService *service = nullptr;
    int64_t m_pcr = TIMESTAMP_INVALID_VALUE;
    std::unique_ptr<RGYTSDemuxResult> pmtResult;

    //本解析
    for (;;) {
        if (tsPackets.empty()) {
            auto err = readTS(tsPackets);
            if (err != RGY_ERR_NONE) {
                return err;
            }
        }

        for (auto& tspkt : tsPackets) {
            if (pat) {
                if (tspkt->header.PID == 0x00) { //PAT
                    if (auto err = writeReplacedVideo(); (err != RGY_ERR_NONE && err != RGY_ERR_MORE_DATA)) {
                        return err;
                    }
                } else {
                    auto pmt_pid = m_demuxer->selectServiceID();
                    if (pmt_pid && tspkt->header.PID == pmt_pid->pmt_pid) { // PMT
                        if (auto err = writeReplacedVideo(); (err != RGY_ERR_NONE && err != RGY_ERR_MORE_DATA)) {
                            return err;
                        }
                    }
                }
            }
            auto [err, ret] = m_demuxer->parse(tspkt.get());
            if (err != RGY_ERR_NONE) {
                return err;
            }
            switch (ret.type) {
            case RGYTSPacketType::PAT:
                pat = m_demuxer->pat();
                writePacket(tspkt.get());
                if (pmtResult) {
                    writeReplacedPMT(*pmtResult);
                    pmtResult.reset();
                    if (m_startPoint == TSRReplaceStartPoint::FirstPacket) {
                        m_vidDTSOutMax = m_vidFirstTimestamp = getStartPointPTS();
                        if (auto err = writeReplacedVideo(); (err != RGY_ERR_NONE && err != RGY_ERR_MORE_DATA)) {
                            return err;
                        }
                    }
                }
                break;
            case RGYTSPacketType::PMT:
                service = m_demuxer->service();
                if (tspkt->header.PayloadStartFlag) {
                    writeReplacedPMT(ret);
                }
                break;
            case RGYTSPacketType::PCR: {
                const auto pcr = m_demuxer->pcr();
                if (pcr != TIMESTAMP_INVALID_VALUE) {
                    if (m_pcr == TIMESTAMP_INVALID_VALUE) {
                        AddMessage(RGY_LOG_INFO, _T("First PCR:           %11lld\n"), pcr);
                    } else if (pcr < m_pcr) {
                        AddMessage(RGY_LOG_WARN, _T("PCR less than lastPCR: PCR %11lld, lastPCR %11lld\n"), pcr, m_pcr);
                    }
                }
                m_pcr = pcr;
                if (pat) {
                    if (auto err = writeReplacedVideo(); (err != RGY_ERR_NONE && err != RGY_ERR_MORE_DATA)) {
                        return err;
                    }
                }
                writePacket(tspkt.get());
                break; }
            case RGYTSPacketType::VID:
                if (tspkt->header.PayloadStartFlag) {
                    m_vidPTS = ret.pts;
                    m_vidDTS = ret.dts;
                    if (m_vidFirstFramePTS == TIMESTAMP_INVALID_VALUE) {
                        m_vidFirstFramePTS = m_vidPTS;
                        //AddMessage(RGY_LOG_INFO, _T("First Video PTS:     %11lld\n"), m_vidFirstFramePTS);
                    }
                    if (m_vidFirstFrameDTS == TIMESTAMP_INVALID_VALUE) {
                        m_vidFirstFrameDTS = m_vidDTS;
                        //AddMessage(RGY_LOG_DEBUG, _T("First Video DTS:     %11lld\n"), m_vidFirstFrameDTS);
                    }
                    if (m_vidFirstTimestamp == TIMESTAMP_INVALID_VALUE) {
                        const auto startPoint = getStartPointPTS();
                        if (startPoint <= m_vidPTS) {
                            m_vidDTSOutMax = m_vidFirstTimestamp = getStartPointPTS();
                        }
                    }
                }
                break;
            case RGYTSPacketType::OTHER:
                writePacket(tspkt.get());
                break;
            default:
                break;
            }
        }
        tsPackets.clear();
    }
    return RGY_ERR_NONE;
}

#if defined(_WIN32) || defined(_WIN64)
static bool check_locale_is_ja() {
    const WORD LangID_ja_JP = MAKELANGID(LANG_JAPANESE, SUBLANG_JAPANESE_JAPAN);
    return GetUserDefaultLangID() == LangID_ja_JP;
}
#endif //#if defined(_WIN32) || defined(_WIN64)

static void show_version() {
    _ftprintf(stdout, _T("%s\n"), get_app_version());
}

static void show_help() {
    tstring str = _T("tsreplace -i <input ts file> -r <replace video file> -o <output ts file>\n");

    str +=
        _T("\n")
        _T("-h,-? --help                    show help\n")
        _T("-v,--version                    show version info\n")
        _T("\n")
        _T("-i,--input <filename>           set input ts filename\n")
        _T("-r,--replace <filename>         set input video filename\n")
        _T("-o,--output <filename>          set output ts filename\n")
        _T("\n")
        _T("  --start-point <string>        set start point\n")
        _T("                                  keyframe, firstframe, firstpacket\n");
        _T("  --(no-)add-aud                auto insert aud unit\n")
        _T("  --(no-)add-headers            auto insert headers\n")
        _T("\n")
        _T("  --replace-format <string>     set replace file format\n")
        _T("\n")
        _T("  --log-level <string>          set log level\n")
        _T("                                  debug, info(default), warn, error\n");
     
    _ftprintf(stdout, _T("%s\n"), str.c_str());
}

int parse_log_level_param(const TCHAR *option_name, const TCHAR *arg_value, RGYParamLogLevel& loglevel) {
    std::vector<std::string> paramList;
    for (const auto& param : RGY_LOG_TYPE_STR) {
        paramList.push_back(tchar_to_string(param.second));
    }

    for (const auto &param : split(arg_value, _T(","))) {
        auto pos = param.find_first_of(_T("="));
        if (pos != std::string::npos) {
            auto param_arg = param.substr(0, pos);
            auto param_val = param.substr(pos + 1);
            param_arg = tolowercase(param_arg);
            int value = 0;
            if (get_list_value(list_log_level, param_val.c_str(), &value)) {
                auto type_ret = std::find_if(RGY_LOG_TYPE_STR.begin(), RGY_LOG_TYPE_STR.end(), [param_arg](decltype(RGY_LOG_TYPE_STR[0])& type) {
                    return param_arg == type.second;
                    });
                if (type_ret != RGY_LOG_TYPE_STR.end()) {
                    loglevel.set((RGYLogLevel)value, type_ret->first);
                    continue;
                } else {
                    _ftprintf(stderr, _T("Unknown paramter for --log-level: %s"), param_arg.c_str());
                    return 1;
                }
            } else {
                _ftprintf(stderr, _T("Unknown paramter for --log-level: %s=%s"), param_arg.c_str(), param_val.c_str());
                return 1;
            }
        } else {
            int value = 0;
            if (get_list_value(list_log_level, param.c_str(), &value)) {
                loglevel.set((RGYLogLevel)value, RGY_LOGT_ALL);
                continue;
            } else {
                _ftprintf(stderr, _T("Unknown value for --log-level: %s"), param.c_str());
                return 1;
            }
        }
    }
    return 0;
}

#define IS_OPTION(x) (0 == _tcscmp(option_name, _T(x)))

int parse_print_options(const TCHAR *option_name) {
    if (IS_OPTION("help")) {
        show_version();
        show_help();
        return 1;
    }
    if (IS_OPTION("version")) {
        show_version();
        return 1;
    }
    if (IS_OPTION("check-avcodec-dll")) {
        const auto ret = check_avcodec_dll();
        _ftprintf(stdout, _T("%s\n"), ret ? _T("yes") : _T("no"));
        if (!ret) {
            _ftprintf(stdout, _T("%s\n"), error_mes_avcodec_dll_not_found().c_str());
        }
        return ret ? 1 : -1;
    }
    if (IS_OPTION("check-avversion")) {
        _ftprintf(stdout, _T("%s\n"), getAVVersions().c_str());
        return 1;
    }
    if (IS_OPTION("check-formats")) {
        _ftprintf(stdout, _T("%s\n"), getAVFormats((RGYAVFormatType)(RGY_AVFORMAT_DEMUX | RGY_AVFORMAT_MUX)).c_str());
        return 1;
    }
    return 0;
}

const TCHAR *cmd_short_opt_to_long(TCHAR short_opt) {
    const TCHAR *option_name = nullptr;
    switch (short_opt) {
    case _T('i'):
        option_name = _T("input");
        break;
    case _T('o'):
        option_name = _T("output");
        break;
    case _T('r'):
        option_name = _T("replace");
        break;
    case _T('v'):
        option_name = _T("version");
        break;
    case _T('h'):
    case _T('?'):
        option_name = _T("help");
        break;
    default:
        break;
    }
    return option_name;
}

int ParseOneOption(const TCHAR *option_name, const TCHAR **strInput, int& i, const int argc, TSRReplaceParams& prm) {
    if (IS_OPTION("input")) {
        i++;
        prm.input = strInput[i];
        return 0;
    }
    if (IS_OPTION("replace")) {
        i++;
        prm.replacefile = strInput[i];
        return 0;
    }
    if (IS_OPTION("output")) {
        i++;
        prm.output = strInput[i];
        return 0;
    }
    if (IS_OPTION("replace-format")) {
        i++;
        prm.replacefileformat = strInput[i];
        return 0;
    }
    if (IS_OPTION("start-point")) {
        i++;
        if (int value = get_value_from_chr(list_startpoint, strInput[i]); value != PARSE_ERROR_FLAG) {
            prm.startpoint = (TSRReplaceStartPoint)value;
        } else {
            _ftprintf(stderr, _T("Unknown value for --%s: \"%s\""), option_name, strInput[i]);
            return 1;
        }
        return 0;
    }
    if (IS_OPTION("encoder")) {
        i++;
        prm.encoderPath = strInput[i++];
        for (; i < argc; i++) {
            prm.encoderArgs.push_back(strInput[i]);
        }
        return 0;
    }
    if (IS_OPTION("add-aud")) {
        prm.addAud = true;
        return 0;
    }
    if (IS_OPTION("no-add-aud")) {
        prm.addAud = false;
        return 0;
    }
    if (IS_OPTION("add-headers")) {
        prm.addHeaders = true;
        return 0;
    }
    if (IS_OPTION("no-add-headers")) {
        prm.addHeaders = false;
        return 0;
    }
    if (IS_OPTION("log-level")) { // 最初に読み取り済み
        i++;
        return 0;
    }
    _ftprintf(stderr, _T("Unknown option: \"%s\""), option_name);
    return 1;
}

int _tmain(const int argc, const TCHAR **argv) {
#if defined(_WIN32) || defined(_WIN64)
    if (check_locale_is_ja()) {
        _tsetlocale(LC_ALL, _T("Japanese"));
    }
#endif //#if defined(_WIN32) || defined(_WIN64)

    //log-levelの取得
    RGYParamLogLevel loglevel(RGY_LOG_INFO);
    for (int iarg = 1; iarg < argc - 1; iarg++) {
        if (tstring(argv[iarg]) == _T("--encoder")) {
            break;
        }
        if (tstring(argv[iarg]) == _T("--log-level")) {
            parse_log_level_param(argv[iarg], argv[iarg + 1], loglevel);
            break;
        }
    }

    //表示系オプション
    for (int i = 1; i < argc; i++) {
        if (argv[i] == nullptr) {
            continue;
        }
        if (tstring(argv[i]) == _T("--encoder")) {
            break;
        }
        const TCHAR *option_name = nullptr;
        if (argv[i][0] == _T('|')) {
            break;
        } else if (argv[i][0] == _T('-')) {
            if (argv[i][1] == _T('-')) {
                option_name = &argv[i][2];
            } else if (argv[i][2] == _T('\0')) {
                option_name = cmd_short_opt_to_long(argv[i][1]);
            }
        }

        if (option_name != nullptr) {
            auto ret = parse_print_options(option_name);
            if (ret != 0) {
                return ret == 1 ? 0 : 1;
            }
        }
    }

    std::vector<const TCHAR *> strInput(argv, argv + argc);
    strInput.push_back(_T(""));

    bool debug_cmd_parser = false;
    for (int i = 1; i < argc; i++) {
        if (tstring(strInput[i]) == _T("--debug-cmd-parser")) {
            debug_cmd_parser = true;
            break;
        }
    }
    if (debug_cmd_parser) {
        for (int i = 1; i < argc; i++) {
            _ftprintf(stderr, _T("arg[%3d]: %s\n"), i, strInput[i]);
        }
    }

    TSRReplaceParams prm;

    for (int i = 1; i < argc; i++) {
        if (strInput[i] == nullptr) {
            continue;
        }

        const TCHAR *option_name = nullptr;

        if (strInput[i][0] == _T('|')) {
            break;
        } else if (strInput[i][0] == _T('-')) {
            if (strInput[i][1] == _T('-')) {
                option_name = &strInput[i][2];
            } else if (strInput[i][2] == _T('\0')) {
                if (nullptr == (option_name = cmd_short_opt_to_long(strInput[i][1]))) {
                    _ftprintf(stderr, _T("Unknown option: \"%s\""), strInput[i]);
                    return 1;
                }
            }
        }

        if (option_name == nullptr) {
            _ftprintf(stderr, _T("Unknown option: \"%s\""), strInput[i]);
            return 1;
        }
        if (debug_cmd_parser) {
            _ftprintf(stderr, _T("parsing %3d: %s\n"), i, strInput[i]);
        }
        auto sts = ParseOneOption(option_name, strInput.data(), i, argc, prm);
        if (sts != 0) {
            return sts;
        }
    }

    if (prm.input.size() == 0) {
        _ftprintf(stderr, _T("ERROR: input file not set.\n"));
        return 1;
    }
    if (prm.replacefile.size() == 0 && prm.encoderPath.size() == 0) {
        _ftprintf(stderr, _T("ERROR: replace video file or encoder path not set.\n"));
        return 1;
    }
    if (prm.replacefile.size() > 0) {
        prm.encoderPath.clear();
        prm.encoderArgs.clear();
    }
    if (prm.output.size() == 0) {
        _ftprintf(stderr, _T("ERROR: output file not set.\n"));
        return 1;
    }
    if (prm.output != _T("-")
        && rgy_path_is_same(prm.input, prm.output)) {
        _ftprintf(stderr, _T("ERROR: input and output file cannot be the same.\n"));
        return 1;
    }
    if (prm.replacefile != _T("-")
        && rgy_path_is_same(prm.input, prm.replacefile)) {
        _ftprintf(stderr, _T("ERROR: input and replace file cannot be the same.\n"));
        return 1;
    }
    if (   prm.replacefile != _T("-")
        && prm.output != _T("-")
        && rgy_path_is_same(prm.replacefile, prm.output)) {
        _ftprintf(stderr, _T("ERROR: output and replace file cannot be the same.\n"));
        return 1;
    }

    auto log = std::make_shared<RGYLog>(nullptr, loglevel);
    log->write(RGY_LOG_INFO, RGY_LOGT_APP, _T("%s\n"), get_app_version());
    TSReplace restruct;
    auto err = restruct.init(log, prm);
    if (err != RGY_ERR_NONE) return 1;

    err = restruct.restruct();
    return (err == RGY_ERR_MORE_DATA || err == RGY_ERR_NONE) ? 0 : 1;
}
