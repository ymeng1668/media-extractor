#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace media_extractor {

static constexpr uint32_t kMediaFrameFixedHeaderSize = 12;

enum class FrameType : uint32_t {
    None = 0,
    H264I = 0x326463u,
    H264P = 0x336463u,
    Audio = 0x346463u,
    H265I = 0x356463u,
    H265P = 0x366463u,
    Mjpeg = 0x376463u,
};

enum class AudioCodec : uint8_t {
    Adpcm = 0,
    G726 = 1,
    G711A = 2,
    G711U = 3,
    Amr = 4,
};

struct VideoInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0;
};

struct AudioInfo {
    uint32_t sample_rate = 0;
    uint32_t bit_width = 0;
    uint8_t channels = 1;
    AudioCodec codec = AudioCodec::Adpcm;
};

struct RealTimeInfo {
    uint16_t year = 0;
    uint8_t month = 0;
    uint8_t day = 0;
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t second = 0;
};

struct MetadataEvent {
    uint64_t frame_index = 0;
    double duration_s = 1.2;
    std::string kind;
    std::string text;
    std::map<std::string, std::string> fields;
};

struct MediaFrameHeader {
    uint8_t channel = 0;
    FrameType frame_type = FrameType::None;
    uint32_t frame_length = 0;
    uint8_t stream_exam = 0;
    uint32_t extend_length = 0;
    uint8_t extend_count = 0;
    uint32_t total_header = 0;

    bool has_video_info = false;
    VideoInfo video_info;

    bool has_video_encoding = false;
    uint8_t video_encoding = 0;

    bool has_audio_info = false;
    AudioInfo audio_info;

    bool has_realtime = false;
    RealTimeInfo realtime;

    bool has_virtual_time = false;
    uint64_t virtual_pts = 0;

    std::vector<MetadataEvent> extension_events;
};

bool is_video_frame(FrameType frame_type);
bool is_h264_frame(FrameType frame_type);
bool is_h265_frame(FrameType frame_type);
bool is_key_frame(FrameType frame_type);
bool has_annexb_start_code(const uint8_t* data, size_t len);

bool media_frame_parse_fixed(const uint8_t* data, size_t len,
                             MediaFrameHeader& hdr);
void media_frame_parse_extensions(const uint8_t* ext_data, uint32_t ext_len,
                                  uint64_t frame_index,
                                  MediaFrameHeader& hdr);

bool looks_like_media_frame_at(const uint8_t* data, size_t len);
size_t find_media_frame_offset(const uint8_t* data, size_t len,
                               size_t max_scan = 4096);

std::string frame_type_name(FrameType frame_type);
std::string audio_codec_name(AudioCodec codec);

} // namespace media_extractor
