#include "media_frame.h"
#include "privacy_render.h"
#include "mp4_metadata.h"
#include "url_writer.h"
#include <media-format/adpcm.h>
#include <media-format/python_video_render.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/document.h>

namespace fs = std::filesystem;
using namespace media_format;

namespace {

struct Options {
    std::string input_file;
    std::string input_folder;
    std::string output_spec = ".raw";
    std::string batch_spec = ".raw";
    bool write_json = false;
    bool write_text = false;
    bool write_hex = false;
    bool batch_spec_set = false;
    bool subtitle_track = true;
    SubtitleStyle subtitle_style;
    bool python_privacy = false;
    std::string privacy_mode;
    std::string privacy_config;
    std::string privacy_plugin_dir = MEDIA_EXTRACTOR_PRIVACY_PLUGIN_DIR;
    std::string privacy_python = MEDIA_EXTRACTOR_PRIVACY_PYTHON;
    bool privacy_options_set = false;
    PythonProcessorOptions processor;
    using FacePrivacyMode = PrivacyMode;
    PrivacyMode face_privacy_mode = PrivacyMode::None;
    PrivacyMode forward_privacy_mode = PrivacyMode::None;
    int forward_blur_strength = 50;
    std::string ai_sdk_dir;
    bool decode_audio = true;
    int face_blur_strength = 25;
    std::string ffmpeg_path = "ffmpeg";
};

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
    std::vector<VideoFrameTime> video_times;
    std::unique_ptr<ForwardPrivacy> forward;
    bool privacy = false;
    bool write_hex = false;
    int video_channel = -1;
    bool decode_audio = true;
    AudioCodec audio_codec = AudioCodec::ADPCM;
    uint32_t sample_rate = 8000, channels = 1;
    FILE* pcm = nullptr;
    std::string pcm_path;
    adpcm_state_t adpcm_state{};

    ~Output()
    {
        close();
    }

