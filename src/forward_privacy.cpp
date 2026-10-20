#include "forward_privacy.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#ifndef MEDIA_AI_SDK_DEFAULT_DIR
#define MEDIA_AI_SDK_DEFAULT_DIR ""
#endif

namespace media_extractor {
namespace {
uint16_t u16(const uint8_t* p) { return p[0] | (uint16_t(p[1]) << 8); }
uint32_t u32(const uint8_t* p) { return u16(p) | (uint32_t(u16(p + 2)) << 16); }
uint64_t u64(const uint8_t* p) { return u32(p) | (uint64_t(u32(p + 4)) << 32); }
bool range(size_t off, size_t len, size_t size) { return off <= size && len <= size - off; }

// The SDK call ABI uses natural alignment; its output protocol uses pack(4).
struct ImageSize { int width, height; char reserved[56]; };
struct Memory { uint64_t pts; unsigned char* data; int size; char reserved[48]; };
struct Output { const uint8_t* data; int size; };

class Library {
public:
    void* handle = nullptr;
    ~Library() {
#ifdef _WIN32
        if (handle) FreeLibrary(static_cast<HMODULE>(handle));
#else
        if (handle) dlclose(handle);
#endif
    }
    void open(const std::filesystem::path& path) {
#ifdef _WIN32
        handle = LoadLibraryExW(std::filesystem::absolute(path).c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!handle) throw std::runtime_error("Cannot load " + path.string() +
                                             " (Windows error " + std::to_string(GetLastError()) + ")");
#else
        handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (!handle) throw std::runtime_error("Cannot load " + path.string() + ": " + dlerror());
#endif
    }
    template<class T> T symbol(const char* name) {
#ifdef _WIN32
        auto p = GetProcAddress(static_cast<HMODULE>(handle), name);
#else
        auto p = dlsym(handle, name);
#endif
        if (!p) throw std::runtime_error(std::string("Missing SDK entry point: ") + name);
        return reinterpret_cast<T>(p);
    }
};

} // namespace

bool decode_forward_output(const uint8_t* data, size_t size,
                           const ForwardObservation& context,
                           std::vector<ForwardObservation>& out,
                           std::string& error) {
    auto fail = [&]() { error = "Malformed Streamax AI SDK output"; return false; };
    if (!data || size < 20 || u32(data) != 0x524d4149 || u16(data + 4) != 1) return fail();
    size_t total = u32(data + 8), goff = u32(data + 12), groups = u16(data + 6);
    if (total < 20 || total > size || goff < 20 || !range(goff, groups * 28, total)) return fail();
    std::vector<ForwardObservation> parsed;
    for (size_t i = 0; i < groups; ++i) {
        const uint8_t* g = data + goff + i * 28;
        uint32_t alg = u32(g + 12);
        size_t po = u32(g + 20), ps = u32(g + 24);
        if (!range(po, ps, total) || ps < 8) return fail();
        const uint8_t* p = data + po;
        size_t count = u16(p + 4);
        if (u32(p) != alg || !range(8, count * 20, ps)) return fail();
        if (u16(g + 16) != 1 || (alg != 2 && alg != 4)) continue;
        ForwardObservation obs = context;
        obs.pts = u64(g);
        obs.kind = alg == 2 ? 1 : 7;
        for (size_t j = 0; j < count; ++j) {
            const uint8_t* b = p + 8 + j * 20;
            uint32_t type = u32(b), n = u32(b + 8);
            size_t offset = u32(b + 12), bytes = u32(b + 16);
            if (!range(offset, bytes, ps)) return fail();
            if (type != static_cast<uint32_t>(obs.kind)) continue;
            if (u16(b + 4) != 16 || n > bytes / 16 || offset < 8 + count * 20) return fail();
            for (size_t k = 0; k < n; ++k) {
                const uint8_t* r = p + offset + k * 16;
                ForwardBox box{int(u16(r)), int(u16(r + 2)), int(u16(r + 4)), int(u16(r + 6)), int(type)};
                if (box.width > 0 && box.height > 0) obs.boxes.push_back(box);
            }
        }
        parsed.push_back(std::move(obs));
    }
    out.insert(out.end(), parsed.begin(), parsed.end());
    return true;
}

std::vector<ForwardRegion> forward_regions(
    const std::vector<ForwardObservation>& observations,
    const std::vector<ForwardFrameTime>& times, int width, int height, bool /*blur*/, TimingStats* timing) {
    if (width <= 0 || height <= 0) return {};
    struct Snapshot {
        uint64_t pts = 0, distance = 0;
        int64_t last = 0;
        std::vector<ForwardBox> boxes;
    };
    std::map<int, std::map<int64_t, Snapshot>> snapshots;
    MetadataTimeline timeline(times);
    for (const auto& obs : observations) {
        if (obs.width <= 0 || obs.height <= 0) continue;
        auto match = timeline.match(obs.pts, obs.carrier_frame);
        if (timing) timing->add(match.status);
        if (match.status != TimeMatchStatus::Matched) continue;
        int64_t frame = match.frame;
        std::map<int, std::vector<ForwardBox>> categories;
        if (obs.kind == 1 || obs.kind == 7) categories[obs.kind];
        for (auto b : obs.boxes) {
            if (b.kind != 1 && b.kind != 7) continue;
            auto& boxes = categories[b.kind];
            double sx = double(width) / obs.width, sy = double(height) / obs.height;
            int x2 = std::clamp(int(std::ceil((b.x + b.width) * sx)), 0, width);
            int y2 = std::clamp(int(std::ceil((b.y + b.height) * sy)), 0, height);
            b.x = std::clamp(int(std::floor(b.x * sx)), 0, width);
            b.y = std::clamp(int(std::floor(b.y * sy)), 0, height);
            b.width = x2 - b.x; b.height = y2 - b.y;
            if (b.width > 0 && b.height > 0) boxes.push_back(b);
        }
        for (auto& category : categories) {
            auto& frames = snapshots[category.first];
            auto found = frames.find(frame);
            // Multiple AI updates can map to the same video frame. Select the
            // nearest snapshot, not the union of observations from other times.
            if (found == frames.end() || match.distance_ms < found->second.distance ||
                (match.distance_ms == found->second.distance && obs.pts >= found->second.pts)) {
                frames[frame] = {obs.pts, match.distance_ms, match.segment_last_frame,
                                 std::move(category.second)};
            }
        }
    }
    std::vector<ForwardRegion> regions;
    for (const auto& category : snapshots) {
        std::vector<size_t> previous;
        for (auto it = category.second.begin(); it != category.second.end(); ++it) {
            auto next = std::next(it);
            int64_t last = std::min(it->first + 30, it->second.last);
            if (next != category.second.end()) last = std::min(last, next->first - 1);
            std::vector<size_t> current;
            for (const auto& box : it->second.boxes) {
                size_t index = regions.size();
                for (size_t candidate : previous) {
                    const auto& r = regions[candidate];
                    if (r.last + 1 == it->first && r.x == box.x && r.y == box.y &&
                        r.width == box.width && r.height == box.height &&
                        std::find(current.begin(), current.end(), candidate) == current.end()) {
                        index = candidate;
                        break;
                    }
                }
                if (index == regions.size()) {
                    ForwardRegion r;
                    static_cast<ForwardBox&>(r) = box;
                    r.first = it->first;
                    r.last = last;
                    regions.push_back(r);
                } else regions[index].last = last;
                current.push_back(index);
            }
            // An empty snapshot is a fresh negative result, not missing data.
            previous = std::move(current);
        }
    }
    return regions;
}

struct ForwardPrivacy::Impl {
    Library dependency, dependency_alias, library;
    int (*create)(ImageSize) = nullptr;
    int (*destroy)(int) = nullptr;
    int (*parse)(int, Memory, Output*) = nullptr;
    int handle = -1, width = 0, height = 0, channel = -1;
    std::string directory, failure;
    std::vector<ForwardObservation> observations;
    std::vector<ForwardFrameTime> times;
    bool external_times = false;
    explicit Impl(const std::string& dir) : directory(dir.empty() ? MEDIA_AI_SDK_DEFAULT_DIR : dir) {}
    ~Impl() { if (handle >= 0 && destroy) destroy(handle); }
    void load() {
        if (create) return;
        namespace fs = std::filesystem;
        fs::path root(directory);
        if (directory.empty()) throw std::runtime_error("Streamax AI SDK required; use --ai-sdk-dir <platform directory>");
#ifdef _WIN32
        fs::path libs = fs::exists(root / "bin") ? root / "bin" : root;
        dependency.open(libs / "rmAIResultParse.dll");
        library.open(libs / "rmai_info_parse.dll");
#else
        fs::path libs = fs::exists(root / "lib") ? root / "lib" : root;
        dependency.open(libs / "librmAIResultParse.so");
        // The supplied dependency lacks an ELF SONAME. Register its basename
        // before loading the SDK that refers to that name in DT_NEEDED.
        dependency_alias.open("librmAIResultParse.so");
        if (dependency.handle != dependency_alias.handle)
            throw std::runtime_error("SDK dependency resolved to a different installation; "
                                     "set LD_LIBRARY_PATH to the selected SDK lib directory before launching");
        library.open(libs / "librmai_info_parse.so");
#endif
        create = library.symbol<decltype(create)>("RMAIInfo_Parse_Create");
        destroy = library.symbol<decltype(destroy)>("RMAIInfo_Parse_Destroy");
        parse = library.symbol<decltype(parse)>("RMAIInfo_ParseStruct");
    }
};

ForwardPrivacy::ForwardPrivacy(const std::string& dir) : impl_(new Impl(dir)) {}
ForwardPrivacy::~ForwardPrivacy() = default;
void ForwardPrivacy::set_image_size(int w, int h) {
    auto& s = *impl_;
    if (s.width == w && s.height == h) return;
    if (s.handle >= 0) { s.destroy(s.handle); s.handle = -1; }
    s.width = w; s.height = h;
}
const std::string& ForwardPrivacy::error() const { return impl_->failure; }
const std::vector<ForwardObservation>& ForwardPrivacy::observations() const { return impl_->observations; }
const std::vector<ForwardFrameTime>& ForwardPrivacy::times() const { return impl_->times; }
void ForwardPrivacy::set_external_times(const std::vector<ForwardFrameTime>& times) {
    impl_->times = times;
    impl_->external_times = true;
}

bool ForwardPrivacy::read_frame(const MediaFrameHeader& hdr, const uint8_t* ext, size_t size, int64_t frame) {
    auto& s = *impl_;
    if (!s.failure.empty()) return false;
    if (!is_video_frame(hdr.frame_type)) return true;
    if (s.channel >= 0 && s.channel != hdr.channel) {
        s.failure = "Forward privacy requires a single video channel per input";
        return false;
    }
    s.channel = hdr.channel;
    if (!s.external_times && hdr.has_virtual_time && hdr.frame_length > 0) s.times.push_back({hdr.virtual_pts, frame});
    if (hdr.has_video_info) set_image_size(hdr.video_info.width, hdr.video_info.height);
    try {
        for (size_t off = 0; off < size;) {
            if (!range(off, 4, size)) throw std::runtime_error("Truncated media extension header");
            size_t len = u32(ext + off) >> 8;
            if (len < 4 || !range(off, len, size)) throw std::runtime_error("Invalid media extension length");
            if (ext[off] == 13) {
                if (len < 20) throw std::runtime_error("Truncated Type 13 AI header");
                uint32_t body = u32(ext + off + 16);
                if (body > len - 20) throw std::runtime_error("Invalid Type 13 AI body length");
                if (body) {
                    if (s.width <= 0 || s.height <= 0) throw std::runtime_error("Missing video dimensions for ADAS decoding");
                    s.load();
                    if (s.handle < 0) s.handle = s.create(ImageSize{s.width, s.height, {}});
                    if (s.handle < 0) throw std::runtime_error("Cannot create Streamax AI parser");
                    Memory mem{}; mem.pts = u64(ext + off + 8);
                    mem.data = const_cast<unsigned char*>(ext + off + 20); mem.size = int(body);
                    Output output{};
                    if (s.parse(s.handle, mem, &output) != 0 || output.size < 0)
                        throw std::runtime_error("Streamax AI parser rejected Type 13 data");
                    ForwardObservation context{mem.pts, frame, s.width, s.height, {}};
                    if (!decode_forward_output(output.data, size_t(output.size), context, s.observations, s.failure)) return false;
                }
            }
            off += len;
        }
    } catch (const std::exception& e) { s.failure = e.what(); return false; }
    return true;
}
} // namespace media_extractor
