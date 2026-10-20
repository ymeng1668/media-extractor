#pragma once
#include "forward_privacy.h"

namespace media_extractor {
// Requires the indexed Streamax export footer; refuses guessed clock offsets.
bool read_indexed_mp4_metadata(const std::string& input,
                              const std::vector<uint8_t>& data,
                              size_t tail_start,
                              std::vector<MetadataEvent>& events,
                              std::vector<VideoFrameTime>& times,
                              ForwardPrivacy* forward);
}
