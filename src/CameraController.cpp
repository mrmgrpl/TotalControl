#include "CameraController.h"

#include "CameraRemote_SDK.h"
#include "IDeviceCallback.h"
#include "ICrCameraObjectInfo.h"
#include "CrDeviceProperty.h"
#include "CrCommandData.h"
#include "CrError.h"

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <sstream>
#include <string>

namespace SDK = SCRSDK;

namespace TotalControl {

// ─── DeviceCallback ───────────────────────────────────────────────────────────

class CameraController::DeviceCallback : public SDK::IDeviceCallback {
public:
    explicit DeviceCallback(CameraController* o) : m_owner(o) {}

    void OnConnected(SDK::DeviceConnectionVersioin) override {
        m_owner->m_connected    = true;
        m_owner->m_connectedSig = true;
        m_owner->m_waitCv.notify_all();
        m_owner->Log(L"[CB] OnConnected");
    }
    void OnDisconnected(CrInt32u code) override {
        m_owner->m_connected = false;
        m_owner->Logf(L"[CB] OnDisconnected 0x%08X", code);
        m_owner->m_waitCv.notify_all();
    }
    void OnWarning(CrInt32u w) override {
        if (w == SDK::CrNotify_Captured_Event || w == SDK::CrNotify_All_Download_Complete) {
            m_owner->m_capturedSig = true;
            m_owner->m_waitCv.notify_all();
            m_owner->Logf(L"[CB] Captured 0x%08X", w);
        }
    }
    void OnCompleteDownload(CrChar* fn, CrInt32u) override {
        m_owner->m_capturedSig = true;
        m_owner->m_waitCv.notify_all();
        m_owner->Logf(L"[CB] Download: %s", fn ? fn : L"?");
    }
    void OnNotifyPostViewImage(CrChar*, CrInt32u sz) override {
        m_owner->m_capturedSig = true;
        m_owner->m_waitCv.notify_all();
        m_owner->Logf(L"[CB] PostView %u B", sz);
    }
    void OnError(CrInt32u code) override { m_owner->Logf(L"[CB] Error 0x%08X", code); }

