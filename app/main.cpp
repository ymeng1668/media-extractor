#include "media_frame.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

namespace fs = std::filesystem;
using namespace media_extractor;

namespace {

struct Options {
    std::string input_file;
    std::string input_folder;
    std::string output_spec = ".raw";
    std::string batch_spec = ".raw";
    bool write_json = false;
    bool write_text = false;
    enum class FacePrivacyMode {
        None,
        Mask,
        Blur,
        Frame,
    } face_privacy_mode = FacePrivacyMode::None;
    int face_blur_strength = 25;
    std::string ffmpeg_path = "ffmpeg";
};

struct FaceRect {
    int64_t frame = 0;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

static constexpr int64_t kFacePrivacyHoldFrames = 30;

struct Stats {
    uint64_t frames = 0;
    uint64_t video_frames = 0;
    uint64_t audio_frames = 0;
    uint64_t video_bytes = 0;
    uint64_t audio_bytes = 0;
    uint64_t type13_events = 0;
    uint64_t type15_events = 0;
    uint64_t gps_events = 0;
    bool h265 = false;
    bool saw_video = false;
    uint32_t video_fps = 0;
};

struct Output {
    FILE* video = nullptr;
    FILE* audio = nullptr;
    std::string base;
    std::string sidecar_base;
    std::string final_output_path;
    std::string video_path;
    std::string audio_path;
    bool mux_mode = false;
    std::vector<MetadataEvent> events;

    ~Output()
    {
        close();
    }

    void close()
    {
        if (video) {
            fclose(video);
            video = nullptr;
        }
        if (audio) {
            fclose(audio);
            audio = nullptr;
        }
    }

    FILE* get_video(bool h265)
    {
        if (video)
            return video;
        video_path = base + (h265 ? ".h265" : ".h264");
        video = fopen(video_path.c_str(), "wb");
        if (!video)
            std::fprintf(stderr, "Cannot open %s: %s\n", video_path.c_str(), std::strerror(errno));
        return video;
    }

    FILE* get_audio()
    {
        if (audio)
            return audio;
        audio_path = base + ".audio";
        audio = fopen(audio_path.c_str(), "wb");
        if (!audio)
            std::fprintf(stderr, "Cannot open %s: %s\n", audio_path.c_str(), std::strerror(errno));
        return audio;
    }
};

enum class InputFormat {
    StreamaxMedia,
    RawAnnexB,
    Mp4Container,
    Unknown,
};

static std::string lower_ascii(std::string s)
{
    for (char& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

static void print_help(const char* prog)
{
    std::printf(
        "media_extractor - Streamax media frame extractor\n\n"
        "Usage:\n"
        "  %s [options] <input.264|input.265|input.mp4>\n"
        "  %s --input-folder <dir> --r <spec> [options]\n\n"
        "Options:\n"
        "  -h, --help              Show help\n"
        "  -i, --input-folder <d>  Recursively process .264/.265/.mp4 files\n"
        "  --o <spec>              Single-file output spec (default: .raw)\n"
        "  --r <spec>              Batch output spec (default: .raw)\n"
        "  --json                  Write metadata sidecar JSON\n"
        "  --text                  Write metadata sidecar text\n"
        "  --face-mask             Fill detected face_rect regions with black; outputs H.264 MP4\n"
        "  --face-blur[=0..100]    Blur detected face_rect regions; 0=off, 100=max (default: 25)\n"
        "  --face-frame            Draw boxes around detected face_rect regions; outputs H.264 MP4\n"
        "  --ffmpeg <path>         ffmpeg executable for privacy modes (default: ffmpeg)\n"
        "\n"
        "Output spec:\n"
        "  .raw                    Auto-name raw output next to input\n"
        "  /out/.raw               Auto-name raw output into /out\n"
        "  base.raw                Use base as output stem\n"
        "  .mp4/.mkv/.mov          Auto-name container next to input\n"
        "  /out/.mp4               Auto-name container into /out\n"
        "  result.mp4              Exact container path for single-file mode\n\n"
        "Scope:\n"
        "  This tool parses documented Streamax media frames, raw Annex-B,\n"
        "  and existing MP4 containers.\n"
        "  It does not support packet streams, device login/query/download,\n"
        "  live view, server mode, network protocols, or video decryption.\n",
        prog, prog);
}

static bool parse_args(int argc, char** argv, Options& opt)
{
    auto parse_int = [](const char* name, const std::string& value,
                        int minv, int maxv, int& out) -> bool {
        try {
            size_t used = 0;
            int v = std::stoi(value, &used);
            if (used != value.size() || v < minv || v > maxv) {
                std::fprintf(stderr, "Error: %s must be in [%d, %d], got %s\n",
                             name, minv, maxv, value.c_str());
                return false;
            }
            out = v;
            return true;
        } catch (...) {
            std::fprintf(stderr, "Error: invalid integer for %s: %s\n",
                         name, value.c_str());
            return false;
        }
    };

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_help(argv[0]);
            std::exit(0);
        } else if ((arg == "-i" || arg == "--input-folder") && i + 1 < argc) {
            opt.input_folder = argv[++i];
        } else if (arg == "--o" && i + 1 < argc) {
            opt.output_spec = argv[++i];
        } else if (arg == "--r" && i + 1 < argc) {
            opt.batch_spec = argv[++i];
        } else if (arg == "--json") {
            opt.write_json = true;
        } else if (arg == "--text") {
            opt.write_text = true;
        } else if (arg == "--face-mask") {
            if (opt.face_privacy_mode != Options::FacePrivacyMode::None) {
                std::fprintf(stderr, "Error: use only one of --face-mask, --face-blur, or --face-frame.\n");
                return false;
            }
            opt.face_privacy_mode = Options::FacePrivacyMode::Mask;
        } else if (arg == "--face-blur" || arg.rfind("--face-blur=", 0) == 0) {
            if (opt.face_privacy_mode != Options::FacePrivacyMode::None) {
                std::fprintf(stderr, "Error: use only one of --face-mask, --face-blur, or --face-frame.\n");
                return false;
            }
            opt.face_privacy_mode = Options::FacePrivacyMode::Blur;
            opt.face_blur_strength = 25;
            if (arg.rfind("--face-blur=", 0) == 0) {
                std::string value = arg.substr(std::string("--face-blur=").size());
                if (value.empty() ||
                    !parse_int("--face-blur", value, 0, 100,
                               opt.face_blur_strength)) {
                    return false;
                }
            }
        } else if (arg == "--face-frame") {
            if (opt.face_privacy_mode != Options::FacePrivacyMode::None) {
                std::fprintf(stderr, "Error: use only one of --face-mask, --face-blur, or --face-frame.\n");
                return false;
            }
            opt.face_privacy_mode = Options::FacePrivacyMode::Frame;
        } else if (arg == "--ffmpeg" && i + 1 < argc) {
            opt.ffmpeg_path = argv[++i];
        } else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            return false;
        } else if (opt.input_file.empty()) {
            opt.input_file = arg;
        } else {
            std::fprintf(stderr, "Unexpected argument: %s\n", arg.c_str());
            return false;
        }
    }
    if (!opt.input_folder.empty() && !opt.input_file.empty()) {
        std::fprintf(stderr, "Error: use either --input-folder or one input file, not both.\n");
        return false;
    }
    if (opt.input_folder.empty() && opt.input_file.empty()) {
        std::fprintf(stderr, "Error: missing input file or --input-folder.\n");
        return false;
    }
    return true;
}

static bool read_file(const std::string& path, std::vector<uint8_t>& data)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    in.seekg(0, std::ios::end);
    std::streamoff size = in.tellg();
    if (size < 0)
        return false;
    in.seekg(0, std::ios::beg);
    data.resize((size_t)size);
    if (!data.empty())
        in.read(reinterpret_cast<char*>(data.data()), (std::streamsize)data.size());
    return in.good() || in.eof();
}

