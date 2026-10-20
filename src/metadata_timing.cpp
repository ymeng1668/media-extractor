#include "metadata_timing.h"
#include <algorithm>

namespace media_extractor {
namespace {
uint64_t distance(uint64_t a, uint64_t b) { return a > b ? a - b : b - a; }
}

void TimingStats::add(TimeMatchStatus status) {
    if (status == TimeMatchStatus::Matched) ++matched;
    else if (status == TimeMatchStatus::OutsideTimeline) ++outside;
    else ++unmatched;
}

MetadataTimeline::MetadataTimeline(std::vector<VideoFrameTime> frames) {
    std::stable_sort(frames.begin(), frames.end(), [](const auto& a, const auto& b) {
        return a.frame < b.frame;
    });
    for (size_t i = 0; i < frames.size(); ++i) {
        if (frames[i].frame < 0) continue;
        if (segments_.empty() || distance(frames[i].pts, frames[i - 1].pts) > discontinuity_ms)
            segments_.push_back({frames[i].frame, frames[i].frame, {}});
        auto& segment = segments_.back();
        segment.last_frame = frames[i].frame;
        segment.frames.push_back(frames[i]);
    }
    for (auto& segment : segments_)
        std::stable_sort(segment.frames.begin(), segment.frames.end(), [](const auto& a, const auto& b) {
            return a.pts < b.pts;
        });
}

TimeMatch MetadataTimeline::match(uint64_t pts, int64_t carrier) const {
    if (empty()) return {};
    auto segment = std::upper_bound(segments_.begin(), segments_.end(), carrier,
        [](int64_t frame, const auto& s) { return frame < s.first_frame; });
    if (segment == segments_.begin()) return {TimeMatchStatus::Unmatched};
    --segment;
    const auto& frames = segment->frames;
    if (pts < frames.front().pts || pts > frames.back().pts) {
        bool outside = (segment == segments_.begin() && pts < frames.front().pts) ||
                       (segments_.size() == 1 && pts > frames.back().pts);
        return {outside ? TimeMatchStatus::OutsideTimeline : TimeMatchStatus::Unmatched};
    }
    auto it = std::lower_bound(frames.begin(), frames.end(), pts,
        [](const auto& frame, uint64_t p) { return frame.pts < p; });
    if (it != frames.begin() && (it == frames.end() || distance((it - 1)->pts, pts) <= distance(it->pts, pts))) --it;
    const uint64_t delta = distance(it->pts, pts);
    if (delta > tolerance_ms) return {TimeMatchStatus::Unmatched};
    return {TimeMatchStatus::Matched, it->frame, segment->last_frame, delta};
}

} // namespace media_extractor
