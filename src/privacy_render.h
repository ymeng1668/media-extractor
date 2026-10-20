#pragma once
#include "forward_privacy.h"

namespace media_extractor {
enum class PrivacyMode { None, Mask, Blur, Frame, ForwardFrame, ForwardBlur };

int render_privacy(const std::string& input, const std::string& output,
                   const std::vector<MetadataEvent>& events,
                   const std::vector<VideoFrameTime>& times,
                   PrivacyMode face, int face_strength,
                   PrivacyMode forward_mode, int forward_strength,
                   const ForwardPrivacy* forward, const std::string& ffmpeg);
}
