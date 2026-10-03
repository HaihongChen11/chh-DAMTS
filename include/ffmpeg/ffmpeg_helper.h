#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace transcode {

struct TranscodeParams {
    std::string input_path;
    std::string output_path;
    std::string resolution = "1280x720";
    int bitrate = 1500000;
    int shard_count = 0;
    std::function<void(int percent)> progress;
};

struct MediaInfo {
    int width = 0;
    int height = 0;
    double duration = 0.0;
    int64_t size = 0;
};

// 使用 FFmpeg 底层 C 库（libavformat/libavcodec/libswscale/libswresample）
// 完成音视频转码，而不是调用 system 命令行。
// 成功返回 true，失败返回 false 并填充 err。
bool transcodeVideo(const TranscodeParams& params, MediaInfo* info, std::string* err);
bool transcodeVideoSharded(const TranscodeParams& params, MediaInfo* info, std::string* err);

// 从视频第 at_seconds 秒处截取一帧，缩放为 width x height，编码为 JPEG 输出。
bool extractThumbnail(const std::string& input_path, const std::string& output_path,
                      int width, int height, double at_seconds, std::string* err);

} // namespace transcode