static InputFormat detect_input_format(const std::string& path,
                                       const std::vector<uint8_t>& data)
{
    const std::string ext = lower_ascii(fs::path(path).extension().string());
    if (data.size() >= 12 && data[4] == 'f' && data[5] == 't' &&
        data[6] == 'y' && data[7] == 'p') {
        return InputFormat::Mp4Container;
    }
    if (ext == ".mp4")
        return InputFormat::Mp4Container;
    if (data.size() >= 4 && data[0] == 0x00 && data[1] == 0x00 &&
        data[2] == 0x00 && data[3] == 0x01) {
        return InputFormat::RawAnnexB;
    }
    if (data.size() >= 3 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01)
        return InputFormat::RawAnnexB;
    if (find_media_frame_offset(data.data(), data.size(), std::min<size_t>(data.size(), 4096)) != SIZE_MAX)
        return InputFormat::StreamaxMedia;
    return InputFormat::Unknown;
}

static std::string sanitize_stem(std::string s)
{
    for (char& c : s) {
        if (c == '/' || c == '\\' || c == ':' || std::isspace((unsigned char)c))
            c = '_';
    }
    return s;
}

static std::string output_base_for(const std::string& input_path,
                                   const std::string& spec,
                                   bool batch_mode)
{
    fs::path in(input_path);
    fs::path spec_path(spec);
    std::string stem = sanitize_stem(in.stem().string() + "_" +
                                     in.extension().string().substr(1) + "_fmt");

    if (spec.empty() || spec == ".raw")
        return (in.parent_path() / stem).string();

    if (spec.size() >= 4 && spec.substr(spec.size() - 4) == ".raw") {
        if (spec_path.filename() == ".raw") {
            fs::path dir = spec_path.parent_path();
            if (dir.empty())
                dir = in.parent_path();
            return (dir / stem).string();
        }
        fs::path parent = spec_path.parent_path();
        fs::path base = spec_path.stem();
        if (parent.empty())
            parent = ".";
        return (parent / base).string();
    }

    if (fs::is_directory(spec_path))
        return (spec_path / stem).string();

    if (batch_mode && spec_path.extension().empty())
        return (spec_path / stem).string();

    return spec;
}

