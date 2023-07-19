
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
#include "tsreplace.h"

static_assert(TIMESTAMP_INVALID_VALUE == AV_NOPTS_VALUE);

AVDemuxFormat::AVDemuxFormat() :
    formatCtx(std::unique_ptr<AVFormatContext, decltype(&avformat_free_context)>(nullptr, avformat_free_context)),
    analyzeSec(0.0),
    isPipe(false),
    lowLatency(false),
    formatOptions(nullptr) {

}

AVDemuxFormat::~AVDemuxFormat() {
    close();
}

void AVDemuxFormat::close() {
    if (formatOptions) {
        av_dict_free(&formatOptions);
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
    hevcbsf(RGYHEVCBsf::INTERNAL),
    bUseHEVCmp42AnnexB(false),
    hevcNaluLengthSize(0) {

}

AVDemuxVideo::~AVDemuxVideo() {
    close();
}

void AVDemuxVideo::close() {
    if (firstPkt) {
        av_packet_unref(firstPkt);
    }
    if (extradata) {
        av_free(extradata);
    }
}

TSReplaceVideo::TSReplaceVideo(std::shared_ptr<RGYLog> log) :
    m_filename(),
    m_Demux(),
    m_log(log),
    m_poolPkt(std::make_unique<RGYPoolAVPacket>()),
    m_hevcMp42AnnexbBuffer(),
    m_packets() {

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

RGY_ERR TSReplaceVideo::initAVReader(const tstring& videofile) {
    if (!check_avcodec_dll()) {
        AddMessage(RGY_LOG_ERROR, error_mes_avcodec_dll_not_found());
        return RGY_ERR_NULL_PTR;
    }

    initAVDevices();

    av_log_set_level((m_log->getLogLevel(RGY_LOGT_IN) == RGY_LOG_DEBUG) ? AV_LOG_DEBUG : RGY_AV_LOG_LEVEL);
    av_qsv_log_set(m_log);

    m_filename = videofile;

    std::string filename_char;
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

    decltype(av_find_input_format(nullptr)) inFormat = nullptr;
#if 0
    if (input_prm->pInputFormat) {
        if (nullptr == (inFormat = av_find_input_format(tchar_to_string(input_prm->pInputFormat).c_str()))) {
            AddMessage(RGY_LOG_ERROR, _T("Unknown Input format: %s.\n"), input_prm->pInputFormat);
            return RGY_ERR_INVALID_FORMAT;
        }
    }
#endif

    //ts向けの設定
    bool scan_all_pmts_set = false;
    if (!av_dict_get(m_Demux.format.formatOptions, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {
        av_dict_set(&m_Demux.format.formatOptions, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
        scan_all_pmts_set = true;
    }
    //ファイルのオープン
    int ret = 0;
    AVFormatContext *format_ctx = avformat_alloc_context();
    if ((ret = avformat_open_input(&format_ctx, filename_char.c_str(), inFormat, &m_Demux.format.formatOptions)) != 0) {
        AddMessage(RGY_LOG_ERROR, _T("error opening file \"%s\": %s\n"), char_to_tstring(filename_char, CP_UTF8).c_str(), qsv_av_err2str(ret).c_str());
        avformat_free_context(format_ctx);
        return RGY_ERR_FILE_OPEN; // Couldn't open file
    }
    m_Demux.format.formatCtx = std::unique_ptr<AVFormatContext, decltype(&avformat_free_context)>(format_ctx, avformat_free_context); format_ctx = nullptr;
    AddMessage(RGY_LOG_DEBUG, _T("opened file \"%s\".\n"), char_to_tstring(filename_char, CP_UTF8).c_str());

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
    AddMessage(RGY_LOG_INFO, _T("Replace to video stream #%d, %s, %dx%d (%s), stream time_base %d/%d, codec_timebase %d/%d.\n"),
        m_Demux.video.stream->index,
        char_to_tstring(avcodec_get_name(m_Demux.video.stream->codecpar->codec_id)).c_str(),
        m_Demux.video.stream->codecpar->width, m_Demux.video.stream->codecpar->height,
        char_to_tstring(av_pix_fmt_desc_get((AVPixelFormat)m_Demux.video.stream->codecpar->format)->name).c_str(),
        m_Demux.video.stream->time_base.num, m_Demux.video.stream->time_base.den,
        av_stream_get_codec_timebase(m_Demux.video.stream).num, av_stream_get_codec_timebase(m_Demux.video.stream).den);

    const auto streamID = getVideoStreamType();
    if (streamID == RGYTSStreamType::UNKNOWN) {
        AddMessage(RGY_LOG_DEBUG, _T("Unsupported codec %s.\n"), char_to_tstring(avcodec_get_name(m_Demux.video.stream->codecpar->codec_id)).c_str());
        return RGY_ERR_INVALID_CODEC;
    }

    //必要ならbitstream filterを初期化
    if (m_Demux.video.stream->codecpar->extradata && m_Demux.video.stream->codecpar->extradata[0] == 1) {
        RGY_ERR sts = initVideoBsfs();
        if (sts != RGY_ERR_NONE) {
            AddMessage(RGY_LOG_ERROR, _T("failed to init bsfs.\n"));
            return sts;
        }
    }

    return RGY_ERR_NONE;
}

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
        if (pkt->stream_index == m_Demux.video.index) {
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
            const bool keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
            if (!m_Demux.video.gotFirstKeyframe && !keyframe) {
                av_packet_unref(pkt.get());
                i_samples++;
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

std::tuple<RGY_ERR, int64_t, int64_t> TSReplaceVideo::getFrontPktPtsDts() {
    if (m_packets.empty()) {
        auto [err, pkt] = getSample();
        if (err != RGY_ERR_NONE) {
            return { (err == AVERROR_EOF) ? RGY_ERR_MORE_DATA : RGY_ERR_UNKNOWN, AV_NOPTS_VALUE, AV_NOPTS_VALUE };
        }
        m_packets.push_back(std::move(pkt));
    }
    auto& fronpkt = m_packets.front();
    return { RGY_ERR_NONE, fronpkt->pts, fronpkt->dts };
}

std::tuple<RGY_ERR, std::unique_ptr<AVPacket, RGYAVDeleter<AVPacket>>> TSReplaceVideo::getFrontPktAndPop() {
    if (m_packets.empty()) {
        auto [err, pkt] = getSample();
        if (err != RGY_ERR_NONE) {
            return { (err == AVERROR_EOF) ? RGY_ERR_MORE_DATA : RGY_ERR_UNKNOWN, nullptr };
        }
        return { RGY_ERR_NONE, std::move(pkt) };
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

TSReplace::TSReplace() :
    m_log(),
    m_demuxer(),
    m_tsReadUpdate(std::chrono::system_clock::now()),
    m_fileTSBufSize(16 * 1024),
    m_fileTS(),
    m_fileOut(),
    m_tsPktSplitter(),
    m_fpTSIn(),
    m_fpTSOut(),
    m_bufferTS(),
    m_vidPIDReplace(0x0100),
    m_vidPTS(TIMESTAMP_INVALID_VALUE),
    m_vidDTS(TIMESTAMP_INVALID_VALUE),
    m_vidFirstPTS(TIMESTAMP_INVALID_VALUE),
    m_vidFirstDTS(TIMESTAMP_INVALID_VALUE),
    m_lastPmt(),
    m_video(),
    m_pmtCounter(0),
    m_vidCounter(0) {

}
TSReplace::~TSReplace() {

}

RGY_ERR TSReplace::init(std::shared_ptr<RGYLog> log, const TSRReplaceParams& prms) {
    m_log = log;
    m_fileTS = prms.input;
    m_fileOut = prms.output;

    AddMessage(RGY_LOG_INFO, _T("Output file \"%s\".\n"), prms.output.c_str());
    AddMessage(RGY_LOG_INFO, _T("Input  file \"%s\".\n"), prms.input.c_str());
    AddMessage(RGY_LOG_INFO, _T("Video  file \"%s\".\n"), prms.vidfile.c_str());

    if (_tcscmp(m_fileTS.c_str(), _T("-")) != 0) {
        AddMessage(RGY_LOG_DEBUG, _T("Open input file \"%s\".\n"), m_fileTS.c_str());
        FILE *fptmp = nullptr;
        if (_tfopen_s(&fptmp, m_fileTS.c_str(), _T("rb")) == 0 && fptmp != nullptr) {
            m_fpTSIn = std::unique_ptr<FILE, fp_deleter>(fptmp, fp_deleter());
            m_fileTSBufSize = 4 * 1024 * 1024;
        } else {
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

    m_video = std::make_unique<TSReplaceVideo>(log);
    if (auto sts = m_video->initAVReader(prms.vidfile); sts != RGY_ERR_NONE) {
        return sts;
    }
    m_vidPIDReplace = 0x0100;
    AddMessage(RGY_LOG_INFO, _T("Output vid pid: 0x%04x.\n"), m_vidPIDReplace);

    return RGY_ERR_NONE;
}

RGY_ERR TSReplace::readTS(std::vector<uniqueRGYTSPacket>& packetBuffer) {
    m_bufferTS.resize(m_fileTSBufSize);

    auto now = std::chrono::system_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - m_tsReadUpdate).count() > 800) {
        _ftprintf(stderr, _T("Reading %.1f MB.\r"), m_tsPktSplitter->pos() / (double)(1024 * 1024));
        fflush(stderr); //リダイレクトした場合でもすぐ読み取れるようflush
        m_tsReadUpdate = now;
    }
    size_t bytes_read = 0;
    while ((bytes_read = fread(m_bufferTS.data(), 1, m_bufferTS.size(), m_fpTSIn.get())) != 0) {
        auto [ret, packets] = m_tsPktSplitter->split(m_bufferTS.data(), bytes_read);
        if (ret != RGY_ERR_NONE) {
            return ret;
        }
        if (packets.size() > 0) {
            for (auto& pkt : packets) {
                packetBuffer.push_back(std::move(pkt));
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

    // Create PMT
    std::vector<uint8_t> buf(1, 0);
    buf.insert(buf.end(), table + 0, table + pos); // descriptor まで

    const int tableLen = 3 + psi->section_length - 4/*CRC32*/;
    while (pos + 4 < tableLen) {
        const auto streamType = (RGYTSStreamType)table[pos];
        const int esPid = ((table[pos + 1] & 0x1f) << 8) | table[pos + 2];
        const int esInfoLength = ((table[pos + 3] & 0x03) << 8) | table[pos + 4];
        
        if (streamType == RGYTSStreamType::H262_VIDEO) {
            std::vector<uint8_t> vidtmp(table + pos, table + pos + 5 + esInfoLength);
            vidtmp[0] = (uint8_t)m_video->getVideoStreamType();     // stream typeの上書き
            vidtmp[1] = (uint8_t)((m_vidPIDReplace & 0x1fff) >> 8); // PIDの上書き
            vidtmp[2] = (uint8_t) (m_vidPIDReplace & 0xff);         // PIDの上書き
#if 0
            if (pos + 5 + esInfoLength <= tableLen) {
                uint8_t componentTag = 0xff;
                uint8_t videoDecCtrlFlags = 0x00;
                int descLen = 0;
                for (int i = 5; i + 2 < 5 + esInfoLength; ) {
                    const RGYTSDescriptor tag = (RGYTSDescriptor)vidtmp[i];
                    const int descLen = vidtmp[i + 1];
                    // stream_identifier_descriptor
                    switch (tag) {
                    case RGYTSDescriptor::StreamIdentifier:
                        componentTag = vidtmp[i + 2];
                        break;
                    case RGYTSDescriptor::VideoDecodeControl:
                        videoDecCtrlFlags = vidtmp[i + 2];
                        break;
                    }
                    i += 2 + descLen;
                }
                if ((m_videoPid == 0 && componentTag == 0xff) || componentTag == 0x00 || componentTag == 0x81) {
                    const uint8_t video_decode_format = (videoDecCtrlFlags & 0x3C) >> 4;
                    const uint8_t transfer_characteristics = (videoDecCtrlFlags & 0xC0) >> 6;
                    videoDecCtrlFlags = (videoDecCtrlFlags & 0x03) | (video_decode_format << 2) | (transfer_characteristics << 6);
                }
            }
#endif
            buf.insert(buf.end(), vidtmp.begin(), vidtmp.end());
        } else {
            buf.insert(buf.end(), table + pos, table + pos + 5 + esInfoLength);
        }
        pos += 5 + esInfoLength;
    }


    buf[2] = 0xb0 | static_cast<uint8_t>((buf.size() + 4 - 4) >> 8);
    buf[3] = static_cast<uint8_t>(buf.size() + 4 - 4);

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

RGY_ERR TSReplace::writeReplacedVideo(AVPacket *avpkt) {
    const uint8_t vidStreamID = 0xe0;
    const auto vidPID = m_vidPIDReplace;
    const bool addDts = (avpkt->pts != avpkt->dts);
    const auto pts = av_rescale_q(avpkt->pts, m_video->getVidTimebase(), av_make_q(1, TS_TIMEBASE)) + m_vidFirstPTS;
    const auto dts = av_rescale_q(avpkt->dts, m_video->getVidTimebase(), av_make_q(1, TS_TIMEBASE)) + m_vidFirstPTS;
    RGYTSPacket pkt;
    pkt.packet.reserve(188);
    for (int i = 0; i < avpkt->size; ) {
        const int pes_header_len = (i > 0) ? 0 : (14 + (addDts ? 5 : 0));
        int len = std::min(184, avpkt->size + pes_header_len - i);
        m_vidCounter = (m_vidCounter + 1) & 0x0f;

        pkt.packet.clear();
        pkt.packet.push_back(0x47);
        pkt.packet.push_back((i == 0 ? 0x40 : 0) | (uint8_t)(vidPID >> 8));
        pkt.packet.push_back((uint8_t)(vidPID & 0xff));
        pkt.packet.push_back((len < 184 ? 0x30 : 0x10) | m_vidCounter);
        if (len < 184) {
            pkt.packet.push_back(static_cast<uint8_t>(183 - len));
            if (len < 183) {
                pkt.packet.push_back(0x00);
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

        pkt.packet.insert(pkt.packet.end(), avpkt->data + i, avpkt->data + i + len - pes_header_len);
        i += (len - pes_header_len);
        writePacket(&pkt);
    }
    return RGY_ERR_NONE;
}

RGY_ERR TSReplace::writeReplacedVideo() {
    if (m_vidFirstPTS == AV_NOPTS_VALUE) {
        return RGY_ERR_NONE;
    }
    const auto ptsOrigOffset = m_vidPTS - m_vidFirstPTS;
    for (;;) {
        auto [err, pts, dts] = m_video->getFrontPktPtsDts();
        if (err != RGY_ERR_NONE) {
            return err;
        }
        const auto ptsVidOffset = av_rescale_q(pts - m_video->getFirstKeyPts(), m_video->getVidTimebase(), av_make_q(1, TS_TIMEBASE));

        if (ptsOrigOffset < ptsVidOffset) {
            break;
        }
        auto [err2, pkt] = m_video->getFrontPktAndPop();
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

RGY_ERR TSReplace::restruct() {
    std::vector<uniqueRGYTSPacket> tsPackets;
    const RGYTS_PAT *pat = nullptr;
    const RGYService *service = nullptr;
    int64_t m_pcr = AV_NOPTS_VALUE;
    while (!pat || !service) {
        if (tsPackets.empty() || !pat || !service) {
            auto err = readTS(tsPackets);
            if (err != RGY_ERR_NONE) {
                return err;
            }
        }
        //最初にPATを探す
        if (!pat) {
            do {
                auto patpkt = std::find_if(tsPackets.begin(), tsPackets.end(), [](const uniqueRGYTSPacket& tspkt) {
                    return tspkt->header.PID == 0x00; //PAT
                    });
                if (patpkt == tsPackets.end()) {
                    break;
                }
                auto [err, ret] = m_demuxer->parse(patpkt->get());
                if (err != RGY_ERR_NONE) {
                    return err;
                }
                pat = m_demuxer->pat();
            } while (!pat);
            if (!pat) {
                continue;
            }
            AddMessage(RGY_LOG_DEBUG, _T("Found first PAT.\n"));
        }

        // PMTを探す
        if (!service) {
            for (auto& tspkt : tsPackets) {
                auto [err, ret] = m_demuxer->parse(tspkt.get());
                if (err != RGY_ERR_NONE) {
                    return err;
                }
                switch (ret.type) {
                case RGYTSPacketType::PAT:
                    pat = m_demuxer->pat();
                    break;
                case RGYTSPacketType::PMT:
                    service = m_demuxer->service();
                    break;
                default:
                    break;
                }
                if (service) {
                    break;
                }
            }
            if (!service) {
                continue;
            }
            AddMessage(RGY_LOG_DEBUG, _T("Found first PMT.\n"));
        }
    }

    m_demuxer->resetPCR();
    m_demuxer->resetPSICache();

    //本解析
    for (;;) {
        if (tsPackets.empty()) {
            auto err = readTS(tsPackets);
            if (err != RGY_ERR_NONE) {
                return err;
            }
        }

        for (auto& tspkt : tsPackets) {
            if (tspkt->header.PID == 0x00) { //PAT
                writeReplacedVideo();
            } else {
                auto pmt_pid = m_demuxer->selectServiceID();
                if (pmt_pid && tspkt->header.PID == pmt_pid->pmt_pid) { // PMT
                    writeReplacedVideo();
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
                break;
            case RGYTSPacketType::PMT:
                service = m_demuxer->service();
                writeReplacedPMT(ret);
                break;
            case RGYTSPacketType::PCR:
                m_pcr = m_demuxer->pcr();
                writeReplacedVideo();
                writePacket(tspkt.get());
                break;
            case RGYTSPacketType::VID:
                if (tspkt->header.PayloadStartFlag) {
                    m_vidPTS = ret.pts;
                    m_vidDTS = ret.dts;
                    if (m_vidFirstPTS == AV_NOPTS_VALUE) {
                        m_vidFirstPTS = m_vidPTS;
                    }
                    if (m_vidFirstDTS == AV_NOPTS_VALUE) {
                        m_vidFirstDTS = m_vidDTS;
                    }
                }
                break;
            case RGYTSPacketType::OTHER:
                writePacket(tspkt.get());
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
    _ftprintf(stdout, _T("tsreplace -i <入力tsファイル> --video-replace <置き換え映像ファイル> -o <出力tsファイル>\n"));
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
    if (IS_OPTION("video-replace")) {
        i++;
        prm.vidfile = strInput[i];
        return 0;
    }
    if (IS_OPTION("output")) {
        i++;
        prm.output = strInput[i];
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
    RGYParamLogLevel loglevel(RGY_LOG_ERROR);
    for (int iarg = 1; iarg < argc - 1; iarg++) {
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
        const TCHAR *option_name = nullptr;
        if (argv[i][0] == _T('|')) {
            break;
        } else if (argv[i][0] == _T('-')) {
            if (argv[i][1] == _T('-')) {
                option_name = &argv[i][2];
            } else if (argv[i][2] == _T('\0')) {
                if (nullptr == (option_name = cmd_short_opt_to_long(argv[i][1]))) {
                    _ftprintf(stderr, _T("Unknown option: \"%s\""), argv[i]);
                    return 1;
                }
            } else {
                _ftprintf(stderr, _T("Invalid option: \"%s\""), argv[i]);
                return 1;
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
            } else {
                _ftprintf(stderr, _T("Invalid option: \"%s\""), strInput[i]);
                return 1;
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
    if (prm.vidfile.size() == 0) {
        _ftprintf(stderr, _T("ERROR: video file not set.\n"));
        return 1;
    }
    if (prm.output.size() == 0) {
        _ftprintf(stderr, _T("ERROR: output file not set.\n"));
        return 1;
    }

    auto log = std::make_shared<RGYLog>(nullptr, loglevel);

    TSReplace restruct;
    restruct.init(log, prm);
    return restruct.restruct();
}
