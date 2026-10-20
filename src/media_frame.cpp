#include "media_frame.h"
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <utility>

#include <algorithm>

namespace media_extractor {

static char hex_digit(unsigned v)
{
    return (v < 10) ? (char)('0' + v) : (char)('a' + (v - 10));
}

static std::string to_hex(const uint8_t* p, size_t n)
{
    std::string out;
    out.resize(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out[2 * i]     = hex_digit((unsigned)(p[i] >> 4));
        out[2 * i + 1] = hex_digit((unsigned)(p[i] & 0x0F));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// FrameType is read 24 bits MSB-first (big-endian).
static inline uint32_t be24(const uint8_t* p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

// FrameLength, ExtendLength, and extension-entry lengths are stored
// little-endian 24-bit (BytesToUInt with padLeft=false in the C# SDK).
static inline uint32_t le24(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

static inline uint16_t le16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint16_t be16(const uint8_t* p) {
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static inline uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static inline uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24)
         | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)
         |  (uint32_t)p[3];
}

static inline uint64_t le64(const uint8_t* p) {
    return ((uint64_t)p[0])
         | ((uint64_t)p[1] << 8)
         | ((uint64_t)p[2] << 16)
         | ((uint64_t)p[3] << 24)
         | ((uint64_t)p[4] << 32)
         | ((uint64_t)p[5] << 40)
         | ((uint64_t)p[6] << 48)
         | ((uint64_t)p[7] << 56);
}

static std::string printable_ascii(const uint8_t* p, size_t n)
{
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        uint8_t c = p[i];
        if (c == 0) break;
        if (c >= 0x20 && c < 0x7f)
            out += (char)c;
    }
    while (!out.empty() && std::isspace((unsigned char)out.back()))
        out.pop_back();
    size_t b = 0;
    while (b < out.size() && std::isspace((unsigned char)out[b]))
        ++b;
    if (b > 0) out.erase(0, b);
    return out;
}

// Read `n` bits from `data` at bit-offset `*bit_pos` (MSB-first per byte).
// Advances *bit_pos by n.
static uint32_t read_bits(const uint8_t* data, uint32_t* bit_pos, int n)
{
    uint32_t val = 0;
    for (int i = 0; i < n; ++i) {
        uint32_t byte_idx = *bit_pos / 8;
        uint32_t bit_idx  = 7 - (*bit_pos % 8);
        val = (val << 1) | ((data[byte_idx] >> bit_idx) & 0x01);
        (*bit_pos)++;
    }
    return val;
}

static bool dir_is_lat(char c)
{
    return c == 'N' || c == 'S';
}

static bool dir_is_lng(char c)
{
    return c == 'E' || c == 'W';
}

static bool parse_rmfi_gps_at(const uint8_t* p, size_t n, size_t off,
                              bool little_endian_multi,
                              std::string& out_lat, std::string& out_lng,
                              std::string& out_speed, std::string& out_heading)
{
    // RMFI_GPS_FRAME (20 bytes):
    //   cGpsStatus(1), cSpeedUnit(1), usSpeed(2),
    //   cLatitudeDegree(1), cLatitudeCent(1),
    //   cLongitudeDegree(1), cLongitudeCent(1),
    //   lLatitudeSec(4), lLongitudeSec(4),
    //   usGpsAngle(2), cDirectionLatitude(1), cDirectionLongitude(1)
    if (off + 20 > n) return false;

    const uint8_t* q = p + off;
    if (q[0] != 'A')
        return false;

    char dlat = (char)q[18];
    char dlng = (char)q[19];
    if ((!dir_is_lat(dlat) || !dir_is_lng(dlng)) && off + 28 <= n) {
        // Some current media files carry an extended 40-byte type-11 GPS block
        // with the direction bytes at 26/27 while the first 20 bytes still
        // match RMFI_GPS_FRAME numeric fields.
        dlat = (char)q[26];
        dlng = (char)q[27];
    }
    if (!dir_is_lat(dlat) || !dir_is_lng(dlng))
        return false;

    unsigned lat_deg = q[4];
    unsigned lat_min = q[5];
    unsigned lng_deg = q[6];
    unsigned lng_min = q[7];
    if (lat_deg > 90 || lat_min >= 60 || lng_deg > 180 || lng_min >= 60)
        return false;

    uint32_t lat_sec = little_endian_multi ? le32(q + 8)  : be32(q + 8);
    uint32_t lng_sec = little_endian_multi ? le32(q + 12) : be32(q + 12);
    // Newest packaging spec stores seconds at 1e-7 precision:
    // valid range is [0, 60 * 10,000,000).
    if (lat_sec >= 600000000u || lng_sec >= 600000000u)
        return false;

    double lat = (double)lat_deg + ((double)lat_min / 60.0)
               + ((double)lat_sec / (3600.0 * 1.0e7));
    double lng = (double)lng_deg + ((double)lng_min / 60.0)
               + ((double)lng_sec / (3600.0 * 1.0e7));
    if (dlat == 'S') lat = -lat;
    if (dlng == 'W') lng = -lng;
    if (lat < -90.0 || lat > 90.0 || lng < -180.0 || lng > 180.0)
        return false;

    uint16_t speed_raw = little_endian_multi ? le16(q + 2)  : be16(q + 2);
    uint16_t ang_raw   = little_endian_multi ? le16(q + 16) : be16(q + 16);

    double heading = (double)ang_raw;
    if (heading >= 360.0 && heading <= 36000.0)
        heading /= 100.0;

    char lat_s[32], lng_s[32], spd_s[24], hdg_s[24];
    std::snprintf(lat_s, sizeof(lat_s), "%.6f", lat);
    std::snprintf(lng_s, sizeof(lng_s), "%.6f", lng);
    std::snprintf(spd_s, sizeof(spd_s), "%u", (unsigned)speed_raw);
    if (heading >= 0.0 && heading <= 360.0)
        std::snprintf(hdg_s, sizeof(hdg_s), "%.2f", heading);
    else
        hdg_s[0] = '\0';

    out_lat = lat_s;
    out_lng = lng_s;
    out_speed = spd_s;
    out_heading = hdg_s;
    return true;
}

static bool parse_rmfi_gps_frame(const uint8_t* p, size_t n,
                                 std::string& out_lat, std::string& out_lng,
                                 std::string& out_speed, std::string& out_heading)
{
    if (n < 20) return false;

    // Typical payloads place RMFI_GPS_FRAME at offset 0 (type 11), but some
    // firmwares wrap it inside another block (e.g. additional-data subtypes).
    size_t max_off = (n > 48) ? 48 : n - 20;
    for (size_t off = 0; off <= max_off; ++off) {
        if (parse_rmfi_gps_at(p, n, off, true, out_lat, out_lng, out_speed, out_heading))
            return true;
        if (parse_rmfi_gps_at(p, n, off, false, out_lat, out_lng, out_speed, out_heading))
            return true;
    }
    return false;
}

// Decode the 38-byte GPS record also embedded in media extensions.
// This allows media-frame extension payloads that embed the same structure to
// populate telemetry_lat/lng without relying on JSON packets.
static bool parse_special_gps_block_at(const uint8_t* p, size_t n, size_t off,
                                       std::string& out_lat, std::string& out_lng,
                                       std::string& out_speed, std::string& out_heading,
                                       std::string& out_alt)
{
    if (off + 38 > n) return false;
    const uint8_t* q = p + off;

    uint8_t reliability = q[0];
    uint8_t extended    = q[1];
    uint8_t realtime    = q[2];
    if (reliability > 2 || extended > 3 || realtime > 3)
        return false;

    // Timestamp bytes must be all ASCII digits and represent a plausible
    // UTC datetime (yyyyMMddHHmmss).
    for (int i = 0; i < 14; ++i) {
        if (q[24 + i] < '0' || q[24 + i] > '9')
            return false;
    }
    int month = (q[28] - '0') * 10 + (q[29] - '0');
    int day   = (q[30] - '0') * 10 + (q[31] - '0');
    int hour  = (q[32] - '0') * 10 + (q[33] - '0');
    int min   = (q[34] - '0') * 10 + (q[35] - '0');
    int sec   = (q[36] - '0') * 10 + (q[37] - '0');
    if (month < 1 || month > 12 || day < 1 || day > 31 ||
        hour > 23 || min > 59 || sec > 59)
        return false;

    uint32_t lng_raw = be32(q + 4);
    uint32_t lat_raw = be32(q + 8);
    double lng = (lng_raw & 0x7FFFFFFFu) / 1.0e6;
    double lat = (lat_raw & 0x7FFFFFFFu) / 1.0e6;
    if (lng_raw & 0x80000000u) lng = -lng;
    if (lat_raw & 0x80000000u) lat = -lat;
    if (lat < -90.0 || lat > 90.0 || lng < -180.0 || lng > 180.0)
        return false;

    uint32_t speed   = be32(q + 12);
    uint32_t heading = be32(q + 16);
    uint32_t alt     = be32(q + 20);
    if (speed > 1200 || heading > 36000u || alt > 20000u)
        return false;

    char lat_s[32], lng_s[32], spd_s[24], hdg_s[24], alt_s[24];
    std::snprintf(lat_s, sizeof(lat_s), "%.6f", lat);
    std::snprintf(lng_s, sizeof(lng_s), "%.6f", lng);
    std::snprintf(spd_s, sizeof(spd_s), "%u", (unsigned)speed);
    if (heading <= 36000u && heading >= 360u) {
        double hd = (double)heading / 100.0;
        std::snprintf(hdg_s, sizeof(hdg_s), "%.2f", hd);
    } else {
        std::snprintf(hdg_s, sizeof(hdg_s), "%u", (unsigned)heading);
    }
    std::snprintf(alt_s, sizeof(alt_s), "%u", (unsigned)alt);

    out_lat = lat_s;
    out_lng = lng_s;
    out_speed = spd_s;
    out_heading = hdg_s;
    out_alt = alt_s;
    return true;
}

static bool parse_special_gps_block(const uint8_t* p, size_t n,
                                    std::string& out_lat, std::string& out_lng,
                                    std::string& out_speed, std::string& out_heading,
                                    std::string& out_alt)
{
    if (n < 38) return false;
    size_t max_off = (n > 128) ? 128 : (n - 38);
    for (size_t off = 0; off <= max_off; ++off) {
        if (parse_special_gps_block_at(p, n, off,
                                       out_lat, out_lng, out_speed, out_heading, out_alt))
            return true;
    }
    return false;
}

static bool parse_industry_alarm_payload(const uint8_t* p, size_t n,
                                         std::vector<AlarmInfo>& alarms)
{
    // EXDATA_ALARM payload after RMFI2_EXDATA_ITEM_HEADER:
    //   count(1), reserve(3), repeated EXDATA_ALARM_ITEM:
    //   mainId le16, subId le16, string length(1), reserve(3), body[length].
    if (!p || n < 4) return false;
    uint8_t count = p[0];
    size_t off = 4;
    size_t before = alarms.size();
    for (uint8_t i = 0; i < count && off + 8 <= n; ++i) {
        AlarmInfo alarm;
        alarm.main_id = le16(p + off);
        alarm.sub_id  = le16(p + off + 2);
        uint8_t body_len = p[off + 4];
        off += 8;
        if (off + body_len > n)
            break;
        alarm.text = printable_ascii(p + off, body_len);
        off += body_len;
        alarms.push_back(std::move(alarm));
    }
    return alarms.size() > before;
}

static bool resolve_industry_item_layout(uint8_t data_type, uint16_t len,
                                         size_t off, size_t items_avail,
                                         size_t& item_total,
                                         size_t& payload_off,
                                         size_t& payload_len);

static bool parse_industry_extension_alarms(const uint8_t* p, size_t n,
                                            std::vector<AlarmInfo>& alarms)
{
    // RMFI2_INDUSTRYEXDATA:
    //   dataLen le16, cIndustryType(1), reserve(1), body[dataLen].
    // For overseas freight OSD/mosaic business, cIndustryType=2 and body is:
    //   dataVer/reserve(1), dataCount(1), dataLen le16, dataTypeMask le32,
    //   followed by RMFI2_EXDATA_ITEM_HEADER entries. Data type 2 is alarm.
    if (!p || n < 12) return false;
    uint16_t body_len = le16(p);
    uint8_t industry_type = p[2];
    if (industry_type != 2)
        return false;
    if (body_len > n - 4)
        body_len = (uint16_t)(n - 4);

    const uint8_t* body = p + 4;
    size_t body_avail = body_len;
    if (body_avail < 8) return false;

    uint8_t data_ver = body[0] & 0x0f;
    if (data_ver != 0) return false;
    uint8_t data_count = body[1];
    uint16_t items_len = le16(body + 2);
    size_t items_avail = body_avail - 8;
    if (items_len > 0 && items_len < items_avail)
        items_avail = items_len;

    const uint8_t* q = body + 8;
    size_t off = 0;
    size_t before = alarms.size();
    for (uint8_t i = 0; i < data_count && off + 4 <= items_avail; ++i) {
        uint8_t data_type = q[off];
        uint16_t len = le16(q + off + 1);

        size_t item_total = 0;
        size_t payload_off = off + 4;
        size_t payload_len = 0;
        if (!resolve_industry_item_layout(data_type, len, off, items_avail,
                                          item_total, payload_off, payload_len)) {
            break;
        }

        if (data_type == 2 && item_total >= 4)
            parse_industry_alarm_payload(q + payload_off, payload_len, alarms);

        off += item_total;
    }
    return alarms.size() > before;
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

static const char* industry_type_name(uint8_t industry_type)
{
    switch (industry_type) {
    case 0: return "reserved";
    case 1: return "reserved";
    case 2: return "osd_mosaic";
    default: return "reserved";
    }
}

static const char* industry_item_frame_scope(uint8_t data_type)
{
    switch (data_type) {
    case 0: return "I";
    case 1: return "I";
    case 2: return "I/P";
    case 3: return "I";
    case 4: return "I";
    case 5: return "I/P";
    default: return "unknown";
    }
}

static std::string industry_mask_names(uint32_t mask)
{
    std::string out;
    for (uint8_t bit = 0; bit < 32; ++bit) {
        if ((mask & (1u << bit)) == 0)
            continue;
        if (!out.empty()) out += ",";
        out += industry_item_name(bit);
        out += "(" + std::to_string((unsigned)bit) + ")";
    }
    return out;
}

static size_t industry_min_payload_len(uint8_t data_type)
{
    switch (data_type) {
    case 0: return 5;  // EXDATA_MOSIC payload after RMFI2_EXDATA_ITEM_HEADER.
    case 1: return 8;  // EXDATA_BASIC payload.
    case 2: return 4;  // EXDATA_ALARM count + reserve.
    case 3: return 16; // EXDATA_CHNNAME chnName[16].
    case 4: return 64; // EXDATA_VEHICLE vehicle[32] + bid[32].
    case 5: return 4;  // EXDATA_REALTIME_DATA speed/turn + reserve1.
    default: return 0;
    }
}

static bool resolve_industry_item_layout(uint8_t data_type, uint16_t len,
                                         size_t off, size_t items_avail,
                                         size_t& item_total,
                                         size_t& payload_off,
                                         size_t& payload_len)
{
    // Vendor reference material confirms RMFI2_EXDATA_ITEM_HEADER.length
    // includes the 4-byte item header. Keep a
    // payload-length fallback so older generated samples still parse.
    const size_t min_payload = industry_min_payload_len(data_type);
    const bool whole_ok = len >= 4 && off + len <= items_avail;
    const size_t whole_payload_len = whole_ok ? (size_t)len - 4 : 0;
    const bool payload_ok = off + 4u + len <= items_avail;
    const size_t payload_payload_len = payload_ok ? (size_t)len : 0;

    if (whole_ok && (min_payload == 0 || whole_payload_len >= min_payload)) {
        item_total = len;
        payload_off = off + 4;
        payload_len = whole_payload_len;
        return true;
    }
    if (payload_ok && (min_payload == 0 || payload_payload_len >= min_payload)) {
        item_total = 4u + len;
        payload_off = off + 4;
        payload_len = payload_payload_len;
        return true;
    }
    if (whole_ok) {
        item_total = len;
        payload_off = off + 4;
        payload_len = whole_payload_len;
        return true;
    }
    if (payload_ok) {
        item_total = 4u + len;
        payload_off = off + 4;
        payload_len = payload_payload_len;
        return true;
    }
    return false;
}

static const char* industry_mosaic_ai_state(unsigned ai_en)
{
    switch (ai_en) {
    case 0: return "not_detected";
    case 1: return "detected";
    case 2: return "detected_masked";
    default: return "reserved";
    }
}

static const char* industry_cali_state_name(unsigned cali_state)
{
    switch (cali_state) {
    case 0: return "not_calibrated";
    case 1: return "calibrated";
    case 2: return "invalid";
    default: return "reserved";
    }
}

static const char* industry_turn_name(unsigned turn)
{
    switch (turn) {
    case 0: return "none";
    case 1: return "left";
    case 2: return "right";
    case 3: return "hazard";
    default: return "unknown";
    }
}

static std::string parse_industry_extension_summary(const uint8_t* p, size_t n)
{
    if (!p || n < 12) return {};

    uint16_t body_len = le16(p);
    uint8_t industry_type = p[2];
    if (body_len > n - 4)
        body_len = (uint16_t)(n - 4);

    const uint8_t* body = p + 4;
    size_t body_avail = body_len;
    if (body_avail < 8) return {};

    uint8_t data_ver = body[0] & 0x0f;
    uint8_t data_count = body[1];
    uint16_t items_len = le16(body + 2);
    uint32_t type_mask = le32(body + 4);

    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "industry=%u|industry_name=%s|ver=%u|count=%u|data_len=%u|mask=0x%08x",
                  (unsigned)industry_type, industry_type_name(industry_type),
                  (unsigned)data_ver,
                  (unsigned)data_count, (unsigned)items_len,
                  (unsigned)type_mask);
    std::string out = buf;
    std::string mask_names = industry_mask_names(type_mask);
    if (!mask_names.empty()) {
        out += "|mask_items=";
        out += mask_names;
    }
    out += "|items=";

    size_t items_avail = body_avail - 8;
    if (items_len > 0 && items_len < items_avail)
        items_avail = items_len;

    const uint8_t* q = body + 8;
    size_t off = 0;
    bool first = true;
    for (uint8_t i = 0; i < data_count && off + 4 <= items_avail; ++i) {
        uint8_t data_type = q[off];
        uint16_t len = le16(q + off + 1);

        size_t item_total = 0;
        size_t payload_off = off + 4;
        size_t payload_len = 0;
        if (!resolve_industry_item_layout(data_type, len, off, items_avail,
                                          item_total, payload_off, payload_len)) {
            if (!first) out += ",";
            out += "truncated";
            break;
        }

        if (!first) out += ",";
        first = false;
        out += industry_item_name(data_type);
        out += "(" + std::to_string((unsigned)data_type) + ":len="
             + std::to_string((unsigned)len)
             + ",frames=" + industry_item_frame_scope(data_type);

        const uint8_t* payload = q + payload_off;
        if (data_type == 0 && payload_len >= 5) {
            unsigned ai_en = payload[0] & 0x07u;
            unsigned mosic_type = payload[1];
            out += ",ai_en=" + std::to_string(ai_en)
                + ",ai_state=" + industry_mosaic_ai_state(ai_en)
                + ",mosic_type=" + std::to_string(mosic_type);
            if (mosic_type & 0x01u) out += ",bsd_pedestrian=1";
            if (mosic_type & 0x02u) out += ",bsd_vehicle=1";
        } else if (data_type == 1 && payload_len >= 8) {
            uint32_t v = le32(payload);
            unsigned ai_use = v & 0x03ffu;
            unsigned cali_state = (v >> 10) & 0x07u;
            unsigned dst = (v >> 13) & 0x03u;
            int zone = (int)(int8_t)payload[4];
            out += ",ai_use=" + std::to_string(ai_use)
                + ",cali_state=" + std::to_string(cali_state)
                + ",cali_state_name=" + industry_cali_state_name(cali_state)
                + ",dst_30m=" + std::to_string(dst)
                + ",dst_min=" + std::to_string(dst * 30u)
                + ",zone_15m=" + std::to_string(zone)
                + ",zone_min=" + std::to_string(zone * 15);
        } else if (data_type == 2 && payload_len >= 4) {
            out += ",alarms=" + std::to_string((unsigned)payload[0]);
        } else if (data_type == 3 && payload_len >= 16) {
            std::string ch = printable_ascii(payload, 16);
            if (!ch.empty()) out += ",name=" + ch;
        } else if (data_type == 4 && payload_len >= 64) {
            std::string vehicle = printable_ascii(payload, 32);
            std::string bid = printable_ascii(payload + 32, 32);
            if (!vehicle.empty()) out += ",vehicle=" + vehicle;
            if (!bid.empty()) out += ",bid=" + bid;
        } else if (data_type == 5 && payload_len >= 4) {
            uint16_t v = le16(payload);
            unsigned speed = v & 0x01ffu;
            unsigned turn = (v >> 9) & 0x03u;
            out += ",speed=" + std::to_string(speed)
                + ",turn=" + std::to_string(turn)
                + ",turn_name=" + industry_turn_name(turn);
        }
        out += ")";

        off += item_total;
    }
    if (first)
        out += "none";
    return out;
}

// ---------------------------------------------------------------------------
// Frame type predicates
// ---------------------------------------------------------------------------

bool is_video_frame(FrameType ft)
{
    return ft == FrameType::H264I || ft == FrameType::H264P
        || ft == FrameType::H265I || ft == FrameType::H265P
        || ft == FrameType::MJPEG;
}

bool is_h264_frame(FrameType ft)
{
    return ft == FrameType::H264I || ft == FrameType::H264P;
}

bool is_h265_frame(FrameType ft)
{
    return ft == FrameType::H265I || ft == FrameType::H265P;
}

bool has_h264_start_code(const uint8_t* data, size_t len)
{
    if (len >= 4 && data[0] == 0x00 && data[1] == 0x00
        && data[2] == 0x00 && data[3] == 0x01) {
        return true;
    }
    return len >= 3 && data[0] == 0x00 && data[1] == 0x00
        && data[2] == 0x01;
}

// ---------------------------------------------------------------------------
// Fixed 12-byte header
// ---------------------------------------------------------------------------

bool media_frame_parse_fixed(const uint8_t* data, size_t data_len,
                              MediaFrameHeader& hdr)
{
    if (data_len < MEDIA_FRAME_FIXED_HDR) {
        return false;
    }

    hdr.channel       = data[0];
    hdr.frame_type    = static_cast<FrameType>(be24(data + 1));  // read U(24) MSB-first
    hdr.frame_length  = le24(data + 4);   // BytesToUInt → little-endian
    hdr.stream_exam   = data[7];
    hdr.extend_length = le24(data + 8);   // BytesToUInt → little-endian
    hdr.extend_count  = data[11];
    hdr.total_header  = MEDIA_FRAME_FIXED_HDR + hdr.extend_length;

    return true;
}

// ---------------------------------------------------------------------------
// Extension block parser
//
// Each extension entry inside the block:
//   Byte 0: ExtensionInfoType  (8-bit)
//   Bytes 1-3: entry length    (little-endian 24-bit, includes these 4 bytes)
//   Bytes 4..: entry data      (entry_length - 4 bytes)
// ---------------------------------------------------------------------------

void media_frame_parse_extensions(const uint8_t* ext_data, uint32_t ext_len,
                                  MediaFrameHeader& hdr)
{
    uint32_t pos = 0;
    while (pos + 4 <= ext_len) {
        uint8_t  type       = ext_data[pos];
        uint32_t entry_len  = le24(ext_data + pos + 1);  // little-endian

        if (entry_len < 4 || pos + entry_len > ext_len) {
            break;  // malformed or truncated
        }

        uint32_t data_bytes = entry_len - 4;  // bytes after the 4-byte header
        const uint8_t* entry_data = ext_data + pos + 4;

        if (static_cast<ExtensionInfoType>(type) == ExtensionInfoType::Video
            && data_bytes >= 4)
        {
            // RMFI2_VIDEOINFO: 32 bits, same reversed-byte bit-field encoding as Audio.
            // Read order after reversal (MSB first):
            //   lFPS   :  8 bits
            //   lHeight: 12 bits
            //   lWidth : 12 bits
            uint8_t rev[4] = {
                entry_data[3], entry_data[2],
                entry_data[1], entry_data[0]
            };
            uint32_t bit_pos = 0;
            hdr.video_info.fps    = read_bits(rev, &bit_pos,  8);
            hdr.video_info.height = read_bits(rev, &bit_pos, 12);
            hdr.video_info.width  = read_bits(rev, &bit_pos, 12);
            hdr.has_video_info = true;
        }

        if (static_cast<ExtensionInfoType>(type) == ExtensionInfoType::VideoEncoding
            && data_bytes >= 1)
        {
            hdr.video_encoding = entry_data[0];
            hdr.has_video_encoding = true;
        }

        if (static_cast<ExtensionInfoType>(type) == ExtensionInfoType::RealTime
            && data_bytes >= 8)
        {
            // 8 bytes:  [year-2000][month][day][hour][minute][second][?][?]
            hdr.realtime.year   = (uint16_t)(2000 + entry_data[0]);
            hdr.realtime.month  = entry_data[1];
            hdr.realtime.day    = entry_data[2];
            hdr.realtime.hour   = entry_data[3];
            hdr.realtime.minute = entry_data[4];
            hdr.realtime.second = entry_data[5];
            hdr.has_realtime    = true;
        }

        if (static_cast<ExtensionInfoType>(type) == ExtensionInfoType::VirtualTime
            && data_bytes >= 8)
        {
            // Streamax AI demo copies this per-video-frame PTS into parsed AI
            // JSON items as video_pts. Keep the same name in T13 summaries.
            hdr.virtual_pts = le64(entry_data);
            hdr.has_virtual_time = true;
        }

        if (static_cast<ExtensionInfoType>(type) == ExtensionInfoType::GPS
            && data_bytes >= 20)
        {
            std::string meta_hex = to_hex(entry_data, data_bytes);
            if (!meta_hex.empty()) {
                if (!hdr.telemetry_meta_raw.empty()) hdr.telemetry_meta_raw += "|";
                hdr.telemetry_meta_raw += "ext" + std::to_string((unsigned)type) + ":" + meta_hex;
            }
            std::string lat, lng, spd, hdg;
            if (parse_rmfi_gps_frame(entry_data, data_bytes, lat, lng, spd, hdg)) {
                hdr.telemetry_lat = lat;
                hdr.telemetry_lng = lng;
                if (!spd.empty()) hdr.telemetry_speed = spd;
                if (!hdg.empty()) hdr.telemetry_heading = hdg;
            }
        }

        if (static_cast<ExtensionInfoType>(type) == ExtensionInfoType::AdditionalData
            && data_bytes >= 20)
        {
            std::string meta_hex = to_hex(entry_data, data_bytes);
            if (!meta_hex.empty()) {
                if (!hdr.telemetry_meta_raw.empty()) hdr.telemetry_meta_raw += "|";
                hdr.telemetry_meta_raw += "ext" + std::to_string((unsigned)type) + ":" + meta_hex;
            }
            std::string lat, lng, spd, hdg;
            if (parse_rmfi_gps_frame(entry_data, data_bytes, lat, lng, spd, hdg)) {
                hdr.telemetry_lat = lat;
                hdr.telemetry_lng = lng;
                if (!spd.empty()) hdr.telemetry_speed = spd;
                if (!hdg.empty()) hdr.telemetry_heading = hdg;
            } else {
                std::string alt;
                if (parse_special_gps_block(entry_data, data_bytes, lat, lng, spd, hdg, alt)) {
                    hdr.telemetry_lat = lat;
                    hdr.telemetry_lng = lng;
                    if (!spd.empty()) hdr.telemetry_speed = spd;
                    if (!hdg.empty()) hdr.telemetry_heading = hdg;
                    if (!alt.empty()) hdr.telemetry_alt = alt;
                }
            }
        }

        if (static_cast<ExtensionInfoType>(type) == ExtensionInfoType::Telemetry
            && data_bytes > 0)
        {
            // Type 13 is RMFI2_VIDEOAIINFO in the Streamax AI parse API:
            // reserve(4), ullPts(8), lAIInfoLen(4), body[lAIInfoLen].
            // Older/current samples still expose useful null-separated ASCII
            // fields inside the body, so parse the documented wrapper first and
            // then scan the AI body for stable key:value diagnostics.
            bool has_ai_header = false;
            uint32_t ai_reserve = 0;
            uint64_t ai_pts = 0;
            uint32_t ai_body_len = 0;
            const uint8_t* ai_body = entry_data;
            uint32_t ai_body_bytes = data_bytes;
            if (data_bytes >= 16) {
                ai_reserve = le32(entry_data);
                ai_pts = le64(entry_data + 4);
                ai_body_len = le32(entry_data + 12);
                if (ai_body_len > 0 && ai_body_len <= data_bytes - 16) {
                    has_ai_header = true;
                    ai_body = entry_data + 16;
                    ai_body_bytes = ai_body_len;
                }
            }

            std::string flat;
            flat.reserve(ai_body_bytes);
            bool last_space = true;
            for (uint32_t j = 0; j < ai_body_bytes; ++j) {
                uint8_t c = ai_body[j];
                bool printable = (c >= 0x20 && c < 0x7f);
                if (printable) {
                    flat += (char)c; last_space = false;
                } else if (!last_space) {
                    flat += ' '; last_space = true;
                }
            }
            std::string lower = flat;
            for (auto& c : lower)
                c = (char)std::tolower((unsigned char)c);
            if (!flat.empty()) {
                if (hdr.telemetry_raw.empty()) hdr.telemetry_raw = flat;
                else hdr.telemetry_raw += " || " + flat;
            }
            std::string hx = to_hex(entry_data, data_bytes);
            if (!hx.empty()) {
                if (hdr.telemetry_raw_hex.empty()) hdr.telemetry_raw_hex = hx;
                else hdr.telemetry_raw_hex += "|" + hx;
            }

            auto trim_token = [](std::string s) -> std::string {
                while (!s.empty() && (s.back() == '"' || s.back() == '\'' ||
                                      s.back() == ',' || s.back() == ';'  ||
                                      s.back() == ')' || s.back() == ']'))
                    s.pop_back();
                size_t p = 0;
                while (p < s.size() && (s[p] == '"' || s[p] == '\'' ||
                                        s[p] == '(' || s[p] == '['))
                    ++p;
                return s.substr(p);
            };
            auto find_field_value = [&](std::initializer_list<const char*> keys,
                                        size_t max_len = 32) -> std::string {
                for (const char* key : keys) {
                    std::string k = key;
                    for (auto& c : k)
                        c = (char)std::tolower((unsigned char)c);
                    size_t pos = 0;
                    while (true) {
                        size_t p = lower.find(k, pos);
                        if (p == std::string::npos) break;
                        if (p > 0) {
                            char pc = lower[p - 1];
                            if (std::isalnum((unsigned char)pc) || pc == '_') {
                                pos = p + 1;
                                continue;
                            }
                        }
                        size_t q = p + k.size();
                        while (q < flat.size() && (flat[q] == ' ' || flat[q] == '\t' ||
                                                   flat[q] == '"' || flat[q] == '\''))
                            ++q;
                        if (q >= flat.size() || (flat[q] != ':' && flat[q] != '=')) {
                            pos = p + 1;
                            continue;
                        }
                        ++q;
                        while (q < flat.size() && (flat[q] == ' ' || flat[q] == '\t' ||
                                                   flat[q] == '"' || flat[q] == '\''))
                            ++q;
                        std::string v;
                        while (q < flat.size() && v.size() < max_len) {
                            char c = flat[q++];
                            if (c == ' ' || c == '/' || c == '|' || c == ';' ||
                                c == ')' || c == ']')
                                break;
                            if (c == ',' && !v.empty())
                                break;
                            v += c;
                        }
                        v = trim_token(v);
                        if (!v.empty()) return v;
                        pos = p + 1;
                    }
                }
                return {};
            };
            auto normalize_speed = [&](std::string v) -> std::string {
                if (v.empty()) return {};
                size_t p = 0;
                while (p < v.size() && !(std::isdigit((unsigned char)v[p]) ||
                       v[p] == '-' || v[p] == '+'))
                    ++p;
                if (p >= v.size()) return {};
                size_t q = p + 1;
                while (q < v.size() && (std::isdigit((unsigned char)v[q]) ||
                       v[q] == '.'))
                    ++q;
                std::string n = v.substr(p, q - p);
                char* endp = nullptr;
                double d = std::strtod(n.c_str(), &endp);
                if (endp == n.c_str()) return {};
                if (d < 0.0 || d > 400.0) return {};
                char buf[24];
                std::snprintf(buf, sizeof(buf), "%.0f", d);
                return buf;
            };
            auto find_device_v_tag = [&]() -> std::string {
                size_t pos = 0;
                while (true) {
                    size_t p = lower.find("v:", pos);
                    if (p == std::string::npos) break;
                    if (p > 0) {
                        char pc = lower[p - 1];
                        if (std::isalnum((unsigned char)pc) || pc == '_') {
                            pos = p + 2;
                            continue;
                        }
                    }
                    size_t q = p + 2;
                    while (q < flat.size() && (flat[q] == ' ' || flat[q] == '\t' ||
                                               flat[q] == '"' || flat[q] == '\''))
                        ++q;
                    std::string v;
                    while (q < flat.size() && v.size() < 16) {
                        char c = flat[q++];
                        if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-'))
                            break;
                        v += c;
                    }
                    if (v.size() >= 5 && v.size() <= 12) return v;
                    pos = p + 2;
                }
                return {};
            };
            auto normalize_coord = [&](std::string v, bool is_lat) -> std::string {
                if (v.empty()) return {};
                auto up = [](char c){ return (char)std::toupper((unsigned char)c); };
                v = trim_token(v);
                if (v.empty()) return {};
                int hemi_sign = 0;
                char first = up(v.front());
                char last  = up(v.back());
                if (first == 'N' || first == 'E' || first == 'S' || first == 'W') {
                    hemi_sign = (first == 'S' || first == 'W') ? -1 : 1;
                    v.erase(v.begin());
                }
                if (!v.empty() && (last == 'N' || last == 'E' || last == 'S' || last == 'W')) {
                    hemi_sign = (last == 'S' || last == 'W') ? -1 : 1;
                    v.pop_back();
                }
                v = trim_token(v);
                char* endp = nullptr;
                double d = std::strtod(v.c_str(), &endp);
                if (endp == v.c_str()) return {};
                if (hemi_sign < 0 && d > 0) d = -d;
                double lim = is_lat ? 90.0 : 180.0;
                if (d < -lim || d > lim) return {};
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.6f", d);
                return buf;
            };
            auto parse_coord_pair_after = [&](std::initializer_list<const char*> keys,
                                              std::string& out_lat,
                                              std::string& out_lng) -> bool {
                for (const char* key : keys) {
                    std::string k = key;
                    for (auto& c : k)
                        c = (char)std::tolower((unsigned char)c);
                    size_t p = lower.find(k);
                    if (p == std::string::npos) continue;
                    size_t q = p + k.size();
                    while (q < flat.size() && (flat[q] == ' ' || flat[q] == '\t' ||
                                               flat[q] == ':' || flat[q] == '=' ||
                                               flat[q] == '"' || flat[q] == '\'' ||
                                               flat[q] == '(' || flat[q] == '['))
                        ++q;
                    size_t a0 = q;
                    while (q < flat.size() && flat[q] != ',' && flat[q] != ' ' &&
                           flat[q] != ';' && flat[q] != '/' && flat[q] != ')' &&
                           flat[q] != ']')
                        ++q;
                    std::string lat = normalize_coord(flat.substr(a0, q - a0), true);
                    while (q < flat.size() && (flat[q] == ',' || flat[q] == ' ' ||
                                               flat[q] == '\t' || flat[q] == ';' ||
                                               flat[q] == '/' || flat[q] == ')' ||
                                               flat[q] == ']'))
                        ++q;
                    size_t b0 = q;
                    while (q < flat.size() && flat[q] != ' ' && flat[q] != ';' &&
                           flat[q] != '/' && flat[q] != ')' && flat[q] != ']')
                        ++q;
                    std::string lng = normalize_coord(flat.substr(b0, q - b0), false);
                    if (!lat.empty() && !lng.empty()) {
                        out_lat = lat;
                        out_lng = lng;
                        return true;
                    }
                }
                return false;
            };
            auto find_token_value = [&](std::initializer_list<const char*> keys,
                                        size_t max_len = 80) -> std::string {
                for (const char* key : keys) {
                    std::string k = key;
                    for (auto& c : k)
                        c = (char)std::tolower((unsigned char)c);
                    size_t pos = 0;
                    while (true) {
                        size_t p = lower.find(k, pos);
                        if (p == std::string::npos) break;
                        if (p > 0) {
                            char pc = lower[p - 1];
                            if (std::isalnum((unsigned char)pc) || pc == '_') {
                                pos = p + 1;
                                continue;
                            }
                        }
                        size_t q = p + k.size();
                        if (q >= flat.size() || (flat[q] != ':' && flat[q] != '=')) {
                            pos = p + 1;
                            continue;
                        }
                        ++q;
                        while (q < flat.size() && (flat[q] == ' ' || flat[q] == '\t' ||
                                                   flat[q] == '"' || flat[q] == '\''))
                            ++q;
                        std::string v;
                        while (q < flat.size() && v.size() < max_len) {
                            char c = flat[q++];
                            if (c == ' ' || c == '|' || c == ';' ||
                                c == ')' || c == ']')
                                break;
                            v += c;
                        }
                        v = trim_token(v);
                        if (!v.empty()) return v;
                        pos = p + 1;
                    }
                }
                return {};
            };
            auto find_word_flag = [&](std::initializer_list<const char*> words) -> std::string {
                for (const char* word : words) {
                    std::string w = word;
                    for (auto& c : w)
                        c = (char)std::tolower((unsigned char)c);
                    size_t p = lower.find(w);
                    while (p != std::string::npos) {
                        bool left_ok = p == 0 ||
                            !(std::isalnum((unsigned char)lower[p - 1]) || lower[p - 1] == '_');
                        size_t r = p + w.size();
                        bool right_ok = r >= lower.size() ||
                            !(std::isalnum((unsigned char)lower[r]) || lower[r] == '_');
                        if (left_ok && right_ok)
                            return word;
                        p = lower.find(w, p + 1);
                    }
                }
                return {};
            };
            auto find_slash_flag = [&](std::initializer_list<const char*> keys,
                                       size_t max_len = 32) -> std::string {
                for (const char* key : keys) {
                    std::string k = key;
                    for (auto& c : k)
                        c = (char)std::tolower((unsigned char)c);
                    size_t pos = 0;
                    while (true) {
                        size_t p = lower.find(k, pos);
                        if (p == std::string::npos) break;
                        if (p > 0) {
                            char pc = lower[p - 1];
                            if (std::isalnum((unsigned char)pc) || pc == '_') {
                                pos = p + 1;
                                continue;
                            }
                        }
                        size_t q = p + k.size();
                        if (q >= flat.size() || flat[q] != '/') {
                            pos = p + 1;
                            continue;
                        }
                        ++q;
                        std::string v;
                        while (q < flat.size() && v.size() < max_len) {
                            char c = flat[q++];
                            if (c == ' ' || c == '|' || c == ';' ||
                                c == ')' || c == ']')
                                break;
                            v += c;
                        }
                        v = trim_token(v);
                        if (!v.empty()) return v;
                        pos = p + 1;
                    }
                }
                return {};
            };
            auto add_summary_kv = [](std::string& summary, const char* key,
                                     const std::string& value) {
                if (value.empty()) return;
                if (!summary.empty()) summary += "|";
                summary += key;
                summary += "=";
                summary += value;
            };
            auto add_summary_u64 = [&](std::string& summary, const char* key,
                                       uint64_t value) {
                if (!summary.empty()) summary += "|";
                summary += key;
                summary += "=";
                summary += std::to_string((unsigned long long)value);
            };
            auto contains_word = [&](const char* word) -> bool {
                std::string w = word;
                for (auto& c : w)
                    c = (char)std::tolower((unsigned char)c);
                size_t p = lower.find(w);
                while (p != std::string::npos) {
                    bool left_ok = p == 0 ||
                        !(std::isalnum((unsigned char)lower[p - 1]) || lower[p - 1] == '_');
                    size_t r = p + w.size();
                    bool right_ok = r >= lower.size() ||
                        !(std::isalnum((unsigned char)lower[r]) || lower[r] == '_');
                    if (left_ok && right_ok)
                        return true;
                    p = lower.find(w, p + 1);
                }
                return false;
            };
            auto contains_any = [&](std::initializer_list<const char*> words) -> bool {
                for (const char* word : words) {
                    std::string w = word;
                    for (auto& c : w)
                        c = (char)std::tolower((unsigned char)c);
                    if (lower.find(w) != std::string::npos)
                        return true;
                }
                return false;
            };
            auto detect_ai_category = [&]() -> std::string {
                if (contains_any({"ttc:", "sttc:", "cipv", "ldw", "pcw", "tsr",
                                  "speed_limit", "rolling_stop", "car_distance",
                                  "hmw", "fcw"}))
                    return "adas";
                if (contains_any({"dms", "face", "driver_ok", "eyelookdown",
                                  "fatisw", "callsw", "smksw", "beltsw",
                                  "nodrvsw", "nomasksw", "sheltersw"}))
                    return "dsc";
                return {};
            };
            auto detect_ai_type = [&]() -> std::string {
                if (contains_any({"ttc:", "sttc:", "cipv", "vdis:", "fcw", "hmw"}))
                    return "fcw_hmw";
                if (contains_any({"ldw", "lane_departure"}))
                    return "ldw";
                if (contains_any({"pcw", "pedestrian"}))
                    return "pcw";
                if (contains_any({"tsr", "speed_limit", "rolling_stop"}))
                    return "tsr";
                if (contains_any({"face_detect", "nodrvsw", "no_driver"}))
                    return "dsc_face_detect";
                if (contains_any({"yawn", "head_pose", "distracted", "eyelookdown"}))
                    return "dsc_yawn_and_pose";
                if (contains_any({"eye", "eys", "fatisw", "fatigue"}))
                    return "dsc_eys_status";
                if (contains_any({"callsw", "phone"}))
                    return "dsc_call";
                if (contains_any({"beltsw", "belt"}))
                    return "dsc_belt";
                return {};
            };
            auto detect_ai_alarm = [&]() -> std::string {
                static const char* alarms[] = {
                    "ADAS_FCW_COLLISION",
                    "ADAS_HMW_CAR_DISTANCE_CLOSE",
                    "ADAS_LDW_LANE_DEPARTURE_LEFT",
                    "ADAS_LDW_LANE_DEPARTURE_RIGHT",
                    "ADAS_PCW_PEDESTRIAN_COLLISION",
                    "TSR_STOP_ALARM",
                    "TSR_SPEED_LIMIT_ALARM",
                    "NO_DRIVER",
                    "FATIGUE",
                    "DISTRACTED",
                    "PHONE_DETECTED",
                    "NO_BELT"
                };
                std::string out;
                for (const char* alarm : alarms) {
                    if (!contains_word(alarm))
                        continue;
                    if (!out.empty()) out += ",";
                    out += alarm;
                }
                return out;
            };
            auto detect_ai_algorithm = [&]() -> std::string {
                static const char* labels[] = {
                    "custom_normal",
                    "custom_adplus",
                    "custom_dms",
                    "custom_dsc"
                };
                for (const char* label : labels) {
                    std::string l = label;
                    for (auto& c : l)
                        c = (char)std::tolower((unsigned char)c);
                    size_t p = lower.find(l);
                    if (p == std::string::npos)
                        continue;
                    size_t q = p;
                    while (q < flat.size() && q - p < 64) {
                        char c = flat[q];
                        if (!(std::isalnum((unsigned char)c) || c == '_' ||
                              c == '-' || c == ':' || c == '.'))
                            break;
                        ++q;
                    }
                    std::string value = trim_token(flat.substr(p, q - p));
                    if (!value.empty())
                        return value;
                }
                return {};
            };
            // Accumulate non-empty fields across all type-13 entries in this
            // frame; do not overwrite a previously-found field with empty.
            std::string spd = find_field_value({"speed", "spd", "speedle"});
            spd = normalize_speed(spd);
            if (!spd.empty()) hdr.telemetry_speed = spd;

            std::string dev = find_field_value({"device_id", "deviceid", "devid"});
            if (dev.empty()) dev = find_device_v_tag();
            if (!dev.empty() && dev.size() >= 4 && dev.size() <= 8)
                hdr.telemetry_device_id = dev;

            std::string lat = find_field_value({"lat", "latitude", "gps_lat", "gpslat"});
            lat = normalize_coord(lat, true);
            if (!lat.empty()) hdr.telemetry_lat = lat;

            std::string lng = find_field_value({"lng", "lon", "long", "longitude", "gps_lng", "gpslon"});
            lng = normalize_coord(lng, false);
            if (!lng.empty()) hdr.telemetry_lng = lng;

            std::string hdg = find_field_value({"hdg", "heading", "direction"});
            if (!hdg.empty()) hdr.telemetry_heading = hdg;

            std::string alt = find_field_value({"alt", "altitude"});
            if (!alt.empty()) hdr.telemetry_alt = alt;

            // Fallback: some firmwares encode GPS as a pair under "gps"/"location"
            // rather than separate LAT/LNG keys.
            if (hdr.telemetry_lat.empty() || hdr.telemetry_lng.empty()) {
                std::string p_lat, p_lng;
                if (parse_coord_pair_after({"gps", "location", "gps_location", "gpslocation"},
                                           p_lat, p_lng)) {
                    if (hdr.telemetry_lat.empty()) hdr.telemetry_lat = p_lat;
                    if (hdr.telemetry_lng.empty()) hdr.telemetry_lng = p_lng;
                }
            }
            // Some firmwares embed PT22-style GPS blocks directly inside type-13.
            // Scan the binary payload as a final fallback.
            if (hdr.telemetry_lat.empty() || hdr.telemetry_lng.empty()) {
                std::string g_lat, g_lng, g_spd, g_hdg, g_alt;
                if (parse_special_gps_block(entry_data, data_bytes,
                                            g_lat, g_lng, g_spd, g_hdg, g_alt)) {
                    if (hdr.telemetry_lat.empty()) hdr.telemetry_lat = g_lat;
                    if (hdr.telemetry_lng.empty()) hdr.telemetry_lng = g_lng;
                    if (hdr.telemetry_speed.empty() && !g_spd.empty()) hdr.telemetry_speed = g_spd;
                    if (hdr.telemetry_heading.empty() && !g_hdg.empty()) hdr.telemetry_heading = g_hdg;
                    if (hdr.telemetry_alt.empty() && !g_alt.empty()) hdr.telemetry_alt = g_alt;
                }
            }

            std::string summary;
            if (hdr.has_virtual_time)
                add_summary_u64(summary, "video_pts", hdr.virtual_pts);
            if (has_ai_header) {
                add_summary_u64(summary, "ai_pts", ai_pts);
                add_summary_u64(summary, "ai_body_len", ai_body_len);
                if (ai_reserve != 0)
                    add_summary_u64(summary, "ai_reserve", ai_reserve);
            }
            add_summary_kv(summary, "ai_category", detect_ai_category());
            add_summary_kv(summary, "ai_type", detect_ai_type());
            add_summary_kv(summary, "ai_algorithm", detect_ai_algorithm());
            add_summary_kv(summary, "ai_alarm", detect_ai_alarm());
            add_summary_kv(summary, "device", hdr.telemetry_device_id);
            add_summary_kv(summary, "speed", hdr.telemetry_speed);
            add_summary_kv(summary, "fps", find_token_value({"fps"}));
            add_summary_kv(summary, "rx", find_token_value({"rx"}));
            add_summary_kv(summary, "spf", find_token_value({"spf"}));
            add_summary_kv(summary, "jt", find_token_value({"jt"}));
            add_summary_kv(summary, "fcnt", find_token_value({"fcnt"}));
            add_summary_kv(summary, "alarm_num", find_token_value({"alarmnum"}));
            add_summary_kv(summary, "ala", find_token_value({"ala"}));
            add_summary_kv(summary, "ebitm", find_token_value({"ebitm"}));
            add_summary_kv(summary, "warn", find_token_value({"warn", "warnt"}));
            add_summary_kv(summary, "danger", find_token_value({"danger"}));
            add_summary_kv(summary, "face_count", find_token_value({"facecnt"}));
            add_summary_kv(summary, "face_status", find_token_value({"facesta"}));
            add_summary_kv(summary, "face_score", find_token_value({"objproface"}));
            add_summary_kv(summary, "face_rect", find_token_value({"rlast"}));
            add_summary_kv(summary, "mask_state", find_token_value({"c_state"}));
            add_summary_kv(summary, "mask_score", find_token_value({"c_score"}));
            add_summary_kv(summary, "mask_alarm", find_token_value({"nomask"}));
            add_summary_kv(summary, "mask_rect", find_token_value({"sid"}));
            add_summary_kv(summary, "phone_rect", find_token_value({"phoner"}));
            add_summary_kv(summary, "ttc", find_token_value({"ttc", "sttc"}));
            add_summary_kv(summary, "car_distance", find_token_value({"vdis", "distance"}));
            add_summary_kv(summary, "cipv", find_token_value({"cipv"}));
            add_summary_kv(summary, "vanishing_point", find_token_value({"vanishing_point", "vp"}));
            add_summary_kv(summary, "country", find_token_value({"country"}));
            add_summary_kv(summary, "status", find_word_flag({"DRIVER_OK"}));
            std::string eye_down = find_token_value({"eyelookdown"});
            if (eye_down.empty())
                eye_down = find_slash_flag({"EyeLookDown"});
            add_summary_kv(summary, "eye_look_down", eye_down);
            add_summary_kv(summary, "dms_type", find_token_value({"dmstype"}));
            add_summary_kv(summary, "dms_state", find_token_value({"dmsst"}));
            add_summary_kv(summary, "dms_rt", find_token_value({"dmsrt"}));
            add_summary_kv(summary, "vehicle", find_token_value({"vehi"}));
            add_summary_kv(summary, "ldw", find_token_value({"ldw"}));
            add_summary_kv(summary, "fatigue_switch", find_token_value({"fatisw"}));
            add_summary_kv(summary, "lane_switch", find_token_value({"ldsw"}));
            add_summary_kv(summary, "eat_switch", find_token_value({"eatsw"}));
            add_summary_kv(summary, "drink_switch", find_token_value({"drinksw"}));
            add_summary_kv(summary, "call_switch", find_token_value({"callsw"}));
            add_summary_kv(summary, "smoke_switch", find_token_value({"smksw"}));
            add_summary_kv(summary, "belt_switch", find_token_value({"beltsw"}));
            if (!summary.empty()) {
                if (hdr.telemetry_type13_summary.empty()) hdr.telemetry_type13_summary = summary;
                else hdr.telemetry_type13_summary += " || " + summary;
            }
        }

        if (static_cast<ExtensionInfoType>(type) == ExtensionInfoType::IndustryData
            && data_bytes >= 12)
        {
            std::string meta_hex = to_hex(entry_data, data_bytes);
            if (!meta_hex.empty())
                hdr.telemetry_type15_raw_hex = meta_hex;
            hdr.telemetry_type15_summary =
                parse_industry_extension_summary(entry_data, data_bytes);
            if (!meta_hex.empty()) {
                if (!hdr.telemetry_meta_raw.empty()) hdr.telemetry_meta_raw += "|";
                hdr.telemetry_meta_raw += "ext15:" + meta_hex;
            }
            parse_industry_extension_alarms(entry_data, data_bytes,
                                            hdr.telemetry_alarms);
        }

        if (static_cast<ExtensionInfoType>(type) == ExtensionInfoType::Audio
            && data_bytes >= 4)
        {
            // Audio extension: 4 bytes read in reverse bit order.
            // Layout (after reversing bytes, MSB first):
            //   skip 2 bits
            //   sample_rate : 18 bits
            //   bit_width   : 6  bits
            //   play        : 1  bit
            //   channels    : 1  bit
            //   codec       : 4  bits
            //
            // The C# code reverses the bytes before reading, so byte 3 is
            // read first, then 2, 1, 0.
            uint8_t rev[4] = {
                entry_data[3], entry_data[2],
                entry_data[1], entry_data[0]
            };
            uint32_t bit_pos = 0;
            read_bits(rev, &bit_pos, 2);                           // skip 2
            hdr.audio_info.sample_rate = read_bits(rev, &bit_pos, 18);
            hdr.audio_info.bit_width   = read_bits(rev, &bit_pos,  6);
            /*play*/                    read_bits(rev, &bit_pos,  1);
            hdr.audio_info.channels    = (uint8_t)read_bits(rev, &bit_pos,  1) + 1;
            hdr.audio_info.codec       = static_cast<AudioCodec>(
                                             read_bits(rev, &bit_pos, 4));
            hdr.has_audio_info = true;
        }

        if (std::getenv("MEDIA_DEBUG_EXT")) {
            std::fprintf(stderr, "[ext] type=%u len=%u\n",
                         (unsigned)type, (unsigned)data_bytes);
            if (static_cast<ExtensionInfoType>(type) == ExtensionInfoType::GPS ||
                static_cast<ExtensionInfoType>(type) == ExtensionInfoType::AdditionalData ||
                static_cast<ExtensionInfoType>(type) == ExtensionInfoType::VideoEncoding ||
                data_bytes >= 16) {
                std::fprintf(stderr, "      hex=%s\n",
                             to_hex(entry_data,
                                    data_bytes < 64 ? data_bytes : 64).c_str());
            }
        }

        pos += entry_len;
    }
    if (std::getenv("MEDIA_DEBUG_EXT")) {
        if (!hdr.telemetry_raw.empty())
            std::fprintf(stderr, "[t13] %s\n", hdr.telemetry_raw.c_str());
        if (!hdr.telemetry_meta_raw.empty())
            std::fprintf(stderr, "[meta] %s\n", hdr.telemetry_meta_raw.c_str());
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


bool is_key_frame(FrameType type) {
    return type == FrameType::H264I || type == FrameType::H265I || type == FrameType::MJPEG;
}
bool has_annexb_start_code(const uint8_t* data, size_t len) {
    return has_h264_start_code(data, len);
}

// Preserve the standalone extractor's structured JSON contract while using
// the same media-field decoder for every extension.
static MetadataEvent event_from_summary(uint64_t frame, const char* kind,
                                        const std::string& summary) {
    MetadataEvent event;
    event.frame_index = frame;
    event.kind = kind;
    event.text = std::string("type=") + kind;
    if (!summary.empty()) event.text += "|" + summary;
    size_t pos = 0;
    while (pos < event.text.size()) {
        size_t end = event.text.find('|', pos);
        if (end == std::string::npos) end = event.text.size();
        auto part = event.text.substr(pos, end - pos);
        size_t equal = part.find('=');
        if (equal != std::string::npos) event.fields[part.substr(0, equal)] = part.substr(equal + 1);
        pos = end + 1;
    }
    return event;
}

void media_frame_parse_extensions(const uint8_t* data, uint32_t length,
                                 uint64_t frame, MediaFrameHeader& header) {
    media_frame_parse_extensions(data, length, header);
    header.extension_events.clear();
    for (size_t offset = 0; offset + 4 <= length;) {
        size_t size = le24(data + offset + 1);
        if (size < 4 || size > length - offset) break;
        const auto type = data[offset];
        if (type == 13 || type == 15) {
            MediaFrameHeader entry;
            entry.has_virtual_time = header.has_virtual_time;
            entry.virtual_pts = header.virtual_pts;
            media_frame_parse_extensions(data + offset, uint32_t(size), entry);
            const std::string& summary = type == 13 ? entry.telemetry_type13_summary
                                                   : entry.telemetry_type15_summary;
            if (!summary.empty())
                header.extension_events.push_back(event_from_summary(frame, type == 13 ? "T13_DATA" : "T15_DATA", summary));
        }
        offset += size;
    }
    if (!header.telemetry_lat.empty() && !header.telemetry_lng.empty()) {
        std::string summary = "lat=" + header.telemetry_lat + "|lng=" + header.telemetry_lng;
        if (!header.telemetry_speed.empty()) summary += "|gps_speed=" + header.telemetry_speed;
        if (!header.telemetry_heading.empty()) summary += "|heading=" + header.telemetry_heading;
        if (!header.telemetry_alt.empty()) summary += "|alt=" + header.telemetry_alt;
        header.extension_events.push_back(event_from_summary(frame, "GPS", summary));
    }
    for (const auto& alarm : header.telemetry_alarms)
        header.extension_events.push_back(event_from_summary(frame, "ALARM",
            "alarm_type=" + std::to_string(alarm.main_id) + "|alarm_sub_id=" +
            std::to_string(alarm.sub_id) + "|alarm_text=" + alarm.text));
}
} // namespace media_extractor
