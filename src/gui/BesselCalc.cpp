#include "BesselCalc.h"
#include <chrono>
#include <cmath>
#include <numbers>

// Algorithms based on:
//   Meeus - Elements of Solar Eclipses (1989)
//   Greg Miller - celestialprogramming.com/apps/SolarEclipseViewer/eclipse.js (public domain)

namespace TotalControl {

static constexpr double kDeg  = std::numbers::pi / 180.0;
static constexpr double kFlat = 1.0 / 298.257;       // WGS84 flattening
static constexpr int    kMaxIter = 20;

// ─── Eclipse geometry at τ (hours from T0) for a given observer ──────────────

struct EclFrame {
    double u, v;      // shadow-observer offset  (x-ξ, y-η)
    double a, b;      // shadow velocity         (x'-ξ', y'-η')
    double n;         // speed = sqrt(a²+b²)
    double L1p, L2p;  // corrected shadow radii
    double H;         // local hour angle (degrees)
    double d;         // solar declination (degrees)
};

static EclFrame evalFrame(const BesselianElements& e, double tau,
                           double rhoSin, double rhoCos, double lonDeg) {
    EclFrame o{};

    // Polynomials (Horner form, τ relative to T0)
    o.d  = e.d0  + tau * (e.d1  + tau * e.d2);
    double mu = e.mu0 + tau * (e.mu1 + tau * e.mu2);
    double x  = e.x0  + tau * (e.x1  + tau * (e.x2  + tau * e.x3));
    double y  = e.y0  + tau * (e.y1  + tau * (e.y2  + tau * e.y3));
    double l1 = e.l10 + tau * (e.l11 + tau * e.l12);
    double l2 = e.l20 + tau * (e.l21 + tau * e.l22);

    // Derivatives
    double xp  = e.x1 + tau * (2.0*e.x2 + 3.0*e.x3*tau);
    double yp  = e.y1 + tau * (2.0*e.y2 + 3.0*e.y3*tau);
    double mup = e.mu1 + 2.0*e.mu2*tau;
    double dp  = e.d1  + 2.0*e.d2 *tau;

    // H = M + λ_E − correction(ΔT) — Meeus eq, ΔT correction converts TT→UT
    // 0.00417807 deg/s  (≈ 15°/h / 3600 s/h, accounting for solar/sidereal difference)
    o.H = mu + lonDeg - 0.00417807 * e.dt;

    const double Hr  = o.H * kDeg;
    const double dr  = o.d * kDeg;
    const double sH  = std::sin(Hr), cH = std::cos(Hr);
    const double sd  = std::sin(dr), cd = std::cos(dr);

    // Observer fundamental-plane coords (Meeus §54)
    const double xi   = rhoCos * sH;
    const double eta  = rhoSin * cd - rhoCos * cH * sd;
    const double zeta = rhoSin * sd + rhoCos * cH * cd;

    // Observer velocity in fundamental plane
    const double xip  = 0.01745329 * mup * rhoCos * cH;
    const double etap = 0.01745329 * (mup * xi * sd - zeta * dp);

    // Corrected shadow radii
    o.L1p = l1 - zeta * e.tan_f1;
    o.L2p = l2 - zeta * e.tan_f2;

    o.u = x  - xi;
    o.v = y  - eta;
    o.a = xp - xip;
    o.b = yp - etap;
    o.n = std::sqrt(o.a*o.a + o.b*o.b);

    return o;
}

// ─── Time conversion: τ (hours from T0 in TT) → UTC ms ───────────────────────

static int64_t tauToUtcMs(const BesselianElements& e, double tau) {
    using namespace std::chrono;
    auto ymd   = year_month_day{ year(e.year),
                                 month(static_cast<unsigned>(e.month)),
                                 day(static_cast<unsigned>(e.day)) };
    int64_t dayMs = duration_cast<milliseconds>(
                        sys_days{ymd}.time_since_epoch()).count();
    double  ut_h  = e.t0 + tau - e.dt / 3600.0;   // UT hours from midnight
    return dayMs + static_cast<int64_t>(ut_h * 3600000.0);
}

// ─── Refine one contact time ──────────────────────────────────────────────────
// sign = -1 for first-type contacts (C1, C2), +1 for last-type (C3, C4)
// usePenumbra: true → use L1p (C1/C4), false → use L2p (C2/C3)

static double refineContact(const BesselianElements& e, double t0,
                             double rhoSin, double rhoCos, double lonDeg,
                             double sign, bool usePenumbra) {
    double t = t0;
    for (int i = 0; i < 10; ++i) {
        auto o   = evalFrame(e, t, rhoSin, rhoCos, lonDeg);
        double L = usePenumbra ? o.L1p : o.L2p;
        double S = (o.a*o.v - o.u*o.b) / (o.n * L);
        double tf = -(o.u*o.a + o.v*o.b) / (o.n*o.n)
                  + sign * L/o.n * std::sqrt(1.0 - S*S);
        t += tf;
    }
    return t;
}

// ─── Main calculator ──────────────────────────────────────────────────────────

ContactTimes CalcBesselian(const BesselianElements& e,
                           double latDeg, double lonDeg, double altM) {
    ContactTimes result;
    result.source = ContactSource::Besselian;

    if (!e.valid) return result;

    // Observer geocentric coordinates (Meeus §11)
    const double phi  = latDeg * kDeg;
    const double u1   = std::atan(0.99664719 * std::tan(phi));
    const double rhoSin = 0.99664719 * std::sin(u1)
                        + (altM / 6378140.0) * std::sin(phi);
    const double rhoCos = std::cos(u1)
                        + (altM / 6378140.0) * std::cos(phi);

    // tmin/tmax in DB are τ relative to T0
    const double tauMin = (e.tmax > e.tmin) ? e.tmin : -3.0;
    const double tauMax = (e.tmax > e.tmin) ? e.tmax : +3.0;
    (void)tauMin; (void)tauMax; // Newton method starts at τ=0, no scan needed

    // Step 1 — find time of maximum eclipse (Newton iteration)
    double t = 0.0;
    for (int iter = 0; iter < kMaxIter; ++iter) {
        auto o  = evalFrame(e, t, rhoSin, rhoCos, lonDeg);
        double tm = -(o.u*o.a + o.v*o.b) / (o.n*o.n);
        t += tm;
        if (std::fabs(tm) < 1e-5) break;
    }
    auto o = evalFrame(e, t, rhoSin, rhoCos, lonDeg);

    result.maxMs = tauToUtcMs(e, t);

    // Step 2 — exterior contacts C1/C4 (penumbra)
    {
        double S = (o.a*o.v - o.u*o.b) / (o.n * o.L1p);
        double SS = S*S;
        if (SS <= 1.0) {
            double tau = o.L1p/o.n * std::sqrt(1.0 - SS);
            double tC1 = refineContact(e, t - tau, rhoSin, rhoCos, lonDeg, -1.0, true);
            double tC4 = refineContact(e, t + tau, rhoSin, rhoCos, lonDeg, +1.0, true);
            result.c1Ms = tauToUtcMs(e, tC1);
            result.c4Ms = tauToUtcMs(e, tC4);
        }
    }

    // Step 3 — interior contacts C2/C3 (umbra/antumbra)
    {
        double S = (o.a*o.v - o.u*o.b) / (o.n * o.L2p);
        double SS = S*S;
        if (SS <= 1.0) {
            double tau = o.L2p/o.n * std::sqrt(1.0 - SS);
            // When L2p < 0 (total eclipse), tau < 0:
            //   t - tau  = t + |tau|  → LATER  = C3 (end of totality)
            //   t + tau  = t - |tau|  → EARLIER = C2 (start of totality)
            // sign convention matches Miller: C3 uses sign=-1, C2 uses sign=+1
            double tC3 = refineContact(e, t - tau, rhoSin, rhoCos, lonDeg, -1.0, false);
            double tC2 = refineContact(e, t + tau, rhoSin, rhoCos, lonDeg, +1.0, false);
            result.c2Ms = tauToUtcMs(e, tC2);
            result.c3Ms = tauToUtcMs(e, tC3);
        }
    }

    result.apiOk = true;
    result.valid = result.c1Ms > 0;
    return result;
}

} // namespace TotalControl
