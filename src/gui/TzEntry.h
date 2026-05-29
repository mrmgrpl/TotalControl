#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace TotalControl {

struct TzEntry {
    std::string code;   // short tag shown in UI  (e.g. "WAW")
    std::string label;  // city name shown in picker (e.g. "Warsaw")
    std::string iana;   // IANA TZ name (e.g. "Europe/Warsaw")
};

// Returns index of first entry whose iana matches, or 0 (UTC) if not found.
inline int TzFindByIana(const std::vector<TzEntry>& list, std::string_view iana) {
    for (int i = 0; i < static_cast<int>(list.size()); ++i)
        if (list[i].iana == iana) return i;
    return 0;
}

// Hardcoded fallback used when TotalControlData.db is unavailable.
// Mirrors the timezones table — covers eclipse locations through 2083.
inline std::vector<TzEntry> TzFallbackList() {
    return {
        // Reference
        { "UTC",  "UTC",            "UTC"                              },
        // Europe
        { "WAW",  "Warsaw",         "Europe/Warsaw"                    },
        { "MAD",  "Madrid",         "Europe/Madrid"                    },
        { "LON",  "London",         "Europe/London"                    },
        { "PAR",  "Paris",          "Europe/Paris"                     },
        { "ROM",  "Rome",           "Europe/Rome"                      },
        { "ATH",  "Athens",         "Europe/Athens"                    },
        { "IST",  "Istanbul",       "Europe/Istanbul"                  },
        // Africa
        { "CAI",  "Cairo",          "Africa/Cairo"                     },
        { "KRT",  "Khartoum",       "Africa/Khartoum"                  },
        { "ADD",  "Addis Ababa",    "Africa/Addis_Ababa"               },
        { "NAI",  "Nairobi",        "Africa/Nairobi"                   },
        { "WIN",  "Windhoek",       "Africa/Windhoek"                  },
        { "CPT",  "Cape Town",      "Africa/Johannesburg"              },
        // Middle East
        { "RUH",  "Riyadh",         "Asia/Riyadh"                      },
        { "DXB",  "Dubai",          "Asia/Dubai"                       },
        { "THR",  "Tehran",         "Asia/Tehran"                      },
        // Asia
        { "KHI",  "Karachi",        "Asia/Karachi"                     },
        { "KOL",  "Kolkata",        "Asia/Kolkata"                     },
        { "DAC",  "Dhaka",          "Asia/Dhaka"                       },
        { "BKK",  "Bangkok",        "Asia/Bangkok"                     },
        { "SHA",  "Shanghai",       "Asia/Shanghai"                    },
        { "SEL",  "Seoul",          "Asia/Seoul"                       },
        { "TYO",  "Tokyo",          "Asia/Tokyo"                       },
        { "YKT",  "Yakutsk",        "Asia/Yakutsk"                     },
        // Oceania
        { "SYD",  "Sydney",         "Australia/Sydney"                 },
        { "MEL",  "Melbourne",      "Australia/Melbourne"              },
        { "DRW",  "Darwin",         "Australia/Darwin"                 },
        { "PER",  "Perth",          "Australia/Perth"                  },
        { "AKL",  "Auckland",       "Pacific/Auckland"                 },
        // Americas
        { "NYC",  "New York",       "America/New_York"                 },
        { "CHI",  "Chicago",        "America/Chicago"                  },
        { "DEN",  "Denver",         "America/Denver"                   },
        { "LAX",  "Los Angeles",    "America/Los_Angeles"              },
        { "ANC",  "Anchorage",      "America/Anchorage"                },
        { "MEX",  "Mexico City",    "America/Mexico_City"              },
        { "BOG",  "Bogota",         "America/Bogota"                   },
        { "SAO",  "Sao Paulo",      "America/Sao_Paulo"                },
        { "BUE",  "Buenos Aires",   "America/Argentina/Buenos_Aires"   },
        { "SCL",  "Santiago",       "America/Santiago"                 },
    };
}

} // namespace TotalControl