    void close()
    {
        if (pcm) { fclose(pcm); pcm = nullptr; }
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
        "  --hex                   Include Type 13/15 raw hex in sidecars only\n"
        "  --sub-track <on|off>     Add telemetry subtitles to extracted containers (default: on)\n"
        "  --sub-color <name|hex>   Subtitle color (default: green; accepts #RRGGBB)\n"
        "  --sub-pos <top|bottom>   Subtitle position hint (default: bottom)\n"
        "  --face-mask             Fill detected face_rect regions with black; outputs H.264 MP4\n"
        "  --face-blur[=0..100]    Blur detected face_rect regions; 0=off, 100=max (default: 25)\n"
        "  --face-frame            Draw boxes around detected face_rect regions; outputs H.264 MP4\n"
        "  --forward-frame         Highlight ADAS vehicles (green) and pedestrians (yellow)\n"
        "  --forward-blur[=0..100] Blur ADAS regions (default: 50); combine with one --face-* mode\n"
        "  --ai-sdk-dir <dir>      Optional Streamax AI SDK platform directory\n"
        "  --privacy-redaction     Detect faces and plates using the Python plugin; H.264 MP4\n"
        "  --privacy-mode <mode>   highlight (default; boxes only), blur, pixelate, or solid\n"
        "  --privacy-config <file> JSON processor options; OCR is enabled by default\n"
        "  --privacy-python <path> Python/virtualenv executable matching the bridge build\n"
        "  --privacy-plugin-dir <dir> Directory containing privacy_redaction.py\n"
        "                          Cannot be combined with --face-* or --forward-*\n"
        "  --no-decode-audio       Keep raw audio; do not decode ADPCM or add it to containers\n"
        "  --ffmpeg <path>         ffmpeg for remux/extraction/metadata privacy (default: ffmpeg)\n"
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

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_sub_color(const std::string& v, SubtitleStyle& style)
{
    std::string s = lower_ascii(v);
    if (s == "green" || s == "lime")      { style = {0, 255, 0, 255, style.position}; return true; }
    if (s == "red")                        { style = {255, 0, 0, 255, style.position}; return true; }
    if (s == "blue")                       { style = {0, 0, 255, 255, style.position}; return true; }
    if (s == "yellow")                     { style = {255, 255, 0, 255, style.position}; return true; }
    if (s == "cyan")                       { style = {0, 255, 255, 255, style.position}; return true; }
    if (s == "magenta")                    { style = {255, 0, 255, 255, style.position}; return true; }
    if (s == "white")                      { style = {255, 255, 255, 255, style.position}; return true; }
    if (s == "black")                      { style = {0, 0, 0, 255, style.position}; return true; }
    if (s == "orange")                     { style = {255, 165, 0, 255, style.position}; return true; }

    if (!s.empty() && s[0] == '#') s.erase(0, 1);
    if (s.size() != 6) return false;

    int d0 = hex_digit(s[0]), d1 = hex_digit(s[1]);
    int d2 = hex_digit(s[2]), d3 = hex_digit(s[3]);
    int d4 = hex_digit(s[4]), d5 = hex_digit(s[5]);
    if (d0 < 0 || d1 < 0 || d2 < 0 || d3 < 0 || d4 < 0 || d5 < 0) return false;

    style.r = (uint8_t)((d0 << 4) | d1);
    style.g = (uint8_t)((d2 << 4) | d3);
    style.b = (uint8_t)((d4 << 4) | d5);
    style.a = 255;
    return true;
}

static bool parse_sub_pos(const std::string& v, SubtitleStyle& style)
{
    std::string s = lower_ascii(v);
    if (s == "top") {
        style.position = SubtitlePosition::Top;
        return true;
    }
    if (s == "bottom") {
        style.position = SubtitlePosition::Bottom;
        return true;
    }
    return false;
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
            opt.batch_spec_set = true;
        } else if (arg == "--json") {
            opt.write_json = true;
        } else if (arg == "--text") {
            opt.write_text = true;
        } else if (arg == "--hex") {
            opt.write_hex = true;
        } else if (arg == "--sub-track" && i + 1 < argc) {
            const std::string value = lower_ascii(argv[++i]);
            if (value == "on" || value == "1" || value == "true" || value == "yes")
                opt.subtitle_track = true;
            else if (value == "off" || value == "0" || value == "false" || value == "no")
                opt.subtitle_track = false;
            else {
                std::fprintf(stderr, "Error: --sub-track requires on or off.\n");
                return false;
            }
        } else if (arg == "--sub-color" && i + 1 < argc) {
            if (!parse_sub_color(argv[++i], opt.subtitle_style)) {
                std::fprintf(stderr, "Error: --sub-color requires a color name or #RRGGBB.\n");
                return false;
            }
        } else if (arg == "--sub-pos" && i + 1 < argc) {
            if (!parse_sub_pos(argv[++i], opt.subtitle_style)) {
                std::fprintf(stderr, "Error: --sub-pos requires top or bottom.\n");
                return false;
            }
        } else if (arg == "--privacy-redaction") {
            opt.python_privacy = true;
        } else if (arg == "--privacy-mode" && i + 1 < argc) {
            opt.privacy_options_set = true;
            opt.privacy_mode = argv[++i];
            if (opt.privacy_mode != "highlight" && opt.privacy_mode != "blur" &&
                opt.privacy_mode != "pixelate" && opt.privacy_mode != "solid") {
                std::fprintf(stderr, "Error: --privacy-mode must be highlight, blur, pixelate, or solid.\n");
                return false;
            }
        } else if (arg == "--privacy-config" && i + 1 < argc) {
            opt.privacy_options_set = true;
            opt.privacy_config = argv[++i];
        } else if (arg == "--privacy-python" && i + 1 < argc) {
            opt.privacy_options_set = true;
            opt.privacy_python = argv[++i];
        } else if (arg == "--privacy-plugin-dir" && i + 1 < argc) {
            opt.privacy_options_set = true;
            opt.privacy_plugin_dir = argv[++i];
        } else if (arg == "--no-decode-audio") {
            opt.decode_audio = false;
        } else if (arg == "--ai-sdk-dir" && i + 1 < argc) {
            opt.ai_sdk_dir = argv[++i];
        } else if (arg == "--forward-frame" || arg == "--forward-blur" ||
                   arg.rfind("--forward-blur=", 0) == 0) {
            if (opt.forward_privacy_mode != PrivacyMode::None) {
                std::fprintf(stderr, "Error: select only one --forward-* rendering mode.\n");
                return false;
            }
            if (arg.rfind("--forward-blur=", 0) == 0 &&
                !parse_int("--forward-blur", arg.substr(15), 0, 100, opt.forward_blur_strength)) return false;
            opt.forward_privacy_mode = arg == "--forward-frame" ? PrivacyMode::ForwardFrame : PrivacyMode::ForwardBlur;
        } else if (arg == "--face-mask") {
            if (opt.face_privacy_mode != Options::FacePrivacyMode::None) {
                std::fprintf(stderr, "Error: select only one --face-* rendering mode.\n");
                return false;
            }
            opt.face_privacy_mode = Options::FacePrivacyMode::Mask;
        } else if (arg == "--face-blur" || arg.rfind("--face-blur=", 0) == 0) {
            if (opt.face_privacy_mode != Options::FacePrivacyMode::None) {
                std::fprintf(stderr, "Error: select only one --face-* rendering mode.\n");
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
                std::fprintf(stderr, "Error: select only one --face-* rendering mode.\n");
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
    if (opt.batch_spec_set && opt.input_folder.empty()) {
        std::fprintf(stderr, "Error: --r requires --input-folder.\n");
        return false;
    }
    if (opt.privacy_options_set && !opt.python_privacy) {
        std::fprintf(stderr, "Error: --privacy-* options require --privacy-redaction.\n");
        return false;
    }
    if (opt.python_privacy) {
        if (opt.face_privacy_mode != PrivacyMode::None || opt.forward_privacy_mode != PrivacyMode::None) {
            std::fprintf(stderr, "Error: --privacy-redaction cannot be combined with --face-* or --forward-*.\n");
            return false;
        }
        if (!CorePythonBridge::available()) {
            std::fprintf(stderr, "Error: rebuild with -DMEDIA_FORMAT_ENABLE_PYTHON_BRIDGE=ON to use --privacy-redaction.\n");
            return false;
        }
        std::error_code ec;
        fs::path plugin = fs::absolute(opt.privacy_plugin_dir, ec);
        if (ec || !fs::is_regular_file(plugin / "privacy_redaction.py", ec)) {
            std::fprintf(stderr, "Error: --privacy-plugin-dir must contain privacy_redaction.py.\n");
            return false;
        }
        rapidjson::Document config;
        config.SetObject();
        if (!opt.privacy_config.empty()) {
            std::ifstream file(opt.privacy_config);
            if (!file) {
                std::fprintf(stderr, "Error: cannot read privacy configuration: %s\n", opt.privacy_config.c_str());
                return false;
            }
            std::string json{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
            config.Parse(json.c_str());
            if (file.bad() || config.HasParseError() || !config.IsObject()) {
                std::fprintf(stderr, "Error: privacy configuration must be a JSON object.\n");
                return false;
            }
        }
        if (!opt.privacy_mode.empty()) {
            config.RemoveMember("mode");
            config.AddMember("mode", rapidjson::Value(opt.privacy_mode.c_str(), config.GetAllocator()), config.GetAllocator());
            config.RemoveMember("apply_redaction");
            config.AddMember("apply_redaction", opt.privacy_mode != "highlight", config.GetAllocator());
        }
        rapidjson::StringBuffer json;
        rapidjson::Writer<rapidjson::StringBuffer> writer(json);
        config.Accept(writer);
        opt.processor.module = "privacy_redaction";
        opt.processor.class_name = "PrivacyRedactionProcessor";
        opt.processor.config_json.assign(json.GetString(), json.GetSize());
        opt.processor.python_paths = {MEDIA_EXTRACTOR_PYTHON_RUNTIME_DIR, plugin.string()};
        opt.processor.python_executable = opt.privacy_python;
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

static bool type13_summary_has_interesting_alarm_state(const std::string& summary)
{
    if (summary.empty()) return false;
    if (summary.find("ai_alarm=") != std::string::npos)
        return true;
    if (summary.find("eye_look_down=1") != std::string::npos)
        return true;
    size_t pos = 0;
    while ((pos = summary.find("ala=", pos)) != std::string::npos) {
        size_t v = pos + 4;
        size_t e = summary.find('|', v);
        std::string value = summary.substr(v, e == std::string::npos ? std::string::npos : e - v);
        if (!value.empty() && value != "0/0")
            return true;
        pos = v;
    }
    pos = 0;
    while ((pos = summary.find("alarm_num=", pos)) != std::string::npos) {
        size_t v = pos + 10;
        size_t e = summary.find('|', v);
        std::string value = summary.substr(v, e == std::string::npos ? std::string::npos : e - v);
        if (!value.empty() && value != "0")
            return true;
        pos = v;
    }
    pos = 0;
    while ((pos = summary.find("warn=", pos)) != std::string::npos) {
        size_t v = pos + 5;
        size_t e = summary.find('|', v);
        std::string value = summary.substr(v, e == std::string::npos ? std::string::npos : e - v);
        if (!value.empty() && value != "0")
            return true;
        pos = v;
    }
    pos = 0;
    while ((pos = summary.find("danger=", pos)) != std::string::npos) {
        size_t v = pos + 7;
        size_t e = summary.find('|', v);
        std::string value = summary.substr(v, e == std::string::npos ? std::string::npos : e - v);
        if (!value.empty() && value != "0" && value != "0/0")
            return true;
        pos = v;
    }
    return false;
}

static bool telemetry_event_is_type(const TelemetryEvent& ev, const char* type)
{
    std::string prefix = "type=";
    prefix += type;
    return ev.text.rfind(prefix, 0) == 0;
}

static bool append_unique_subtitle_line(std::vector<std::string>& lines,
                                        const std::string& text,
                                        size_t max_lines)
{
    if (text.empty()) return false;
    if (std::find(lines.begin(), lines.end(), text) != lines.end())
        return false;
    if (lines.size() >= max_lines)
        return false;
    lines.push_back(text);
    return true;
}

static std::vector<TelemetryEvent> build_subtitle_events(
    const std::vector<MetadataEvent>& metadata,
    uint32_t video_fps)
{
    std::vector<TelemetryEvent> events;
    for (const auto& item : metadata)
        events.push_back({static_cast<int64_t>(item.frame_index), item.duration_s, item.text});
    // MP4 timed-text is a single subtitle stream. Many per-frame cues at the
    // same/adjacent timestamps become zero-duration or near-invisible in common
    // players, so keep the sidecars exhaustive and make subtitles readable.
    if (events.empty()) return {};

    const int64_t fps = video_fps > 0 ? (int64_t)video_fps : 25;
    struct Bucket {
        int64_t first_frame = -1;
        std::vector<std::string> priority;
        std::vector<std::string> alarms;
        std::string first_t13;
        std::string interesting_t13;
        std::string first_t15;
    };

    std::map<int64_t, Bucket> buckets;
    for (const auto& ev : events) {
        int64_t bucket_idx = ev.video_frame_idx >= 0 ? ev.video_frame_idx / fps : 0;
        Bucket& b = buckets[bucket_idx];
        if (b.first_frame < 0 || ev.video_frame_idx < b.first_frame)
            b.first_frame = ev.video_frame_idx;

        if (telemetry_event_is_type(ev, "T13_DATA")) {
            if (b.first_t13.empty())
                b.first_t13 = ev.text;
            if (b.interesting_t13.empty() &&
                type13_summary_has_interesting_alarm_state(ev.text))
                b.interesting_t13 = ev.text;
        } else if (telemetry_event_is_type(ev, "T15_DATA")) {
            if (b.first_t15.empty())
                b.first_t15 = ev.text;
        } else if (telemetry_event_is_type(ev, "ALARM")) {
            if (b.alarms.size() < 3)
                b.alarms.push_back(ev.text);
        } else if (telemetry_event_is_type(ev, "T13_HEX") ||
                   telemetry_event_is_type(ev, "T15_HEX")) {
            continue;
        } else {
            append_unique_subtitle_line(b.priority, ev.text, 2);
        }
    }

    std::vector<TelemetryEvent> out;
    out.reserve(buckets.size());
    for (auto& kv : buckets) {
        Bucket& b = kv.second;
        std::vector<std::string> lines;
        for (const auto& line : b.priority)
            append_unique_subtitle_line(lines, line, 6);
        for (const auto& line : b.alarms)
            append_unique_subtitle_line(lines, line, 6);
        if (!b.interesting_t13.empty())
            append_unique_subtitle_line(lines, b.interesting_t13, 6);
        if (!b.first_t13.empty())
            append_unique_subtitle_line(lines, b.first_t13, 6);
        if (!b.first_t15.empty())
            append_unique_subtitle_line(lines, b.first_t15, 6);
        if (lines.empty())
            continue;

        TelemetryEvent ev;
        ev.video_frame_idx = b.first_frame >= 0 ? b.first_frame : kv.first * fps;
        ev.duration_s = 1.2;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i) ev.text += "\n";
            ev.text += lines[i];
        }
        out.push_back(std::move(ev));
    }
    return out;
}

static int mux_video(const std::string& video_path, uint32_t fps,
                     const Output& out, const std::string& output_path, const Options& opt)
{
    if (out.pcm_path.empty() && !out.audio_path.empty())
        std::fprintf(stderr, "Warning: audio was not decoded; raw audio retained separately.\n");
    return UrlWriter::mux(video_path, out.pcm_path, output_path, fps,
                          out.sample_rate, out.channels, false,
                          opt.subtitle_track ? build_subtitle_events(out.events, fps) : std::vector<TelemetryEvent>{},
                          {}, opt.subtitle_style);
}

static int remux_container_with_ffmpeg(const std::string& input_path,
                                       const std::string& output_path, const std::string& ffmpeg)
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

    std::string cmd = shell_quote(ffmpeg) + " -y -hide_banner -loglevel warning ";
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
                                               const std::string& output_base, const std::string& ffmpeg)
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
    cmd += shell_quote(ffmpeg) + " -y -hide_banner -loglevel warning -i " + shell_quote(input_path) +
           " -map 0:v:0 -an -c:v copy -f hevc " + shell_quote(out_h265) + "; ";
    cmd += "else ";
    cmd += shell_quote(ffmpeg) + " -y -hide_banner -loglevel warning -i " + shell_quote(input_path) +
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
    if (out.write_hex) {
        auto append_hex = [&](const char* kind, const std::string& hex) {
            if (hex.empty()) return;
            MetadataEvent event;
            event.frame_index = stats.video_frames;
            event.duration_s = 0.0;
            event.kind = kind;
            event.text = "type=" + event.kind + "|hex=" + hex;
            event.fields = {{"type", kind}, {"hex", hex}};
            out.events.push_back(std::move(event));
        };
        append_hex("T13_HEX", hdr.telemetry_raw_hex);
        append_hex("T15_HEX", hdr.telemetry_type15_raw_hex);
    }
}

static bool read_indexed_metadata(const std::string& input, const std::vector<uint8_t>& data,
                                  Output& out, Stats& stats)
{
    std::vector<IndexedMediaRecord> records;
    int width = 0, height = 0;
    if (!index_mp4_metadata(input, data, find_mp4_appended_media_offset(data.data(), data.size()),
                            records, out.video_times, width, height)) return false;
    if (out.forward) {
        out.forward->set_image_size(width, height);
        out.forward->set_external_times(out.video_times);
    }
    for (const auto& record : records) {
        MediaFrameHeader header;
        media_frame_parse_fixed(data.data() + record.offset, record.size, header);
        const auto* ext = data.data() + record.offset + kMediaFrameFixedHeaderSize;
        // The validated export's first entry is a sample index, not audio info.
        media_frame_parse_extensions(ext + 12, header.extend_length - 12, record.index, header);
        stats.video_frames = record.index;
        append_frame_events(out, header, stats);
        if (out.forward && !out.forward->read_frame(header, ext, header.extend_length, record.index)) {
            std::fprintf(stderr, "Forward metadata failed: %s\n", out.forward->error().c_str());
            return false;
        }
    }
    stats.video_frames = out.video_times.size();
    return true;
}

static int render_python_privacy(const std::string& input, const std::string& output, const Options& opt)
{
    std::fprintf(stderr, "Python privacy redaction: %s\n", output.c_str());
    std::string error;
    if (!render_python_video(input, output, opt.processor, error)) {
        std::fprintf(stderr, "Python privacy failed: %s\n", error.c_str());
        return 1;
    }
    return 0;
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
        if (out.forward && !out.forward->read_frame(hdr, ext, hdr.extend_length, stats.video_frames)) {
            std::fprintf(stderr, "Forward metadata failed: %s\n", out.forward->error().c_str());
            return 1;
        }
        if (out.privacy && is_video_frame(hdr.frame_type)) {
            if (out.video_channel >= 0 && out.video_channel != hdr.channel) {
                std::fprintf(stderr, "Privacy rendering requires a single video channel per input.\n");
                return 1;
            }
            out.video_channel = hdr.channel;
        }
        if (is_video_frame(hdr.frame_type) && hdr.frame_length > 0 && hdr.has_virtual_time)
            out.video_times.push_back({hdr.virtual_pts, static_cast<int64_t>(stats.video_frames)});
        if (hdr.has_audio_info && hdr.frame_type == FrameType::Audio) {
            if (!out.pcm_path.empty() && (out.audio_codec != hdr.audio_info.codec ||
                out.sample_rate != hdr.audio_info.sample_rate || out.channels != hdr.audio_info.channels)) {
                std::fprintf(stderr, "Audio format changed during recording; refusing mixed PCM output.\n");
                return 1;
            }
            out.audio_codec = hdr.audio_info.codec;
            if (hdr.audio_info.sample_rate) out.sample_rate = hdr.audio_info.sample_rate;
            out.channels = hdr.audio_info.channels;
        }

        if (is_video_frame(hdr.frame_type) && hdr.frame_length > 0) {
            bool h265 = frame_payload_is_h265(hdr);
            FILE* f = out.get_video(h265);
            if (!f || fwrite(payload, 1, hdr.frame_length, f) != hdr.frame_length) return 1;
            stats.video_frames++;
            stats.video_bytes += hdr.frame_length;
            stats.h265 = h265;
            stats.saw_video = true;
        } else if (hdr.frame_type == FrameType::Audio && hdr.frame_length > 0) {
            FILE* f = out.get_audio();
            if (!f || fwrite(payload, 1, hdr.frame_length, f) != hdr.frame_length) return 1;
            if (out.decode_audio && out.audio_codec == AudioCodec::ADPCM && hdr.frame_length > 4) {
                if (!out.pcm) {
                    out.pcm_path = out.base + ".pcm";
                    out.pcm = fopen(out.pcm_path.c_str(), "wb");
                    if (!out.pcm) return 1;
                }
                std::vector<short> decoded((hdr.frame_length - 4) * 4);
                int bytes = adpcm_decoder(reinterpret_cast<char*>(const_cast<uint8_t*>(payload + 4)),
                    decoded.data(), static_cast<int>(hdr.frame_length - 4), &out.adpcm_state);
                if (bytes <= 0 || fwrite(decoded.data(), 1, bytes, out.pcm) != static_cast<size_t>(bytes)) return 1;
            }
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
                              Stats& stats, bool h265)
{
    FILE* f = out.get_video(h265);
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
    const bool metadata_privacy = opt.face_privacy_mode != PrivacyMode::None || opt.forward_privacy_mode != PrivacyMode::None;
    const bool privacy = metadata_privacy || opt.python_privacy;
    if (metadata_privacy && fmt == InputFormat::RawAnnexB) {
        std::fprintf(stderr, "Privacy rendering requires Type 13 metadata and video PTS; raw Annex-B has neither.\n");
        return 1;
    }
    if (privacy) {
        fs::path spec_path(effective_spec);
        if (effective_spec.empty()) {
            effective_spec = ".mp4";
        } else if (lower_ascii(spec_path.filename().string()) == ".raw") {
            effective_spec = (spec_path.parent_path() / ".mp4").string();
        } else if (lower_ascii(spec_path.extension().string()) == ".raw") {
            effective_spec = spec_path.replace_extension(".mp4").string();
        } else if (spec_path.extension().empty() &&
                   spec_path.filename().string().find('.') == std::string::npos) {
            effective_spec = (spec_path / ".mp4").string();
        }
    }

    Output out;
    out.privacy = metadata_privacy;
    out.write_hex = opt.write_hex;
    out.decode_audio = opt.decode_audio;
    if (opt.forward_privacy_mode != PrivacyMode::None)
        out.forward = std::make_unique<ForwardPrivacy>(opt.ai_sdk_dir);
    out.mux_mode = privacy || is_container_spec(effective_spec);
    if (out.mux_mode) {
        out.final_output_path = container_output_for(input_path, effective_spec, batch_mode);
        if (privacy) {
            fs::path mp4(out.final_output_path);
            mp4.replace_extension(".mp4");
            out.final_output_path = mp4.string();
            std::error_code ec;
            if (fs::equivalent(input_path, out.final_output_path, ec) ||
                fs::absolute(input_path).lexically_normal() == fs::absolute(out.final_output_path).lexically_normal()) {
                std::fprintf(stderr, "Privacy rendering requires an output different from the input.\n");
                return 1;
            }
        }
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
        rc = process_raw_annexb(data, out, stats, lower_ascii(fs::path(input_path).extension().string()) == ".265");
    } else if (fmt == InputFormat::Mp4Container) {
        if (metadata_privacy) {
            rc = read_indexed_metadata(input_path, data, out, stats) ? 0 : 1;
        } else if (opt.write_json || opt.write_text) {
            rc = process_mp4_appended_metadata(input_path, data, out, stats);
        }
        if (rc != 0)
            return rc;
        if (opt.python_privacy) {
            rc = render_python_privacy(input_path, out.final_output_path, opt);
        } else if (metadata_privacy) {
            rc = render_privacy(input_path, out.final_output_path, out.events, out.video_times,
                    opt.face_privacy_mode, opt.face_blur_strength, opt.forward_privacy_mode,
                    opt.forward_blur_strength, out.forward.get(), opt.ffmpeg_path);
        } else {
            rc = out.mux_mode
                ? remux_container_with_ffmpeg(input_path, out.final_output_path, opt.ffmpeg_path)
                : extract_container_video_with_ffmpeg(input_path, out.base, opt.ffmpeg_path);
        }
    }
    out.close();

    if (rc == 0 && out.mux_mode && fmt != InputFormat::Mp4Container) {
        std::string mux_target = out.final_output_path;
        if (privacy)
            mux_target = out.final_output_path + ".privacy_src.mp4";
        rc = mux_video(out.video_path, stats.video_fps, out, mux_target, opt);
        if (rc == 0 && privacy) {
            rc = opt.python_privacy ? render_python_privacy(mux_target, out.final_output_path, opt)
                : render_privacy(mux_target, out.final_output_path, out.events, out.video_times,
                    opt.face_privacy_mode, opt.face_blur_strength, opt.forward_privacy_mode,
                    opt.forward_blur_strength, out.forward.get(), opt.ffmpeg_path);
            std::error_code ec;
            fs::remove(mux_target, ec);
        }
        if (rc == 0) {
            std::error_code ec;
            fs::remove(out.video_path, ec);
            if (!out.pcm_path.empty()) {
                fs::remove(out.pcm_path, ec);
                fs::remove(out.audio_path, ec);
            }
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