    void OnPropertyChanged() override {}
    void OnPropertyChangedCodes(CrInt32u, CrInt32u*) override {}
    void OnLvPropertyChanged() override {}
    void OnLvPropertyChangedCodes(CrInt32u, CrInt32u*) override {}
    void OnCompleteOperation(CrInt32u, SDK::CrOperationResultData*) override {}
    void OnNotifyContentsTransfer(CrInt32u, SDK::CrContentHandle, CrChar*) override {}
    void OnWarningExt(CrInt32u, CrInt32, CrInt32, CrInt32) override {}
    void OnNotifyFTPTransferResult(CrInt32u, CrInt32u, CrInt32u) override {}
    void OnNotifyRemoteTransferResult(CrInt32u, CrInt32u, CrChar*) override {}
    void OnNotifyRemoteTransferResult(CrInt32u, CrInt32u, CrInt8u*, CrInt64u) override {}
    void OnNotifyRemoteTransferContentsListChanged(CrInt32u, CrInt32u, CrInt32u) override {}
    void OnNotifyRemoteFirmwareUpdateResult(CrInt32u, const void*) override {}
    void OnReceivePlaybackTimeCode(CrInt32u) override {}
    void OnReceivePlaybackData(CrInt8u, CrInt32, CrInt8u*, CrInt64, CrInt64, CrInt32, CrInt32) override {}
    void OnNotifyMonitorUpdated(CrInt32u, CrInt32u) override {}

private:
    CameraController* m_owner;
};

// ─── Lifecycle ────────────────────────────────────────────────────────────────

CameraController::CameraController(LogFn log) : m_log(std::move(log)) {}
CameraController::~CameraController() { Shutdown(); }

void CameraController::Log(const wchar_t* msg) {
    ::OutputDebugStringW(msg); ::OutputDebugStringW(L"\n");
    if (m_log) m_log(msg);
}
void CameraController::Logf(const wchar_t* fmt, ...) {
    wchar_t buf[512]; va_list a; va_start(a, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, a); va_end(a);
    Log(buf);
}

bool CameraController::Init() {
    if (m_initialized) return true;
    m_callback = new DeviceCallback(this);
    if (!SDK::Init(0)) {
        Log(L"SDK::Init FAILED");
        delete m_callback; m_callback = nullptr;
        return false;
    }
    m_initialized = true;
    Log(L"SDK::Init OK");
    return true;
}

void CameraController::Shutdown() {
    Disconnect();
    if (m_initialized) { SDK::Release(); m_initialized = false; }
    delete m_callback; m_callback = nullptr;
}

bool CameraController::Connect(int enumTimeoutSec, int connectTimeoutMs) {
    if (!m_initialized) return false;
    if (m_connected) Disconnect();

    SDK::ICrEnumCameraObjectInfo* pEnum = nullptr;
    SDK::CrError err = SDK::EnumCameraObjects(&pEnum, enumTimeoutSec);
    if (err != 0 || !pEnum || pEnum->GetCount() == 0) {
        if (pEnum) pEnum->Release();
        Log(L"Connect: brak kamery");
        return false;
    }

    const auto* info0 = pEnum->GetCameraObjectInfo(0);
    m_model = (info0 && info0->GetModel()) ? info0->GetModel() : L"Unknown";
    auto* info = const_cast<SDK::ICrCameraObjectInfo*>(info0);

    SDK::CrDeviceHandle h = 0;
    m_connectedSig = false;
    err = SDK::Connect(info, m_callback, &h);
    pEnum->Release();

    if (err != 0 || h == 0) { Logf(L"Connect FAILED err=0x%04X", err); return false; }
    m_deviceHandle = static_cast<uint64_t>(h);

    std::unique_lock<std::mutex> lk(m_waitMutex);
    bool ok = m_waitCv.wait_for(lk, std::chrono::milliseconds(connectTimeoutMs),
        [this] { return m_connectedSig.load(); });
    if (!ok) { Log(L"Connect: timeout OnConnected"); Disconnect(); return false; }
    Logf(L"Połączono: %s", m_model.c_str());
    return true;
}

void CameraController::Disconnect() {
    if (!m_connected || !m_deviceHandle) return;
    auto h = static_cast<SDK::CrDeviceHandle>(m_deviceHandle);
    SDK::Disconnect(h);
    SDK::ReleaseDevice(h);
    m_deviceHandle = 0;
    m_connected    = false;
}

// ─── SetPropRaw ───────────────────────────────────────────────────────────────

bool CameraController::SetPropRaw(unsigned code, unsigned type, long long value, const wchar_t* desc) {
    auto h = static_cast<SDK::CrDeviceHandle>(m_deviceHandle);
    SDK::CrDeviceProperty prop;
    prop.SetCode(static_cast<SDK::CrDevicePropertyCode>(code));
    prop.SetValueType(static_cast<SDK::CrDataType>(type));
    prop.SetCurrentValue(static_cast<CrInt64u>(value));
    SDK::CrError err = SDK::SetDeviceProperty(h, &prop);
    Logf(L"Set %-24s = 0x%llX  err=0x%04X", desc, (unsigned long long)value, (unsigned)err);
    return err == 0;
}

// ─── Nearest helpers ──────────────────────────────────────────────────────────

uint32_t CameraController::NearestFromList16(unsigned propCode, uint32_t target) {
    auto h    = static_cast<SDK::CrDeviceHandle>(m_deviceHandle);
    CrInt32u codeU = static_cast<CrInt32u>(propCode);
    SDK::CrDeviceProperty* props = nullptr; CrInt32 num = 0;
    if (SDK::GetSelectDeviceProperties(h, 1, &codeU, &props, &num) != 0 || !props || num == 0) {
        if (props) SDK::ReleaseDeviceProperties(h, props); return target;
    }
    auto*    vals  = reinterpret_cast<CrInt16u*>(props[0].GetSetValues());
    uint32_t count = props[0].GetSetValueSize() / sizeof(CrInt16u);
    uint32_t best  = target, bestDiff = UINT32_MAX;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t d = (vals[i] > target) ? (vals[i] - target) : (target - vals[i]);
        if (d < bestDiff) { bestDiff = d; best = vals[i]; }
    }
    SDK::ReleaseDeviceProperties(h, props);
    return best;
}