static bool is_container_extension(const std::string& ext)
{
    std::string e = lower_ascii(ext);
    return e == ".mp4" || e == ".mkv" || e == ".mov" || e == ".avi" ||
           e == ".ts" || e == ".flv" || e == ".webm";
}

static std::string output_extension_spec(const std::string& spec)
{
    fs::path p(spec);
    std::string ext = lower_ascii(p.extension().string());
    if (ext.empty() && is_container_extension(p.filename().string()))
        ext = lower_ascii(p.filename().string());
    return ext;
}

static bool is_container_spec(const std::string& spec)
{
    return is_container_extension(output_extension_spec(spec));
}

static std::string container_output_for(const std::string& input_path,
                                        const std::string& spec,
                                        bool batch_mode)
{
    fs::path in(input_path);
    fs::path spec_path(spec);
    std::string ext = output_extension_spec(spec);
    if (ext.empty())
        ext = ".mp4";
    std::string stem = sanitize_stem(in.stem().string() + "_" +
                                     in.extension().string().substr(1) + "_fmt");

    if (spec.empty() || spec == ext || spec_path.filename().string() == ext) {
        fs::path dir = spec_path.parent_path();
        if (dir.empty())
            dir = in.parent_path();
        return (dir / (stem + ext)).string();
    }

    if (batch_mode) {
        fs::path parent = spec_path.parent_path();
        if (parent.empty())
            parent = ".";
        return (parent / (stem + ext)).string();
    }

    return spec;
}

static void ensure_parent_dir(const std::string& path)
{
    fs::path parent = fs::path(path).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
    }
}

static std::string shell_quote(const std::string& s)
{
    std::string out = "'";
    for (char c : s) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += "'";
    return out;
}

static std::string event_field_value(const MetadataEvent& ev,
                                     const std::string& key)
{
    auto it = ev.fields.find(key);
    return it == ev.fields.end() ? std::string{} : it->second;
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
        std::vector<int> nums;
        std::string spf = event_field_value(ev, "spf");
        if (!spf.empty() && parse_slash_ints(spf, nums, 8)) {
            if (nums.size() >= 4 && nums[2] > 0 && nums[3] > 0) {
                width = nums[2];
                height = nums[3];
                return true;
            }
            if (nums.size() >= 5 && nums[3] > 0 && nums[4] > 0) {
                width = nums[3];
                height = nums[4];
                return true;
            }
        }
        std::string rx = event_field_value(ev, "rx");
        if (!rx.empty() && parse_slash_ints(rx, nums, 8) &&
            nums.size() >= 4 && nums[2] > 0 && nums[3] > 0) {
            width = nums[2];
            height = nums[3];
            return true;
        }
    }
    return false;
}

static std::vector<FaceRect> extract_face_rects_from_events(
    const std::vector<MetadataEvent>& events)
{
    std::map<int64_t, FaceRect> by_frame;
    int frame_w = 1920;
    int frame_h = 1080;
    parse_frame_size_from_events(events, frame_w, frame_h);

    for (const auto& ev : events) {
        if (ev.kind != "T13_DATA")
            continue;
        std::string rect_value = event_field_value(ev, "face_rect");
        if (rect_value.empty())
            continue;

        int x = 0, y = 0, w = 0, h = 0;
        if (!parse_rect_value(rect_value, x, y, w, h))
            continue;

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
        r.frame = std::max<int64_t>(0, (int64_t)ev.frame_index);
        r.x = x;
        r.y = y;
        r.w = w;
        r.h = h;
        by_frame.emplace(r.frame, r);
    }

    std::vector<FaceRect> out;
    out.reserve(by_frame.size());
    for (const auto& kv : by_frame)
        out.push_back(kv.second);
    return out;
}

static std::vector<FaceRect> coalesce_face_rects(std::vector<FaceRect> rects,
                                                 bool merge_nearby = false)
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
        int64_t end = start + kFacePrivacyHoldFrames;
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
        return std::abs(a.x - b.x) <= 3 && std::abs(a.y - b.y) <= 3 &&
               std::abs(a.w - b.w) <= 3 && std::abs(a.h - b.h) <= 3;
    };
    auto nearby_rect = [](const FaceRect& a, const FaceRect& b) {
        static constexpr int kFacePrivacyMergePad = 64;
        return a.x <= b.x + b.w + kFacePrivacyMergePad &&
               b.x <= a.x + a.w + kFacePrivacyMergePad &&
               a.y <= b.y + b.h + kFacePrivacyMergePad &&
               b.y <= a.y + a.h + kFacePrivacyMergePad;
    };
    auto union_rect = [](FaceRect& a, const FaceRect& b) {
        int x1 = std::min(a.x, b.x);
        int y1 = std::min(a.y, b.y);
        int x2 = std::max(a.x + a.w, b.x + b.w);
        int y2 = std::max(a.y + a.h, b.y + b.h);
        a.x = x1;
        a.y = y1;
        a.w = x2 - x1;
        a.h = y2 - y1;
    };

    std::vector<FaceRect> out;
    FaceRect cur = ranges[0];
    int64_t cur_start = cur.frame >> 32;
    int64_t cur_end = cur.frame & 0xffffffffLL;
    for (size_t i = 1; i < ranges.size(); ++i) {
        int64_t start = ranges[i].frame >> 32;
        int64_t end = ranges[i].frame & 0xffffffffLL;
        bool same_region = merge_nearby ? nearby_rect(cur, ranges[i])
                                        : close_rect(cur, ranges[i]);
        if (start <= cur_end + 1 && same_region) {
            cur_end = std::max(cur_end, end);
            if (merge_nearby)
                union_rect(cur, ranges[i]);
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

static bool probe_video_size_ffprobe(const std::string& input_path,
                                     int& width,
                                     int& height)
{
    std::string cmd =
        "ffprobe -v error -select_streams v:0 "
        "-show_entries stream=width,height -of csv=p=0:s=x " +
        shell_quote(input_path);
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe)
        return false;
    char buf[128] = {};
    std::string out;
    while (fgets(buf, sizeof(buf), pipe))
        out += buf;
    pclose(pipe);
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
                                      int video_h)
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
        r.w = std::max(2, std::min(r.w, video_w - r.x));
        r.h = std::max(2, std::min(r.h, video_h - r.y));
    }
}

