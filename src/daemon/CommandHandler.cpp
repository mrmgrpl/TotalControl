#include "CommandHandler.h"
#include "SequencerEngine.h"
#include <algorithm>
#include <cassert>
#include <cwctype>
#include <sstream>

// CrSDK enums referenced by code only (no CrSDK headers needed here)
// All raw property/command codes are plain integer literals.

namespace TotalControl {

CommandHandler::CommandHandler(std::vector<CameraController*> cams)
    : m_cams(std::move(cams)) {
    assert(!m_cams.empty());  // daemon must connect at least one camera before constructing the handler
}

void CommandHandler::SetSequencer(SequencerEngine* seq) {
    m_seq = seq;
}

CameraController* CommandHandler::RouteCamera(const std::wstring& req) const {
    if (m_cams.empty()) return nullptr;
    if (!JHas(req, L"cam")) return m_cams[0];

    // "cam":"<guid>" lub "cam":"<prefix_guid>"
    std::wstring guidOrIdx = JStr(req, L"cam");
    if (!guidOrIdx.empty()) {
        for (auto* c : m_cams)
            if (c->Guid() == guidOrIdx) return c;
        // dopasowanie po prefiksie (wygodne dla skróconych GUIDów)
        for (auto* c : m_cams)
            if (!c->Guid().empty() && c->Guid().rfind(guidOrIdx, 0) == 0) return c;
        return nullptr;
    }
    // "cam":0  (indeks numeryczny)
    int idx = JInt(req, L"cam", -1);
    if (idx >= 0 && idx < static_cast<int>(m_cams.size())) return m_cams[idx];
    return nullptr;
}

// ─── JSON helpers ─────────────────────────────────────────────────────────────

// Skip whitespace (spaces, tabs, newlines) at position pos in j.
static void SkipWs(const std::wstring& j, size_t& pos) {
    while (pos < j.size() &&
           (j[pos] == L' ' || j[pos] == L'\t' || j[pos] == L'\r' || j[pos] == L'\n'))
        ++pos;
}

std::wstring CommandHandler::JStr(const std::wstring& j, const wchar_t* key) {
    std::wstring k = std::wstring(L"\"") + key + L"\":";
    auto pos = j.find(k);
    if (pos == std::wstring::npos) return L"";
    pos += k.size();
    SkipWs(j, pos);
    if (pos >= j.size() || j[pos] != L'"') return L"";
    ++pos;
    auto end = j.find(L'"', pos);
    return (end != std::wstring::npos) ? j.substr(pos, end - pos) : L"";
}

int CommandHandler::JInt(const std::wstring& j, const wchar_t* key, int def) {
    std::wstring k = std::wstring(L"\"") + key + L"\":";
    auto pos = j.find(k);
    if (pos == std::wstring::npos) return def;
    pos += k.size();
    SkipWs(j, pos);
    if (pos < j.size() && j[pos] == L'"') ++pos;
    try { return std::stoi(j.substr(pos)); } catch (...) { return def; }
}

float CommandHandler::JFlt(const std::wstring& j, const wchar_t* key, float def) {
    std::wstring k = std::wstring(L"\"") + key + L"\":";
    auto pos = j.find(k);
    if (pos == std::wstring::npos) return def;
    pos += k.size();
    SkipWs(j, pos);
    if (pos < j.size() && j[pos] == L'"') ++pos;
    try { return std::stof(j.substr(pos)); } catch (...) { return def; }
}

bool CommandHandler::JHas(const std::wstring& j, const wchar_t* key) {
    return j.find(std::wstring(L"\"") + key + L"\":") != std::wstring::npos;
}

std::wstring CommandHandler::Ok(const std::wstring& extra) {
    return extra.empty() ? L"{\"ok\":true}" : L"{\"ok\":true," + extra + L"}";
}

std::wstring CommandHandler::Err(const wchar_t* code, const wchar_t* msg) {
    std::wstring r = std::wstring(L"{\"ok\":false,\"err\":\"") + code + L"\"";
    if (msg) r += std::wstring(L",\"msg\":\"") + msg + L"\"";
    return r + L"}";
}

std::wstring CommandHandler::NormProp(const std::wstring& raw) {
    std::wstring s = raw;
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    std::replace(s.begin(), s.end(), L'-', L'_');
    // legacy short aliases
    if (s == L"ss")       return L"shutter_speed";
    if (s == L"f")        return L"f_number";
    if (s == L"mode")     return L"exposure_mode";
    if (s == L"ev")       return L"ev_comp";
    if (s == L"wb")       return L"white_balance";
    return s;
}

// ─── Property registry ────────────────────────────────────────────────────────
// Each entry: { cli-name, propCode, dataType, writable }
// dataType: 0x0001=UInt8, 0x0002=UInt16, 0x0003=UInt32
// Codes from CrDevicePropertyCode enum (computed sequentially from 0x0100 base)

struct PropDef {
    const wchar_t* name;
    uint32_t       code;
    uint32_t       dataType;
    bool           writable;
};

static const PropDef kProps[] = {
    // Exposure
    { L"f_number",          0x0100, 0x0002, true  },
    { L"ev_comp",           0x0101, 0x0002, true  },
    { L"shutter_speed",     0x0103, 0x0003, true  },
    { L"iso",               0x0104, 0x0003, true  },
    { L"exposure_mode",     0x0105, 0x0003, true  },
    { L"file_type",         0x0106, 0x0002, true  },
    { L"jpeg_quality",      0x0107, 0x0002, true  },
    { L"white_balance",     0x0108, 0x0002, true  },
    { L"focus_mode",        0x0109, 0x0002, true  },
    { L"metering_mode",     0x010a, 0x0002, true  },
    { L"flash_mode",        0x010b, 0x0002, true  },
    { L"drive_mode",        0x010e, 0x0003, true  },
    { L"dro",               0x010f, 0x0002, true  },
    { L"image_size",        0x0110, 0x0002, true  },
    { L"aspect_ratio",      0x0111, 0x0002, true  },
    { L"picture_effect",    0x0112, 0x0003, true  },
    { L"focus_area",        0x0113, 0x0002, true  },
    { L"color_temp",        0x0115, 0x0002, true  },
    { L"lv_display_effect", 0x0118, 0x0002, true  },
    { L"store_dest",        0x0119, 0x0002, true  },
    { L"priority_key",      0x011a, 0x0002, true  },
    { L"af_tracking",       0x011b, 0x0001, true  },
    { L"zoom_setting",      0x0124, 0x0001, true  },
    { L"movie_format",      0x0127, 0x0002, true  },
    { L"movie_rec_setting", 0x0128, 0x0002, true  },
    { L"movie_fps",         0x0129, 0x0001, true  },
    { L"compression",       0x012a, 0x0001, true  },
    { L"slot1_filetype",    0x012b, 0x0002, true  },
    { L"slot2_filetype",    0x012c, 0x0002, true  },
    { L"slot1_imgsize",     0x012f, 0x0002, true  },
    { L"slot2_imgsize",     0x0130, 0x0002, true  },
    { L"raw_compression",   0x0131, 0x0002, true  },
    { L"shutter_slow",      0x0150, 0x0001, true  },
    { L"bracket_order",     0x01fd, 0x0001, true  },
    { L"nd_filter",         0x017e, 0x0001, true  },
    { L"subject_recog",     0x018d, 0x0001, true  },
    { L"silent_mode",       0x0200, 0x0001, true  },  // approx
    // Status (read-only)
    { L"battery",           0x0702, 0x0003, false },
    { L"battery_level",     0x0703, 0x0003, false },
    { L"slot1_remaining",   0x0709, 0x0003, false },
    { L"slot2_remaining",   0x070f, 0x0003, false },
    { L"slot1_status",      0x0708, 0x0002, false },
    { L"slot2_status",      0x070d, 0x0002, false },
    { L"slot1_writing",     0x0773, 0x0003, false },
    { L"slot2_writing",     0x0774, 0x0003, false },
    { L"focus_indicator",   0x0707, 0x0003, false },
};
static const size_t kPropsCount = sizeof(kProps) / sizeof(kProps[0]);

static const PropDef* FindProp(const std::wstring& name) {
    for (size_t i = 0; i < kPropsCount; ++i)
        if (name == kProps[i].name) return &kProps[i];
    return nullptr;
}

// ─── Value decode (raw → human-readable) ─────────────────────────────────────

static std::wstring DecodePropValue(uint32_t code, uint64_t raw) {
    wchar_t buf[64];
    switch (code) {
    case 0x0103: return CameraController::DecodeShutterSpeed(raw);
    case 0x0104: // ISO
        if (raw == 0xFFFFFF) return L"auto";
        swprintf_s(buf, L"%u", (unsigned)raw);
        return buf;
    case 0x0100: // FNumber  raw = f*100
        swprintf_s(buf, L"%.1f", raw / 100.0);
        return buf;
    case 0x0101: // EV comp: signed int16 / 1000
        swprintf_s(buf, L"%.3f", (int16_t)raw / 1000.0);
        return buf;
    case 0x0105: // ExposureMode
        switch (raw) {
        case 0x00000001: return L"M";
        case 0x00000002: return L"P";
        case 0x00000004: return L"A";
        case 0x00000008: return L"S";
        default: swprintf_s(buf, L"0x%X", (unsigned)raw); return buf;
        }
    case 0x0108: // WhiteBalance (CrWhiteBalanceSetting enum)
        switch (raw) {
        case 0x0000: return L"AWB";
        case 0x0001: return L"underwater-auto";
        case 0x0011: return L"daylight";
        case 0x0012: return L"shadow";
        case 0x0013: return L"cloudy";
        case 0x0014: return L"tungsten";
        case 0x0020: return L"fluorescent";
        case 0x0021: return L"fluorescent-warm";
        case 0x0022: return L"fluorescent-cool";
        case 0x0023: return L"fluorescent-day-white";
        case 0x0024: return L"fluorescent-daylight";
        case 0x0030: return L"flash";
        case 0x0100: return L"color-temp";
        case 0x0101: return L"custom-1";
        case 0x0102: return L"custom-2";
        case 0x0103: return L"custom-3";
        default: swprintf_s(buf, L"0x%X", (unsigned)raw); return buf;
        }
    case 0x0109: // FocusMode (CrFocus_MF=1, AF_S=2, AF_C=3, AF_A=4, DMF=6)
        switch (raw) {
        case 0x0001: return L"MF";
        case 0x0002: return L"AF-S";
        case 0x0003: return L"AF-C";
        case 0x0004: return L"AF-A";
        case 0x0005: return L"AF-D";
        case 0x0006: return L"DMF";
        case 0x0007: return L"PF";
        default: swprintf_s(buf, L"0x%X", (unsigned)raw); return buf;
        }
    case 0x010a: // MeteringMode (CrMetering_Multi=5, CenterWeighted=6, Spot_Standard=8, HL=10)
        switch (raw) {
        case 0x0001: return L"average";
        case 0x0002: return L"center-weighted-avg";
        case 0x0005: return L"multi";
        case 0x0006: return L"center";
        case 0x0008: return L"spot";
        case 0x000a: return L"hl";
        default: swprintf_s(buf, L"0x%X", (unsigned)raw); return buf;
        }
    case 0x010e: // DriveMode
        switch (raw) {
        case 0x00000001: return L"single";
        case 0x00010001: return L"cont-hi";
        case 0x00010002: return L"cont-hi-plus";  // confirmed on ILCE-7RM4A
        case 0x00010006: return L"cont-hi-live";
        case 0x00010004: return L"cont-lo";       // unconfirmed — swap if wrong
        case 0x00010003: return L"cont";
        case 0x00010007: return L"cont-mid";
        case 0x00011001: return L"burst-lo";
        case 0x00011002: return L"burst-mid";
        case 0x00011003: return L"burst-hi";
        case 0x00012001: return L"focus-bracket";
        case 0x00020001: return L"timelapse";
        case 0x00030001: return L"timer-2s";
        case 0x00030002: return L"timer-5s";
        case 0x00030003: return L"timer-10s";
        case 0x00040301: return L"bracket-0.3ev-3-cont";
        case 0x00040302: return L"bracket-0.3ev-5-cont";
        case 0x00040303: return L"bracket-0.3ev-9-cont";
        case 0x00040304: return L"bracket-0.5ev-3-cont";
        case 0x00040305: return L"bracket-0.5ev-5-cont";
        case 0x00040306: return L"bracket-0.5ev-9-cont";
        case 0x00040307: return L"bracket-0.7ev-3-cont";
        case 0x00040308: return L"bracket-0.7ev-5-cont";
        case 0x00040309: return L"bracket-0.7ev-9-cont";
        case 0x0004030a: return L"bracket-1ev-3-cont";
        case 0x0004030b: return L"bracket-1ev-5-cont";
        case 0x0004030c: return L"bracket-1ev-9-cont";
        case 0x0004030d: return L"bracket-2ev-3-cont";
        case 0x0004030e: return L"bracket-2ev-5-cont";
        case 0x0004030f: return L"bracket-3ev-3-cont";
        case 0x00040310: return L"bracket-3ev-5-cont";
        case 0x00050001: return L"bracket-0.3ev-3";
        case 0x00050002: return L"bracket-0.3ev-5";
        case 0x00050003: return L"bracket-0.3ev-9";
        case 0x00050004: return L"bracket-0.5ev-3";
        case 0x00050005: return L"bracket-0.5ev-5";
        case 0x00050006: return L"bracket-0.5ev-9";
        case 0x00050007: return L"bracket-0.7ev-3";
        case 0x00050008: return L"bracket-0.7ev-5";
        case 0x00050009: return L"bracket-0.7ev-9";
        case 0x0005000a: return L"bracket-1ev-3";
        case 0x0005000b: return L"bracket-1ev-5";
        case 0x0005000c: return L"bracket-1ev-9";
        case 0x0005000d: return L"bracket-2ev-3";
        case 0x0005000e: return L"bracket-2ev-5";
        case 0x0005000f: return L"bracket-3ev-3";
        case 0x00050010: return L"bracket-3ev-5";
        case 0x00060001: return L"wb-bracket-lo";
        case 0x00060002: return L"wb-bracket-hi";
        case 0x00070001: return L"dro-bracket-lo";
        case 0x00070002: return L"dro-bracket-hi";
        case 0x10000001: return L"lpf-bracket";
        default: swprintf_s(buf, L"0x%08X", (unsigned)raw); return buf;
        }
    case 0x0110: // ImageSize
        switch (raw) {
        case 1: return L"L"; case 2: return L"M"; case 3: return L"S";
        default: swprintf_s(buf, L"%u", (unsigned)raw); return buf;
        }
    case 0x0106: // FileType (CrFileType: Jpeg=1,Raw=2,RawJpeg=3,RawHeif=4,Heif=5)
        switch (raw) {
        case 0x0001: return L"JPEG";
        case 0x0002: return L"RAW";
        case 0x0003: return L"RAW+JPEG";
        case 0x0004: return L"RAW+HEIF";
        case 0x0005: return L"HEIF";
        default: swprintf_s(buf, L"0x%X", (unsigned)raw); return buf;
        }
    case 0x0119: // StoreDestination (HostPC=1, MemoryCard=2, Both=3)
        switch (raw) {
        case 0x0001: return L"pc"; case 0x0002: return L"card"; case 0x0003: return L"both";
        default: swprintf_s(buf, L"0x%X", (unsigned)raw); return buf;
        }
    case 0x0702: // BatteryRemain
        swprintf_s(buf, L"%u%%", (unsigned)raw); return buf;
    case 0x0703: // BatteryLevel
        switch (raw) {
        case 0x00000001: return L"pre-end";
        case 0x00000010: return L"1/4";
        case 0x00000020: return L"2/4";
        case 0x00000030: return L"3/4";
        case 0x00000040: return L"full";
        case 0x00010000: return L"usb";
        default: swprintf_s(buf, L"0x%X", (unsigned)raw); return buf;
        }
    case 0x0708: case 0x070d: // Slot status
        switch (raw) {
        case 0x0000: return L"no-card"; case 0x0001: return L"ok";
        case 0x0002: return L"full";    default: return L"error";
        }
    case 0x0707: // FocusIndication
        switch (raw) {
        case 0x00000001: return L"unlocked";
        case 0x00000102: case 0x00000103: return L"focused";
        case 0x00000202: case 0x00000203: return L"not-focused";
        default: swprintf_s(buf, L"0x%X", (unsigned)raw); return buf;
        }
    case 0x01fd: // BracketOrder
        switch (raw) {
        case 0x01: return L"0-to-minus-to-plus";
        case 0x02: return L"minus-to-0-to-plus";
        default: swprintf_s(buf, L"0x%X", (unsigned)raw); return buf;
        }
    default:
        swprintf_s(buf, L"%llu", (unsigned long long)raw); return buf;
    }
}

// ─── Value encode (human-readable → raw) ─────────────────────────────────────

static bool EncodePropValue(uint32_t code, const std::wstring& val, long long& outRaw) {
    try {
        switch (code) {
        case 0x0103: // ShutterSpeed — handled specially via CameraController
            return false; // caller should use SetShutterSpeed
        case 0x0104: // ISO
            if (val == L"auto" || val == L"AUTO") { outRaw = 0xFFFFFF; return true; }
            outRaw = std::stoll(val); return true;
        case 0x0100: // FNumber — handled specially
            return false; // caller should use SetFNumber
        case 0x0101: // EV comp: accept float like "+1.3" → 1300
            outRaw = static_cast<long long>(std::stof(val) * 1000.0f);
            return true;
        case 0x0105: // ExposureMode
            if (val == L"M")   { outRaw = 0x00000001; return true; }
            if (val == L"P")   { outRaw = 0x00000002; return true; }
            if (val == L"A")   { outRaw = 0x00000004; return true; }
            if (val == L"S")   { outRaw = 0x00000008; return true; }
            break;
        case 0x0108: // WhiteBalance (CrWhiteBalanceSetting enum)
            if (val == L"AWB" || val == L"awb")           { outRaw = 0x0000; return true; }
            if (val == L"daylight")                       { outRaw = 0x0011; return true; }
            if (val == L"shadow")                         { outRaw = 0x0012; return true; }
            if (val == L"cloudy")                         { outRaw = 0x0013; return true; }
            if (val == L"tungsten" || val == L"incand")   { outRaw = 0x0014; return true; }
            if (val == L"fluorescent")                    { outRaw = 0x0020; return true; }
            if (val == L"flash")                          { outRaw = 0x0030; return true; }
            if (val == L"color-temp" || val == L"colortemp") { outRaw = 0x0100; return true; }
            if (val == L"custom-1" || val == L"custom1") { outRaw = 0x0101; return true; }
            if (val == L"custom-2" || val == L"custom2") { outRaw = 0x0102; return true; }
            if (val == L"custom-3" || val == L"custom3") { outRaw = 0x0103; return true; }
            break;
        case 0x0109: // FocusMode (CrFocus_* sequential from 1)
            if (val == L"MF")   { outRaw = 0x0001; return true; }
            if (val == L"AF-S") { outRaw = 0x0002; return true; }
            if (val == L"AF-C") { outRaw = 0x0003; return true; }
            if (val == L"AF-A") { outRaw = 0x0004; return true; }
            if (val == L"DMF")  { outRaw = 0x0006; return true; }
            if (val == L"PF")   { outRaw = 0x0007; return true; }
            break;
        case 0x010a: // MeteringMode
            if (val == L"multi")  { outRaw = 0x0005; return true; }
            if (val == L"center") { outRaw = 0x0006; return true; }
            if (val == L"spot")   { outRaw = 0x0008; return true; }
            if (val == L"hl")     { outRaw = 0x000a; return true; }
            break;
        case 0x0110: // ImageSize
            if (val == L"L") { outRaw = 1; return true; }
            if (val == L"M") { outRaw = 2; return true; }
            if (val == L"S") { outRaw = 3; return true; }
            break;
        case 0x0106: // FileType (CrFileType: Jpeg=1,Raw=2,RawJpeg=3,RawHeif=4,Heif=5)
            if (val == L"JPEG" || val == L"jpeg")         { outRaw = 0x0001; return true; }
            if (val == L"RAW"  || val == L"raw")          { outRaw = 0x0002; return true; }
            if (val == L"RAW+JPEG" || val == L"raw+jpeg") { outRaw = 0x0003; return true; }
            if (val == L"RAW+HEIF" || val == L"raw+heif") { outRaw = 0x0004; return true; }
            if (val == L"HEIF" || val == L"heif")         { outRaw = 0x0005; return true; }
            break;
        case 0x0119: // StoreDestination (HostPC=1, MemoryCard=2, Both=3)
            if (val == L"pc")   { outRaw = 0x0001; return true; }
            if (val == L"card") { outRaw = 0x0002; return true; }
            if (val == L"both") { outRaw = 0x0003; return true; }
            break;
        case 0x011a: // PriorityKey
            if (val == L"pc" || val == L"PC") { outRaw = 0x0002; return true; }
            if (val == L"camera")             { outRaw = 0x0001; return true; }
            break;
        case 0x01fd: // BracketOrder
            if (val == L"minus-to-0-to-plus" || val == L"minus") { outRaw = 0x02; return true; }
            if (val == L"0-to-minus-to-plus" || val == L"zero")  { outRaw = 0x01; return true; }
            break;
        }
        // Fallback: parse as decimal or hex integer
        if (val.size() > 2 && val[0] == L'0' && (val[1] == L'x' || val[1] == L'X'))
            outRaw = std::stoll(val.substr(2), nullptr, 16);
        else
            outRaw = std::stoll(val);
        return true;
    } catch (...) {
        return false;
    }
}

// ─── Bracket drive mode table ─────────────────────────────────────────────────
// (ev_tenths, count, single_mode_code, cont_mode_code)
struct BracketEntry { int ev10; int count; uint32_t single_code; uint32_t cont_code; };
// EV codes: 03=0.3ev, 05=0.5ev, 07=0.7ev, 10=1.0ev, 13=1.3ev, 15=1.5ev,
//           17=1.7ev, 20=2.0ev, 23=2.3ev, 25=2.5ev, 27=2.7ev, 30=3.0ev
// Computed from CrDriveMode enum (sequential from base values in header)
// 7-pic variants: base at 0x00050011/0x00040311, sequential +3 per EV step
static const BracketEntry kBrackets[] = {
    //ev10 cnt  single       cont
    {  3,  3, 0x00050001, 0x00040301 },
    {  3,  5, 0x00050002, 0x00040302 },
    {  3,  7, 0x00050013, 0x00040313 },
    {  3,  9, 0x00050003, 0x00040303 },
    {  5,  3, 0x00050004, 0x00040304 },
    {  5,  5, 0x00050005, 0x00040305 },
    {  5,  7, 0x00050016, 0x00040316 },
    {  5,  9, 0x00050006, 0x00040306 },
    {  7,  3, 0x00050007, 0x00040307 },
    {  7,  5, 0x00050008, 0x00040308 },
    {  7,  7, 0x00050019, 0x00040319 },
    {  7,  9, 0x00050009, 0x00040309 },
    { 10,  3, 0x0005000a, 0x0004030a },
    { 10,  5, 0x0005000b, 0x0004030b }, // CrDrive_Single/Continuous_Bracket_10Ev_5pics
    { 10,  7, 0x0005001c, 0x0004031c }, // CrDrive_Single/Continuous_Bracket_10Ev_7pics
    { 10,  9, 0x0005000c, 0x0004030c },
    { 13,  3, 0x0005001f, 0x0004031f },
    { 13,  5, 0x00050020, 0x00040320 },
    { 13,  7, 0x00050021, 0x00040321 },
    { 15,  3, 0x00050024, 0x00040324 },
    { 15,  5, 0x00050025, 0x00040325 },
    { 15,  7, 0x00050026, 0x00040326 },
    { 17,  3, 0x00050029, 0x00040329 },
    { 17,  5, 0x0005002a, 0x0004032a },
    { 17,  7, 0x0005002b, 0x0004032b },
    { 20,  3, 0x0005000d, 0x0004030d },
    { 20,  5, 0x0005000e, 0x0004030e },
    { 20,  7, 0x0005002e, 0x0004032e },
    { 23,  3, 0x00050031, 0x00040331 },
    { 23,  5, 0x00050032, 0x00040332 },
    { 25,  3, 0x00050035, 0x00040335 },
    { 25,  5, 0x00050036, 0x00040336 },
    { 27,  3, 0x00050039, 0x00040339 },
    { 27,  5, 0x0005003a, 0x0004033a },
    { 30,  3, 0x0005000f, 0x0004030f },
    { 30,  5, 0x00050010, 0x00040310 },
};

static const BracketEntry* FindBracket(int ev10, int count) {
    for (const auto& b : kBrackets)
        if (b.ev10 == ev10 && b.count == count) return &b;
    return nullptr;
}

// ─── Command table (CrCommandId sequential from 0) ───────────────────────────

struct CmdEntry { const wchar_t* name; int id; };
static const CmdEntry kCmds[] = {
    { L"release",         0  },
    { L"movie-rec",       1  },
    { L"cancel-shoot",    2  },
    { L"media-format",    4  },
    { L"media-quick-fmt", 5  },
    { L"cancel-format",   6  },
    { L"s1-and-s2",       7  },
    { L"cancel-transfer", 8  },
    { L"cam-reset",       9  },
    { L"crop-switch",     10 },
    { L"movie-toggle",    11 },
    { L"cancel-touch",    12 },
    { L"pixel-mapping",   13 },
    { L"tc-preset-reset", 14 },
    { L"ub-preset-reset", 15 },
    { L"sensor-clean",    16 },
    { L"pp-reset",        17 },
    { L"cl-reset",        18 },
    { L"power-off",       19 },
    { L"cancel-focus",    20 },
    { L"flicker-scan",    21 },
    { L"spot-boost",      22 },
    { L"file-num-reset",  23 },
    { L"tracking-af-on",  24 },
    { L"cancel-zoom",     25 },
    { L"movie-toggle2",   26 },
    { L"standby",         27 },
    { L"power-on",        28 },
    { L"stream",          29 },
    { L"reset-multi-matrix", 30 },
    { L"nav-up",          31 },
    { L"nav-down",        32 },
    { L"nav-left",        33 },
    { L"nav-right",       34 },
    { L"nav-back",        35 },
    { L"nav-disp",        36 },
    { L"nav-set",         37 },
    { L"nav-right-up",    38 },
    { L"nav-right-down",  39 },
    { L"nav-left-up",     40 },
    { L"nav-left-down",   41 },
    { L"nav-menu",        42 },
};

static int FindCmd(const std::wstring& name) {
    for (const auto& c : kCmds)
        if (name == c.name) return c.id;
    return -1;
}

// ─── Dispatcher ───────────────────────────────────────────────────────────────

bool CommandHandler::Handle(const std::wstring& req, std::wstring& resp) {
    std::wstring cmd = JStr(req, L"cmd");

    // ── quit ──────────────────────────────────────────────────────────────────
    if (cmd == L"quit") {
        resp = Ok(); return false;
    }

    // ── list_cameras ──────────────────────────────────────────────────────────
    if (cmd == L"list_cameras") {
        std::wostringstream ss;
        ss << L"\"cameras\":[";
        for (size_t i = 0; i < m_cams.size(); ++i) {
            if (i > 0) ss << L",";
            ss << L"{\"index\":" << i
               << L",\"model\":\"" << m_cams[i]->Model() << L"\""
               << L",\"guid\":\""  << m_cams[i]->Guid()  << L"\""
               << L",\"connected\":" << (m_cams[i]->IsConnected() ? L"true" : L"false")
               << L"}";
        }
        ss << L"]";
        resp = Ok(ss.str());
        return true;
    }

    // Wszystkie pozostałe komendy wymagają konkretnej kamery
    CameraController* cam = RouteCamera(req);
    if (!cam) {
        resp = Err(L"camera_not_found", JHas(req, L"cam") ? JStr(req, L"cam").c_str() : L"brak kamer");
        return true;
    }

    // ── status ────────────────────────────────────────────────────────────────
    if (cmd == L"status") {
        CameraStatus s = cam->GetStatus();
        std::wostringstream ss;
        ss << L"\"connected\":"  << (s.connected ? L"true" : L"false")
           << L",\"guid\":\""    << cam->Guid()     << L"\""
           << L",\"model\":\""   << s.model         << L"\""
           << L",\"battery\":"   << s.batteryPct
           << L",\"battery_level\":\"" << s.batteryLevel << L"\""
           << L",\"remaining\":" << s.remainingShots
           << L",\"slot2_remaining\":" << s.slot2Remaining
           << L",\"slot1_status\":\"" << s.slot1Status << L"\""
           << L",\"slot2_status\":\"" << s.slot2Status << L"\""
           << L",\"slot1_writing\":\"" << s.slot1Writing << L"\""
           << L",\"slot2_writing\":\"" << s.slot2Writing << L"\""
           << L",\"ss\":\""      << s.shutterSpeed  << L"\""
           << L",\"iso\":"       << s.iso
           << L",\"f\":"         << s.fNumber
           << L",\"ev\":"        << s.exposureBias
           << L",\"mode\":\""    << s.exposureMode  << L"\""
           << L",\"focus\":\""   << s.focusMode     << L"\""
           << L",\"focus_area\":\"" << s.focusArea  << L"\""
           << L",\"focus_ind\":\"" << s.focusIndicator << L"\""
           << L",\"drive\":\""   << s.driveMode     << L"\""
           << L",\"wb\":\""      << s.whiteBalance  << L"\""
           << L",\"color_temp\":" << s.colorTemp
           << L",\"img_size\":\"" << s.imageSize    << L"\""
           << L",\"file_type\":\"" << s.fileType    << L"\""
           << L",\"metering\":\"" << s.metering     << L"\""
           << L",\"store\":\""   << s.storeDestination << L"\"";
        resp = Ok(ss.str()); return true;
    }

    // ── shoot ─────────────────────────────────────────────────────────────────
    if (cmd == L"shoot") {
        if (!cam->IsConnected()) { resp = Err(L"not_connected"); return true; }

        cam->SetPCRemotePriority();

        if (JHas(req, L"mode"))  cam->SetExposureMode(JStr(req, L"mode").c_str());
        if (JHas(req, L"focus")) cam->SetFocusMode(JStr(req, L"focus").c_str());
        if (JHas(req, L"iso"))   cam->SetISO(JInt(req, L"iso"));
        if (JHas(req, L"f"))     cam->SetFNumber(JFlt(req, L"f"));
        if (JHas(req, L"ss"))    cam->SetShutterSpeed(JStr(req, L"ss").c_str());

        std::wstring store = JHas(req, L"store") ? JStr(req, L"store") : L"card";
        cam->SetStoreDestination(store.c_str());
        ::Sleep(300);

        int count = JHas(req, L"count") ? JInt(req, L"count", 1) : 1;
        if (count < 1) count = 1;

        int timeout = JHas(req, L"timeout_ms") ? JInt(req, L"timeout_ms")
                                                : (count > 1 ? count * 1500 + 5000 : 5000);
        int latency = 0;
        bool ok;

        std::wstring driveStr = JHas(req, L"drive") ? JStr(req, L"drive") : L"";

        if (count > 1) {
            // Burst: resolve drive mode string → code (default cont-hi)
            uint32_t driveCode = 0x00010001; // cont-hi (default)
            if      (driveStr == L"cont-hi-plus") driveCode = 0x00010002;  // confirmed on ILCE-7RM4A
            else if (driveStr == L"cont-hi-live") driveCode = 0x00010006;
            else if (driveStr == L"cont-lo")      driveCode = 0x00010004;  // unconfirmed — swap if wrong
            else if (driveStr == L"cont-mid")     driveCode = 0x00010007;
            cam->SetPropAndVerify(0x010e, 0x0003, (long long)driveCode,
                                  L"DriveMode(burst)", 2000);
            ok = cam->Shoot(&latency, timeout, count, /*holdForBurst=*/true);
        } else {
            // Single shot: if "drive":"single" specified, switch drive mode back from bracket
            if (driveStr == L"single")
                cam->SetPropAndVerify(0x010e, 0x0003, 0x00000001LL, L"DriveMode(single)", 2000);
            ok = cam->Shoot(&latency, timeout);
        }

        if (ok) {
            std::wostringstream ss;
            ss << L"\"latency_ms\":" << latency;
            if (count > 1) ss << L",\"captures\":" << count;
            resp = Ok(ss.str());
        } else {
            resp = Err(L"timeout", L"captured event timeout");
        }
        return true;
    }

    // ── bracket ───────────────────────────────────────────────────────────────
    // {"cmd":"bracket","ev":"1ev","count":5,"mode":"single","order":"minus","ss":"1/250","iso":200}
    if (cmd == L"bracket") {
        if (!cam->IsConnected()) { resp = Err(L"not_connected"); return true; }

        // Parse EV step: "0.3ev","0.5ev","0.7ev","1ev","1ev","2ev","3ev"
        std::wstring evStr = JStr(req, L"ev");
        if (evStr.empty()) evStr = L"1ev";
        // Strip trailing "ev"
        if (!evStr.empty() && (evStr.back() == L'v' || evStr.back() == L'V'))
            evStr.pop_back();
        if (!evStr.empty() && (evStr.back() == L'e' || evStr.back() == L'E'))
            evStr.pop_back();
        // Convert "0.3" → 3, "1.0" → 10, "1" → 10, etc.
        int ev10 = 10; // default 1EV
        try {
            float evf = std::stof(evStr);
            ev10 = static_cast<int>(evf * 10.0f + 0.5f);
        } catch (...) {}

        int count = JHas(req, L"count") ? JInt(req, L"count", 5) : 5;
        std::wstring modeStr = JStr(req, L"mode");
        bool single = (modeStr.empty() || modeStr == L"single");

        const BracketEntry* br = FindBracket(ev10, count);
        if (!br) {
            resp = Err(L"unsupported_bracket", L"ev/count combo not in table");
            return true;
        }
        uint32_t driveCode = single ? br->single_code : br->cont_code;

        // Bracket order: default minus-to-0-to-plus
        std::wstring orderStr = JStr(req, L"order");
        uint8_t bracketOrderVal = (orderStr == L"zero" || orderStr == L"0-to-minus-to-plus")
                                  ? 0x01 : 0x02;

        cam->SetPCRemotePriority();

        // Apply optional exposure overrides
        if (JHas(req, L"mode"))  cam->SetExposureMode(JStr(req, L"mode").c_str());
        if (JHas(req, L"focus")) cam->SetFocusMode(JStr(req, L"focus").c_str());
        if (JHas(req, L"iso"))   cam->SetISO(JInt(req, L"iso"));
        if (JHas(req, L"f"))     cam->SetFNumber(JFlt(req, L"f"));
        if (JHas(req, L"ss"))    cam->SetShutterSpeed(JStr(req, L"ss").c_str());

        // Set bracket order
        if (cam->SupportsProperty(0x01fd))
            cam->SetProp(0x01fd, 0x0001, bracketOrderVal, L"BracketOrder");

        // Set drive mode to bracket
        if (!cam->SupportsProperty(0x010e)) {
            resp = Err(L"not_supported", L"DriveMode not supported by this camera");
            return true;
        }
        cam->SetPropAndVerify(0x010e, 0x0003, (long long)driveCode, L"DriveMode", 2000);

        std::wstring store = JHas(req, L"store") ? JStr(req, L"store") : L"card";
        cam->SetStoreDestination(store.c_str());
        ::Sleep(200);

        int timeout = JHas(req, L"timeout_ms") ? JInt(req, L"timeout_ms") : 15000;
        int latency = 0;
        bool ok;

        if (!single) {
            // Cont_Bracket: hold Release button — camera fires all N shots in one burst.
            ok = cam->Shoot(&latency, timeout, count, /*holdForBurst=*/true);
        } else {
            // Single_Bracket: each Release fires one shot at next bracket exposure.
            // Loop N times, each press expects exactly 1 capture.
            int perShot = (timeout / count) + 3000;
            int captured = 0, totalLatency = 0;
            for (int i = 0; i < count; ++i) {
                int lat = 0;
                if (!cam->Shoot(&lat, perShot, 1)) break;
                totalLatency += lat;
                ++captured;
                if (i + 1 < count) ::Sleep(300);
            }
            ok      = (captured == count);
            latency = totalLatency;
        }

        if (ok) {
            std::wostringstream ss;
            ss << L"\"latency_ms\":" << latency
               << L",\"captures\":" << count
               << L",\"ev\":\"" << evStr << L"ev\""
               << L",\"drive\":\"0x" << driveCode << L"\"";
            resp = Ok(ss.str());
        } else {
            resp = Err(L"timeout", L"bracket captures timeout");
        }
        return true;
    }

    // ── movie ─────────────────────────────────────────────────────────────────
    // {"cmd":"movie","action":"start"|"stop"|"toggle"}
    // Sony CrSDK does not have separate start/stop commands for movie recording.
    // CrCommandId_MovieRec (id=1) is the physical record button — the camera toggles
    // the recording state internally. "start" and "stop" both press this button;
    // the caller is responsible for knowing whether the camera is currently recording.
    if (cmd == L"movie") {
        if (!cam->IsConnected()) { resp = Err(L"not_connected"); return true; }
        std::wstring action = JStr(req, L"action");
        bool ok;
        if (action == L"start" || action == L"stop") {
            // Both map to CrCommandId_MovieRec (id=1): press + release.
            ok = cam->SendCmd(1, 1); ::Sleep(50); cam->SendCmd(1, 0);
        } else {
            // "toggle" uses CrCommandId_movie_toggle (id=11), a software-level toggle.
            ok = cam->SendCmd(11, 1); ::Sleep(50); cam->SendCmd(11, 0);
        }
        resp = ok ? Ok() : Err(L"cmd_failed");
        return true;
    }

    // ── af ────────────────────────────────────────────────────────────────────
    // {"cmd":"af","button":"s1"|"s2"|"s1+s2"|"ael"|"awb"|"fel","state":"down"|"up"|"press"}
    if (cmd == L"af") {
        if (!cam->IsConnected()) { resp = Err(L"not_connected"); return true; }
        std::wstring btn   = JStr(req, L"button");
        std::wstring state = JStr(req, L"state");
        if (state.empty()) state = L"press";

        // Map button to CrControlCode / CrCommandId
        // Using CrControlCode values sent via SendCmd
        // Note: CameraController::SendCmd uses CrCommandId enum, not CrControlCode
        // The available CrCommandId values are 0-42 as listed above.
        // For S1, S2, AEL we need to use SetDeviceProperty with S1/AEL/FEL/AWBL codes
        // which are at codes 1,2,3,5 in the enum (before 0x0100)
        // CrDeviceProperty_S1 = 1, _AEL = 2, _FEL = 3, _AWBL = 5
        // DataType for these is CrDataType_UInt8 (button) with value 1=down, 0=up

        struct { const wchar_t* name; uint32_t code; } btns[] = {
            { L"s1",  1 }, { L"ael", 2 }, { L"fel", 3 }, { L"awb", 5 },
        };

        // S2 and S1+S2 use CrCommandId_Release (0) or CrCommandId_S1andRelease (7)
        bool ok = false;
        if (btn == L"s2" || btn == L"shoot") {
            ok = cam->SendCmd(0, 1);
            if (state == L"press") { ::Sleep(50); cam->SendCmd(0, 0); }
        } else if (btn == L"s1+s2") {
            ok = cam->SendCmd(7, 1);
            if (state == L"press") { ::Sleep(50); cam->SendCmd(7, 0); }
        } else {
            bool found = false;
            for (const auto& b : btns) {
                if (btn != b.name) continue;
                const uint8_t val = (state == L"up") ? 0U : 1U;
                ok = cam->SetProp(b.code, 0x0001, val, b.name);
                if (state == L"press") {
                    ::Sleep(500);
                    cam->SetProp(b.code, 0x0001, 0U, b.name);
                }
                found = true;
                break;
            }
            if (!found) { resp = Err(L"unknown_button", btn.c_str()); return true; }
        }
        resp = ok ? Ok() : Err(L"cmd_failed");
        return true;
    }

    // ── cmd ───────────────────────────────────────────────────────────────────
    // {"cmd":"cmd","id":"power-off"|"sensor-clean"|"nav-up",...,"param":1}
    if (cmd == L"cmd") {
        if (!cam->IsConnected()) { resp = Err(L"not_connected"); return true; }
        std::wstring idStr = JStr(req, L"id");
        int id = -1;
        // Try name lookup
        id = FindCmd(idStr);
        // Try raw numeric
        if (id == -1 && !idStr.empty()) {
            try {
                if (idStr.size() > 2 && idStr[0] == L'0' && (idStr[1] == L'x' || idStr[1] == L'X'))
                    id = static_cast<int>(std::stoi(idStr.substr(2), nullptr, 16));
                else
                    id = std::stoi(idStr);
            } catch (...) {}
        }
        if (id == -1) { resp = Err(L"unknown_cmd_id", idStr.c_str()); return true; }

        int param = JInt(req, L"param", 1);
        bool ok = cam->SendCmd(id, param);
        if (JHas(req, L"param_up") || JBool(req, L"press")) {
            ::Sleep(50);
            cam->SendCmd(id, 0);
        }
        resp = ok ? Ok() : Err(L"cmd_failed");
        return true;
    }

    // ── set ───────────────────────────────────────────────────────────────────
    if (cmd == L"set") {
        if (!cam->IsConnected()) { resp = Err(L"not_connected"); return true; }
        std::wstring prop = NormProp(JStr(req, L"prop"));
        std::wstring val  = JStr(req, L"val");

        if (prop == L"shutter_speed") {
            resp = cam->SetShutterSpeed(val.c_str()) ? Ok() : Err(L"set_failed"); return true;
        }
        if (prop == L"f_number") {
            try { resp = cam->SetFNumber(std::stof(val)) ? Ok() : Err(L"set_failed"); }
            catch (...) { resp = Err(L"invalid_value"); }
            return true;
        }
        if (prop == L"iso") {
            if (val == L"auto" || val == L"AUTO") {
                resp = cam->SetISO(0xFFFFFF) ? Ok() : Err(L"set_failed"); return true;
            }
            try { resp = cam->SetISO(std::stoi(val)) ? Ok() : Err(L"set_failed"); }
            catch (...) { resp = Err(L"invalid_value"); }
            return true;
        }
        if (prop == L"exposure_mode") {
            resp = cam->SetExposureMode(val.c_str()) ? Ok() : Err(L"set_failed"); return true;
        }
        if (prop == L"focus_mode") {
            resp = cam->SetFocusMode(val.c_str()) ? Ok() : Err(L"set_failed"); return true;
        }
        if (prop == L"store_dest") {
            resp = cam->SetStoreDestination(val.c_str()) ? Ok() : Err(L"set_failed"); return true;
        }
        if (prop == L"priority_key" && (val == L"pc" || val == L"PC")) {
            resp = cam->SetPCRemotePriority() ? Ok() : Err(L"set_failed"); return true;
        }

        // Generic table-driven set
        const PropDef* pd = FindProp(prop);
        if (!pd) { resp = Err(L"unknown_prop", prop.c_str()); return true; }
        if (!pd->writable) { resp = Err(L"read_only", prop.c_str()); return true; }
        if (!cam->SupportsProperty(pd->code)) {
            resp = Err(L"not_supported", (cam->Model() + L" nie obsługuje: " + prop).c_str());
            return true;
        }
        long long raw;
        if (!EncodePropValue(pd->code, val, raw)) {
            resp = Err(L"invalid_value", (prop + L"=" + val).c_str()); return true;
        }
        resp = cam->SetProp(pd->code, pd->dataType, raw, prop.c_str()) ? Ok() : Err(L"set_failed");
        return true;
    }

    // ── get ───────────────────────────────────────────────────────────────────
    if (cmd == L"get") {
        if (!cam->IsConnected()) { resp = Err(L"not_connected"); return true; }
        std::wstring prop = NormProp(JStr(req, L"prop"));

        CameraStatus s = cam->GetStatus();
        auto respVal = [&](const std::wstring& v) {
            resp = Ok(L"\"val\":\"" + v + L"\"");
        };
        auto respNum = [&](long long v) {
            std::wostringstream ss; ss << v;
            resp = Ok(L"\"val\":\"" + ss.str() + L"\"");
        };

        if (prop == L"shutter_speed")  { respVal(s.shutterSpeed); return true; }
        if (prop == L"iso")            { respNum(s.iso); return true; }
        if (prop == L"f_number")       { std::wostringstream ss; ss << s.fNumber; respVal(ss.str()); return true; }
        if (prop == L"ev_comp")        { std::wostringstream ss; ss << s.exposureBias; respVal(ss.str()); return true; }
        if (prop == L"exposure_mode")  { respVal(s.exposureMode); return true; }
        if (prop == L"focus_mode")     { respVal(s.focusMode); return true; }
        if (prop == L"focus_area")     { respVal(s.focusArea); return true; }
        if (prop == L"focus_ind" || prop == L"focus_indicator") { respVal(s.focusIndicator); return true; }
        if (prop == L"drive_mode")     { respVal(s.driveMode); return true; }
        if (prop == L"white_balance")  { respVal(s.whiteBalance); return true; }
        if (prop == L"color_temp")     { respNum(s.colorTemp); return true; }
        if (prop == L"image_size")     { respVal(s.imageSize); return true; }
        if (prop == L"file_type")      { respVal(s.fileType); return true; }
        if (prop == L"metering_mode" || prop == L"metering") { respVal(s.metering); return true; }
        if (prop == L"store_dest")     { respVal(s.storeDestination); return true; }
        if (prop == L"battery")        { respNum(s.batteryPct); return true; }
        if (prop == L"battery_level")  { respVal(s.batteryLevel); return true; }
        if (prop == L"remaining")      { respNum(s.remainingShots); return true; }
        if (prop == L"slot1_remaining"){ respNum(s.remainingShots); return true; }
        if (prop == L"slot2_remaining"){ respNum(s.slot2Remaining); return true; }
        if (prop == L"slot1_status")   { respVal(s.slot1Status);  return true; }
        if (prop == L"slot2_status")   { respVal(s.slot2Status);  return true; }
        if (prop == L"slot1_writing")  { respVal(s.slot1Writing); return true; }
        if (prop == L"slot2_writing")  { respVal(s.slot2Writing); return true; }
        if (prop == L"model")          { respVal(s.model); return true; }

        // Generic table-driven get via raw prop read
        const PropDef* pd = FindProp(prop);
        if (!pd) { resp = Err(L"unknown_prop", prop.c_str()); return true; }
        if (!cam->SupportsProperty(pd->code)) {
            resp = Err(L"not_supported", (cam->Model() + L" nie obsługuje: " + prop).c_str());
            return true;
        }
        uint64_t raw = 0;
        if (!cam->GetPropRaw(pd->code, raw)) {
            resp = Err(L"get_failed"); return true;
        }
        std::wstring decoded = DecodePropValue(pd->code, raw);
        resp = Ok(L"\"val\":\"" + decoded + L"\"");
        return true;
    }

    // ── seq_start ─────────────────────────────────────────────────────────────
    // {"cmd":"seq_start","file":"C:\\sequences\\eclipse.json"}
    // {"cmd":"seq_start","file":"...","sim_offset_ms":-37926976000}  (--test mode)
    if (cmd == L"seq_start") {
        if (!m_seq) { resp = Err(L"no_sequencer", L"SequencerEngine not configured"); return true; }
        if (m_seq->State() == SeqState::Running) {
            resp = Err(L"already_running", L"seq_stop first"); return true;
        }
        std::wstring file = JStr(req, L"file");
        if (file.empty()) { resp = Err(L"missing_file", L"seq_start requires \"file\""); return true; }

        // Optional test-mode offset injected by CLI --test flag.
        long long simOffsetMs = 0;
        if (JHas(req, L"sim_offset_ms")) {
            const std::wstring key = L"\"sim_offset_ms\":";
            auto pos = req.find(key);
            if (pos != std::wstring::npos) {
                pos += key.size();
                try { simOffsetMs = std::stoll(req.substr(pos)); } catch (...) {}
            }
        }

        std::wstring err = m_seq->Load(file, (int64_t)simOffsetMs);
        if (!err.empty()) { resp = Err(L"load_failed", err.c_str()); return true; }
        if (!m_seq->Start()) { resp = Err(L"start_failed", L"no steps or dispatch not set"); return true; }
        resp = Ok(m_seq->StatusJson());
        return true;
    }

    // ── seq_stop ──────────────────────────────────────────────────────────────
    if (cmd == L"seq_stop") {
        if (!m_seq) { resp = Err(L"no_sequencer"); return true; }
        m_seq->Stop();
        resp = Ok(m_seq->StatusJson());
        return true;
    }

    // ── seq_status ────────────────────────────────────────────────────────────
    if (cmd == L"seq_status") {
        if (!m_seq) { resp = Err(L"no_sequencer"); return true; }
        resp = Ok(m_seq->StatusJson());
        return true;
    }

    resp = Err(L"unknown_cmd");
    return true;
}

// Helper: parse bool from JSON (value = true/false)
bool CommandHandler::JBool(const std::wstring& j, const wchar_t* key, bool def) {
    std::wstring k = std::wstring(L"\"") + key + L"\":";
    auto pos = j.find(k);
    if (pos == std::wstring::npos) return def;
    pos += k.size();
    if (j.substr(pos, 4) == L"true") return true;
    if (j.substr(pos, 5) == L"false") return false;
    return def;
}

} // namespace TotalControl
