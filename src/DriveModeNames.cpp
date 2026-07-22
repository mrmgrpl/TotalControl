#include "DriveModeNames.h"
#include <cassert>
#include <cwchar>

namespace TotalControl {

std::wstring DecodeDriveMode(uint32_t raw) {
    assert(raw != 0xFFFFFFFFu);  // sanity: caller must pass an actual property value
    switch (raw) {
    case 0x00000001: return L"single";
    case 0x00010001: return L"cont-hi";
    case 0x00010002: return L"cont-hi-plus";  // CrDrive_Continuous_Hi_Plus  — confirmed ILCE-7RM4A
    case 0x00010003: return L"cont-hi-live";  // CrDrive_Continuous_Hi_Live
    case 0x00010004: return L"cont-lo";       // CrDrive_Continuous_Lo       — confirmed ILCE-7RM4A
    case 0x00010005: return L"cont";          // CrDrive_Continuous
    case 0x00010006: return L"cont-speed";    // CrDrive_Continuous_SpeedPriority
    case 0x00010007: return L"cont-mid";      // CrDrive_Continuous_Mid      — confirmed ILCE-7RM4A
    case 0x00010008: return L"cont-mid-live"; // CrDrive_Continuous_Mid_Live
    case 0x00010009: return L"cont-lo-live";  // CrDrive_Continuous_Lo_Live
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
    default: {
        wchar_t buf[16];
        swprintf_s(buf, L"0x%08X", raw);
        return buf;
    }
    }
}

} // namespace TotalControl