static bool write_face_draw_filter_script(const fs::path& path,
                                          const std::vector<FaceRect>& rects,
                                          Options::FacePrivacyMode mode)
{
    FILE* fp = fopen(path.string().c_str(), "wb");
    if (!fp) {
        std::fprintf(stderr, "Cannot write face filter script: %s\n",
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
        const char* color = mode == Options::FacePrivacyMode::Frame
                          ? "lime@0.95" : "black@1.0";
        const char* thick = mode == Options::FacePrivacyMode::Frame ? "4" : "fill";
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
                                          int blur_strength)
{
    FILE* fp = fopen(path.string().c_str(), "wb");
    if (!fp) {
        std::fprintf(stderr, "Cannot write face blur filter script: %s\n",
                     path.string().c_str());
        return false;
    }

    blur_strength = std::max(0, std::min(100, blur_strength));
    auto ranges = coalesce_face_rects(rects, true);
    if (ranges.empty() || blur_strength == 0) {
        fputs("[0:v]null[vout]\n", fp);
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
        if (i == 0) {
            std::fprintf(fp, "[0:v]split=2[base%zu][crop%zu];\n", i, i);
        } else {
            std::fprintf(fp, "[v%zu]split=2[base%zu][crop%zu];\n",
                         i - 1, i, i);
        }
        std::fprintf(fp,
                     "[crop%zu]crop=w=%d:h=%d:x=%d:y=%d,"
                     "boxblur=luma_radius=%d:luma_power=%d:chroma_radius=%d:chroma_power=%d"
                     "[blur%zu];\n",
                     i, r.w, r.h, r.x, r.y,
                     luma_radius, blur_power, chroma_radius, blur_power, i);
        if (i + 1 == ranges.size()) {
            std::fprintf(fp,
                         "[base%zu][blur%zu]overlay=x=%d:y=%d:enable='between(n,%lld,%lld)'[vout]\n",
                         i, i, r.x, r.y,
                         (long long)start, (long long)end);
        } else {
            std::fprintf(fp,
                         "[base%zu][blur%zu]overlay=x=%d:y=%d:enable='between(n,%lld,%lld)'[v%zu];\n",
                         i, i, r.x, r.y,
                         (long long)start, (long long)end, i);
        }
    }

    fclose(fp);
    return true;
}

static int transcode_face_privacy_mp4(const std::string& input_path,
                                      const std::string& output_path,
                                      const std::vector<MetadataEvent>& events,
                                      Options::FacePrivacyMode mode,
                                      int blur_strength,
                                      const std::string& ffmpeg_path)
{
    if (mode == Options::FacePrivacyMode::None)
        return 0;

    ensure_parent_dir(output_path);
    std::vector<FaceRect> rects = extract_face_rects_from_events(events);
    int metadata_w = 1920;
    int metadata_h = 1080;
    parse_frame_size_from_events(events, metadata_w, metadata_h);
    int video_w = 0;
    int video_h = 0;
    if (probe_video_size_ffprobe(input_path, video_w, video_h)) {
        scale_face_rects_to_video(rects, metadata_w, metadata_h,
                                  video_w, video_h);
    }

    if (rects.empty()) {
        std::fprintf(stderr,
                     "Warning: --face-* requested but no face_rect metadata was found; "
                     "transcoding without face overlay.\n");
    } else {
        std::fprintf(stderr,
                     "Face privacy: %zu face metadata frame(s); holding last region up to %lld frame(s)\n",
                     rects.size(), (long long)kFacePrivacyHoldFrames);
        if (mode == Options::FacePrivacyMode::Blur) {
            std::fprintf(stderr, "Face blur strength: %d/100\n",
                         std::max(0, std::min(100, blur_strength)));
        }
    }

    fs::path filter_path = output_path;
    filter_path += ".face_filter.txt";
    std::string cmd = shell_quote(ffmpeg_path) +
        " -y -hide_banner -loglevel warning -i " + shell_quote(input_path);
    if (mode == Options::FacePrivacyMode::Blur) {
        if (!write_face_blur_filter_script(filter_path, rects, blur_strength))
            return 1;
        cmd += " -filter_complex_script " + shell_quote(filter_path.string()) +
               " -map " + shell_quote("[vout]") +
               " -map " + shell_quote("0:a?") +
               " -map " + shell_quote("0:s?");
    } else {
        if (!write_face_draw_filter_script(filter_path, rects, mode))
            return 1;
        cmd += " -filter_script:v " + shell_quote(filter_path.string()) +
               " -map 0";
    }
    cmd += " -c:v libx264 -preset veryfast -crf 27 -pix_fmt yuv420p"
           " -c:a copy -c:s copy -movflags +faststart " +
           shell_quote(output_path);

    std::fprintf(stderr, "Face privacy transcode: %s\n", output_path.c_str());
    int rc = std::system(cmd.c_str());
    std::error_code ec;
    fs::remove(filter_path, ec);
    if (rc != 0) {
        std::fprintf(stderr, "ffmpeg face privacy transcode failed with code %d\n", rc);
        return 1;
    }
    return 0;
}

static int mux_video_with_ffmpeg(const std::string& video_path,
                                 bool h265,
                                 uint32_t fps,
                                 const std::string& output_path)
{
    if (video_path.empty() || output_path.empty())
        return 1;
    std::string cmd = "ffmpeg -y -hide_banner -loglevel warning -fflags +genpts ";
    cmd += h265 ? "-f hevc " : "-f h264 ";
    if (fps > 0)
        cmd += "-r " + std::to_string(fps) + " ";
    cmd += "-i " + shell_quote(video_path) + " ";
    cmd += "-an -c:v copy -movflags +faststart ";
    cmd += shell_quote(output_path);
    std::fprintf(stderr, "Mux:     %s\n", output_path.c_str());
    int rc = std::system(cmd.c_str());
    if (rc != 0)
        std::fprintf(stderr, "ffmpeg mux failed with code %d\n", rc);
    return rc == 0 ? 0 : 1;
}

static int remux_container_with_ffmpeg(const std::string& input_path,
                                       const std::string& output_path)
{
    if (input_path.empty() || output_path.empty())
        return 1;

    std::error_code ec;
    fs::path in_abs = fs::absolute(input_path, ec);
    fs::path out_abs = fs::absolute(output_path, ec);
    if (!ec && in_abs == out_abs) {
        std::fprintf(stderr, "Container input already matches output: %s\n",
                     output_path.c_str());
        return 0;
    }

    std::string cmd = "ffmpeg -y -hide_banner -loglevel warning ";
    cmd += "-i " + shell_quote(input_path) + " ";
    cmd += "-map 0 -c copy -movflags +faststart ";
    cmd += shell_quote(output_path);
    std::fprintf(stderr, "Remux:   %s\n", output_path.c_str());
    int rc = std::system(cmd.c_str());
    if (rc != 0)
        std::fprintf(stderr, "ffmpeg remux failed with code %d\n", rc);
    return rc == 0 ? 0 : 1;
}

static int extract_container_video_with_ffmpeg(const std::string& input_path,
                                               const std::string& output_base)
{
    if (input_path.empty() || output_base.empty())
        return 1;

    const std::string out_h264 = output_base + ".h264";
    const std::string out_h265 = output_base + ".h265";
    std::string cmd = "codec=$(ffprobe -v error -select_streams v:0 "
                      "-show_entries stream=codec_name -of default=nw=1:nk=1 ";
    cmd += shell_quote(input_path);
    cmd += " | head -n 1); ";
    cmd += "if [ \"$codec\" = \"hevc\" ] || [ \"$codec\" = \"h265\" ]; then ";
    cmd += "ffmpeg -y -hide_banner -loglevel warning -i " + shell_quote(input_path) +
           " -map 0:v:0 -an -c:v copy -f hevc " + shell_quote(out_h265) + "; ";
    cmd += "else ";
    cmd += "ffmpeg -y -hide_banner -loglevel warning -i " + shell_quote(input_path) +
           " -map 0:v:0 -an -c:v copy -bsf:v h264_mp4toannexb -f h264 " +
           shell_quote(out_h264) + "; ";
    cmd += "fi";
    std::fprintf(stderr, "Extract: %s.{h264|h265}\n", output_base.c_str());
    int rc = std::system(cmd.c_str());
    if (rc != 0)
        std::fprintf(stderr, "ffmpeg video extract failed with code %d\n", rc);
    return rc == 0 ? 0 : 1;
}

static uint32_t be32_at(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t be64_at(const uint8_t* p)
{
    return ((uint64_t)be32_at(p) << 32) | be32_at(p + 4);
}

static bool is_printable_mp4_box_type(const uint8_t* p)
{
    for (int i = 0; i < 4; ++i) {
        if (p[i] < 0x20 || p[i] > 0x7e)
            return false;
    }
    return true;
}

static size_t mp4_top_level_box_end(const uint8_t* data, size_t len)
{
    size_t off = 0;
    size_t last_box_end = 0;

    while (off + 8 <= len) {
        uint64_t box_size = be32_at(data + off);
        if (!is_printable_mp4_box_type(data + off + 4))
            break;

        size_t header_size = 8;
        if (box_size == 1) {
            if (off + 16 > len)
                break;
            box_size = be64_at(data + off + 8);
            header_size = 16;
        } else if (box_size == 0) {
            box_size = len - off;
        }

        if (box_size < header_size || box_size > len - off)
            break;

        off += (size_t)box_size;
        last_box_end = off;
    }

    return last_box_end;
}

static size_t find_mp4_appended_media_offset(const uint8_t* data, size_t len)
{
    if (!data || len < 8)
        return SIZE_MAX;

    const size_t box_end = mp4_top_level_box_end(data, len);
    if (box_end >= len)
        return SIZE_MAX;

    const size_t remaining = len - box_end;
    const size_t rel = find_media_frame_offset(data + box_end, remaining, remaining);
    if (rel == SIZE_MAX)
        return SIZE_MAX;
    return box_end + rel;
}

static int write_json_sidecar(const std::string& path,
                              const std::vector<MetadataEvent>& events)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "Cannot open %s: %s\n", path.c_str(), std::strerror(errno));
        return 1;
    }

    rapidjson::StringBuffer buffer;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
    writer.SetIndent(' ', 2);

    writer.StartArray();
    for (const auto& ev : events) {
        writer.StartObject();
        writer.Key("video_frame_idx");
        writer.Uint64(ev.frame_index);
        writer.Key("duration_s");
        writer.Double(ev.duration_s);
        writer.Key("kind");
        writer.String(ev.kind.c_str(), static_cast<rapidjson::SizeType>(ev.kind.size()));
        writer.Key("text");
        writer.String(ev.text.c_str(), static_cast<rapidjson::SizeType>(ev.text.size()));
        writer.Key("fields");
        writer.StartObject();
        for (const auto& kv : ev.fields) {
            writer.Key(kv.first.c_str(), static_cast<rapidjson::SizeType>(kv.first.size()));
            writer.String(kv.second.c_str(), static_cast<rapidjson::SizeType>(kv.second.size()));
        }
        writer.EndObject();
        writer.EndObject();
    }
    writer.EndArray();

    fwrite(buffer.GetString(), 1, buffer.GetSize(), f);
    fputc('\n', f);
    fclose(f);
    return 0;
}

