#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <map>

namespace media_extractor {

// -------------------------------------------------------------------------
// Streamax Media Frame Header (12 bytes fixed, variable extension data)
//
// Wire layout:
//   Byte  0     : Channel  (8-bit)
//   Bytes 1-3   : FrameType (big-endian 24-bit)
//   Bytes 4-6   : FrameLength  (little-endian 24-bit) - payload bytes AFTER the header
//   Byte  7     : StreamExam  (8-bit)
//   Bytes 8-10  : ExtendLength (little-endian 24-bit) - extension data bytes following this header
//   Byte  11    : ExtendCount  (8-bit)
//   Bytes 12..  : Extension data  (ExtendLength bytes)
//   Bytes 12+ExtendLength .. : Frame payload  (FrameLength bytes)
//
// The full header consumed before payload = 12 + ExtendLength bytes.
// -------------------------------------------------------------------------

static constexpr uint32_t MEDIA_FRAME_FIXED_HDR = 12;

// Known FrameType values (24-bit big-endian, first byte distinguishes type)
enum class FrameType : uint32_t {
    None  = 0,
    H264I = 0x326463u,   // I-frame  H.264
    H264P = 0x336463u,   // P-frame  H.264
    Audio = 0x346463u,   // Audio    (ADPCM / G.711 / etc.)
    H265I = 0x356463u,   // I-frame  H.265
    H265P = 0x366463u,   // P-frame  H.265
    MJPEG = 0x376463u,
    Mjpeg = MJPEG,   // MJPEG
};

// Extension info type tags used inside the extension block
enum class ExtensionInfoType : uint8_t {
    Video         = 1,
    RealTime      = 2,
    VirtualTime   = 3,
    List          = 4,
    Audio         = 5,
    VideoStatus   = 6,
    H264          = 7,
    VideoReference= 8,
    VideoEncoding = 9,
    AdditionalData= 10,
    GPS           = 11,
    EncodingExtension = 12,
    // Newest spec names type 13 AIBOX algorithm overlay information. Existing
    // Velocitor samples also carry ASCII telemetry in this extension.
    Telemetry     = 13,
    DataSecurity  = 14,
    IndustryData  = 15,
    CustomDataSecurity = 16,
};

enum class AudioCodec : uint8_t {
    ADPCM = 0,
    G726  = 1,
    G711A = 2,
    G711U = 3,
    AMR   = 4,
    Adpcm = ADPCM,
    Amr = AMR,
};

// RMFI2_VIDEOINFO  (extension type 1)
//   unsigned long lWidth:12;
//   unsigned long lHeight:12;
//   unsigned long lFPS:8;
struct VideoInfo {
    uint32_t width  = 0;
    uint32_t height = 0;
    uint32_t fps    = 0;
};

// RMFI2_AUDIOINFO  (extension type 5)
struct AudioInfo {
    uint32_t  sample_rate = 0;
    uint32_t  bit_width   = 0;
    uint8_t   channels    = 1;
    AudioCodec codec      = AudioCodec::ADPCM;
};

// RealTime extension (type 2) — 8-byte fixed wall-clock per frame.
// Layout observed:  [year-2000] [month] [day] [hour] [minute] [second] [?] [?]
struct RealTimeInfo {
    uint16_t year   = 0;
    uint8_t  month  = 0;
    uint8_t  day    = 0;
    uint8_t  hour   = 0;
    uint8_t  minute = 0;
    uint8_t  second = 0;
};

struct AlarmInfo {
    uint16_t main_id = 0;        // Maps to camera alarm types.
    uint16_t sub_id  = 0;        // Alarm-specific additional ID.
    std::string text;            // Optional alarm string body.
};

struct MetadataEvent {
    uint64_t frame_index = 0;
    double duration_s = 1.2;
    std::string kind;
    std::string text;
    std::map<std::string, std::string> fields;
};

struct MediaFrameHeader {
    uint8_t   channel       = 0;
    FrameType frame_type    = FrameType::None;
    uint32_t  frame_length  = 0;   // payload bytes (after full header + extensions)
    uint8_t   stream_exam   = 0;
    uint32_t  extend_length = 0;   // extension block total bytes
    uint8_t   extend_count  = 0;
    uint32_t  total_header  = 0;   // MEDIA_FRAME_FIXED_HDR + extend_length

