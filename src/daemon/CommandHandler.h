#pragma once
#include "CameraController.h"
#include <string>
#include <vector>

namespace TotalControl {

class CommandHandler {
public:
    // Przyjmuje listę wskaźników do połączonych kamer.
    // "cam":guid lub "cam":index w JSON → routing; brak pola → kamera[0].
    explicit CommandHandler(std::vector<CameraController*> cams);

    // Zwraca false gdy daemon ma się zakończyć (cmd=quit)
    bool Handle(const std::wstring& request, std::wstring& response);

private:
    std::vector<CameraController*> m_cams;

    // Routing: zwraca kamerę na podstawie pola "cam" w JSON lub nullptr jeśli nie znaleziono.
    CameraController* RouteCamera(const std::wstring& req) const;

    // Minimalne narzędzia JSON
    static std::wstring JStr(const std::wstring& json, const wchar_t* key);
    static int          JInt(const std::wstring& json, const wchar_t* key, int def = 0);
    static float        JFlt(const std::wstring& json, const wchar_t* key, float def = 0.f);
    static bool         JHas(const std::wstring& json, const wchar_t* key);
    static bool         JBool(const std::wstring& json, const wchar_t* key, bool def = false);

    static std::wstring Ok(const std::wstring& extra = L"");
    static std::wstring Err(const wchar_t* code, const wchar_t* msg = nullptr);

    // Normalizacja nazw właściwości: "shutter-speed"/"ss" → "shutter_speed"
    static std::wstring NormProp(const std::wstring& raw);
};

} // namespace TotalControl
