#include "ffmpeg/ffmpeg_helper.h"

#include <algorithm>
#include <cstdio>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/rational.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include "common/logger.h"

namespace transcode {

namespace {
std::string avErr(int errnum) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errnum, buf, sizeof(buf));
    return std::string(buf);
}
} // namespace

bool transcodeVideo(const TranscodeParams& p, MediaInfo* info, std::string* err) {
    AVFormatContext* ifmt = nullptr;
    AVFormatContext* ofmt = nullptr;
    AVCodecContext *vdecCtx = nullptr, *vencCtx = nullptr;
    AVCodecContext *adecCtx = nullptr, *aencCtx = nullptr;
    SwsContext* sws = nullptr;
    SwrContext* swr = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* scaled = nullptr;
    bool ok = false;
    int ret;
    int vs = -1;
    int as = -1;
    int outW = 1280, outH = 720;
    const AVCodec* vdec = nullptr;
    const AVCodec* venc = nullptr;
    AVStream* vOut = nullptr;
    AVStream* aOut = nullptr;
    double durationSec = 0.0;
    AVRational videoTimeBase{1, 25};

#define FAIL(msg)        \
    do {                 \
        if (err) *err = (msg); \
        goto cleanup;    \
    } while (0)

    // ---- 打开输入 ----
    ret = avformat_open_input(&ifmt, p.input_path.c_str(), nullptr, nullptr);
    if (ret < 0) FAIL("open input failed: " + avErr(ret));
    if (avformat_find_stream_info(ifmt, nullptr) < 0) FAIL("find stream info failed");

    vs = av_find_best_stream(ifmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0) FAIL("no video stream");
    as = av_find_best_stream(ifmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    if (sscanf(p.resolution.c_str(), "%dx%d", &outW, &outH) != 2) {
        outW = 1280;
        outH = 720;
    }

    // ---- 创建输出容器 ----
    ret = avformat_alloc_output_context2(&ofmt, nullptr, nullptr, p.output_path.c_str());
    if (ret < 0 || !ofmt) FAIL("alloc output context failed");

    // ---- 视频解码器 ----
    vdec = avcodec_find_decoder(ifmt->streams[vs]->codecpar->codec_id);
    if (!vdec) FAIL("video decoder not found");
    vdecCtx = avcodec_alloc_context3(vdec);
    avcodec_parameters_to_context(vdecCtx, ifmt->streams[vs]->codecpar);
    if (avcodec_open2(vdecCtx, vdec, nullptr) < 0) FAIL("open video decoder failed");

    // ---- 视频编码器 (H.264) ----
    venc = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!venc) FAIL("h264 encoder not found");
    vencCtx = avcodec_alloc_context3(venc);
    vencCtx->width = outW;
    vencCtx->height = outH;
    vencCtx->framerate = av_guess_frame_rate(ifmt, ifmt->streams[vs], nullptr);
    if (vencCtx->framerate.num <= 0 || vencCtx->framerate.den <= 0) {
        vencCtx->framerate = AVRational{25, 1};
    }
    vencCtx->time_base = AVRational{vencCtx->framerate.den, vencCtx->framerate.num};
    vencCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    vencCtx->bit_rate = p.bitrate;
    vencCtx->gop_size = 25;
    vencCtx->max_b_frames = 1;
    if (ofmt->oformat->flags & AVFMT_GLOBALHEADER)
        vencCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(vencCtx, venc, nullptr) < 0) FAIL("open video encoder failed");

    durationSec = (ifmt->duration > 0) ? ifmt->duration / (double)AV_TIME_BASE : 0.0;
    videoTimeBase = ifmt->streams[vs]->time_base;

    vOut = avformat_new_stream(ofmt, nullptr);
    avcodec_parameters_from_context(vOut->codecpar, vencCtx);
    vOut->time_base = vencCtx->time_base;

    // ---- 音频（可选）----
    if (as >= 0) {
        const AVCodec* adec = avcodec_find_decoder(ifmt->streams[as]->codecpar->codec_id);
        if (adec) {
            adecCtx = avcodec_alloc_context3(adec);
            avcodec_parameters_to_context(adecCtx, ifmt->streams[as]->codecpar);
            if (avcodec_open2(adecCtx, adec, nullptr) == 0) {
                const AVCodec* aenc = avcodec_find_encoder(AV_CODEC_ID_AAC);
                if (aenc) {
                    aencCtx = avcodec_alloc_context3(aenc);
                    aencCtx->sample_rate = 44100;
                    aencCtx->channel_layout = AV_CH_LAYOUT_STEREO;
                    aencCtx->channels = 2;
                    aencCtx->sample_fmt = aenc->sample_fmts[0];
                    aencCtx->time_base = AVRational{1, 44100};
                    if (ofmt->oformat->flags & AVFMT_GLOBALHEADER)
                        aencCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
                    if (avcodec_open2(aencCtx, aenc, nullptr) < 0) {
                        avcodec_free_context(&aencCtx);
                        aencCtx = nullptr;
                    } else {
                        aOut = avformat_new_stream(ofmt, nullptr);
                        avcodec_parameters_from_context(aOut->codecpar, aencCtx);
                        aOut->time_base = aencCtx->time_base;
                    }
                }
            }
        }
    }

    // ---- 打开输出 ----
    if (avio_open(&ofmt->pb, p.output_path.c_str(), AVIO_FLAG_WRITE) < 0) FAIL("open output failed");
    if (avformat_write_header(ofmt, nullptr) < 0) FAIL("write header failed");

    // ---- 缩放上下文 ----
    sws = sws_getContext(vdecCtx->width, vdecCtx->height, vdecCtx->pix_fmt,
                         outW, outH, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) FAIL("sws_getContext failed");

    // ---- 重采样上下文 ----
    if (adecCtx && aencCtx) {
        swr = swr_alloc_set_opts(nullptr, aencCtx->channel_layout, aencCtx->sample_fmt,
                                 aencCtx->sample_rate, adecCtx->channel_layout,
                                 adecCtx->sample_fmt, adecCtx->sample_rate, 0, nullptr);
        swr_init(swr);
    }

    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    scaled = av_frame_alloc();

    // ---- 主循环 ----
    while (av_read_frame(ifmt, pkt) >= 0) {
        if (pkt->stream_index == vs) {
            if (avcodec_send_packet(vdecCtx, pkt) == 0) {
                while (avcodec_receive_frame(vdecCtx, frame) == 0) {
                    if (p.progress) {
                        const double ptsSec = frame->pts * av_q2d(videoTimeBase);
                        const int percent = durationSec > 0
                                                ? std::min(99, (int)(ptsSec / durationSec * 100))
                                                : 0;
                        p.progress(std::max(0, percent));
                    }

                    // 缩放
                    scaled->format = AV_PIX_FMT_YUV420P;
                    scaled->width = outW;
                    scaled->height = outH;
                    av_frame_get_buffer(scaled, 0);
                    sws_scale(sws, frame->data, frame->linesize, 0, frame->height,
                              scaled->data, scaled->linesize);
                    scaled->pts = av_rescale_q(frame->pts,
                                              ifmt->streams[vs]->time_base,
                                              vencCtx->time_base);

                    // 编码
                    if (avcodec_send_frame(vencCtx, scaled) == 0) {
                        while (avcodec_receive_packet(vencCtx, pkt) == 0) {
                            pkt->stream_index = vOut->index;
                            av_packet_rescale_ts(pkt, vencCtx->time_base, vOut->time_base);
                            av_interleaved_write_frame(ofmt, pkt);
                            av_packet_unref(pkt);
                        }
                    }
                    av_frame_unref(frame);
                }
            }
        } else if (as >= 0 && pkt->stream_index == as && adecCtx && aencCtx) {
            if (avcodec_send_packet(adecCtx, pkt) == 0) {
                while (avcodec_receive_frame(adecCtx, frame) == 0) {
                    AVFrame* aframe = av_frame_alloc();
                    aframe->nb_samples = frame->nb_samples;
                    aframe->format = aencCtx->sample_fmt;
                    aframe->channel_layout = aencCtx->channel_layout;
                    aframe->sample_rate = aencCtx->sample_rate;
                    av_frame_get_buffer(aframe, 0);
                    swr_convert(swr, aframe->data, aframe->nb_samples,
                                (const uint8_t**)frame->data, frame->nb_samples);
                    aframe->pts = av_rescale_q(frame->pts,
                                               ifmt->streams[as]->time_base,
                                               aencCtx->time_base);

                    if (avcodec_send_frame(aencCtx, aframe) == 0) {
                        while (avcodec_receive_packet(aencCtx, pkt) == 0) {
                            pkt->stream_index = aOut->index;
                            av_packet_rescale_ts(pkt, aencCtx->time_base, aOut->time_base);
                            av_interleaved_write_frame(ofmt, pkt);
                            av_packet_unref(pkt);
                        }
                    }
                    av_frame_free(&aframe);
                    av_frame_unref(frame);
                }
            }
        }
        av_packet_unref(pkt);
    }

    // flush 视频编码器
    avcodec_send_frame(vencCtx, nullptr);
    while (avcodec_receive_packet(vencCtx, pkt) == 0) {
        pkt->stream_index = vOut->index;
        av_packet_rescale_ts(pkt, vencCtx->time_base, vOut->time_base);
        av_interleaved_write_frame(ofmt, pkt);
        av_packet_unref(pkt);
    }

    // flush 音频编码器，避免尾部音频丢失导致时长不一致
    if (aencCtx && aOut) {
        avcodec_send_frame(aencCtx, nullptr);
        while (avcodec_receive_packet(aencCtx, pkt) == 0) {
            pkt->stream_index = aOut->index;
            av_packet_rescale_ts(pkt, aencCtx->time_base, aOut->time_base);
            av_interleaved_write_frame(ofmt, pkt);
            av_packet_unref(pkt);
        }
    }

    av_write_trailer(ofmt);

    // ---- 填充元信息 ----
    if (info) {
        info->width = vdecCtx->width;
        info->height = vdecCtx->height;
        info->duration = (ifmt->duration > 0) ? ifmt->duration / (double)AV_TIME_BASE : 0.0;
        FILE* f = fopen(p.output_path.c_str(), "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            info->size = ftell(f);
            fclose(f);
        }
    }
    if (p.progress) p.progress(100);
    ok = true;

