#pragma once
#include "TzEntry.h"
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;

namespace TotalControl {

class Database {
public:
    // Open for read/write (creates file if missing).
    bool Open(const std::wstring& path);

    // Open existing file read-only. Returns false if file doesn't exist.
    bool OpenReadOnly(const std::wstring& path);

    void Close();
    bool IsOpen() const noexcept { return m_db != nullptr; }

    ~Database() { Close(); }

    // Execute arbitrary SQL (DDL, multi-statement, no results needed).
    bool Exec(std::string_view sql);

    // settings table helpers
    std::string GetSetting(const char* key, const char* def = "") const;
    void        SetSetting(const char* key, const char* value);
    int         GetSettingInt(const char* key, int def = 0) const;
    void        SetSettingInt(const char* key, int value);

    // reference data queries
    std::vector<TzEntry> LoadTimezones() const;

private:
    sqlite3* m_db = nullptr;
};

} // namespace TotalControl
