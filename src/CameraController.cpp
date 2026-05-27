#include "CameraController.h"

// Sony CrSDK — external headers, do not modify; warnings suppressed intentionally
#pragma warning(push, 0)
#include "CameraRemote_SDK.h"
#include "IDeviceCallback.h"
#include "ICrCameraObjectInfo.h"
#include "CrDeviceProperty.h"
#include "CrCommandData.h"
#include "CrError.h"
#pragma warning(pop)

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <sstream>
#include <string>

namespace SDK = SCRSDK;

namespace TotalControl {

// ─── Static helpers ───────────────────────────────────────────────────────────

// Returns the camera's UUID string when available (WiFi/Ethernet cameras).
// For USB cameras GetGuid() is empty; GetId() contains a null-terminated UTF-16LE
// string (e.g. serial number), so reinterpret as wchar_t* rather than hex-encoding.
static std::wstring GuidOrIdHex(const SDK::ICrCameraObjectInfo* info) {
    if (!info) return L"";
    const CrChar* g = info->GetGuid();
    if (g && *g) return g;
    const CrInt8u* id = info->GetId();
    const CrInt32u sz = info->GetIdSize();
    if (!id || sz < sizeof(wchar_t)) return L"";
    const wchar_t* wid      = reinterpret_cast<const wchar_t*>(id);
    const size_t   maxChars = sz / sizeof(wchar_t);
    size_t len = 0;
    while (len < maxChars && wid[len] != L'\0') ++len;
    return std::wstring(wid, len);
}

// ─── Static SDK lifecycle ─────────────────────────────────────────────────────

static std::mutex s_sdkMutex;
static int        s_sdkRef = 0;

bool CameraController::InitSDK() {
    std::lock_guard<std::mutex> lk(s_sdkMutex);
    if (s_sdkRef == 0 && !SDK::Init(0)) return false;
    ++s_sdkRef;
    return true;
}

void CameraController::ReleaseSDK() {
    std::lock_guard<std::mutex> lk(s_sdkMutex);
    if (s_sdkRef > 0 && --s_sdkRef == 0) SDK::Release();
}

std::vector<CameraInfo> CameraController::Enumerate(int timeoutSec) {
    std::vector<CameraInfo> result;
    SDK::ICrEnumCameraObjectInfo* pEnum = nullptr;
    SDK::EnumCameraObjects(&pEnum, static_cast<CrInt8u>(timeoutSec));
    if (!pEnum) return result;
    CrInt32u n = pEnum->GetCount();
    for (CrInt32u i = 0; i < n; ++i) {
        const auto* info = pEnum->GetCameraObjectInfo(i);
        if (!info) continue;
        CameraInfo ci;
        ci.guid = GuidOrIdHex(info);
        if (auto* m  = info->GetModel(); m  && *m)  ci.model = m;
        else ci.model = L"Unknown";
        if (auto* nm = info->GetName();  nm && *nm) ci.name  = nm;
        else ci.name = ci.model;
        result.push_back(std::move(ci));
    }
    pEnum->Release();
    return result;
}

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
            if (CountCapture()) {
                m_owner->Logf(L"[CB] Captured 0x%08X count=%d", w, m_owner->m_capturedCount.load());
            } else {
                m_owner->Logf(L"[CB] Captured 0x%08X SUPPRESSED (count=%d >= target=%d)",
                              w, m_owner->m_capturedCount.load(), m_owner->m_capturedTarget);
            }
        }
    }
    void OnCompleteDownload(CrChar* fn, CrInt32u) override {
        if (CountCapture()) {
            m_owner->Logf(L"[CB] Download: %s count=%d",
                          fn ? fn : L"?", m_owner->m_capturedCount.load());
        } else {
            m_owner->Logf(L"[CB] Download: %s SUPPRESSED (count=%d >= target=%d)",
                          fn ? fn : L"?",
                          m_owner->m_capturedCount.load(), m_owner->m_capturedTarget);
        }
    }
    void OnNotifyPostViewImage(CrChar*, CrInt32u sz) override {
        if (CountCapture()) {
            m_owner->Logf(L"[CB] PostView %u B count=%d", sz, m_owner->m_capturedCount.load());
        } else {
            m_owner->Logf(L"[CB] PostView %u B SUPPRESSED (count=%d >= target=%d)",
                          sz, m_owner->m_capturedCount.load(), m_owner->m_capturedTarget);
        }
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

    // Atomically increments m_capturedCount only if it is still below m_capturedTarget.
    // Returns true when the increment happened; false when the event is a stray/late.
    bool CountCapture() {
        int cur = m_owner->m_capturedCount.load(std::memory_order_relaxed);
        while (cur < m_owner->m_capturedTarget) {
            if (m_owner->m_capturedCount.compare_exchange_weak(
                    cur, cur + 1, std::memory_order_release, std::memory_order_relaxed)) {
                m_owner->m_waitCv.notify_all();
                return true;
            }
        }
        return false;
    }
};