uint32_t CameraController::NearestFromList32log(unsigned propCode, uint32_t target) {
    auto h    = static_cast<SDK::CrDeviceHandle>(m_deviceHandle);
    CrInt32u codeU = static_cast<CrInt32u>(propCode);
    SDK::CrDeviceProperty* props = nullptr; CrInt32 num = 0;
    if (SDK::GetSelectDeviceProperties(h, 1, &codeU, &props, &num) != 0 || !props || num == 0) {
        if (props) SDK::ReleaseDeviceProperties(h, props); return target;
    }
    auto*    vals  = reinterpret_cast<CrInt32u*>(props[0].GetSetValues());
    uint32_t count = props[0].GetSetValueSize() / sizeof(CrInt32u);
    uint32_t best  = target; double bestDiff = 1e18;
    for (uint32_t i = 0; i < count; ++i) {
        if (vals[i] == 0) continue;
        double diff = std::abs(std::log((double)vals[i]) - std::log((double)target));
        if (diff < bestDiff) { bestDiff = diff; best = vals[i]; }
    }
    SDK::ReleaseDeviceProperties(h, props);
    return best;
}

// ─── ShutterSpeed ────────────────────────────────────────────────────────────

uint32_t CameraController::ParseShutterSpeedToRaw(const wchar_t* value) {
    std::wstring v = value;
    if (v == L"bulb" || v == L"BULB") return 0;
    auto slash = v.find(L'/');
    if (slash != std::wstring::npos) {
        // "1/100" format → (1 << 16) | 100
        uint32_t n = static_cast<uint32_t>(std::stoi(v.substr(0, slash)));
        uint32_t d = static_cast<uint32_t>(std::stoi(v.substr(slash + 1)));
        return (n << 16) | d;
    }
    // "25s" or "25" — sekundy → encode as tenths: 25s → (250 << 16) | 10
    if (!v.empty() && (v.back() == L's' || v.back() == L'S')) v.pop_back();
    float sec = std::stof(v);
    auto  tenths = static_cast<uint32_t>(sec * 10.0f + 0.5f);
    return (tenths << 16) | 10u;
}

uint32_t CameraController::NearestShutterSpeed(uint32_t targetRaw) {
    auto h    = static_cast<SDK::CrDeviceHandle>(m_deviceHandle);
    CrInt32u codeU = static_cast<CrInt32u>(SDK::CrDeviceProperty_ShutterSpeed);
    SDK::CrDeviceProperty* props = nullptr; CrInt32 num = 0;
    if (SDK::GetSelectDeviceProperties(h, 1, &codeU, &props, &num) != 0 || !props || num == 0) {
        if (props) SDK::ReleaseDeviceProperties(h, props); return targetRaw;
    }
    auto* vals  = reinterpret_cast<CrInt32u*>(props[0].GetSetValues());
    uint32_t count = props[0].GetSetValueSize() / sizeof(CrInt32u);

    auto toSec = [](uint32_t v) -> double {
        if (v == 0 || v == 0xFFFFFFFF) return -1.0;
        uint32_t n = (v >> 16) & 0xFFFF, d = v & 0xFFFF;
        return (d > 0) ? (double)n / d : (double)n;
    };

    double tSec = toSec(targetRaw);
    if (tSec <= 0.0) { SDK::ReleaseDeviceProperties(h, props); return targetRaw; }

    uint32_t best = targetRaw; double bestDiff = 1e18;
    for (uint32_t i = 0; i < count; ++i) {
        double s = toSec(vals[i]);
        if (s <= 0.0) continue;
        double diff = std::abs(std::log(s) - std::log(tSec));
        if (diff < bestDiff) { bestDiff = diff; best = vals[i]; }
    }
    SDK::ReleaseDeviceProperties(h, props);
    return best;
}

