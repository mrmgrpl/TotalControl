#include "CommandHandler.h"
#include <algorithm>
#include <cwctype>
#include <sstream>

namespace TotalControl {

CommandHandler::CommandHandler(CameraController& cam) : m_cam(cam) {}

// ─── Minimalne parsowanie JSON ────────────────────────────────────────────────
// Obsługuje tylko płaskie obiekty z wartościami string i liczbami.

std::wstring CommandHandler::JStr(const std::wstring& j, const wchar_t* key) {
    std::wstring k = std::wstring(L"\"") + key + L"\":\"";
    auto pos = j.find(k);
    if (pos == std::wstring::npos) return L"";
    pos += k.size();
    auto end = j.find(L'"', pos);
    return (end != std::wstring::npos) ? j.substr(pos, end - pos) : L"";
}

int CommandHandler::JInt(const std::wstring& j, const wchar_t* key, int def) {
    std::wstring k = std::wstring(L"\"") + key + L"\":";
    auto pos = j.find(k);
    if (pos == std::wstring::npos) return def;
    pos += k.size();
    // Pomijamy cudzysłów jeśli wartość to string
    if (pos < j.size() && j[pos] == L'"') { ++pos; }
    try { return std::stoi(j.substr(pos)); } catch (...) { return def; }
}

float CommandHandler::JFlt(const std::wstring& j, const wchar_t* key, float def) {
    std::wstring k = std::wstring(L"\"") + key + L"\":";
    auto pos = j.find(k);
    if (pos == std::wstring::npos) return def;
    pos += k.size();
    if (pos < j.size() && j[pos] == L'"') { ++pos; }
    try { return std::stof(j.substr(pos)); } catch (...) { return def; }
}

bool CommandHandler::JHas(const std::wstring& j, const wchar_t* key) {
    return j.find(std::wstring(L"\"") + key + L"\":") != std::wstring::npos;
}

// ─── Budowanie odpowiedzi JSON ────────────────────────────────────────────────

std::wstring CommandHandler::Ok(const std::wstring& extra) {
    return extra.empty() ? L"{\"ok\":true}" : L"{\"ok\":true," + extra + L"}";
}

std::wstring CommandHandler::Err(const wchar_t* code, const wchar_t* msg) {
    std::wstring r = std::wstring(L"{\"ok\":false,\"err\":\"") + code + L"\"";
    if (msg) r += std::wstring(L",\"msg\":\"") + msg + L"\"";
    return r + L"}";
}

// ─── Normalizacja nazw właściwości ────────────────────────────────────────────

std::wstring CommandHandler::NormProp(const std::wstring& raw) {
    std::wstring s = raw;
    // Zamień '-' na '_', lower-case
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    std::replace(s.begin(), s.end(), L'-', L'_');
    // Aliasy krótkie
    if (s == L"ss")   return L"shutter_speed";
    if (s == L"f")    return L"f_number";
    if (s == L"mode") return L"exposure_mode";
    return s;
}

// ─── Główny dispatcher ────────────────────────────────────────────────────────

bool CommandHandler::Handle(const std::wstring& req, std::wstring& resp) {
    std::wstring cmd = JStr(req, L"cmd");

    // ── quit ──────────────────────────────────────────────────────────────────
    if (cmd == L"quit") {
        resp = Ok();
        return false;   // sygnalizuje PipeServer: zakończ pętlę
    }

    // ── status ────────────────────────────────────────────────────────────────
    if (cmd == L"status") {
        CameraStatus s = m_cam.GetStatus();
        std::wostringstream ss;
        ss << L"\"connected\":" << (s.connected ? L"true" : L"false")
           << L",\"model\":\""  << s.model << L"\""
           << L",\"battery\":"  << s.batteryPct
           << L",\"remaining\":" << s.remainingShots
           << L",\"ss\":\""     << s.shutterSpeed << L"\""
           << L",\"iso\":"      << s.iso
           << L",\"f\":"        << s.fNumber
           << L",\"mode\":\""   << s.exposureMode << L"\""
           << L",\"focus\":\""  << s.focusMode << L"\""
           << L",\"store\":\""  << s.storeDestination << L"\"";
        resp = Ok(ss.str());
        return true;
    }

    // ── shoot ─────────────────────────────────────────────────────────────────
    if (cmd == L"shoot") {
        if (!m_cam.IsConnected()) { resp = Err(L"not_connected"); return true; }

        // 1. PCRemote priority — zawsze
        m_cam.SetPCRemotePriority();
        ::Sleep(300);

        // 2. Opcjonalne parametry ekspozycji
        if (JHas(req, L"mode"))  m_cam.SetExposureMode(JStr(req, L"mode").c_str());
        if (JHas(req, L"focus")) m_cam.SetFocusMode(JStr(req, L"focus").c_str());
        if (JHas(req, L"iso"))   m_cam.SetISO(JInt(req, L"iso"));
        if (JHas(req, L"f"))     m_cam.SetFNumber(JFlt(req, L"f"));
        if (JHas(req, L"ss")) {
            m_cam.SetShutterSpeed(JStr(req, L"ss").c_str());
            ::Sleep(800);   // daj kamerze czas na zatwierdzenie SS
        }

        // 3. Zapis na kartę → CrNotify_Captured_Event działa niezawodnie
        std::wstring store = JHas(req, L"store") ? JStr(req, L"store") : L"card";
        m_cam.SetStoreDestination(store.c_str());
        ::Sleep(300);

        // 4. Strzał
        int timeout = JHas(req, L"timeout_ms") ? JInt(req, L"timeout_ms") : 5000;
        int latency = 0;
        bool ok = m_cam.Shoot(&latency, timeout);

        if (ok) {
            std::wostringstream ss;
            ss << L"\"latency_ms\":" << latency;
            resp = Ok(ss.str());
        } else {
            resp = Err(L"timeout", L"CrNotify_Captured_Event nie przyszedł");
        }
        return true;
    }

    // ── set ───────────────────────────────────────────────────────────────────
    if (cmd == L"set") {
        if (!m_cam.IsConnected()) { resp = Err(L"not_connected"); return true; }
        std::wstring prop = NormProp(JStr(req, L"prop"));
        std::wstring val  = JStr(req, L"val");

        bool ok = false;
        if      (prop == L"shutter_speed")   ok = m_cam.SetShutterSpeed(val.c_str());
        else if (prop == L"iso")             ok = m_cam.SetISO(JInt(req, L"val"));
        else if (prop == L"f_number")        ok = m_cam.SetFNumber(JFlt(req, L"val"));
        else if (prop == L"exposure_mode")   ok = m_cam.SetExposureMode(val.c_str());
        else if (prop == L"focus_mode")      ok = m_cam.SetFocusMode(val.c_str());
        else if (prop == L"store_dest")      ok = m_cam.SetStoreDestination(val.c_str());
        else if (prop == L"priority_key" && val == L"pc") ok = m_cam.SetPCRemotePriority();
        else { resp = Err(L"unknown_prop", prop.c_str()); return true; }

        resp = ok ? Ok() : Err(L"set_failed");
        return true;
    }

    // ── get ───────────────────────────────────────────────────────────────────
    if (cmd == L"get") {
        if (!m_cam.IsConnected()) { resp = Err(L"not_connected"); return true; }
        std::wstring prop = NormProp(JStr(req, L"prop"));
        CameraStatus s = m_cam.GetStatus();
        std::wstring val;
        if      (prop == L"shutter_speed")  val = s.shutterSpeed;
        else if (prop == L"iso")            { std::wostringstream ss; ss << s.iso;      val = ss.str(); }
        else if (prop == L"f_number")       { std::wostringstream ss; ss << s.fNumber;  val = ss.str(); }
        else if (prop == L"exposure_mode")  val = s.exposureMode;
        else if (prop == L"focus_mode")     val = s.focusMode;
        else if (prop == L"store_dest")     val = s.storeDestination;
        else if (prop == L"battery")        { std::wostringstream ss; ss << s.batteryPct; val = ss.str(); }
        else if (prop == L"remaining")      { std::wostringstream ss; ss << s.remainingShots; val = ss.str(); }
        else { resp = Err(L"unknown_prop", prop.c_str()); return true; }

        resp = Ok(std::wstring(L"\"val\":\"") + val + L"\"");
        return true;
    }

    // ── nieznana komenda ──────────────────────────────────────────────────────
    resp = Err(L"unknown_cmd");
    return true;
}

} // namespace TotalControl
