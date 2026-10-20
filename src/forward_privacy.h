#pragma once

#include "media_frame.h"
#include "metadata_timing.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace media_extractor {

struct ForwardBox {
    int x = 0, y = 0, width = 0, height = 0;
    int kind = 0; // 1 = vehicle, 7 = pedestrian (SDK block IDs).
};

struct ForwardObservation {
    uint64_t pts = 0;
    int64_t carrier_frame = 0;
    int width = 0, height = 0;
    std::vector<ForwardBox> boxes;
    int kind = 0; // 1 = vehicle, 7 = pedestrian; empty boxes clear this category.
};

using ForwardFrameTime = VideoFrameTime;

struct ForwardRegion : ForwardBox {
    int64_t first = 0, last = 0;
};

// Decode the SDK's output protocol, not the raw Type 13 wire payload.
bool decode_forward_output(const uint8_t* data, size_t size,
                           const ForwardObservation& context,
                           std::vector<ForwardObservation>& out,
                           std::string& error);

std::vector<ForwardRegion> forward_regions(
    const std::vector<ForwardObservation>& observations,
    const std::vector<ForwardFrameTime>& times,
    int width, int height, bool blur, TimingStats* timing = nullptr);

// One instance per input stream. Loads the optional vendor SDK on first use.
class ForwardPrivacy {
public:
    explicit ForwardPrivacy(const std::string& sdk_directory);
    ~ForwardPrivacy();
    ForwardPrivacy(const ForwardPrivacy&) = delete;
    ForwardPrivacy& operator=(const ForwardPrivacy&) = delete;
    void set_image_size(int width, int height);
    // Container timeline translated into the camera clock using its footer anchor.
    void set_external_times(const std::vector<ForwardFrameTime>& times);
    bool read_frame(const MediaFrameHeader& header, const uint8_t* extensions,
                    size_t length, int64_t frame);
    const std::string& error() const;
    const std::vector<ForwardObservation>& observations() const;
    const std::vector<ForwardFrameTime>& times() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace media_extractor