// ─── Settery ekspozycji ───────────────────────────────────────────────────────

bool CameraController::SetPCRemotePriority() {
    return SetPropRaw(SDK::CrDeviceProperty_PriorityKeySettings,
                      SDK::CrDataType_UInt16, SDK::CrPriorityKey_PCRemote, L"PriorityKey");
}

bool CameraController::SetExposureMode(const wchar_t* mode) {
    uint32_t val = SDK::CrExposure_M_Manual;
    if      (!wcscmp(mode, L"P")) val = SDK::CrExposure_P_Auto;
    else if (!wcscmp(mode, L"A")) val = SDK::CrExposure_A_AperturePriority;
    else if (!wcscmp(mode, L"S")) val = SDK::CrExposure_S_ShutterSpeedPriority;
    return SetPropRaw(SDK::CrDeviceProperty_ExposureProgramMode,
                      SDK::CrDataType_UInt32, val, L"ExposureMode");
}

bool CameraController::SetFocusMode(const wchar_t* mode) {
    uint32_t val = SDK::CrFocus_MF;
    if      (!wcscmp(mode, L"AF-S")) val = SDK::CrFocus_AF_S;
    else if (!wcscmp(mode, L"AF-C")) val = SDK::CrFocus_AF_C;
    else if (!wcscmp(mode, L"AF-A")) val = SDK::CrFocus_AF_A;
    return SetPropRaw(SDK::CrDeviceProperty_FocusMode,
                      SDK::CrDataType_UInt16, val, L"FocusMode");
}

bool CameraController::SetShutterSpeed(const wchar_t* value) {
    uint32_t raw    = ParseShutterSpeedToRaw(value);
    uint32_t actual = NearestShutterSpeed(raw);
    uint32_t n = (actual >> 16) & 0xFFFF, d = actual & 0xFFFF;
    if (d == 10) Logf(L"ShutterSpeed: %s → %.0fs", value, (double)n / 10.0);
    else         Logf(L"ShutterSpeed: %s → %u/%u", value, n, d);
    return SetPropRaw(SDK::CrDeviceProperty_ShutterSpeed,
                      SDK::CrDataType_UInt32, actual, L"ShutterSpeed");
}

bool CameraController::SetISO(int iso) {
    uint32_t actual = NearestFromList32log(SDK::CrDeviceProperty_IsoSensitivity,
                                           static_cast<uint32_t>(iso));
    Logf(L"ISO: %d → %u", iso, actual);
    return SetPropRaw(SDK::CrDeviceProperty_IsoSensitivity,
                      SDK::CrDataType_UInt32, actual, L"ISO");
}

bool CameraController::SetFNumber(float f) {
    uint32_t target = static_cast<uint32_t>(f * 100.0f + 0.5f);
    uint32_t actual = NearestFromList16(SDK::CrDeviceProperty_FNumber, target);
    Logf(L"FNumber: F/%.1f → F/%.1f", f, actual / 100.0);
    return SetPropRaw(SDK::CrDeviceProperty_FNumber,
                      SDK::CrDataType_UInt16, actual, L"FNumber");
}

bool CameraController::SetStoreDestination(const wchar_t* dest) {
    uint32_t val = SDK::CrStillImageStoreDestination_MemoryCard;
    if      (!wcscmp(dest, L"pc"))   val = SDK::CrStillImageStoreDestination_HostPC;
    else if (!wcscmp(dest, L"both")) val = SDK::CrStillImageStoreDestination_HostPCAndMemoryCard;
    return SetPropRaw(SDK::CrDeviceProperty_StillImageStoreDestination,
                      SDK::CrDataType_UInt16, val, L"StoreDestination");
}

// ─── Shoot ────────────────────────────────────────────────────────────────────