static int write_text_sidecar(const std::string& path,
                              const std::vector<MetadataEvent>& events)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "Cannot open %s: %s\n", path.c_str(), std::strerror(errno));
        return 1;
    }
    for (const auto& ev : events) {
        std::fprintf(f, "frame=%llu kind=%s %s\n",
                     (unsigned long long)ev.frame_index,
                     ev.kind.c_str(),
                     ev.text.c_str());
    }
    fclose(f);
    return 0;
}

static bool frame_payload_is_h265(const MediaFrameHeader& hdr)
{
    if (hdr.has_video_encoding && hdr.video_encoding <= 1)
        return hdr.video_encoding == 1;
    return is_h265_frame(hdr.frame_type);
}

static void append_frame_events(Output& out, const MediaFrameHeader& hdr, Stats& stats)
{
    for (auto ev : hdr.extension_events) {
        if (ev.kind == "T13_DATA")
            stats.type13_events++;
        else if (ev.kind == "T15_DATA")
            stats.type15_events++;
        else if (ev.kind == "GPS")
            stats.gps_events++;
        out.events.push_back(std::move(ev));
    }
}

static int process_streamax_media(const std::string& input_path,
                                  const std::vector<uint8_t>& data,
                                  Output& out,
                                  Stats& stats)
{
    size_t offset = find_media_frame_offset(data.data(), data.size(), std::min<size_t>(data.size(), 4096));
    if (offset == SIZE_MAX) {
        std::fprintf(stderr, "No Streamax media frame found: %s\n", input_path.c_str());
        return 1;
    }
    if (offset > 0)
        std::fprintf(stderr, "Detected wrapped media header; first frame at offset %zu.\n", offset);

    while (offset + kMediaFrameFixedHeaderSize <= data.size()) {
        MediaFrameHeader hdr;
        if (!media_frame_parse_fixed(data.data() + offset, data.size() - offset, hdr))
            break;
        uint64_t need = (uint64_t)hdr.total_header + hdr.frame_length;
        if (need < kMediaFrameFixedHeaderSize || offset + need > data.size())
            break;
        const uint8_t* ext = data.data() + offset + kMediaFrameFixedHeaderSize;
        const uint8_t* payload = data.data() + offset + hdr.total_header;

        if (hdr.extend_length > 0) {
            media_frame_parse_extensions(ext, hdr.extend_length, stats.video_frames, hdr);
            if (stats.video_fps == 0 && hdr.has_video_info && hdr.video_info.fps > 0)
                stats.video_fps = hdr.video_info.fps;
            append_frame_events(out, hdr, stats);
        }

        if (is_video_frame(hdr.frame_type) && hdr.frame_length > 0) {
            bool h265 = frame_payload_is_h265(hdr);
            FILE* f = out.get_video(h265);
            if (f)
                fwrite(payload, 1, hdr.frame_length, f);
            stats.video_frames++;
            stats.video_bytes += hdr.frame_length;
            stats.h265 = h265;
            stats.saw_video = true;
        } else if (hdr.frame_type == FrameType::Audio && hdr.frame_length > 0) {
            FILE* f = out.get_audio();
            if (f)
                fwrite(payload, 1, hdr.frame_length, f);
            stats.audio_frames++;
            stats.audio_bytes += hdr.frame_length;
        }

        stats.frames++;
        offset += (size_t)need;
    }

    return stats.frames > 0 ? 0 : 1;
}

