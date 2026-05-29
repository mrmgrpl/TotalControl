#include "Database.h"
#include "sqlite3.h"
#include <windows.h>
#include <format>

namespace TotalControl {

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

bool Database::Open(const std::wstring& path) {
    std::string p = WideToUtf8(path);
    if (sqlite3_open(p.c_str(), &m_db) != SQLITE_OK) {
        m_db = nullptr;
        return false;
    }
    sqlite3_exec(m_db,
        "CREATE TABLE IF NOT EXISTS settings"
        " (key TEXT PRIMARY KEY, value TEXT NOT NULL);",
        nullptr, nullptr, nullptr);
    return true;
}

void Database::Close() {
    if (m_db) { sqlite3_close(m_db); m_db = nullptr; }
}

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

} // namespace TotalControl