cleanup:
    if (swr) swr_free(&swr);
    if (sws) sws_freeContext(sws);
    if (scaled) av_frame_free(&scaled);
    if (frame) av_frame_free(&frame);
    if (pkt) av_packet_free(&pkt);
    if (vdecCtx) avcodec_free_context(&vdecCtx);
    if (vencCtx) avcodec_free_context(&vencCtx);
    if (adecCtx) avcodec_free_context(&adecCtx);
    if (aencCtx) avcodec_free_context(&aencCtx);
    if (ofmt) {
        if (ofmt->pb) avio_closep(&ofmt->pb);
        avformat_free_context(ofmt);
    }
    if (ifmt) avformat_close_input(&ifmt);
    return ok;
#undef FAIL
}

bool extractThumbnail(const std::string& input_path, const std::string& output_path,
                      int width, int height, double at_seconds, std::string* err) {
    AVFormatContext* ifmt = nullptr;
    AVCodecContext* decCtx = nullptr;
    AVCodecContext* encCtx = nullptr;
    SwsContext* sws = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* scaled = nullptr;
    bool ok = false;
    int ret;
    int vs = -1;
    const AVCodec* dec = nullptr;
    int64_t target = 0;
    const AVCodec* enc = nullptr;

#define FAIL(msg)         \
    do {                  \
        if (err) *err = (msg); \
        goto cleanup;     \
    } while (0)

    ret = avformat_open_input(&ifmt, input_path.c_str(), nullptr, nullptr);
    if (ret < 0) FAIL("open input failed");
    if (avformat_find_stream_info(ifmt, nullptr) < 0) FAIL("find stream info failed");

    vs = av_find_best_stream(ifmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0) FAIL("no video stream");

    dec = avcodec_find_decoder(ifmt->streams[vs]->codecpar->codec_id);
    decCtx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(decCtx, ifmt->streams[vs]->codecpar);
    if (avcodec_open2(decCtx, dec, nullptr) < 0) FAIL("open decoder failed");

    // seek 到目标时间
    target = (int64_t)(at_seconds * AV_TIME_BASE);
    if (av_seek_frame(ifmt, -1, target, AVSEEK_FLAG_BACKWARD) < 0) {
        // seek 失败则从头解码
    }

    enc = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    encCtx = avcodec_alloc_context3(enc);
    encCtx->width = width;
    encCtx->height = height;
    encCtx->pix_fmt = AV_PIX_FMT_YUVJ420P;
    encCtx->time_base = AVRational{1, 25};
    if (avcodec_open2(encCtx, enc, nullptr) < 0) FAIL("open mjpeg encoder failed");

    sws = sws_getContext(decCtx->width, decCtx->height, decCtx->pix_fmt, width, height,
                         AV_PIX_FMT_YUVJ420P, SWS_BILINEAR, nullptr, nullptr, nullptr);

    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    scaled = av_frame_alloc();

    while (av_read_frame(ifmt, pkt) >= 0) {
        if (pkt->stream_index != vs) {
            av_packet_unref(pkt);
            continue;
        }
        if (avcodec_send_packet(decCtx, pkt) == 0) {
            if (avcodec_receive_frame(decCtx, frame) == 0) {
                // 找到一帧，缩放后编码
                scaled->format = AV_PIX_FMT_YUVJ420P;
                scaled->width = width;
                scaled->height = height;
                av_frame_get_buffer(scaled, 0);
                sws_scale(sws, frame->data, frame->linesize, 0, frame->height,
                          scaled->data, scaled->linesize);
                scaled->pts = 1;

                if (avcodec_send_frame(encCtx, scaled) == 0) {
                    while (avcodec_receive_packet(encCtx, pkt) == 0) {
                        FILE* f = fopen(output_path.c_str(), "wb");
                        if (f) {
                            fwrite(pkt->data, 1, pkt->size, f);
                            fclose(f);
                            ok = true;
                        }
                        av_packet_unref(pkt);
                        goto cleanup;
                    }
                }
            }
        }
        av_packet_unref(pkt);
    }

cleanup:
    if (sws) sws_freeContext(sws);
    if (scaled) av_frame_free(&scaled);
    if (frame) av_frame_free(&frame);
    if (pkt) av_packet_free(&pkt);
    if (encCtx) avcodec_free_context(&encCtx);
    if (decCtx) avcodec_free_context(&decCtx);
    if (ifmt) avformat_close_input(&ifmt);
    return ok;
#undef FAIL
}

} // namespace transcode
