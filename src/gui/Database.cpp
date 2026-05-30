#include "Database.h"
#include "sqlite3.h"
#include <windows.h>
#include <format>
#include <vector>

namespace TotalControl {

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

// ─── Open ─────────────────────────────────────────────────────────────────────

bool Database::Open(const std::wstring& path) {
    std::string p = WideToUtf8(path);
    if (sqlite3_open(p.c_str(), &m_db) != SQLITE_OK) {
        m_db = nullptr;
        return false;
    }
    sqlite3_exec(m_db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    return true;
}

bool Database::OpenReadOnly(const std::wstring& path) {
    std::string p = WideToUtf8(path);
    int rc = sqlite3_open_v2(p.c_str(), &m_db,
                              SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK) {
        m_db = nullptr;
        return false;
    }
    return true;
}

void Database::Close() {
    if (m_db) { sqlite3_close(m_db); m_db = nullptr; }
}

// ─── Exec ─────────────────────────────────────────────────────────────────────

bool Database::Exec(std::string_view sql) {
    if (!m_db) return false;
    char* err = nullptr;
    int rc = sqlite3_exec(m_db, sql.data(), nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK;
}

// ─── Settings helpers ─────────────────────────────────────────────────────────

std::string Database::GetSetting(const char* key, const char* def) const {
    if (!m_db) return def ? def : "";
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(m_db,
        "SELECT value FROM settings WHERE key=?;", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    std::string result = def ? def : "";
    if (sqlite3_step(st) == SQLITE_ROW)
        result = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return result;
}

void Database::SetSetting(const char* key, const char* value) {
    if (!m_db) return;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(m_db,
        "INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?);",
        -1, &st, nullptr);
    sqlite3_bind_text(st, 1, key,   -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, value, -1, SQLITE_STATIC);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

int Database::GetSettingInt(const char* key, int def) const {
    std::string s = GetSetting(key, nullptr);
    if (s.empty()) return def;
    try { return std::stoi(s); } catch (...) { return def; }
}

void Database::SetSettingInt(const char* key, int value) {
    SetSetting(key, std::format("{}", value).c_str());
}

// ─── Reference data ───────────────────────────────────────────────────────────

std::vector<TzEntry> Database::LoadTimezones() const {
    std::vector<TzEntry> result;
    if (!m_db) return result;

    sqlite3_stmt* st = nullptr;
    int rc = sqlite3_prepare_v2(m_db,
        "SELECT code, label, iana FROM timezones ORDER BY sort_order, id;",
        -1, &st, nullptr);
    if (rc != SQLITE_OK) return result;

    while (sqlite3_step(st) == SQLITE_ROW) {
        TzEntry e;
        auto col = [&](int i) -> std::string {
            const auto* p = sqlite3_column_text(st, i);
            return p ? reinterpret_cast<const char*>(p) : "";
        };
        e.code  = col(0);
        e.label = col(1);
        e.iana  = col(2);
        result.push_back(std::move(e));
    }
    sqlite3_finalize(st);
    return result;
}

std::vector<EclipseEntry> Database::LoadEclipses() const {
    std::vector<EclipseEntry> out;
    if (!m_db) return out;

    sqlite3_stmt* st = nullptr;
    int rc = sqlite3_prepare_v2(m_db,
        "SELECT year, month, day, eclipse_type, td_ge,"
        "       lat_dd_ge, lng_dd_ge, central_duration, duration_secs, dt"
        " FROM eclipse_besselian"
        " ORDER BY julian_date;",
        -1, &st, nullptr);
    if (rc != SQLITE_OK) return out;

    auto col = [&](int i) -> std::string {
        const auto* p = sqlite3_column_text(st, i);
        return p ? reinterpret_cast<const char*>(p) : "";
    };

    while (sqlite3_step(st) == SQLITE_ROW) {
        EclipseEntry e;
        e.year         = sqlite3_column_int(st, 0);
        e.month        = sqlite3_column_int(st, 1);
        e.day          = sqlite3_column_int(st, 2);
        e.type         = col(3);
        e.timeGe       = col(4);
        e.latGe        = static_cast<float>(sqlite3_column_double(st, 5));
        e.lonGe        = static_cast<float>(sqlite3_column_double(st, 6));
        e.duration     = col(7);
        e.durationSecs = static_cast<float>(sqlite3_column_double(st, 8));
        e.dt           = static_cast<float>(sqlite3_column_double(st, 9));
        out.push_back(std::move(e));
    }
    sqlite3_finalize(st);
    return out;
}

BesselianElements Database::LoadBesselianElements(int year, int month, int day) const {
    BesselianElements b;
    if (!m_db) return b;

    sqlite3_stmt* st = nullptr;
    int rc = sqlite3_prepare_v2(m_db,
        "SELECT t0, dt,"
        " x0,x1,x2,x3, y0,y1,y2,y3,"
        " d0,d1,d2, mu0,mu1,mu2,"
        " l10,l11,l12, l20,l21,l22,"
        " tan_f1, tan_f2, tmin, tmax"
        " FROM eclipse_besselian"
        " WHERE year=? AND month=? AND day=?"
        " LIMIT 1;",
        -1, &st, nullptr);
    if (rc != SQLITE_OK) return b;

    sqlite3_bind_int(st, 1, year);
    sqlite3_bind_int(st, 2, month);
    sqlite3_bind_int(st, 3, day);

    if (sqlite3_step(st) == SQLITE_ROW) {
        auto d = [&](int i){ return sqlite3_column_double(st, i); };
        b.year  = year; b.month = month; b.day = day;
        b.t0    = d(0);  b.dt   = d(1);
        b.x0    = d(2);  b.x1   = d(3);  b.x2  = d(4);  b.x3  = d(5);
        b.y0    = d(6);  b.y1   = d(7);  b.y2  = d(8);  b.y3  = d(9);
        b.d0    = d(10); b.d1   = d(11); b.d2  = d(12);
        b.mu0   = d(13); b.mu1  = d(14); b.mu2 = d(15);
        b.l10   = d(16); b.l11  = d(17); b.l12 = d(18);
        b.l20   = d(19); b.l21  = d(20); b.l22 = d(21);
        b.tan_f1= d(22); b.tan_f2= d(23);
        b.tmin  = d(24); b.tmax = d(25);
        b.valid = true;
    }
    sqlite3_finalize(st);
    return b;
}

} // namespace TotalControl
