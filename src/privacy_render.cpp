#include "privacy_render.h"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <utility>

namespace media_extractor {
namespace fs = std::filesystem;
namespace {
struct FaceRect {
    int64_t frame = 0, last_frame = 0;
    int x = 0, y = 0, w = 0, h = 0;
};
static constexpr int64_t kFacePrivacyHoldFrames = 30;
static std::string shell_quote(const std::string& s)
{
#ifdef _WIN32
    std::string out = "\"";
    for (char c : s) out += c == '"' ? "\\\"" : std::string(1, c);
    return out + "\"";
#else
    std::string out = "'";
    for (char c : s) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += "'";
    return out;
#endif
}

static std::vector<std::pair<std::string, std::string>>
parse_pipe_fields(const std::string& text)
{
    std::vector<std::pair<std::string, std::string>> fields;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('|', pos);
        std::string part = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        size_t eq = part.find('=');
        if (eq != std::string::npos && eq > 0)
            fields.push_back({part.substr(0, eq), part.substr(eq + 1)});
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return fields;
}

static std::string pipe_field_value(const std::string& text, const char* key)
{
    for (const auto& kv : parse_pipe_fields(text)) {
        if (kv.first == key)
            return kv.second;
    }
    return {};
}

static bool parse_slash_ints(const std::string& value,
                             std::vector<int>& out,
                             size_t max_items = 8)
{
    out.clear();
    size_t pos = 0;
    while (pos < value.size() && out.size() < max_items) {
        while (pos < value.size() &&
               !(std::isdigit((unsigned char)value[pos]) ||
                 value[pos] == '-' || value[pos] == '+')) {
            ++pos;
        }
        if (pos >= value.size())
            break;
        char* endp = nullptr;
        long v = std::strtol(value.c_str() + pos, &endp, 10);
        if (endp == value.c_str() + pos)
            return false;
        out.push_back((int)v);
        pos = (size_t)(endp - value.c_str());
    }
    return !out.empty();
}

static bool parse_rect_value(const std::string& value,
                             int& x, int& y, int& w, int& h)
{
    std::vector<int> nums;
    if (!parse_slash_ints(value, nums, 8) || nums.size() < 4)
        return false;
    x = nums[0];
    y = nums[1];
    w = nums[2];
    h = nums[3];
    return w > 8 && h > 8;
}

static bool parse_frame_size_from_events(const std::vector<MetadataEvent>& events,
                                         int& width,
                                         int& height)
{
    for (const auto& ev : events) {
        std::string spf = pipe_field_value(ev.text, "spf");
        std::string rx = pipe_field_value(ev.text, "rx");
        std::vector<int> nums;
        if (!spf.empty() && parse_slash_ints(spf, nums, 8) && nums.size() >= 4 &&
            nums[2] > 0 && nums[3] > 0) {
            width = nums[2];
            height = nums[3];
            return true;
        }
        if (!spf.empty() && nums.size() >= 5 && nums[3] > 0 && nums[4] > 0) {
            width = nums[3];
            height = nums[4];
            return true;
        }
        if (!rx.empty() && parse_slash_ints(rx, nums, 8) && nums.size() >= 4 &&
            nums[2] > 0 && nums[3] > 0) {
            width = nums[2];
            height = nums[3];
            return true;
        }
    }
    return false;
}

static std::vector<FaceRect> extract_face_rects_from_events(
    const std::vector<MetadataEvent>& events,
    const media_extractor::MetadataTimeline& timeline, media_extractor::TimingStats& timing)
{
    std::map<int64_t, FaceRect> by_frame;
    std::map<int64_t, uint64_t> distances;
    int frame_w = 1920;
    int frame_h = 1080;
    parse_frame_size_from_events(events, frame_w, frame_h);

    for (const auto& ev : events) {
        if (ev.kind != "T13_DATA")
            continue;
        std::string rect_value = pipe_field_value(ev.text, "face_rect");
        if (rect_value.empty())
            continue;

        int x = 0, y = 0, w = 0, h = 0;
        if (!parse_rect_value(rect_value, x, y, w, h))
            continue;

        const std::string pts_text = pipe_field_value(ev.text, "ai_pts");
        uint64_t pts = 0;
        auto parsed_pts = std::from_chars(pts_text.data(), pts_text.data() + pts_text.size(), pts);
        if (parsed_pts.ec != std::errc{} || parsed_pts.ptr != pts_text.data() + pts_text.size()) {
            ++timing.unmatched;
            continue;
        }
        auto match = timeline.match(pts, ev.frame_index);
        timing.add(match.status);
        if (match.status != media_extractor::TimeMatchStatus::Matched) continue;

        // Expand the detected face/head box slightly for privacy coverage.
        int mx = std::max(8, w / 8);
        int my = std::max(8, h / 8);
        x -= mx;
        y -= my;
        w += mx * 2;
        h += my * 2;

        x = std::max(0, x);
        y = std::max(0, y);
        if (frame_w > 0 && x + w > frame_w)
            w = frame_w - x;
        if (frame_h > 0 && y + h > frame_h)
            h = frame_h - y;
        if (w <= 8 || h <= 8)
            continue;

        FaceRect r;
        r.frame = match.frame;
        r.last_frame = match.segment_last_frame;
        r.x = x;
        r.y = y;
        r.w = w;
        r.h = h;
        // Several late results can map to one video frame. Keep the nearest.
        if (!by_frame.count(r.frame) || match.distance_ms < distances[r.frame]) {
            by_frame[r.frame] = r;
            distances[r.frame] = match.distance_ms;
        }
    }

    std::vector<FaceRect> out;
    out.reserve(by_frame.size());
    for (const auto& kv : by_frame)
        out.push_back(kv.second);
    return out;
}

static std::vector<FaceRect> coalesce_face_rects(std::vector<FaceRect> rects)
{
    if (rects.empty())
        return rects;

    std::sort(rects.begin(), rects.end(),
              [](const FaceRect& a, const FaceRect& b) {
                  return a.frame < b.frame;
              });

    std::vector<FaceRect> ranges;
    ranges.reserve(rects.size());
    for (size_t i = 0; i < rects.size(); ++i) {
        if (i > 0 && rects[i].frame <= rects[i - 1].frame)
            continue;

        int64_t start = rects[i].frame;
        int64_t end = std::min(start + kFacePrivacyHoldFrames, rects[i].last_frame);
        if (i + 1 < rects.size()) {
            int64_t next = rects[i + 1].frame;
            if (next > start && next <= end + 1)
                end = next - 1;
        }

        FaceRect r = rects[i];
        r.frame = (start << 32) | (end & 0xffffffffLL);
        ranges.push_back(r);
    }

    if (ranges.empty())
        return ranges;

    auto close_rect = [](const FaceRect& a, const FaceRect& b) {
        return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
    };

    std::vector<FaceRect> out;
    FaceRect cur = ranges[0];
    int64_t cur_start = cur.frame >> 32;
    int64_t cur_end = cur.frame & 0xffffffffLL;
    for (size_t i = 1; i < ranges.size(); ++i) {
        int64_t start = ranges[i].frame >> 32;
        int64_t end = ranges[i].frame & 0xffffffffLL;
        bool same_region = close_rect(cur, ranges[i]);
        if (start <= cur_end + 1 && same_region && cur.last_frame == ranges[i].last_frame) {
            cur_end = std::max(cur_end, end);
            continue;
        }
        cur.frame = (cur_start << 32) | (cur_end & 0xffffffffLL);
        out.push_back(cur);
        cur = ranges[i];
        cur_start = start;
        cur_end = end;
    }
    cur.frame = (cur_start << 32) | (cur_end & 0xffffffffLL);
    out.push_back(cur);
    return out;
}

static bool write_face_filter_script(const fs::path& path,
                                     const std::vector<FaceRect>& rects,
                                     PrivacyMode mode)
{
    FILE* fp = fopen(path.string().c_str(), "wb");
    if (!fp) {
        fprintf(stderr, "Cannot write face filter script: %s\n",
                path.string().c_str());
        return false;
    }

    if (rects.empty()) {
        fputs("null\n", fp);
        fclose(fp);
        return true;
    }

    auto ranges = coalesce_face_rects(rects);
    for (size_t i = 0; i < ranges.size(); ++i) {
        FaceRect r = ranges[i];
        int64_t start = r.frame >> 32;
        int64_t end = r.frame & 0xffffffffLL;
        if (i)
            fputs(",\n", fp);
        const char* color = mode == PrivacyMode::Frame
                          ? "lime@0.95" : "black@1.0";
        const char* thick = mode == PrivacyMode::Frame ? "4" : "fill";
        std::fprintf(fp,
                     "drawbox=x=%d:y=%d:w=%d:h=%d:color=%s:t=%s:enable='between(n,%lld,%lld)'",
                     r.x, r.y, r.w, r.h, color, thick,
                     (long long)start, (long long)end);
    }
    fputc('\n', fp);
    fclose(fp);
    return true;
}

static bool write_face_blur_filter_script(const fs::path& path,
                                          const std::vector<FaceRect>& rects,
                                          int blur_strength,
                                          const char* input_label = "0:v")
{
    FILE* fp = fopen(path.string().c_str(), "wb");
    if (!fp) {
        fprintf(stderr, "Cannot write face blur filter script: %s\n",
                path.string().c_str());
        return false;
    }

    blur_strength = std::max(0, std::min(100, blur_strength));
    auto ranges = coalesce_face_rects(rects);
    if (ranges.empty() || blur_strength == 0) {
        fprintf(fp, "[%s]null[vout]\n", input_label);
        fclose(fp);
        return true;
    }

    const int luma_radius = std::max(1, 1 + (blur_strength * 9 + 50) / 100);
    const int chroma_radius = std::max(1, luma_radius / 2);
    const int blur_power = std::max(1, 1 + (blur_strength * 2 + 50) / 100);
    for (size_t i = 0; i < ranges.size(); ++i) {
        FaceRect r = ranges[i];
        int64_t start = r.frame >> 32;
        int64_t end = r.frame & 0xffffffffLL;
        const char* in_prefix = i == 0 ? "[" : "[v";
        if (i == 0) {
            std::fprintf(fp, "[%s]split=2[base%zu][crop%zu];\n",
                         input_label, i, i);
        } else {
            std::fprintf(fp, "%s%zu]split=2[base%zu][crop%zu];\n",
                         in_prefix, i - 1, i, i);
        }
        std::fprintf(fp,
                     "[crop%zu]crop=w=%d:h=%d:x=%d:y=%d,"
                     "boxblur=luma_radius=%d:luma_power=%d:chroma_radius=%d:chroma_power=%d"
                     "[blur%zu];\n",
                     i, r.w, r.h, r.x, r.y,
                     luma_radius, blur_power, chroma_radius, blur_power, i);
        std::fprintf(fp,
                     "[base%zu][blur%zu]overlay=x=%d:y=%d:enable='between(n,%lld,%lld)'%s\n",
                     i, i, r.x, r.y,
                     (long long)start, (long long)end,
                     i + 1 == ranges.size() ? "[vout]" :
                                               ("[v" + std::to_string(i) + "];").c_str());
    }

    fclose(fp);
    return true;
}

static bool probe_video_size_ffprobe(const std::string& input_path,
                                     int& width,
                                     int& height)
{
    std::string cmd =
        "ffprobe -v error -select_streams v:0 "
        "-show_entries stream=width,height -of csv=p=0:s=x " +
        shell_quote(input_path);
#ifdef _WIN32
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe)
        return false;
    char buf[128] = {};
    std::string out;
    while (fgets(buf, sizeof(buf), pipe))
        out += buf;
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    size_t x = out.find('x');
    if (x == std::string::npos)
        return false;
    try {
        int w = std::stoi(out.substr(0, x));
        int h = std::stoi(out.substr(x + 1));
        if (w <= 0 || h <= 0)
            return false;
        width = w;
        height = h;
        return true;
    } catch (...) {
        return false;
    }
}

static void scale_face_rects_to_video(std::vector<FaceRect>& rects,
                                      int metadata_w,
                                      int metadata_h,
                                      int video_w,
                                      int video_h,
                                      PrivacyMode mode)
{
    if (rects.empty() || metadata_w <= 0 || metadata_h <= 0 ||
        video_w <= 0 || video_h <= 0)
        return;
    const double sx = (double)video_w / (double)metadata_w;
    const double sy = (double)video_h / (double)metadata_h;
    for (auto& r : rects) {
        r.x = (int)std::llround((double)r.x * sx);
        r.y = (int)std::llround((double)r.y * sy);
        r.w = std::max(2, (int)std::llround((double)r.w * sx));
        r.h = std::max(2, (int)std::llround((double)r.h * sy));
        r.x = std::max(0, std::min(r.x, video_w - 2));
        r.y = std::max(0, std::min(r.y, video_h - 2));
        int max_w = video_w - r.x;
        int max_h = video_h - r.y;
        r.w = std::max(2, std::min(r.w, max_w));
        r.h = std::max(2, std::min(r.h, max_h));
    }
}

static int transcode_forward_privacy_mp4(const std::string& input_path,
                                         const std::string& output_path,
                                         const media_extractor::ForwardPrivacy* metadata,
                                         bool blur, int strength,
                                         const std::string& ffmpeg_path,
                                         const std::vector<MetadataEvent>& events,
                                         const std::vector<media_extractor::VideoFrameTime>& face_times,
                                         PrivacyMode face_mode,
                                         int face_strength)
{
    if (!metadata || !metadata->error().empty() ||
        (metadata->observations().empty() && face_mode == PrivacyMode::None)) {
        fprintf(stderr, "Forward privacy failed: %s. No forward output generated.\n",
                metadata && !metadata->error().empty() ? metadata->error().c_str()
                : "no supported ADAS vehicle/pedestrian metadata found");
        return 1;
    }
    if (metadata->observations().empty())
        fprintf(stderr, "Warning: no supported forward ADAS metadata; no forward objects will be hidden. Applying requested face mode only.\n");
    std::error_code ec;
    if (fs::equivalent(input_path, output_path, ec) ||
        fs::absolute(input_path).lexically_normal() == fs::absolute(output_path).lexically_normal()) {
        fprintf(stderr, "Forward privacy requires an output different from the input.\n");
        return 1;
    }
    int width = 0, height = 0;
    if (!probe_video_size_ffprobe(input_path, width, height)) {
        fprintf(stderr, "Cannot probe forward video dimensions; ffprobe is required.\n");
        return 1;
    }
    const auto& times = metadata->times();
    media_extractor::TimingStats timing;
    auto regions = media_extractor::forward_regions(metadata->observations(), times, width, height, blur, &timing);
    fprintf(stderr, "Forward timing: matched=%zu outside-clip=%zu unmatched=%zu (maximum PTS distance: 50 ms).\n",
            timing.matched, timing.outside, timing.unmatched);
    if (times.empty() || timing.unmatched || (timing.matched == 0 && !metadata->observations().empty())) {
        fprintf(stderr, "Forward privacy failed: no reliable AI/video PTS mapping for all in-clip observations. "
                        "No output generated; carrier-frame fallback is disabled.\n");
        return 1;
    }
    size_t vehicles = 0, pedestrians = 0;
    for (const auto& r : regions) {
        if (r.kind == 1) ++vehicles;
        else if (r.kind == 7) ++pedestrians;
    }
    fprintf(stderr, "Forward ADAS: %zu vehicle and %zu pedestrian region intervals; "
                    "replace on fresh results, hold missing updates up to 30 frames.\n",
            vehicles, pedestrians);
    fprintf(stderr, "Forward privacy covers whole vehicles/pedestrians, not individual faces or plates.\n");
    if (regions.empty()) fprintf(stderr, "Warning: ADAS metadata contains no usable object boxes; no forward regions will be hidden.\n");
    if (blur) fprintf(stderr, "Forward blur strength: %d/100%s\n", strength, strength == 0 ? " (disabled)" : "");

    fs::path parent = fs::path(output_path).parent_path();
    ec.clear();
    if (!parent.empty()) fs::create_directories(parent, ec);
    if (ec) { fprintf(stderr, "Cannot create forward output directory: %s\n", ec.message().c_str()); return 1; }
    fs::path scratch;
    auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 100; ++attempt) {
        scratch = parent / (".media_forward_" + std::to_string(tick) + "_" + std::to_string(attempt));
        if (fs::create_directory(scratch, ec)) break;
        if (attempt == 99 || ec) { fprintf(stderr, "Cannot create forward temporary directory.\n"); return 1; }
    }
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code ignored; fs::remove_all(path, ignored); }
    } cleanup{scratch};
    const bool with_face = face_mode != PrivacyMode::None;
    fs::path face_filter = scratch / "face.txt";
    if (with_face) {
        media_extractor::MetadataTimeline timeline(face_times);
        media_extractor::TimingStats face_timing;
        auto rects = extract_face_rects_from_events(events, timeline, face_timing);
        fprintf(stderr, "Face timing: matched=%zu outside-clip=%zu unmatched=%zu (maximum PTS distance: 50 ms).\n",
                face_timing.matched, face_timing.outside, face_timing.unmatched);
        if (timeline.empty() || face_timing.unmatched ||
            (face_timing.outside && !face_timing.matched)) {
            fprintf(stderr, "Face privacy failed: no reliable AI/video PTS mapping. No output generated.\n");
            return 1;
        }
        int metadata_w = 1920, metadata_h = 1080;
        parse_frame_size_from_events(events, metadata_w, metadata_h);
        scale_face_rects_to_video(rects, metadata_w, metadata_h, width, height, face_mode);
        if (rects.empty())
            fprintf(stderr, "Warning: --face-* requested but no face_rect metadata was found; no face overlay.\n");
        if (face_mode == PrivacyMode::Blur) {
            fprintf(stderr, "Face blur strength: %d/100\n", face_strength);
            if (!write_face_blur_filter_script(face_filter, rects, face_strength, "forwardout")) return 1;
        } else if (!write_face_filter_script(face_filter, rects, face_mode)) return 1;
    }
    const char* output_label = with_face ? "[forwardout];\n" : "[vout]\n";
    fs::path filter = scratch / "filter.txt";
    FILE* fp = fopen(filter.string().c_str(), "wb");
    if (!fp) { fprintf(stderr, "Cannot write forward filter script.\n"); return 1; }
    if (regions.empty() || (blur && strength == 0)) {
        fprintf(fp, "[0:v:0]null%s", output_label);
    } else {
        // Blur the image once, then select only the union of object regions.
        // This avoids one crop/blur/overlay branch per object per frame.
        fputs(blur ? "[0:v:0]format=yuv420p,split=3[original][soft][mask];\n[mask]lut=y=0:u=0:v=0"
                   : "[0:v:0]null", fp);
        for (const auto& r : regions) {
            fprintf(fp, ",\ndrawbox=x=%d:y=%d:w=%d:h=%d:color=%s:t=%s:enable='between(n,%lld,%lld)'",
                    r.x, r.y, r.width, r.height,
                    blur ? "white" : (r.kind == 1 ? "lime" : "yellow"),
                    blur ? "fill" : "3", (long long)r.first, (long long)r.last);
        }
        if (blur) {
            int radius = std::min(std::min(width, height) / 4, 1 + (strength * 19 + 50) / 100);
            int power = 1 + (strength * 3 + 50) / 100;
            fprintf(fp, ",lut=y='if(gt(val,0),255,0)':u='if(gt(val,0),255,0)':"
                        "v='if(gt(val,0),255,0)'[regions];\n[soft]boxblur=luma_radius=%d:luma_power=%d:"
                        "chroma_radius=%d:chroma_power=%d[blurred];\n"
                        "[original][blurred][regions]maskedmerge%s",
                    radius, power, radius / 2, power, output_label);
        } else fputs(output_label, fp);
    }
    if (with_face) {
        FILE* face_fp = fopen(face_filter.string().c_str(), "rb");
        if (!face_fp) { fclose(fp); return 1; }
        if (face_mode != PrivacyMode::Blur) fputs("[forwardout]", fp);
        char buffer[4096];
        size_t count;
        while ((count = fread(buffer, 1, sizeof(buffer), face_fp)) != 0)
            fwrite(buffer, 1, count, fp);
        bool read_failed = ferror(face_fp) != 0;
        fclose(face_fp);
        if (read_failed) { fclose(fp); return 1; }
        if (face_mode != PrivacyMode::Blur) fputs("[vout]\n", fp);
    }
    bool write_failed = ferror(fp) != 0;
    if (fclose(fp) != 0) write_failed = true;
    if (write_failed) { fprintf(stderr, "Cannot finish forward filter script.\n"); return 1; }
    fs::path rendered = scratch / "rendered.mp4";
    std::string cmd = shell_quote(ffmpeg_path) +
        " -y -hide_banner -loglevel warning -i " + shell_quote(input_path) +
        " -filter_complex_script " + shell_quote(filter.string()) +
        " -map " + shell_quote("[vout]") + " -map " + shell_quote("0:a?") +
        " -map " + shell_quote("0:s?") +
        " -c:v libx264 -preset veryfast -crf 27 -pix_fmt yuv420p -fps_mode passthrough -enc_time_base:v -1"
        " -c:a copy -c:s mov_text -movflags +faststart " + shell_quote(rendered.string());
    fprintf(stderr, "Forward privacy transcode: %s\n", output_path.c_str());
    if (std::system(cmd.c_str()) != 0) { fprintf(stderr, "Forward privacy ffmpeg transcode failed.\n"); return 1; }
    fs::copy_file(rendered, output_path, fs::copy_options::overwrite_existing, ec);
    if (ec) { fprintf(stderr, "Cannot publish forward output: %s\n", ec.message().c_str()); return 1; }
    return 0;
}

