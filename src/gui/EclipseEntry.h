#pragma once
#include <string>

namespace TotalControl {

struct EclipseEntry {
    int         year          = 0;
    int         month         = 0;
    int         day           = 0;
    std::string type;           // "T","A","H","P" — first char = eclipse class
    std::string timeGe;         // td_ge — TT time of greatest eclipse "HH:MM:SS"
    float       latGe         = 0.f;
    float       lonGe         = 0.f;
    std::string duration;       // central_duration text e.g. "02m10s"
    float       durationSecs   = 0.f;
    float       dt            = 0.f;  // ΔT = TT − UTC (seconds); UTC = TT − dt

    int DateInt() const { return year * 10000 + month * 100 + day; }
};

} // namespace TotalControl