static size_t process_streamax_metadata_payload(const uint8_t* data,
                                                size_t len,
                                                Output& out,
                                                Stats& stats)
{
    size_t offset = 0;
    size_t parsed_frames = 0;

    while (offset + kMediaFrameFixedHeaderSize <= len) {
        MediaFrameHeader hdr;
        if (!media_frame_parse_fixed(data + offset, len - offset, hdr))
            break;
        uint64_t need = (uint64_t)hdr.total_header + hdr.frame_length;
        if (need < kMediaFrameFixedHeaderSize || offset + need > len)
            break;

        const uint8_t* ext = data + offset + kMediaFrameFixedHeaderSize;
        if (hdr.extend_length > 0) {
            media_frame_parse_extensions(ext, hdr.extend_length, stats.video_frames, hdr);
            if (stats.video_fps == 0 && hdr.has_video_info && hdr.video_info.fps > 0)
                stats.video_fps = hdr.video_info.fps;
            append_frame_events(out, hdr, stats);
        }

        if (is_video_frame(hdr.frame_type)) {
            stats.video_frames++;
        } else if (hdr.frame_type == FrameType::Audio) {
            stats.audio_frames++;
        }

        stats.frames++;
        parsed_frames++;
        offset += (size_t)need;
    }

    return parsed_frames;
}

