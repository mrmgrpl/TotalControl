#pragma once
#include <string>

struct sqlite3;

namespace TotalControl {

class Database {
public:
    bool Open(const std::wstring& path);
    void Close();
    ~Database() { Close(); }

    std::string GetSetting(const char* key, const char* def = "") const;
    void        SetSetting(const char* key, const char* value);
    int         GetSettingInt(const char* key, int def = 0) const;
    void        SetSettingInt(const char* key, int value);

private:
    sqlite3* m_db = nullptr;
};

} // namespace TotalControl
