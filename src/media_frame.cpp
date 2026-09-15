#include "media_frame.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace media_extractor {
namespace {

enum class ExtensionInfoType : uint8_t {
    Video = 1,
    RealTime = 2,
    VirtualTime = 3,
    List = 4,
    Audio = 5,
    VideoStatus = 6,
    H264 = 7,
    VideoReference = 8,
    VideoEncoding = 9,
    AdditionalData = 10,
    GPS = 11,
    EncodingExtension = 12,
    VideoAIInfo = 13,
    DataSecurity = 14,
    IndustryData = 15,
    CustomDataSecurity = 16,
};

static uint32_t be24(const uint8_t* p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

static uint32_t le24(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

static uint16_t le16(const uint8_t* p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t* p)
{
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) |
           ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

static std::string to_hex(const uint8_t* p, size_t n)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out[i * 2] = kHex[p[i] >> 4];
        out[i * 2 + 1] = kHex[p[i] & 0x0f];
    }
    return out;
}

static uint32_t read_bits(const uint8_t* data, uint32_t* bit_pos, int n)
{
    uint32_t val = 0;
    for (int i = 0; i < n; ++i) {
        uint32_t byte_idx = *bit_pos / 8;
        uint32_t bit_idx = 7 - (*bit_pos % 8);
        val = (val << 1) | ((data[byte_idx] >> bit_idx) & 1u);
        (*bit_pos)++;
    }
    return val;
}

static std::string lower_ascii(std::string s)
{
    for (char& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

static std::string printable_ascii(const uint8_t* p, size_t n)
{
    std::string out;
    out.reserve(n);
    bool last_space = true;
    for (size_t i = 0; i < n; ++i) {
        uint8_t c = p[i];
        if (c >= 0x20 && c < 0x7f) {
            out += (char)c;
            last_space = false;
        } else if (!last_space) {
            out += ' ';
            last_space = true;
        }
    }
    while (!out.empty() && std::isspace((unsigned char)out.back()))
        out.pop_back();
    return out;
}

static std::string trim_token(std::string s)
{
    while (!s.empty() && (s.back() == '"' || s.back() == '\'' ||
                          s.back() == ',' || s.back() == ';' ||
                          s.back() == ')' || s.back() == ']')) {
        s.pop_back();
    }
    size_t p = 0;
    while (p < s.size() && (s[p] == '"' || s[p] == '\'' ||
                            s[p] == '(' || s[p] == '[')) {
        ++p;
    }
    return s.substr(p);
}

static bool contains_word(const std::string& lower, const std::string& word)
{
    size_t p = lower.find(word);
    while (p != std::string::npos) {
        bool left_ok = p == 0 ||
            !(std::isalnum((unsigned char)lower[p - 1]) || lower[p - 1] == '_');
        size_t r = p + word.size();
        bool right_ok = r >= lower.size() ||
            !(std::isalnum((unsigned char)lower[r]) || lower[r] == '_');
        if (left_ok && right_ok)
            return true;
        p = lower.find(word, p + 1);
    }
    return false;
}

static bool contains_any(const std::string& lower,
                         std::initializer_list<const char*> words)
{
    for (const char* word : words) {
        if (lower.find(lower_ascii(word)) != std::string::npos)
            return true;
    }
    return false;
}

static std::string find_value(const std::string& flat, const std::string& lower,
                              std::initializer_list<const char*> keys,
                              size_t max_len = 80)
{
    for (const char* key_c : keys) {
        std::string key = lower_ascii(key_c);
        size_t pos = 0;
        while (true) {
            size_t p = lower.find(key, pos);
            if (p == std::string::npos)
                break;
            if (p > 0) {
                char pc = lower[p - 1];
                if (std::isalnum((unsigned char)pc) || pc == '_') {
                    pos = p + 1;
                    continue;
                }
            }
            size_t q = p + key.size();
            while (q < flat.size() && (flat[q] == ' ' || flat[q] == '\t' ||
                                       flat[q] == '"' || flat[q] == '\'')) {
                ++q;
            }
            if (q >= flat.size() || (flat[q] != ':' && flat[q] != '=' && flat[q] != '/')) {
                pos = p + 1;
                continue;
            }
            ++q;
            while (q < flat.size() && (flat[q] == ' ' || flat[q] == '\t' ||
                                       flat[q] == '"' || flat[q] == '\'')) {
                ++q;
            }
            std::string v;
            while (q < flat.size() && v.size() < max_len) {
                char c = flat[q++];
                if (c == ' ' || c == '|' || c == ';' || c == ')' || c == ']')
                    break;
                if (c == ',' && !v.empty())
                    break;
                v += c;
            }
            v = trim_token(v);
            if (!v.empty())
                return v;
            pos = p + 1;
        }
    }
    return {};
}

static void add_field(MetadataEvent& ev, const std::string& key,
                      const std::string& value)
{
    if (value.empty())
        return;
    ev.fields[key] = value;
    if (!ev.text.empty())
        ev.text += "|";
    ev.text += key + "=" + value;
}

static void add_field_u64(MetadataEvent& ev, const std::string& key, uint64_t value)
{
    add_field(ev, key, std::to_string((unsigned long long)value));
}

static std::string detect_ai_type(const std::string& lower)
{
    if (contains_any(lower, {"ttc:", "sttc:", "cipv", "vdis:", "fcw", "hmw"}))
        return "fcw_hmw";
    if (contains_any(lower, {"ldw", "lane_departure"}))
        return "ldw";
    if (contains_any(lower, {"pcw", "pedestrian"}))
        return "pcw";
    if (contains_any(lower, {"tsr", "speed_limit", "rolling_stop"}))
        return "tsr";
    if (contains_any(lower, {"dsc_face_detect", "face_detect", "nodrvsw", "no_driver"}))
        return "dsc_face_detect";
    if (contains_any(lower, {"dsc_yawn_and_pose", "yawn", "head_pose", "distracted", "eyelookdown"}))
        return "dsc_yawn_and_pose";
    if (contains_any(lower, {"dsc_eys_status", "eye", "eys", "fatisw", "fatigue"}))
        return "dsc_eys_status";
    if (contains_any(lower, {"dsc_call", "callsw", "phone"}))
        return "dsc_call";
    if (contains_any(lower, {"dsc_belt", "beltsw", "belt"}))
        return "dsc_belt";
    return {};
}

static std::string detect_ai_category(const std::string& lower,
                                      const std::string& ai_type)
{
    if (ai_type.rfind("dsc_", 0) == 0 ||
        contains_any(lower, {"dms", "face", "driver_ok", "eyelookdown",
                             "fatisw", "callsw", "smksw", "beltsw",
                             "nodrvsw", "nomasksw", "sheltersw",
                             "custom_dsc"})) {
        return "dsc";
    }
    if (ai_type == "fcw_hmw" || ai_type == "ldw" || ai_type == "pcw" ||
        ai_type == "tsr" || lower.find("custom_adplus") != std::string::npos) {
        return "adas";
    }
    return {};
}

static std::string detect_ai_alarm(const std::string& lower)
{
    static const char* alarms[] = {
        "adas_fcw_collision",
        "adas_hmw_car_distance_close",
        "adas_ldw_lane_departure_left",
        "adas_ldw_lane_departure_right",
        "adas_pcw_pedestrian_collision",
        "tsr_stop_alarm",
        "tsr_speed_limit_alarm",
        "no_driver",
        "fatigue",
        "distracted",
        "phone_detected",
        "no_belt",
    };
    std::string out;
    for (const char* alarm : alarms) {
        if (!contains_word(lower, alarm))
            continue;
        if (!out.empty())
            out += ",";
        out += alarm;
    }
    return out;
}

static std::string normalize_speed(std::string v)
{
    if (v.empty())
        return {};
    size_t p = 0;
    while (p < v.size() && !(std::isdigit((unsigned char)v[p]) ||
                             v[p] == '-' || v[p] == '+')) {
        ++p;
    }
    if (p >= v.size())
        return {};
    size_t q = p + 1;
    while (q < v.size() && (std::isdigit((unsigned char)v[q]) || v[q] == '.'))
        ++q;
    char* endp = nullptr;
    double d = std::strtod(v.substr(p, q - p).c_str(), &endp);
    if (!endp || d < 0.0 || d > 400.0)
        return {};
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f", d);
    return buf;
}

static bool dir_is_lat(char c) { return c == 'N' || c == 'S'; }
static bool dir_is_lng(char c) { return c == 'E' || c == 'W'; }

static bool parse_rmfi_gps_frame(const uint8_t* p, size_t n,
                                 std::string& lat_out,
                                 std::string& lng_out,
                                 std::string& speed_out,
                                 std::string& heading_out)
{
    if (n < 20 || p[0] != 'A')
        return false;
    char dlat = (char)p[18];
    char dlng = (char)p[19];
    if (!dir_is_lat(dlat) || !dir_is_lng(dlng))
        return false;
    unsigned lat_deg = p[4];
    unsigned lat_min = p[5];
    unsigned lng_deg = p[6];
    unsigned lng_min = p[7];
    if (lat_deg > 90 || lat_min >= 60 || lng_deg > 180 || lng_min >= 60)
        return false;
    uint32_t lat_sec = le32(p + 8);
    uint32_t lng_sec = le32(p + 12);
    if (lat_sec >= 600000000u || lng_sec >= 600000000u)
        return false;
    double lat = (double)lat_deg + ((double)lat_min / 60.0) +
                 ((double)lat_sec / (3600.0 * 10000000.0));
    double lng = (double)lng_deg + ((double)lng_min / 60.0) +
                 ((double)lng_sec / (3600.0 * 10000000.0));
    if (dlat == 'S')
        lat = -lat;
    if (dlng == 'W')
        lng = -lng;
    uint16_t speed = le16(p + 2);
    uint16_t heading = le16(p + 16);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", lat);
    lat_out = buf;
    std::snprintf(buf, sizeof(buf), "%.6f", lng);
    lng_out = buf;
    speed_out = std::to_string((unsigned)speed);
    if (heading <= 36000 && heading >= 360) {
        std::snprintf(buf, sizeof(buf), "%.2f", (double)heading / 100.0);
        heading_out = buf;
    } else {
        heading_out = std::to_string((unsigned)heading);
    }
    return true;
}

static const char* industry_type_name(uint8_t industry_type)
{
    return industry_type == 2 ? "osd_mosaic" : "reserved";
}

static const char* industry_item_name(uint8_t data_type)
{
    switch (data_type) {
    case 0: return "mosaic";
    case 1: return "basic";
    case 2: return "alarm";
    case 3: return "channel";
    case 4: return "vehicle";
    case 5: return "realtime";
    default: return "unknown";
    }
}

static const char* mosaic_ai_state(unsigned ai_en)
{
    switch (ai_en) {
    case 0: return "not_detected";
    case 1: return "detected";
    case 2: return "detected_masked";
    default: return "reserved";
    }
}

static size_t industry_min_payload_len(uint8_t data_type)
{
    switch (data_type) {
    case 0: return 5;
    case 1: return 8;
    case 2: return 4;
    case 3: return 16;
    case 4: return 64;
    case 5: return 4;
    default: return 0;
    }
}

static bool resolve_industry_item_layout(uint8_t data_type, uint16_t len,
                                         size_t off, size_t items_avail,
                                         size_t& item_total,
                                         size_t& payload_off,
                                         size_t& payload_len)
{
    const size_t min_payload = industry_min_payload_len(data_type);
    const bool whole_ok = len >= 4 && off + len <= items_avail;
    const bool payload_ok = off + 4u + len <= items_avail;
    if (whole_ok && (min_payload == 0 || (size_t)len - 4 >= min_payload)) {
        item_total = len;
        payload_off = off + 4;
        payload_len = len - 4;
        return true;
    }
    if (payload_ok && (min_payload == 0 || len >= min_payload)) {
        item_total = 4u + len;
        payload_off = off + 4;
        payload_len = len;
        return true;
    }
    return false;
}

static MetadataEvent parse_type15(uint64_t frame_index, const uint8_t* p, size_t n)
{
    MetadataEvent ev;
    ev.frame_index = frame_index;
    ev.kind = "T15_DATA";
    add_field(ev, "type", "T15_DATA");
    if (!p || n < 12)
        return ev;

    uint16_t body_len = le16(p);
    uint8_t industry_type = p[2];
    if (body_len > n - 4)
        body_len = (uint16_t)(n - 4);
    const uint8_t* body = p + 4;
    if (body_len < 8)
        return ev;

    uint8_t data_ver = body[0] & 0x0f;
    uint8_t data_count = body[1];
    uint16_t items_len = le16(body + 2);
    uint32_t type_mask = le32(body + 4);
    add_field_u64(ev, "industry", industry_type);
    add_field(ev, "industry_name", industry_type_name(industry_type));
    add_field_u64(ev, "ver", data_ver);
    add_field_u64(ev, "count", data_count);
    add_field_u64(ev, "data_len", items_len);

    size_t items_avail = body_len - 8;
    if (items_len > 0 && items_len < items_avail)
        items_avail = items_len;
    const uint8_t* q = body + 8;
    size_t off = 0;
    for (uint8_t i = 0; i < data_count && off + 4 <= items_avail; ++i) {
        uint8_t data_type = q[off];
        uint16_t len = le16(q + off + 1);
        size_t item_total = 0, payload_off = off + 4, payload_len = 0;
        if (!resolve_industry_item_layout(data_type, len, off, items_avail,
                                          item_total, payload_off, payload_len)) {
            break;
        }
        const uint8_t* payload = q + payload_off;
        std::string prefix = std::string("item") + std::to_string(i);
        add_field(ev, prefix + "_name", industry_item_name(data_type));
        add_field_u64(ev, prefix + "_type", data_type);
        add_field_u64(ev, prefix + "_len", len);
        if (data_type == 0 && payload_len >= 5) {
            unsigned ai_en = payload[0] & 0x07u;
            unsigned mosic_type = payload[1];
            add_field_u64(ev, prefix + "_ai_en", ai_en);
            add_field(ev, prefix + "_ai_state", mosaic_ai_state(ai_en));
            add_field_u64(ev, prefix + "_mosic_type", mosic_type);
            if (mosic_type & 0x01u)
                add_field(ev, prefix + "_bsd_pedestrian", "1");
            if (mosic_type & 0x02u)
                add_field(ev, prefix + "_bsd_vehicle", "1");
        } else if (data_type == 2 && payload_len >= 4) {
            add_field_u64(ev, prefix + "_alarms", payload[0]);
        } else if (data_type == 5 && payload_len >= 4) {
            uint16_t v = le16(payload);
            add_field_u64(ev, prefix + "_speed", v & 0x01ffu);
            add_field_u64(ev, prefix + "_turn", (v >> 9) & 0x03u);
        }
        off += item_total;
    }

    (void)type_mask;
    return ev;
}

static MetadataEvent parse_type13(uint64_t frame_index,
                                  const MediaFrameHeader& hdr,
                                  const uint8_t* data,
                                  size_t n)
{
    MetadataEvent ev;
    ev.frame_index = frame_index;
    ev.kind = "T13_DATA";
    add_field(ev, "type", "T13_DATA");
    if (hdr.has_virtual_time)
        add_field_u64(ev, "video_pts", hdr.virtual_pts);

    uint32_t ai_reserve = 0;
    uint64_t ai_pts = 0;
    uint32_t ai_body_len = 0;
    const uint8_t* body = data;
    size_t body_len = n;
    if (n >= 16) {
        ai_reserve = le32(data);
        ai_pts = le64(data + 4);
        ai_body_len = le32(data + 12);
        if (ai_body_len > 0 && ai_body_len <= n - 16) {
            body = data + 16;
            body_len = ai_body_len;
            add_field_u64(ev, "ai_pts", ai_pts);
            add_field_u64(ev, "ai_body_len", ai_body_len);
            if (ai_reserve)
                add_field_u64(ev, "ai_reserve", ai_reserve);
        }
    }

    std::string flat = printable_ascii(body, body_len);
    std::string lower = lower_ascii(flat);
    std::string ai_type = detect_ai_type(lower);
    add_field(ev, "ai_category", detect_ai_category(lower, ai_type));
    add_field(ev, "ai_type", ai_type);
    add_field(ev, "ai_alarm", detect_ai_alarm(lower));
    add_field(ev, "speed", normalize_speed(find_value(flat, lower, {"speed", "spd", "speedle"})));
    add_field(ev, "fps", find_value(flat, lower, {"fps"}));
    add_field(ev, "jt", find_value(flat, lower, {"jt"}));
    add_field(ev, "fcnt", find_value(flat, lower, {"fcnt"}));
    add_field(ev, "alarm_num", find_value(flat, lower, {"alarmnum"}));
    add_field(ev, "ala", find_value(flat, lower, {"ala"}));
    add_field(ev, "ebitm", find_value(flat, lower, {"ebitm"}));
    add_field(ev, "warn", find_value(flat, lower, {"warn", "warnt"}));
    add_field(ev, "danger", find_value(flat, lower, {"danger"}));
    add_field(ev, "spf", find_value(flat, lower, {"spf"}));
    add_field(ev, "rx", find_value(flat, lower, {"rx"}));
    add_field(ev, "face_count", find_value(flat, lower, {"facecnt", "face_count"}));
    add_field(ev, "face_status", find_value(flat, lower, {"facesta"}));
    add_field(ev, "face_score", find_value(flat, lower, {"objproface"}));
    add_field(ev, "face_rect", find_value(flat, lower, {"rlast", "face_rect"}));
    add_field(ev, "mask_state", find_value(flat, lower, {"c_state"}));
    add_field(ev, "mask_score", find_value(flat, lower, {"c_score"}));
    add_field(ev, "mask_alarm", find_value(flat, lower, {"nomask"}));
    add_field(ev, "mask_rect", find_value(flat, lower, {"sid"}));
    add_field(ev, "phone_rect", find_value(flat, lower, {"phoner"}));
    add_field(ev, "ttc", find_value(flat, lower, {"ttc", "sttc"}));
    add_field(ev, "car_distance", find_value(flat, lower, {"vdis", "distance"}));
    add_field(ev, "vanishing_point", find_value(flat, lower, {"vanishing_point", "vp"}));
    add_field(ev, "dms_type", find_value(flat, lower, {"dmstype"}));
    add_field(ev, "dms_state", find_value(flat, lower, {"dmsst"}));
    add_field(ev, "dms_rt", find_value(flat, lower, {"dmsrt"}));
    add_field(ev, "vehicle", find_value(flat, lower, {"vehi"}));
    add_field(ev, "ldw", find_value(flat, lower, {"ldw"}));
    add_field(ev, "fatigue_switch", find_value(flat, lower, {"fatisw"}));
    add_field(ev, "lane_switch", find_value(flat, lower, {"ldsw"}));
    add_field(ev, "eat_switch", find_value(flat, lower, {"eatsw"}));
    add_field(ev, "drink_switch", find_value(flat, lower, {"drinksw"}));
    add_field(ev, "call_switch", find_value(flat, lower, {"callsw"}));
    add_field(ev, "smoke_switch", find_value(flat, lower, {"smksw"}));
    add_field(ev, "belt_switch", find_value(flat, lower, {"beltsw"}));
    if (contains_word(lower, "driver_ok"))
        add_field(ev, "status", "DRIVER_OK");
    return ev;
}

} // namespace

bool is_video_frame(FrameType frame_type)
{
    return frame_type == FrameType::H264I || frame_type == FrameType::H264P ||
           frame_type == FrameType::H265I || frame_type == FrameType::H265P ||
           frame_type == FrameType::Mjpeg;
}

bool is_h264_frame(FrameType frame_type)
{
    return frame_type == FrameType::H264I || frame_type == FrameType::H264P;
}

bool is_h265_frame(FrameType frame_type)
{
    return frame_type == FrameType::H265I || frame_type == FrameType::H265P;
}

bool is_key_frame(FrameType frame_type)
{
    return frame_type == FrameType::H264I || frame_type == FrameType::H265I ||
           frame_type == FrameType::Mjpeg;
}

bool has_annexb_start_code(const uint8_t* data, size_t len)
{
    return (len >= 4 && data[0] == 0x00 && data[1] == 0x00 &&
            data[2] == 0x00 && data[3] == 0x01) ||
           (len >= 3 && data[0] == 0x00 && data[1] == 0x00 &&
            data[2] == 0x01);
}

bool media_frame_parse_fixed(const uint8_t* data, size_t len,
                             MediaFrameHeader& hdr)
{
    if (!data || len < kMediaFrameFixedHeaderSize)
        return false;
    hdr = {};
    hdr.channel = data[0];
    hdr.frame_type = static_cast<FrameType>(be24(data + 1));
    hdr.frame_length = le24(data + 4);
    hdr.stream_exam = data[7];
    hdr.extend_length = le24(data + 8);
    hdr.extend_count = data[11];
    hdr.total_header = kMediaFrameFixedHeaderSize + hdr.extend_length;
    return true;
}

void media_frame_parse_extensions(const uint8_t* ext_data, uint32_t ext_len,
                                  uint64_t frame_index,
                                  MediaFrameHeader& hdr)
{
    uint32_t pos = 0;
    while (pos + 4 <= ext_len) {
        uint8_t type = ext_data[pos];
        uint32_t entry_len = le24(ext_data + pos + 1);
        if (entry_len < 4 || pos + entry_len > ext_len)
            break;
        uint32_t data_bytes = entry_len - 4;
        const uint8_t* entry_data = ext_data + pos + 4;

        switch (static_cast<ExtensionInfoType>(type)) {
        case ExtensionInfoType::Video:
            if (data_bytes >= 4) {
                uint8_t rev[4] = {entry_data[3], entry_data[2], entry_data[1], entry_data[0]};
                uint32_t bit_pos = 0;
                hdr.video_info.fps = read_bits(rev, &bit_pos, 8);
                hdr.video_info.height = read_bits(rev, &bit_pos, 12);
                hdr.video_info.width = read_bits(rev, &bit_pos, 12);
                hdr.has_video_info = true;
            }
            break;
        case ExtensionInfoType::RealTime:
            if (data_bytes >= 8) {
                hdr.realtime.year = (uint16_t)(2000 + entry_data[0]);
                hdr.realtime.month = entry_data[1];
                hdr.realtime.day = entry_data[2];
                hdr.realtime.hour = entry_data[3];
                hdr.realtime.minute = entry_data[4];
                hdr.realtime.second = entry_data[5];
                hdr.has_realtime = true;
            }
            break;
        case ExtensionInfoType::VirtualTime:
            if (data_bytes >= 8) {
                hdr.virtual_pts = le64(entry_data);
                hdr.has_virtual_time = true;
            }
            break;
        case ExtensionInfoType::Audio:
            if (data_bytes >= 4) {
                uint8_t rev[4] = {entry_data[3], entry_data[2], entry_data[1], entry_data[0]};
                uint32_t bit_pos = 0;
                read_bits(rev, &bit_pos, 2);
                hdr.audio_info.sample_rate = read_bits(rev, &bit_pos, 18);
                hdr.audio_info.bit_width = read_bits(rev, &bit_pos, 6);
                read_bits(rev, &bit_pos, 1);
                hdr.audio_info.channels = (uint8_t)read_bits(rev, &bit_pos, 1) + 1;
                hdr.audio_info.codec = static_cast<AudioCodec>(read_bits(rev, &bit_pos, 4));
                hdr.has_audio_info = true;
            }
            break;
        case ExtensionInfoType::VideoEncoding:
            if (data_bytes >= 1) {
                hdr.video_encoding = entry_data[0];
                hdr.has_video_encoding = true;
            }
            break;
        case ExtensionInfoType::GPS:
            if (data_bytes >= 20) {
                std::string lat, lng, speed, heading;
                if (parse_rmfi_gps_frame(entry_data, data_bytes, lat, lng, speed, heading)) {
                    MetadataEvent ev;
                    ev.frame_index = frame_index;
                    ev.kind = "GPS";
                    add_field(ev, "type", "GPS");
                    add_field(ev, "lat", lat);
                    add_field(ev, "lng", lng);
                    add_field(ev, "gps_speed", speed);
                    add_field(ev, "heading", heading);
                    hdr.extension_events.push_back(std::move(ev));
                }
            }
            break;
        case ExtensionInfoType::VideoAIInfo: {
            MetadataEvent ev = parse_type13(frame_index, hdr, entry_data, data_bytes);
            if (ev.fields.size() > 1)
                hdr.extension_events.push_back(std::move(ev));
            break;
        }
        case ExtensionInfoType::IndustryData: {
            MetadataEvent ev = parse_type15(frame_index, entry_data, data_bytes);
            if (ev.fields.size() > 1)
                hdr.extension_events.push_back(std::move(ev));
            break;
        }
        default:
            break;
        }

        pos += entry_len;
    }
}

bool looks_like_media_frame_at(const uint8_t* data, size_t len)
{
    if (!data || len < kMediaFrameFixedHeaderSize)
        return false;
    if (data[1] < 0x32 || data[1] > 0x37 || data[2] != 0x64 || data[3] != 0x63)
        return false;
    MediaFrameHeader hdr;
    if (!media_frame_parse_fixed(data, len, hdr))
        return false;
    uint64_t need = (uint64_t)hdr.total_header + hdr.frame_length;
    return need >= kMediaFrameFixedHeaderSize && need <= len;
}

size_t find_media_frame_offset(const uint8_t* data, size_t len, size_t max_scan)
{
    if (!data || len < kMediaFrameFixedHeaderSize)
        return SIZE_MAX;
    size_t end = std::min(len, max_scan);
    for (size_t i = 0; i + kMediaFrameFixedHeaderSize <= end; ++i) {
        if (looks_like_media_frame_at(data + i, len - i))
            return i;
    }
    return SIZE_MAX;
}

std::string frame_type_name(FrameType frame_type)
{
    switch (frame_type) {
    case FrameType::H264I: return "h264_i";
    case FrameType::H264P: return "h264_p";
    case FrameType::Audio: return "audio";
    case FrameType::H265I: return "h265_i";
    case FrameType::H265P: return "h265_p";
    case FrameType::Mjpeg: return "mjpeg";
    case FrameType::None: return "none";
    }
    return "unknown";
}

std::string audio_codec_name(AudioCodec codec)
{
    switch (codec) {
    case AudioCodec::Adpcm: return "adpcm";
    case AudioCodec::G726: return "g726";
    case AudioCodec::G711A: return "g711a";
    case AudioCodec::G711U: return "g711u";
    case AudioCodec::Amr: return "amr";
    }
    return "unknown";
}

} // namespace media_extractor