bool CameraController::Shoot(int* latencyMs, int timeoutMs) {
    if (!m_connected) { Log(L"Shoot: brak połączenia"); return false; }
    auto h = static_cast<SDK::CrDeviceHandle>(m_deviceHandle);
    m_capturedSig = false;

    auto t0 = std::chrono::steady_clock::now();
    SDK::SendCommand(h, SDK::CrCommandId_Release, SDK::CrCommandParam_Down);
    ::Sleep(35);
    SDK::SendCommand(h, SDK::CrCommandId_Release, SDK::CrCommandParam_Up);

    std::unique_lock<std::mutex> lk(m_waitMutex);
    bool ok = m_waitCv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
        [this] { return m_capturedSig.load(); });

    int ms = static_cast<int>(
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count());
    if (latencyMs) *latencyMs = ms;
    Logf(L"Shoot: %s  %d ms", ok ? L"OK" : L"TIMEOUT", ms);
    return ok;
}

// ─── GetStatus ────────────────────────────────────────────────────────────────

CameraStatus CameraController::GetStatus() {
    CameraStatus s;
    s.connected = m_connected;
    s.model     = m_model;
    if (!m_connected) return s;

    auto h = static_cast<SDK::CrDeviceHandle>(m_deviceHandle);
    SDK::CrDeviceProperty* props = nullptr; CrInt32 num = 0;
    if (SDK::GetDeviceProperties(h, &props, &num) != 0 || !props) return s;

    auto ssToStr = [](uint64_t cur) -> std::wstring {
        if (cur == 0) return L"bulb";
        uint32_t n = (cur >> 16) & 0xFFFF, d = cur & 0xFFFF;
        double sec = (d > 0) ? (double)n / d : (double)n;
        wchar_t b[32];
        if (sec >= 1.0) swprintf_s(b, L"%.0fs", sec);
        else            swprintf_s(b, L"1/%.0f", 1.0 / sec);
        return b;
    };

    for (CrInt32 i = 0; i < num; ++i) {
        auto code = props[i].GetCode();
        auto cur  = props[i].GetCurrentValue();
        switch (code) {
        case SDK::CrDeviceProperty_BatteryRemain:
            s.batteryPct = static_cast<int>(cur); break;
        case SDK::CrDeviceProperty_MediaSLOT1_RemainingNumber:
            s.remainingShots = static_cast<int>(cur); break;
        case SDK::CrDeviceProperty_ShutterSpeed:
            s.shutterSpeed = ssToStr(cur); break;
        case SDK::CrDeviceProperty_IsoSensitivity:
            s.iso = static_cast<int>(cur); break;
        case SDK::CrDeviceProperty_FNumber:
            s.fNumber = static_cast<float>(cur) / 100.0f; break;
        case SDK::CrDeviceProperty_ExposureProgramMode:
            switch (cur) {
            case SDK::CrExposure_M_Manual:               s.exposureMode = L"M"; break;
            case SDK::CrExposure_P_Auto:                 s.exposureMode = L"P"; break;
            case SDK::CrExposure_A_AperturePriority:     s.exposureMode = L"A"; break;
            case SDK::CrExposure_S_ShutterSpeedPriority: s.exposureMode = L"S"; break;
            default:                                     s.exposureMode = L"?"; break;
            }
            break;
        case SDK::CrDeviceProperty_FocusMode:
            switch (cur) {
            case SDK::CrFocus_MF:   s.focusMode = L"MF";   break;
            case SDK::CrFocus_AF_S: s.focusMode = L"AF-S"; break;
            case SDK::CrFocus_AF_C: s.focusMode = L"AF-C"; break;
            default:                s.focusMode = L"?";    break;
            }
            break;
        case SDK::CrDeviceProperty_StillImageStoreDestination:
            switch (cur) {
            case SDK::CrStillImageStoreDestination_HostPC:              s.storeDestination = L"pc";   break;
            case SDK::CrStillImageStoreDestination_MemoryCard:          s.storeDestination = L"card"; break;
            case SDK::CrStillImageStoreDestination_HostPCAndMemoryCard: s.storeDestination = L"both"; break;
            }
            break;
        }
    }
    SDK::ReleaseDeviceProperties(h, props);
    return s;
}

} // namespace TotalControl