static int process_mp4_appended_metadata(const std::string& input_path,
                                         const std::vector<uint8_t>& data,
                                         Output& out,
                                         Stats& stats)
{
    const size_t start = find_mp4_appended_media_offset(data.data(), data.size());
    if (start == SIZE_MAX) {
        std::fprintf(stderr, "MP4 metadata tail: none found.\n");
        return 0;
    }

    const uint64_t before_frames = stats.frames;
    const uint64_t before_type13 = stats.type13_events;
    const uint64_t before_type15 = stats.type15_events;
    const uint64_t before_gps = stats.gps_events;
    const size_t parsed = process_streamax_metadata_payload(data.data() + start,
                                                            data.size() - start,
                                                            out,
                                                            stats);
    if (parsed == 0) {
        std::fprintf(stderr, "MP4 metadata tail: detected at offset %zu but no complete frames parsed.\n",
                     start);
        return 0;
    }

    std::fprintf(stderr,
                 "MP4 metadata tail: first frame at offset %zu in %s; frames=%llu type13=%llu type15=%llu gps=%llu\n",
                 start,
                 input_path.c_str(),
                 (unsigned long long)(stats.frames - before_frames),
                 (unsigned long long)(stats.type13_events - before_type13),
                 (unsigned long long)(stats.type15_events - before_type15),
                 (unsigned long long)(stats.gps_events - before_gps));
    return 0;
}

static int process_raw_annexb(const std::vector<uint8_t>& data,
                              Output& out,
                              Stats& stats)
{
    FILE* f = out.get_video(false);
    if (!f)
        return 1;
    if (!data.empty())
        fwrite(data.data(), 1, data.size(), f);
    stats.video_frames = 1;
    stats.video_bytes = data.size();
    stats.saw_video = true;
    return 0;
}

