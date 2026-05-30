#pragma once
#include "IqpClient.h"
#include <cstdint>

namespace TotalControl {

// All Besselian polynomial coefficients for one eclipse — loaded on demand.
struct BesselianElements {
    int    year = 0, month = 0, day = 0;
    double t0   = 0;   // time of greatest eclipse, hours from midnight TT
    double dt   = 0;   // ΔT = TT − UTC (seconds)
    double x0, x1, x2, x3;        // shadow axis X  (Earth radii)
    double y0, y1, y2, y3;        // shadow axis Y
    double d0, d1, d2;            // solar declination (degrees)
    double mu0, mu1, mu2;         // Greenwich Hour Angle (degrees)
    double l10, l11, l12;         // penumbra cone radius
    double l20, l21, l22;         // umbra cone radius (negative = total)
    double tan_f1, tan_f2;        // cone slope angles
    double tmin, tmax;            // valid time range (hours TT)
    bool   valid = false;
};

// Compute C1/C2/Max/C3/C4 from Besselian elements for observer at
// latDeg (°N), lonDeg (°E), altM (m above sea level).
// Returns ContactTimes with source = ContactSource::Besselian.
ContactTimes CalcBesselian(const BesselianElements& e,
                           double latDeg, double lonDeg, double altM);

} // namespace TotalControl
