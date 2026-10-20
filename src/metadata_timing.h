#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace media_extractor {

struct VideoFrameTime {
    uint64_t pts = 0; // Streamax virtual/AI PTS, milliseconds.
    int64_t frame = 0;
};

enum class TimeMatchStatus { Matched, OutsideTimeline, Unmatched, MissingTimeline };

struct TimeMatch {
    TimeMatchStatus status = TimeMatchStatus::MissingTimeline;
    int64_t frame = 0;
    int64_t segment_last_frame = 0;
    uint64_t distance_ms = 0;
};

struct TimingStats {
    std::size_t matched = 0;
    std::size_t outside = 0;
    std::size_t unmatched = 0;
    void add(TimeMatchStatus status);
};

// Carrier indices select the clock segment, never the matching video frame.
class MetadataTimeline {
public:
    static constexpr uint64_t tolerance_ms = 50;
    static constexpr uint64_t discontinuity_ms = 1000;
    explicit MetadataTimeline(std::vector<VideoFrameTime> frames);
    bool empty() const { return segments_.empty(); }
    TimeMatch match(uint64_t ai_pts, int64_t carrier_frame) const;

private:
    struct Segment {
        int64_t first_frame = 0, last_frame = 0;
        std::vector<VideoFrameTime> frames;
    };
    std::vector<Segment> segments_;
};

} // namespace media_extractor