    // Optional info parsed from extension blocks
    bool         has_video_info = false;
    VideoInfo    video_info;
    bool         has_video_encoding = false;
    uint8_t      video_encoding = 0;    // observed: 0=H.264, 1=H.265
    bool         has_audio_info = false;
    AudioInfo    audio_info;
    bool         has_realtime   = false;
    RealTimeInfo realtime;
    bool         has_virtual_time = false;
    uint64_t     virtual_pts = 0;       // Extension type 3, demo names this video_pts.
    // Per-field telemetry extracted from one or more extension-type-13 entries.
    // Each entry typically carries a SUBSET of the available fields, so the
    // parser accumulates non-empty values across all entries in the frame.
    std::string  telemetry_speed;       // e.g. "49" (km/h)
    std::string  telemetry_device_id;   // e.g. "AAJDB12"
    std::string  telemetry_lat;         // e.g. "37.774930"
    std::string  telemetry_lng;         // e.g. "-122.419420"
    std::string  telemetry_heading;     // e.g. "180"
    std::string  telemetry_alt;         // e.g. "12"
    std::string  telemetry_raw;         // flattened type-13 payload text
    std::string  telemetry_raw_hex;     // exact type-13 payload bytes (hex)
    std::string  telemetry_type13_summary; // best-effort decoded type13 key fields
    std::string  telemetry_meta_raw;    // raw hex for ext type10/11 blocks
    std::string  telemetry_type15_raw_hex; // ext type15/industry data bytes (hex preview)
    std::string  telemetry_type15_summary; // decoded ext type15 industry/item summary
    std::vector<MetadataEvent> extension_events;
    std::vector<AlarmInfo> telemetry_alarms; // ext type15/industry EXTEND_ALARM
};

// Returns true if ft represents a video frame (H264 or H265)
bool is_video_frame(FrameType ft);

// Returns true if ft is an H.264 frame (I or P)
bool is_h264_frame(FrameType ft);

// Returns true if ft is an H.265 frame (I or P)
bool is_h265_frame(FrameType ft);

// Parse the fixed 12-byte header from `data`.
// On success returns true and fills `hdr` (with extend_length set but extension
// block NOT yet parsed).  Caller must ensure data_len >= MEDIA_FRAME_FIXED_HDR.
bool media_frame_parse_fixed(const uint8_t* data, size_t data_len,
                              MediaFrameHeader& hdr);

// Parse extension block that immediately follows the 12-byte fixed header.
// `ext_data` must point to the extension bytes; `ext_len` == hdr.extend_length.
void media_frame_parse_extensions(const uint8_t* ext_data, uint32_t ext_len,
                                  MediaFrameHeader& hdr);

// Annex-B start-code check (00 00 01 or 00 00 00 01).
bool has_h264_start_code(const uint8_t* data, size_t len);

static constexpr uint32_t kMediaFrameFixedHeaderSize = MEDIA_FRAME_FIXED_HDR;
void media_frame_parse_extensions(const uint8_t* data, uint32_t length,
                                 uint64_t frame_index, MediaFrameHeader& header);
bool is_key_frame(FrameType type);
bool has_annexb_start_code(const uint8_t* data, size_t len);
bool looks_like_media_frame_at(const uint8_t* data, size_t len);
size_t find_media_frame_offset(const uint8_t* data, size_t len,
                               size_t max_scan = 4096);

std::string frame_type_name(FrameType frame_type);
std::string audio_codec_name(AudioCodec codec);


} // namespace media_extractor