static int transcode_face_privacy_mp4(const std::string& input_path,
                                      const std::string& output_path,
                                      const std::vector<MetadataEvent>& events,
                                      const std::vector<media_extractor::VideoFrameTime>& times,
                                      PrivacyMode mode,
                                      int face_blur_strength,
                                      const std::string& ffmpeg_path)
{
    if (mode == PrivacyMode::None)
        return 0;

    fs::path out_path(output_path);
    fs::path parent = out_path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
        if (ec) {
            fprintf(stderr, "Cannot create output directory %s: %s\n",
                    parent.string().c_str(), ec.message().c_str());
            return 1;
        }
    }

    media_extractor::MetadataTimeline timeline(times);
    media_extractor::TimingStats timing;
    std::vector<FaceRect> rects = extract_face_rects_from_events(events, timeline, timing);
    fprintf(stderr, "Face timing: matched=%zu outside-clip=%zu unmatched=%zu (maximum PTS distance: 50 ms).\n",
            timing.matched, timing.outside, timing.unmatched);
    if (timeline.empty() || timing.unmatched || (timing.outside && !timing.matched)) {
        fprintf(stderr, "Face privacy failed: no reliable AI/video PTS mapping. "
                        "No output generated; carrier-frame fallback is disabled.\n");
        return 1;
    }
    int metadata_w = 1920;
    int metadata_h = 1080;
    parse_frame_size_from_events(events, metadata_w, metadata_h);
    int video_w = 0;
    int video_h = 0;
    if (probe_video_size_ffprobe(input_path, video_w, video_h)) {
        scale_face_rects_to_video(rects, metadata_w, metadata_h,
                                  video_w, video_h, mode);
    }
    if (rects.empty()) {
        fprintf(stderr,
                "Warning: --face-* requested but no face_rect metadata was found; "
                "transcoding without face overlay.\n");
    } else {
        fprintf(stderr,
                "Face privacy: %zu face metadata frame(s); holding last region up to %lld frame(s)\n",
                rects.size(), (long long)kFacePrivacyHoldFrames);
        if (mode == PrivacyMode::Blur) {
            fprintf(stderr, "Face blur strength: %d/100\n",
                    std::max(0, std::min(100, face_blur_strength)));
        }
    }

    fs::path filter_path = out_path;
    filter_path += ".face_filter.txt";
    std::string cmd = shell_quote(ffmpeg_path) +
        " -y -hide_banner -loglevel warning -i " + shell_quote(input_path);
    if (mode == PrivacyMode::Blur) {
        if (!write_face_blur_filter_script(filter_path, rects,
                                           face_blur_strength))
            return 1;
        cmd += " -filter_complex_script " + shell_quote(filter_path.string()) +
               " -map " + shell_quote("[vout]") +
               " -map " + shell_quote("0:a?") +
               " -map " + shell_quote("0:s?");
    } else {
        if (!write_face_filter_script(filter_path, rects, mode))
            return 1;
        cmd += " -filter_script:v " + shell_quote(filter_path.string()) +
               " -map 0";
    }
    cmd += " -c:v libx264 -preset veryfast -crf 27 -pix_fmt yuv420p -fps_mode passthrough -enc_time_base:v -1"
           " -c:a copy -c:s copy -movflags +faststart " +
           shell_quote(output_path);

    fprintf(stderr, "Face privacy transcode: %s\n", output_path.c_str());
    int rc = std::system(cmd.c_str());
    std::error_code ec;
    fs::remove(filter_path, ec);
    if (rc != 0) {
        fprintf(stderr, "ffmpeg face privacy transcode failed with code %d\n", rc);
        return 1;
    }
    return 0;
}


} // namespace
int render_privacy(const std::string& input, const std::string& output,
                   const std::vector<MetadataEvent>& events,
                   const std::vector<VideoFrameTime>& times,
                   PrivacyMode face, int face_strength,
                   PrivacyMode forward_mode, int forward_strength,
                   const ForwardPrivacy* forward, const std::string& ffmpeg) {
    if (forward_mode != PrivacyMode::None)
        return transcode_forward_privacy_mp4(input, output, forward,
            forward_mode == PrivacyMode::ForwardBlur, forward_strength,
            ffmpeg, events, times, face, face_strength);
    return transcode_face_privacy_mp4(input, output, events, times, face, face_strength, ffmpeg);
}
} // namespace media_extractor
