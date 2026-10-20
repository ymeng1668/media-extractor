#include "mp4_metadata.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <rapidjson/document.h>

namespace media_extractor {
namespace {
uint32_t u32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t u64(const uint8_t* p) { return u32(p) | (uint64_t(u32(p + 4)) << 32); }

std::string quote(const std::string& s) {
#ifdef _WIN32
    std::string out = "\"";
    for (char c : s) out += c == '"' ? "\\\"" : std::string(1, c);
    return out + "\"";
#else
    std::string out = "'";
    for (char c : s) out += c == '\'' ? "'\\''" : std::string(1, c);
    return out + "'";
#endif
}

bool probe(const std::string& path, std::vector<VideoFrameTime>& times, ForwardPrivacy* forward) {
    std::string cmd = "ffprobe -v error -select_streams v:0 -count_frames -show_packets "
        "-show_entries stream=width,height,has_b_frames,nb_read_frames:packet=pts_time,dts_time "
        "-of json " + quote(path);
#ifdef _WIN32
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) return false;
    std::string json;
    char buffer[8192];
    while (fgets(buffer, sizeof(buffer), pipe)) json += buffer;
#ifdef _WIN32
    int rc = _pclose(pipe);
#else
    int rc = pclose(pipe);
#endif
    rapidjson::Document doc;
    doc.Parse(json.c_str());
    if (rc || doc.HasParseError() || !doc.IsObject() ||
        !doc.HasMember("streams") || !doc["streams"].IsArray() || doc["streams"].Size() != 1 ||
        !doc.HasMember("packets") || !doc["packets"].IsArray() || doc["packets"].Empty()) return false;
    const auto& stream = doc["streams"][0];
    if (!stream.IsObject() ||
        !stream.HasMember("has_b_frames") || !stream["has_b_frames"].IsInt() || stream["has_b_frames"].GetInt() != 0 ||
        !stream.HasMember("width") || !stream["width"].IsInt() || stream["width"].GetInt() <= 0 ||
        !stream.HasMember("height") || !stream["height"].IsInt() || stream["height"].GetInt() <= 0 ||
        !stream.HasMember("nb_read_frames") || !stream["nb_read_frames"].IsString()) return false;
    try {
        if (std::stoull(stream["nb_read_frames"].GetString()) != doc["packets"].Size()) return false;
        for (const auto& packet : doc["packets"].GetArray()) {
            if (!packet.IsObject() || !packet.HasMember("pts_time") || !packet["pts_time"].IsString() ||
                !packet.HasMember("dts_time") || !packet["dts_time"].IsString()) return false;
            double pts = std::stod(packet["pts_time"].GetString());
            double dts = std::stod(packet["dts_time"].GetString());
            if (!std::isfinite(pts) || pts < 0 || pts > 1e12 || pts != dts) return false;
            uint64_t ms = static_cast<uint64_t>(std::llround(pts * 1000));
            if (times.empty() ? ms != 0 : ms <= times.back().pts) return false;
            times.push_back({ms, static_cast<int64_t>(times.size())});
        }
    } catch (...) { return false; }
    if (forward) forward->set_image_size(stream["width"].GetInt(), stream["height"].GetInt());
    return true;
}
}

bool read_indexed_mp4_metadata(const std::string& input, const std::vector<uint8_t>& data,
                              size_t start, std::vector<MetadataEvent>& events,
                              std::vector<VideoFrameTime>& times, ForwardPrivacy* forward) {
    auto fail = []() {
        fprintf(stderr, "Privacy rendering refused: no verified mapping for MP4 metadata "
                        "(requires Streamax clock footer, ordered in-range sample indices, "
                        "and non-reordered video starting at zero).\n");
        return false;
    };
    if (data.size() < 56 || start == SIZE_MAX || start >= data.size() - 56) return fail();
    const size_t end = data.size() - 56;
    const auto* footer = data.data() + end;
    if (u64(footer) != start || u32(footer + 44) != 0xff313233 ||
        u64(footer + 48) != 0 || u64(footer + 8) == 0) return fail();
    std::vector<VideoFrameTime> timeline;
    if (!probe(input, timeline, forward)) return fail();
    const uint64_t origin = u64(footer + 8);
    if (origin > UINT64_MAX - timeline.back().pts) return fail();
    struct Record { size_t offset; uint32_t index; };
    std::vector<Record> records;
    int channel = -1;
    for (size_t off = start; off < end;) {
        MediaFrameHeader header;
        if (!media_frame_parse_fixed(data.data() + off, end - off, header) ||
            !is_video_frame(header.frame_type) || header.frame_length != 0 ||
            header.total_header > end - off || header.extend_length < 12) return fail();
        if (channel >= 0 && channel != header.channel) return fail();
        channel = header.channel;
        const auto* ext = data.data() + off + MEDIA_FRAME_FIXED_HDR;
        // This exporter-specific type-5 entry is an index, not AudioInfo.
        if (u32(ext) != 0x00000c05 || u32(ext + 4) != 20201030) return fail();
        uint32_t index = u32(ext + 8);
        if (index >= timeline.size() || (!records.empty() && index <= records.back().index)) return fail();
        for (size_t pos = 12; pos < header.extend_length;) {
            if (header.extend_length - pos < 4) return fail();
            size_t len = u32(ext + pos) >> 8;
            if (len < 4 || len > header.extend_length - pos) return fail();
            if (ext[pos] == 13 && (len < 20 || u32(ext + pos + 16) > len - 20)) return fail();
            pos += len;
        }
        records.push_back({off, index});
        off += header.total_header;
    }
    if (records.empty()) return fail();
    for (auto& sample : timeline) sample.pts += origin;
    if (forward) forward->set_external_times(timeline);
    std::vector<MetadataEvent> parsed;
    for (const auto& record : records) {
        MediaFrameHeader header;
        media_frame_parse_fixed(data.data() + record.offset, end - record.offset, header);
        const auto* ext = data.data() + record.offset + MEDIA_FRAME_FIXED_HDR;
        media_frame_parse_extensions(ext + 12, header.extend_length - 12, record.index, header);
        parsed.insert(parsed.end(), header.extension_events.begin(), header.extension_events.end());
        if (forward && !forward->read_frame(header, ext, header.extend_length, record.index)) {
            fprintf(stderr, "Forward metadata failed: %s\n", forward->error().c_str());
            return false;
        }
    }
    events.insert(events.end(), parsed.begin(), parsed.end());
    times = std::move(timeline);
    fprintf(stderr, "MP4 indexed metadata: %zu records mapped to %zu video samples.\n", records.size(), times.size());
    fprintf(stderr, "MP4 camera-clock anchor: %llu ms; AI PTS matched against anchor + container PTS.\n",
            static_cast<unsigned long long>(origin));
    return true;
}
}