// ─── Lifecycle ────────────────────────────────────────────────────────────────

CameraController::CameraController(LogFn log) : m_log(std::move(log)) {}
CameraController::~CameraController() { Shutdown(); }

void CameraController::Log(const wchar_t* msg) {
    if (msg == nullptr) return;
    ::OutputDebugStringW(msg); ::OutputDebugStringW(L"\n");
    if (m_log) m_log(msg);
}
void CameraController::Logf(const wchar_t* fmt, ...) {
    if (fmt == nullptr) return;
    wchar_t buf[512]; va_list a; va_start(a, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, a); va_end(a);
    Log(buf);
}

bool CameraController::Init() {
    if (m_initialized) return true;
    assert(m_callback == nullptr);  // Init() must not be called twice without Shutdown()
    m_callback = new DeviceCallback(this);
    if (!InitSDK()) {
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
    if (m_initialized) {
        m_initialized = false;
        ReleaseSDK();
    }
    delete m_callback; m_callback = nullptr;
}

bool CameraController::Connect(const wchar_t* guid, int enumTimeoutSec, int connectTimeoutMs) {
    if (!m_initialized)       { Log(L"Connect: Init() not called");       return false; }
    if (m_callback == nullptr){ Log(L"Connect: callback is null");        return false; }
    if (enumTimeoutSec <= 0)  { Log(L"Connect: enumTimeout must be > 0");return false; }
    if (connectTimeoutMs <= 0){ Log(L"Connect: connTimeout must be > 0");return false; }
    if (m_connected) Disconnect();

    SDK::ICrEnumCameraObjectInfo* pEnum = nullptr;
    SDK::CrError err = SDK::EnumCameraObjects(&pEnum, static_cast<CrInt8u>(enumTimeoutSec));
    if (err != 0 || !pEnum || pEnum->GetCount() == 0) {
        if (pEnum) pEnum->Release();
        Log(L"Connect: no cameras found");
        return false;
    }

    // Find camera by GUID, or take the first one (guid==nullptr → compat mode)
    const SDK::ICrCameraObjectInfo* target = nullptr;
    CrInt32u camCount = pEnum->GetCount();
    if (guid && *guid) {
        for (CrInt32u i = 0; i < camCount; ++i) {
            const auto* ci = pEnum->GetCameraObjectInfo(i);
            if (ci && GuidOrIdHex(ci) == guid) {
                target = ci; break;
            }
        }
        if (!target) {
            pEnum->Release();
            Logf(L"Connect: camera GUID '%s' not found", guid);
            return false;
        }
    } else {
        target = pEnum->GetCameraObjectInfo(0);
    }

    m_model = (target->GetModel() && *target->GetModel()) ? target->GetModel() : L"Unknown";
    m_guid  = GuidOrIdHex(target);
    auto* info = const_cast<SDK::ICrCameraObjectInfo*>(target);

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

    lk.unlock();

    // Camera needs a moment after OnConnected before reporting all properties.
    // Wait up to 5 s until property count stabilises (≥50).
    for (int attempt = 0; attempt < 10; ++attempt) {
        ::Sleep(500);
        PopulateSupportedCodes();
        if (m_supportedCodes.size() >= 50) break;
        Logf(L"PopulateSupportedCodes attempt %d: %zu props", attempt + 1, m_supportedCodes.size());
    }
    Logf(L"Connected: %s  (%zu properties)", m_model.c_str(), m_supportedCodes.size());
    WarmCache();
    return true;
}

void CameraController::Disconnect() {
    if (!m_connected) return;
    if (m_deviceHandle == 0) {
        Log(L"Disconnect: connected flag set but no handle — inconsistent state");
        m_connected = false;
        return;
    }
    auto h = static_cast<SDK::CrDeviceHandle>(m_deviceHandle);
    const SDK::CrError e1 = SDK::Disconnect(h);
    if (e1 != 0) Logf(L"Disconnect WARN 0x%04X", static_cast<unsigned>(e1));
    const SDK::CrError e2 = SDK::ReleaseDevice(h);
    if (e2 != 0) Logf(L"ReleaseDevice WARN 0x%04X", static_cast<unsigned>(e2));
    m_deviceHandle = 0;
    m_connected    = false;
    m_supportedCodes.clear();
    m_propSetCache.clear();
}

// ─── Capability ───────────────────────────────────────────────────────────────

void CameraController::PopulateSupportedCodes() {
    if (!m_connected || m_deviceHandle == 0) return;
    auto h = static_cast<SDK::CrDeviceHandle>(m_deviceHandle);
    SDK::CrDeviceProperty* props = nullptr; CrInt32 num = 0;
    if (SDK::GetDeviceProperties(h, &props, &num) != 0 || !props) return;
    m_supportedCodes.clear();
    m_supportedCodes.reserve(static_cast<size_t>(num));
    for (CrInt32 i = 0; i < num; ++i)
        m_supportedCodes.insert(static_cast<uint32_t>(props[i].GetCode()));
    SDK::ReleaseDeviceProperties(h, props);
}

bool CameraController::SupportsProperty(uint32_t code) const {
    return m_supportedCodes.count(code) > 0;
}

void CameraController::WarmCache() {
    if (!m_connected || m_deviceHandle == 0) return;
    auto h = static_cast<SDK::CrDeviceHandle>(m_deviceHandle);
    SDK::CrDeviceProperty* props = nullptr; CrInt32 num = 0;
    if (SDK::GetDeviceProperties(h, &props, &num) != 0 || !props) return;
    m_propSetCache.clear();
    for (CrInt32 i = 0; i < num; ++i) {
        uint32_t  code = static_cast<uint32_t>(props[i].GetCode());
        long long val  = static_cast<long long>(props[i].GetCurrentValue());
        m_propSetCache[code] = val;
    }
    SDK::ReleaseDeviceProperties(h, props);
    Logf(L"WarmCache: %d properties → cache (first Set will be Skip if value matches)",
         num);
}

// ─── Generic set / get ────────────────────────────────────────────────────────

bool CameraController::SetPropRaw(unsigned code, unsigned type, long long value, const wchar_t* desc) {
    assert(code != 0);      // caller must supply a valid CrDevicePropertyCode
    assert(type != 0);      // caller must supply a valid CrDataType
    if (!m_connected)       { Log(L"SetPropRaw: not connected"); return false; }
    if (m_deviceHandle == 0){ Log(L"SetPropRaw: no device handle"); return false; }
    auto h = static_cast<SDK::CrDeviceHandle>(m_deviceHandle);
    SDK::CrDeviceProperty prop;
    prop.SetCode(static_cast<SDK::CrDevicePropertyCode>(code));
    prop.SetValueType(static_cast<SDK::CrDataType>(type));
    prop.SetCurrentValue(static_cast<CrInt64u>(value));
    SDK::CrError err = SDK::SetDeviceProperty(h, &prop);
    Logf(L"Set %-28s = 0x%llX  err=0x%04X",
         desc ? desc : L"?", (unsigned long long)value, (unsigned)err);
    return err == 0;
}

bool CameraController::SetPropCached(unsigned code, unsigned type, long long value,
                                      const wchar_t* desc) {
    uint32_t k = static_cast<uint32_t>(code);
    auto it = m_propSetCache.find(k);
    if (it != m_propSetCache.end() && it->second == value) {
        Logf(L"Skip %-26s = 0x%llX (cached)", desc ? desc : L"?", (unsigned long long)value);
        return true;
    }
    bool ok = SetPropRaw(code, type, value, desc);
    if (ok) m_propSetCache[k] = value;
    return ok;
}

bool CameraController::SetPropAndVerify(uint32_t code, uint32_t dataType, long long value,
                                         const wchar_t* desc, int maxWaitMs) {
    if (!m_connected) return false;
    auto it = m_propSetCache.find(code);
    if (it != m_propSetCache.end() && it->second == value) {
        Logf(L"Skip %-26s = 0x%llX (cached)", desc ? desc : L"?", (unsigned long long)value);
        return true;
    }
    SetPropRaw(code, dataType, value, desc);
    int steps = maxWaitMs / 200;
    if (steps < 1) steps = 1;
    for (int i = 0; i < steps; ++i) {
        ::Sleep(200);
        uint64_t cur = 0;
        if (GetPropRaw(code, cur) &&
            static_cast<long long>(static_cast<uint32_t>(cur)) == value) {
            if (i > 0) Logf(L"%-26s confirmed after %d × 200ms", desc ? desc : L"?", i + 1);
            m_propSetCache[code] = value;
            return true;
        }
        if (steps > 2 && i == steps / 2)
            SetPropRaw(code, dataType, value,
                       (std::wstring(desc ? desc : L"?") + L"(retry)").c_str());
    }
    Logf(L"%-26s verify timeout (%dms)", desc ? desc : L"?", maxWaitMs);
    return true;
}

bool CameraController::SetProp(uint32_t code, uint32_t dataType, long long value,
                                const wchar_t* desc) {
    if (!m_connected) return false;
    return SetPropRaw(code, dataType, value, desc ? desc : L"SetProp");
}

bool CameraController::GetPropRaw(uint32_t code, uint64_t& out) {
    if (!m_connected) return false;
    auto h    = static_cast<SDK::CrDeviceHandle>(m_deviceHandle);
    CrInt32u codeU = code;
    SDK::CrDeviceProperty* props = nullptr; CrInt32 num = 0;
    if (SDK::GetSelectDeviceProperties(h, 1, &codeU, &props, &num) != 0 || !props || num == 0) {
        if (props) SDK::ReleaseDeviceProperties(h, props);
        return false;
    }
    out = props[0].GetCurrentValue();
    SDK::ReleaseDeviceProperties(h, props);
    return true;
}

bool CameraController::SendCmd(int cmdId, int param) {
    if (!m_connected) return false;
    auto h   = static_cast<SDK::CrDeviceHandle>(m_deviceHandle);
    auto err = SDK::SendCommand(h,
                                static_cast<SDK::CrCommandId>(cmdId),
                                static_cast<SDK::CrCommandParam>(param));
    Logf(L"SendCmd id=%d param=%d  err=0x%04X", cmdId, param, (unsigned)err);
    return err == 0;
}

// ─── Nearest helpers ──────────────────────────────────────────────────────────

uint32_t CameraController::NearestFromList16(unsigned propCode, uint32_t target) {
    if (!m_connected || m_deviceHandle == 0) return target;
    auto h     = static_cast<SDK::CrDeviceHandle>(m_deviceHandle);
    CrInt32u codeU = propCode;
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
    if (!m_connected || m_deviceHandle == 0) return target;
    auto h     = static_cast<SDK::CrDeviceHandle>(m_deviceHandle);
    CrInt32u codeU = propCode;
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

// ─── ShutterSpeed ─────────────────────────────────────────────────────────────

std::wstring CameraController::DecodeShutterSpeed(uint64_t raw) {
    if (raw == 0 || raw == 0xFFFFFFFF) return L"bulb";
    uint32_t n = (raw >> 16) & 0xFFFF, d = raw & 0xFFFF;
    double sec = (d > 0) ? (double)n / d : (double)n;
    wchar_t b[32];
    if (d == 10)        swprintf_s(b, L"%.1fs", sec);   // long exposure
    else if (sec >= 1.) swprintf_s(b, L"%.0fs", sec);
    else                swprintf_s(b, L"1/%.0f", 1.0 / sec);
    return b;
}

uint32_t CameraController::ParseShutterSpeedToRaw(const wchar_t* value) {
    std::wstring v = value;
    if (v == L"bulb" || v == L"BULB") return 0;
    auto slash = v.find(L'/');
    if (slash != std::wstring::npos) {
        uint32_t n = static_cast<uint32_t>(std::stoi(v.substr(0, slash)));
        uint32_t d = static_cast<uint32_t>(std::stoi(v.substr(slash + 1)));
        return (n << 16) | d;
    }
    // "25s" or bare number — seconds → encode as tenths: 25s → (250<<16)|10
    if (!v.empty() && (v.back() == L's' || v.back() == L'S')) v.pop_back();
    float sec    = std::stof(v);
    auto  tenths = static_cast<uint32_t>(sec * 10.0f + 0.5f);
    return (tenths << 16) | 10u;
}

uint32_t CameraController::NearestShutterSpeed(uint32_t targetRaw) {
    if (!m_connected || m_deviceHandle == 0) return targetRaw;
    auto h     = static_cast<SDK::CrDeviceHandle>(m_deviceHandle);
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

// ─── Exposure setters ─────────────────────────────────────────────────────────

bool CameraController::SetPCRemotePriority() {
    return SetPropCached(SDK::CrDeviceProperty_PriorityKeySettings,
                         SDK::CrDataType_UInt16, SDK::CrPriorityKey_PCRemote, L"PriorityKey");
}

bool CameraController::SetExposureMode(const wchar_t* mode) {
    uint32_t val = SDK::CrExposure_M_Manual;
    if      (!wcscmp(mode, L"P")) val = SDK::CrExposure_P_Auto;
    else if (!wcscmp(mode, L"A")) val = SDK::CrExposure_A_AperturePriority;
    else if (!wcscmp(mode, L"S")) val = SDK::CrExposure_S_ShutterSpeedPriority;
    return SetPropCached(SDK::CrDeviceProperty_ExposureProgramMode,
                         SDK::CrDataType_UInt32, val, L"ExposureMode");
}

bool CameraController::SetFocusMode(const wchar_t* mode) {
    uint32_t val = SDK::CrFocus_MF;
    if      (!wcscmp(mode, L"AF-S")) val = SDK::CrFocus_AF_S;
    else if (!wcscmp(mode, L"AF-C")) val = SDK::CrFocus_AF_C;
    else if (!wcscmp(mode, L"AF-A")) val = SDK::CrFocus_AF_A;
    else if (!wcscmp(mode, L"DMF"))  val = SDK::CrFocus_DMF;
    return SetPropCached(SDK::CrDeviceProperty_FocusMode,
                         SDK::CrDataType_UInt16, val, L"FocusMode");
}

bool CameraController::SetShutterSpeed(const wchar_t* value) {
    uint32_t raw    = ParseShutterSpeedToRaw(value);
    uint32_t actual = NearestShutterSpeed(raw);
    Logf(L"ShutterSpeed: %s → %s", value, DecodeShutterSpeed(actual).c_str());
    auto ssCode = static_cast<uint32_t>(SDK::CrDeviceProperty_ShutterSpeed);
    return SetPropAndVerify(ssCode, SDK::CrDataType_UInt32, (long long)actual,
                            L"ShutterSpeed", 3000);
}

bool CameraController::SetISO(int iso) {
    uint32_t actual = NearestFromList32log(SDK::CrDeviceProperty_IsoSensitivity,
                                           static_cast<uint32_t>(iso));
    Logf(L"ISO: %d → %u", iso, actual);
    return SetPropCached(SDK::CrDeviceProperty_IsoSensitivity,
                         SDK::CrDataType_UInt32, actual, L"ISO");
}

bool CameraController::SetFNumber(float f) {
    uint32_t target = static_cast<uint32_t>(f * 100.0f + 0.5f);
    uint32_t actual = NearestFromList16(SDK::CrDeviceProperty_FNumber, target);
    Logf(L"FNumber: F/%.1f → F/%.2f", f, actual / 100.0);
    return SetPropCached(SDK::CrDeviceProperty_FNumber,
                         SDK::CrDataType_UInt16, actual, L"FNumber");
}

bool CameraController::SetStoreDestination(const wchar_t* dest) {
    uint32_t val = SDK::CrStillImageStoreDestination_MemoryCard;
    if      (!wcscmp(dest, L"pc"))   val = SDK::CrStillImageStoreDestination_HostPC;
    else if (!wcscmp(dest, L"both")) val = SDK::CrStillImageStoreDestination_HostPCAndMemoryCard;
    return SetPropCached(SDK::CrDeviceProperty_StillImageStoreDestination,
                         SDK::CrDataType_UInt16, val, L"StoreDestination");
}

// ─── Shoot ────────────────────────────────────────────────────────────────────

void CameraController::RequestShutdown() {
    m_shutdownReq = true;
    m_waitCv.notify_all();
}

bool CameraController::Shoot(int* latencyMs, int timeoutMs, int expectedCaptures, bool holdForBurst) {
    assert(timeoutMs > 0);          // caller contract — negative timeout makes no sense
    assert(expectedCaptures >= 1);  // caller contract — at least one capture expected
    if (!m_connected)        { Log(L"Shoot: not connected");              return false; }
    if (m_deviceHandle == 0) { Log(L"Shoot: no device handle");          return false; }
    if (timeoutMs <= 0)      { Log(L"Shoot: timeout must be > 0");       return false; }
    if (expectedCaptures < 1){ Log(L"Shoot: expectedCaptures must be >= 1"); return false; }
    auto h = static_cast<SDK::CrDeviceHandle>(m_deviceHandle);
    m_capturedCount  = 0;
    m_capturedTarget = expectedCaptures;

    auto t0 = std::chrono::steady_clock::now();
    SDK::SendCommand(h, SDK::CrCommandId_Release, SDK::CrCommandParam_Down);

    bool ok;
    if (holdForBurst) {
        // Keep button pressed — Cont_Bracket fires all N shots while held.
        // Release only after all captures arrive (or timeout/abort).
        std::unique_lock<std::mutex> lk(m_waitMutex);
        ok = m_waitCv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
            [this] { return m_capturedCount.load() >= m_capturedTarget
                         || m_shutdownReq.load(); });
        lk.unlock();
        SDK::SendCommand(h, SDK::CrCommandId_Release, SDK::CrCommandParam_Up);
    } else {
        // Quick click — single shot or Single_Bracket (one press per shot).
        ::Sleep(35);
        SDK::SendCommand(h, SDK::CrCommandId_Release, SDK::CrCommandParam_Up);
        std::unique_lock<std::mutex> lk(m_waitMutex);
        ok = m_waitCv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
            [this] { return m_capturedCount.load() >= m_capturedTarget
                         || m_shutdownReq.load(); });
    }

    int ms = static_cast<int>(
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count());
    if (latencyMs) *latencyMs = ms;
    Logf(L"Shoot: %s  captures=%d/%d  %d ms",
         ok ? L"OK" : L"TIMEOUT", m_capturedCount.load(), m_capturedTarget, ms);
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

    for (CrInt32 i = 0; i < num; ++i) {
        auto code = static_cast<uint32_t>(props[i].GetCode());
        auto cur  = props[i].GetCurrentValue();

        switch (code) {
        // ── Battery ──────────────────────────────────────────────────────────
        case SDK::CrDeviceProperty_BatteryRemain:
            s.batteryPct = static_cast<int>(cur); break;
        case SDK::CrDeviceProperty_BatteryLevel:
            switch (cur) {
            case SDK::CrBatteryLevel_PreEndBattery:          s.batteryLevel = L"pre-end"; break;
            case SDK::CrBatteryLevel_1_4:                    s.batteryLevel = L"1/4";     break;
            case SDK::CrBatteryLevel_2_4:                    s.batteryLevel = L"2/4";     break;
            case SDK::CrBatteryLevel_3_4:                    s.batteryLevel = L"3/4";     break;
            case SDK::CrBatteryLevel_4_4:                    s.batteryLevel = L"full";    break;
            case SDK::CrBatteryLevel_1_3:                    s.batteryLevel = L"1/3";     break;
            case SDK::CrBatteryLevel_2_3:                    s.batteryLevel = L"2/3";     break;
            case SDK::CrBatteryLevel_3_3:                    s.batteryLevel = L"full";    break;
            case SDK::CrBatteryLevel_USBPowerSupply:         s.batteryLevel = L"usb";       break;
            case SDK::CrBatteryLevel_PreEnd_PowerSupply:     s.batteryLevel = L"pre-end+usb"; break;
            case SDK::CrBatteryLevel_1_4_PowerSupply:        s.batteryLevel = L"1/4+usb";   break;
            case SDK::CrBatteryLevel_2_4_PowerSupply:        s.batteryLevel = L"2/4+usb";   break;
            case SDK::CrBatteryLevel_3_4_PowerSupply:        s.batteryLevel = L"3/4+usb";   break;
            case SDK::CrBatteryLevel_4_4_PowerSupply:        s.batteryLevel = L"full+usb";  break;
            case SDK::CrBatteryLevel_BatteryNotInstalled:    s.batteryLevel = L"none";      break;
            default:                                         s.batteryLevel = L"?";         break;
            }
            break;
        // ── Media ─────────────────────────────────────────────────────────────
        case SDK::CrDeviceProperty_MediaSLOT1_RemainingNumber:
            s.remainingShots = static_cast<int>(cur); break;
        case SDK::CrDeviceProperty_MediaSLOT2_RemainingNumber:
            s.slot2Remaining = static_cast<int>(cur); break;
        case SDK::CrDeviceProperty_MediaSLOT1_Status: {
            switch (cur) {
            case SDK::CrSlotStatus_OK:          s.slot1Status = L"ok";       break;
            case SDK::CrSlotStatus_NoCard:      s.slot1Status = L"no-card";  break;
            case SDK::CrSlotStatus_CardError:   s.slot1Status = L"error";    break;
            case SDK::CrSlotStatus_CardRecognizing: s.slot1Status = L"recognizing"; break;
            default:                            s.slot1Status = L"error";    break;
            }
            break;
        }
        case SDK::CrDeviceProperty_MediaSLOT2_Status: {
            switch (cur) {
            case SDK::CrSlotStatus_OK:          s.slot2Status = L"ok";       break;
            case SDK::CrSlotStatus_NoCard:      s.slot2Status = L"no-card";  break;
            case SDK::CrSlotStatus_CardError:   s.slot2Status = L"error";    break;
            case SDK::CrSlotStatus_CardRecognizing: s.slot2Status = L"recognizing"; break;
            default:                            s.slot2Status = L"error";    break;
            }
            break;
        }
        case SDK::CrDeviceProperty_MediaSLOT1_WritingState:
            s.slot1Writing = (cur == SDK::CrMediaSlotWritingState_ContentsWriting)
                             ? L"writing" : L"idle";
            break;
        case SDK::CrDeviceProperty_MediaSLOT2_WritingState:
            s.slot2Writing = (cur == SDK::CrMediaSlotWritingState_ContentsWriting)
                             ? L"writing" : L"idle";
            break;
        // ── Exposure ──────────────────────────────────────────────────────────
        case SDK::CrDeviceProperty_ShutterSpeed:
            s.shutterSpeed = DecodeShutterSpeed(cur); break;
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
            default: {
                wchar_t b[16]; swprintf_s(b, L"0x%X", (unsigned)cur);
                s.exposureMode = b; break;
            }
            }
            break;
        case SDK::CrDeviceProperty_ExposureBiasCompensation:
            s.exposureBias = static_cast<float>(static_cast<int16_t>(cur)) / 1000.f; break;
        // ── Focus ─────────────────────────────────────────────────────────────
        case SDK::CrDeviceProperty_FocusMode:
            switch (cur) {
            case SDK::CrFocus_MF:   s.focusMode = L"MF";   break;
            case SDK::CrFocus_AF_S: s.focusMode = L"AF-S"; break;
            case SDK::CrFocus_AF_C: s.focusMode = L"AF-C"; break;
            case SDK::CrFocus_AF_A: s.focusMode = L"AF-A"; break;
            case SDK::CrFocus_DMF:  s.focusMode = L"DMF";  break;
            default:                s.focusMode = L"?";    break;
            }
            break;
        case SDK::CrDeviceProperty_FocusArea:
            switch (cur) {
            case SDK::CrFocusArea_Wide:              s.focusArea = L"wide";     break;
            case SDK::CrFocusArea_Zone:              s.focusArea = L"zone";     break;
            case SDK::CrFocusArea_Center:            s.focusArea = L"center";   break;
            case SDK::CrFocusArea_Flexible_Spot_S:  s.focusArea = L"flex-s";   break;
            case SDK::CrFocusArea_Flexible_Spot_M:  s.focusArea = L"flex-m";   break;
            case SDK::CrFocusArea_Flexible_Spot_L:  s.focusArea = L"flex-l";   break;
            default: {
                wchar_t b[16]; swprintf_s(b, L"0x%X", (unsigned)cur);
                s.focusArea = b; break;
            }
            }
            break;
        case SDK::CrDeviceProperty_FocusIndication:
            switch (cur) {
            case SDK::CrFocusIndicator_Unlocked:        s.focusIndicator = L"unlocked";    break;
            case SDK::CrFocusIndicator_Focused_AF_S:    s.focusIndicator = L"focused";     break;
            case SDK::CrFocusIndicator_NotFocused_AF_S: s.focusIndicator = L"not-focused"; break;
            case SDK::CrFocusIndicator_Focused_AF_C:    s.focusIndicator = L"focused";     break;
            case SDK::CrFocusIndicator_NotFocused_AF_C: s.focusIndicator = L"not-focused"; break;
            default:                                     s.focusIndicator = L"?";           break;
            }
            break;
        // ── Drive ─────────────────────────────────────────────────────────────
        case SDK::CrDeviceProperty_DriveMode:
            switch (cur) {
            case SDK::CrDrive_Single:                        s.driveMode = L"single";        break;
            case SDK::CrDrive_Continuous_Hi:                 s.driveMode = L"cont-hi";       break;
            case SDK::CrDrive_Continuous_Hi_Plus:            s.driveMode = L"cont-hi-plus";  break;
            case SDK::CrDrive_Continuous_Hi_Live:            s.driveMode = L"cont-hi-live";  break;
            case SDK::CrDrive_Continuous_Lo:                 s.driveMode = L"cont-lo";       break;
            case SDK::CrDrive_Continuous:                    s.driveMode = L"cont";          break;
            case SDK::CrDrive_Continuous_SpeedPriority:      s.driveMode = L"cont-speed";    break;
            case SDK::CrDrive_Continuous_Mid:                s.driveMode = L"cont-mid";      break;
            case SDK::CrDrive_Continuous_Mid_Live:           s.driveMode = L"cont-mid-live"; break;
            case SDK::CrDrive_Continuous_Lo_Live:            s.driveMode = L"cont-lo-live";  break;
            case SDK::CrDrive_Single_Bracket_10Ev_5pics:     s.driveMode = L"bracket-1ev-5";  break;
            case SDK::CrDrive_Continuous_Bracket_10Ev_5pics: s.driveMode = L"bracket-1ev-5c"; break;
            default: {
                wchar_t b[16]; swprintf_s(b, L"0x%08X", (unsigned)cur);
                s.driveMode = b; break;
            }
            }
            break;
        // ── WB / Image ────────────────────────────────────────────────────────
        case SDK::CrDeviceProperty_WhiteBalance:
            switch (cur) {
            case SDK::CrWhiteBalance_AWB:        s.whiteBalance = L"AWB";        break;
            case SDK::CrWhiteBalance_Daylight:   s.whiteBalance = L"daylight";   break;
            case SDK::CrWhiteBalance_Shadow:     s.whiteBalance = L"shadow";     break;
            case SDK::CrWhiteBalance_Cloudy:     s.whiteBalance = L"cloudy";     break;
            case SDK::CrWhiteBalance_Tungsten:   s.whiteBalance = L"tungsten";   break;
            case SDK::CrWhiteBalance_Flush:      s.whiteBalance = L"flash";      break;
            case SDK::CrWhiteBalance_ColorTemp:  s.whiteBalance = L"color-temp"; break;
            case SDK::CrWhiteBalance_Custom_1:   s.whiteBalance = L"custom-1";   break;
            case SDK::CrWhiteBalance_Custom_2:   s.whiteBalance = L"custom-2";   break;
            case SDK::CrWhiteBalance_Custom_3:   s.whiteBalance = L"custom-3";   break;
            default: {
                wchar_t b[16]; swprintf_s(b, L"0x%X", (unsigned)cur);
                s.whiteBalance = b; break;
            }
            }
            break;
        case SDK::CrDeviceProperty_Colortemp:
            s.colorTemp = static_cast<int>(cur); break;
        case SDK::CrDeviceProperty_ImageSize:
            switch (cur) {
            case SDK::CrImageSize_L:   s.imageSize = L"L"; break;
            case SDK::CrImageSize_M:   s.imageSize = L"M"; break;
            case SDK::CrImageSize_S:   s.imageSize = L"S"; break;
            default:                   s.imageSize = L"?"; break;
            }
            break;
        case SDK::CrDeviceProperty_FileType:
            switch (cur) {
            case SDK::CrFileType_Jpeg:    s.fileType = L"JPEG";      break;
            case SDK::CrFileType_Raw:     s.fileType = L"RAW";       break;
            case SDK::CrFileType_RawJpeg: s.fileType = L"RAW+JPEG";  break;
            case SDK::CrFileType_RawHeif: s.fileType = L"RAW+HEIF";  break;
            case SDK::CrFileType_Heif:    s.fileType = L"HEIF";      break;
            default:                      s.fileType = L"?";          break;
            }
            break;
        case SDK::CrDeviceProperty_MeteringMode:
            switch (cur) {
            case SDK::CrMetering_Multi:               s.metering = L"multi";   break;
            case SDK::CrMetering_CenterWeighted:      s.metering = L"center";  break;
            case SDK::CrMetering_Spot_Standard:       s.metering = L"spot";    break;
            case SDK::CrMetering_HighLightWeighted:   s.metering = L"hl";      break;
            default: {
                wchar_t b[16]; swprintf_s(b, L"0x%X", (unsigned)cur);
                s.metering = b; break;
            }
            }
            break;
        // ── Output ────────────────────────────────────────────────────────────
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

    // ── Host clock snapshot (always available) ────────────────────────────────
    auto toUnixMs = [](const FILETIME& ft) -> int64_t {
        return (static_cast<int64_t>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime)
               / 10000LL - 11644473600000LL;
    };
    FILETIME ftBefore, ftAfter;
    GetSystemTimePreciseAsFileTime(&ftBefore);
    s.camTimeHostMs = toUnixMs(ftBefore);

    // ── Camera datetime (GetTimeZoneSetting — USB: not supported on ILCE-7RM4A) ─
    SDK::CrTimeZoneSetting tz{};
    const SDK::CrError tzErr = SDK::GetTimeZoneSetting(h, tz);
    GetSystemTimePreciseAsFileTime(&ftAfter);
    if (tzErr == 0) {
        // Refine host timestamp to midpoint of SDK round-trip
        s.camTimeHostMs = (toUnixMs(ftBefore) + toUnixMs(ftAfter)) / 2;
        if (tz.dateTimeSetting.exists == SDK::CrTimeZoneSettingExists_True) {
            const char* dt = tz.dateTimeSetting.dateTime;
            s.camTime.assign(dt, dt + strnlen(dt, 17));
        }
        if (tz.areaSetting.exists == SDK::CrTimeZoneSettingExists_True) {
            const char* area = tz.areaSetting.area;
            s.camTimeArea.assign(area, area + strnlen(area, 5));
        }
    }

    return s;
}

} // namespace TotalControl
