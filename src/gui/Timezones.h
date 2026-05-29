#pragma once

// Timezone list using IANA identifiers (RFC 6557 / Olson database).
// On Windows + MSVC 17.4+, std::chrono::locate_zone() resolves these names
// via a CLDR-based mapping to the Windows TZ registry, which is kept current
// by Windows Update.  No manual offsets — DST is handled by the runtime.
//
// Coverage: eclipse locations through 2083 and major observer homelands.
// To add a zone: append a TzDef row with any valid IANA name from
// https://www.iana.org/time-zones

namespace TotalControl {

struct TzDef {
    const char* code;     // ≤5-char display tag shown in the clock row
    const char* label;    // readable city/region name shown in the picker
    const char* iana;     // canonical IANA TZ identifier
};

// clang-format off
static constexpr TzDef kTzList[] = {

    // ── Reference ─────────────────────────────────────────────────────────
    { "UTC",   "UTC",                    "UTC"                       },

    // ── Europe ────────────────────────────────────────────────────────────
    { "WAW",   "Warsaw",                 "Europe/Warsaw"             },  // PL — home default
    { "MAD",   "Madrid",                 "Europe/Madrid"             },  // ES — TSE 2026-08-12
    { "BCN",   "Barcelona",              "Europe/Madrid"             },  // ES — totality path 2026
    { "LON",   "London",                 "Europe/London"             },
    { "PAR",   "Paris",                  "Europe/Paris"              },
    { "ROM",   "Rome",                   "Europe/Rome"               },
    { "ATH",   "Athens",                 "Europe/Athens"             },
    { "IST",   "Istanbul",               "Europe/Istanbul"           },  // TSE 2006 ref

    // ── Africa ────────────────────────────────────────────────────────────
    { "CAI",   "Cairo",                  "Africa/Cairo"              },  // TSE 2027-08-02
    { "KRT",   "Khartoum",               "Africa/Khartoum"           },  // TSE 2027 / 2034
    { "ADD",   "Addis Ababa",            "Africa/Addis_Ababa"        },  // TSE 2027 path
    { "NAI",   "Nairobi",                "Africa/Nairobi"            },
    { "WIN",   "Windhoek",               "Africa/Windhoek"           },  // TSE 2030-11-25
    { "CPT",   "Cape Town",              "Africa/Johannesburg"       },

    // ── Middle East ───────────────────────────────────────────────────────
    { "RUH",   "Riyadh",                 "Asia/Riyadh"               },  // TSE 2027 path
    { "DXB",   "Dubai",                  "Asia/Dubai"                },
    { "THR",   "Tehran",                 "Asia/Tehran"               },

    // ── Asia ──────────────────────────────────────────────────────────────
    { "KHI",   "Karachi",                "Asia/Karachi"              },
    { "KOL",   "Kolkata",                "Asia/Kolkata"              },
    { "DEL",   "New Delhi",              "Asia/Kolkata"              },
    { "DAC",   "Dhaka",                  "Asia/Dhaka"                },
    { "RGN",   "Yangon",                 "Asia/Yangon"               },
    { "BKK",   "Bangkok",                "Asia/Bangkok"              },
    { "SGN",   "Ho Chi Minh",            "Asia/Ho_Chi_Minh"          },
    { "SHA",   "Shanghai",               "Asia/Shanghai"             },  // TSE 2034 / 2035
    { "SEL",   "Seoul",                  "Asia/Seoul"                },  // TSE 2035-09-02
    { "TYO",   "Tokyo",                  "Asia/Tokyo"                },  // TSE 2035 path
    { "YKT",   "Yakutsk",                "Asia/Yakutsk"              },  // TSE 2033-03-30

    // ── Oceania ───────────────────────────────────────────────────────────
    { "SYD",   "Sydney",                 "Australia/Sydney"          },  // TSE 2028-07-22
    { "MEL",   "Melbourne",              "Australia/Melbourne"       },  // TSE 2028 path
    { "DRW",   "Darwin",                 "Australia/Darwin"          },  // TSE 2028 path
    { "PER",   "Perth",                  "Australia/Perth"           },
    { "AKL",   "Auckland",               "Pacific/Auckland"          },  // TSE 2031 path

    // ── Americas ──────────────────────────────────────────────────────────
    { "NYC",   "New York",               "America/New_York"          },
    { "CHI",   "Chicago",                "America/Chicago"           },
    { "DEN",   "Denver",                 "America/Denver"            },
    { "LAX",   "Los Angeles",            "America/Los_Angeles"       },
    { "ANC",   "Anchorage",              "America/Anchorage"         },
    { "MEX",   "Mexico City",            "America/Mexico_City"       },
    { "BOG",   "Bogotá",                 "America/Bogota"            },
    { "SAO",   "São Paulo",              "America/Sao_Paulo"         },  // TSE 2045 region
    { "BUE",   "Buenos Aires",           "America/Argentina/Buenos_Aires" },
    { "SCL",   "Santiago",               "America/Santiago"          },  // TSE 2020 Patagonia ref
};
// clang-format on

static constexpr int kTzCount = static_cast<int>(sizeof(kTzList) / sizeof(kTzList[0]));

// Default IANA names stored in SQLite
static constexpr const char* kTzDefaultHome    = "Europe/Warsaw";   // operator home
static constexpr const char* kTzDefaultEclipse = "Europe/Madrid";   // TSE 2026-08-12

// Returns index into kTzList for the given IANA name, or 0 (UTC) if not found.
inline int TzFindByIana(const char* iana) {
    for (int i = 0; i < kTzCount; ++i)
        if (std::string_view(kTzList[i].iana) == iana)
            return i;
    return 0;
}

} // namespace TotalControl
