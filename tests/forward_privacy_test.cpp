#include "forward_privacy.h"
#include <cstdio>
#include <cstdlib>

static void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}

int main() {
    // One SDK ADAS group with two vehicle rectangles.
    std::vector<uint8_t> bytes(108);
    auto put = [&](size_t p, uint32_t v) {
        for (size_t i = 0; i < 4; ++i) bytes[p + i] = uint8_t(v >> (8 * i));
    };
    put(0, 0x524d4149); put(4, 1 | (1u << 16)); put(8, 108); put(12, 20);
    put(20, 1040); put(28, 2); put(32, 2); put(36, 1); put(40, 48); put(44, 60);
    put(48, 2); put(52, 1);
    put(56, 1); put(60, 16); put(64, 2); put(68, 28); put(72, 32);
    put(76, 10 | (20u << 16)); put(80, 40 | (30u << 16));
    put(92, 130 | (20u << 16)); put(96, 40 | (30u << 16));
    media_extractor::ForwardObservation context{0, 5, 200, 100, {}};
    std::vector<media_extractor::ForwardObservation> parsed;
    std::string error;
    check(media_extractor::decode_forward_output(bytes.data(), bytes.size(), context, parsed, error), "valid SDK output rejected");
    check(parsed.size() == 1 && parsed[0].boxes.size() == 2, "multiple boxes were lost");
    check(parsed[0].pts == 1040 && parsed[0].boxes[1].x == 130, "wrong SDK field offsets");
    check(parsed[0].kind == 1, "vehicle snapshot category lost");
    put(64, 0);
    std::vector<media_extractor::ForwardObservation> empty_result;
    check(media_extractor::decode_forward_output(bytes.data(), bytes.size(), context, empty_result, error) &&
          empty_result.size() == 1 && empty_result[0].kind == 1 && empty_result[0].boxes.empty(),
          "empty vehicle result lost its category");
    put(64, 2);
    put(52, 0);
    empty_result.clear();
    check(media_extractor::decode_forward_output(bytes.data(), bytes.size(), context, empty_result, error) &&
          empty_result.size() == 1 && empty_result[0].kind == 1 && empty_result[0].boxes.empty(),
          "ADAS group without object block did not produce a clearing snapshot");
    put(52, 1);
    for (size_t size = 0; size < bytes.size(); ++size) {
        std::vector<media_extractor::ForwardObservation> result;
        check(!media_extractor::decode_forward_output(bytes.data(), size, context, result, error), "truncation accepted");
        check(result.empty(), "failed parse leaked partial results");
    }
    put(64, 0xffffffff);
    check(!media_extractor::decode_forward_output(bytes.data(), bytes.size(), context, parsed, error), "oversized element count accepted");
    put(64, 2); put(68, 0xfffffff0);
    check(!media_extractor::decode_forward_output(bytes.data(), bytes.size(), context, parsed, error), "invalid offset accepted");
    put(68, 28); put(36, 2);
    std::vector<media_extractor::ForwardObservation> dsc;
    check(media_extractor::decode_forward_output(bytes.data(), bytes.size(), context, dsc, error) && dsc.empty(), "DSC treated as ADAS");

    std::vector<media_extractor::ForwardFrameTime> times{{1000, 0}, {1040, 1}, {1080, 2}};
    auto regions = media_extractor::forward_regions(parsed, times, 100, 50, false);
    check(regions.size() == 2, "same-frame objects collapsed");
    check(regions[0].first == 1 && regions[0].last == 2, "AI timestamp alignment/clip-bounded hold failed");
    check(regions[0].x == 5 && regions[0].y == 10 && regions[0].width == 20, "coordinate scaling failed");
    auto fallback = media_extractor::forward_regions(parsed, {}, 100, 50, true);
    check(fallback.empty(), "unsafe carrier-frame fallback accepted");
    parsed[0].pts = 900;
    check(media_extractor::forward_regions(parsed, times, 100, 50, true).empty(), "pre-clip detection leaked into output");

    using media_extractor::MetadataTimeline;
    using media_extractor::TimeMatchStatus;
    MetadataTimeline delayed({{1000, 0}, {1040, 1}, {1080, 2}, {1120, 3}});
    check(delayed.match(1000, 3).frame == 0, "late face/object observation used carrier frame");
    check(delayed.match(1080, 3).frame == 2, "different AI timestamps in one carrier collapsed");
    check(delayed.match(900, 3).status == TimeMatchStatus::OutsideTimeline, "pre-roll was not identified");
    check(delayed.match(999, 3).status == TimeMatchStatus::OutsideTimeline, "pre-roll leaked into first frame");
    check(delayed.match(1121, 3).status == TimeMatchStatus::OutsideTimeline, "post-roll leaked into last frame");
    check(MetadataTimeline({}).match(1000, 3).status == TimeMatchStatus::MissingTimeline, "missing PTS accepted");
    MetadataTimeline reordered({{1000, 0}, {1080, 1}, {1040, 2}, {1120, 3}});
    check(reordered.match(1040, 3).frame == 2, "out-of-order PTS disabled alignment");
    MetadataTimeline gap({{1000, 0}, {1040, 1}, {1800, 2}, {1840, 3}});
    check(gap.match(1400, 3).status == TimeMatchStatus::Unmatched, "large nearest-frame distance accepted");
    check(gap.match(1090, 3).status == TimeMatchStatus::Matched, "50 ms boundary rejected");
    check(gap.match(1091, 3).status == TimeMatchStatus::Unmatched, "51 ms boundary accepted");
    MetadataTimeline reset({{10000, 0}, {10040, 1}, {1000, 2}, {1040, 3}});
    check(reset.match(1000, 3).frame == 2, "clock reset associated result with wrong segment");
    check(reset.match(10000, 3).status == TimeMatchStatus::Unmatched, "result crossed clock reset");
    check(reset.match(10000, 1).segment_last_frame == 1, "hold crossed clock reset");
    std::vector<media_extractor::ForwardFrameTime> discontinuity{{1000, 0}, {1040, 1}, {5000, 2}, {5040, 3}};
    parsed[0].pts = 1000;
    parsed[0].carrier_frame = 1;
    auto limited = media_extractor::forward_regions(parsed, discontinuity, 100, 50, true);
    check(limited.size() == 2 && limited[0].last == 1, "forward hold crossed timestamp gap");
    parsed[0].pts = 1400;
    parsed[0].carrier_frame = 3;
    media_extractor::TimingStats timing;
    check(media_extractor::forward_regions(parsed, {{1000, 0}, {1800, 3}}, 100, 50, true, &timing).empty()
          && timing.unmatched == 1, "unmatched forward observation not reported");
    MetadataTimeline large_pts({{UINT64_MAX - 80, 0}, {UINT64_MAX - 40, 1}});
    check(large_pts.match(UINT64_MAX - 60, 1).frame == 0, "PTS subtraction overflowed");

    std::vector<media_extractor::ForwardFrameTime> sequence;
    for (int i = 0; i < 40; ++i) sequence.push_back({uint64_t(1000 + 40 * i), i});
    media_extractor::ForwardObservation first{1000, 1, 200, 100, {{10, 10, 20, 20, 1}}, 1};
    auto moved = first;
    moved.pts = 1080; moved.carrier_frame = 3; moved.boxes[0].x = 100;
    auto empty = first;
    empty.pts = 1160; empty.carrier_frame = 5; empty.boxes.clear();
    media_extractor::ForwardObservation person{1000, 1, 200, 100, {{50, 20, 10, 30, 7}}, 7};
    for (bool blur : {false, true}) {
        auto snapshots = media_extractor::forward_regions({first, moved, empty, person}, sequence, 200, 100, blur);
        check(snapshots.size() == 3, "snapshot objects lost or ghost regions added");
        check(snapshots[0].x == 10 && snapshots[0].width == 20 && snapshots[0].first == 0 && snapshots[0].last == 1,
              "old vehicle held after replacement or merged with future position");
        check(snapshots[1].x == 100 && snapshots[1].first == 2 && snapshots[1].last == 3,
              "empty vehicle snapshot did not clear previous detection");
        check(snapshots[2].kind == 7 && snapshots[2].last == 30,
              "vehicle update incorrectly cleared independent pedestrian hold");
    }
    auto same_frame = first;
    same_frame.pts = 1010; same_frame.boxes[0].x = 150;
    auto nearest = media_extractor::forward_regions({same_frame, first}, sequence, 200, 100, true);
    check(nearest.size() == 1 && nearest[0].x == 10 && nearest[0].width == 20,
          "multiple updates on one frame produced a temporal union");
    auto unchanged = first;
    unchanged.pts = 1080; unchanged.carrier_frame = 3;
    auto compact = media_extractor::forward_regions({first, unchanged}, sequence, 200, 100, true);
    check(compact.size() == 1 && compact[0].last == 32, "identical snapshots were not coalesced");

    media_extractor::ForwardPrivacy collector("/missing/sdk");
    media_extractor::MediaFrameHeader hdr;
    hdr.frame_type = media_extractor::FrameType::H264I;
    const uint8_t truncated[] = {13, 0, 0, 0};
    check(!collector.read_frame(hdr, truncated, sizeof(truncated), 0), "malformed extension accepted");
    check(!collector.error().empty(), "missing error surface");
    return 0;
}
