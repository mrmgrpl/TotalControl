#pragma once
#include "CameraController.h"
#include <mutex>
#include <string>
#include <vector>

namespace TotalControl {

class SequencerEngine; // forward declare — no full include needed here

class CommandHandler {
public:
    // Przyjmuje listę wskaźników do połączonych kamer.
    // "cam":guid lub "cam":index w JSON → routing; brak pola → kamera[0].
    explicit CommandHandler(std::vector<CameraController*> cams);

    // Opcjonalnie: ustaw sekwencer do obsługi seq_start/stop/status.
    void SetSequencer(SequencerEngine* seq);

    // Zwraca false gdy daemon ma się zakończyć (cmd=quit)
    // Thread-safe: commands targeting different cameras run concurrently
    // (locked per-camera below); commands to the SAME camera, and global
    // commands (list_cameras/seq_*/quit), are serialised against each other.
    bool Handle(const std::wstring& request, std::wstring& response);

private:
    std::vector<CameraController*> m_cams;
    SequencerEngine*               m_seq = nullptr;

    // One mutex per camera — held for the duration of any command routed to
    // that camera, so a slow bracket capture on cam0 never blocks a command
    // to cam1/cam2. Sized to m_cams.size() in the constructor.
    mutable std::vector<std::mutex> m_camLocks;
    // Guards commands not tied to a single camera: list_cameras, seq_start,
    // seq_stop, seq_status, quit.
    mutable std::mutex m_globalLock;

    // Routing: zwraca kamerę na podstawie pola "cam" w JSON lub nullptr jeśli nie znaleziono.
    CameraController* RouteCamera(const std::wstring& req) const;
    // Index of `cam` within m_cams — used to pick the per-camera lock.
    size_t CamIndex(const CameraController* cam) const;

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