static int process_one(const std::string& input_path,
                       const std::string& spec,
                       const Options& opt,
                       bool batch_mode)
{
    std::vector<uint8_t> data;
    if (!read_file(input_path, data)) {
        std::fprintf(stderr, "Cannot read %s\n", input_path.c_str());
        return 1;
    }

    InputFormat fmt = detect_input_format(input_path, data);
    const char* fmt_name = fmt == InputFormat::StreamaxMedia ? "Streamax media frame" :
                           fmt == InputFormat::RawAnnexB ? "raw Annex-B" :
                           fmt == InputFormat::Mp4Container ? "MP4 container" :
                           "unknown";
    std::fprintf(stderr, "Input:   %s\n", input_path.c_str());
    std::fprintf(stderr, "Format:  %s\n", fmt_name);
    if (fmt == InputFormat::Unknown) {
        std::fprintf(stderr, "Error: unsupported input. Packet/network streams are intentionally unsupported.\n");
        return 1;
    }

    std::string effective_spec = spec;
    if (opt.face_privacy_mode != Options::FacePrivacyMode::None) {
        fs::path spec_path(effective_spec);
        if (effective_spec.empty() ||
            lower_ascii(spec_path.extension().string()) == ".raw") {
            effective_spec = ".mp4";
        } else if (spec_path.extension().empty() &&
                   spec_path.filename().string().find('.') == std::string::npos) {
            effective_spec = (spec_path / ".mp4").string();
        }
    }

    Output out;
    out.mux_mode = is_container_spec(effective_spec);
    if (out.mux_mode) {
        out.final_output_path = container_output_for(input_path, effective_spec, batch_mode);
        fs::path final_path(out.final_output_path);
        fs::path dir = final_path.parent_path();
        if (dir.empty())
            dir = ".";
        out.sidecar_base = (dir / final_path.stem()).string();
        out.base = (dir / (final_path.stem().string() + "_tmp")).string();
        ensure_parent_dir(out.final_output_path);
        std::fprintf(stderr, "Output:  %s\n", out.final_output_path.c_str());
    } else {
        out.base = output_base_for(input_path, effective_spec, batch_mode);
        out.sidecar_base = out.base;
        ensure_parent_dir(out.base);
        std::fprintf(stderr, "Output:  %s.{h264|h265|audio}\n", out.base.c_str());
    }

    Stats stats;
    int rc = 0;
    if (fmt == InputFormat::StreamaxMedia) {
        rc = process_streamax_media(input_path, data, out, stats);
    } else if (fmt == InputFormat::RawAnnexB) {
        rc = process_raw_annexb(data, out, stats);
    } else if (fmt == InputFormat::Mp4Container) {
        if (opt.write_json || opt.write_text ||
            opt.face_privacy_mode != Options::FacePrivacyMode::None) {
            rc = process_mp4_appended_metadata(input_path, data, out, stats);
        }
        if (rc != 0)
            return rc;
        if (opt.face_privacy_mode != Options::FacePrivacyMode::None) {
            rc = transcode_face_privacy_mp4(input_path, out.final_output_path,
                                            out.events,
                                            opt.face_privacy_mode,
                                            opt.face_blur_strength,
                                            opt.ffmpeg_path);
        } else {
            rc = out.mux_mode
                ? remux_container_with_ffmpeg(input_path, out.final_output_path)
                : extract_container_video_with_ffmpeg(input_path, out.base);
        }
    }
    out.close();

    if (rc == 0 && out.mux_mode && fmt != InputFormat::Mp4Container) {
        std::string mux_target = out.final_output_path;
        if (opt.face_privacy_mode != Options::FacePrivacyMode::None)
            mux_target = out.final_output_path + ".privacy_src.mp4";
        rc = mux_video_with_ffmpeg(out.video_path, stats.h265, stats.video_fps,
                                   mux_target);
        if (rc == 0 && opt.face_privacy_mode != Options::FacePrivacyMode::None) {
            rc = transcode_face_privacy_mp4(mux_target, out.final_output_path,
                                            out.events,
                                            opt.face_privacy_mode,
                                            opt.face_blur_strength,
                                            opt.ffmpeg_path);
            std::error_code ec;
            fs::remove(mux_target, ec);
        }
        if (rc == 0) {
            std::error_code ec;
            fs::remove(out.video_path, ec);
            fs::remove(out.audio_path, ec);
        }
    }

    if (rc == 0 && opt.write_json) {
        rc = write_json_sidecar(out.sidecar_base + ".json", out.events);
    }
    if (rc == 0 && opt.write_text) {
        rc = write_text_sidecar(out.sidecar_base + ".txt", out.events);
    }

    std::fprintf(stderr,
                 "Done. frames=%llu video=%llu audio=%llu type13=%llu type15=%llu gps=%llu\n",
                 (unsigned long long)stats.frames,
                 (unsigned long long)stats.video_frames,
                 (unsigned long long)stats.audio_frames,
                 (unsigned long long)stats.type13_events,
                 (unsigned long long)stats.type15_events,
                 (unsigned long long)stats.gps_events);
    return rc;
}

static std::vector<std::string> find_inputs(const std::string& folder)
{
    std::vector<std::string> files;
    std::error_code ec;
    fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec);
    if (ec)
        return files;
    for (const auto& entry : it) {
        if (!entry.is_regular_file(ec) || ec)
            continue;
        std::string ext = lower_ascii(entry.path().extension().string());
        if (ext == ".264" || ext == ".265" || ext == ".mp4")
            files.push_back(entry.path().string());
    }
    std::sort(files.begin(), files.end());
    return files;
}

} // namespace

int main(int argc, char** argv)
{
    Options opt;
    if (!parse_args(argc, argv, opt))
        return 2;

    if (!opt.input_folder.empty()) {
        std::vector<std::string> files = find_inputs(opt.input_folder);
        if (files.empty()) {
            std::fprintf(stderr, "No .264/.265/.mp4 files found under %s\n", opt.input_folder.c_str());
            return 1;
        }
        int failures = 0;
        for (const auto& file : files) {
            if (process_one(file, opt.batch_spec, opt, true) != 0)
                failures++;
        }
        std::fprintf(stderr, "Batch done. files=%zu failures=%d\n", files.size(), failures);
        return failures == 0 ? 0 : 1;
    }

    return process_one(opt.input_file, opt.output_spec, opt, false);
}
