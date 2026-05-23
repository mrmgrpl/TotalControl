#pragma once
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_set>

namespace SCRSDK { class IDeviceCallback; }

namespace TotalControl {

struct CameraStatus {
    bool         connected      = false;
    std::wstring model;
    // Battery
    int          batteryPct     = 0;
    std::wstring batteryLevel;              // "pre-end","1/4","2/4","3/4","full","usb"
    // Media
    int          remainingShots = 0;        // slot 1
    int          slot2Remaining = 0;
    std::wstring slot1Status;               // "ok","no-card","full","error"
    std::wstring slot2Status;
    // Exposure
    std::wstring shutterSpeed;              // "1/100","25s","bulb"
    int          iso            = 0;
    float        fNumber        = 0;
    std::wstring exposureMode;              // "M","A","S","P"
    float        exposureBias   = 0.f;      // EV compensation
    // Focus
    std::wstring focusMode;                 // "MF","AF-S","AF-C","AF-A","DMF"
    std::wstring focusArea;                 // "wide","center","flex-s","flex-m","flex-l"
    std::wstring focusIndicator;            // "focused","not-focused","unlocked"
    // Drive
    std::wstring driveMode;                 // "single","cont-hi","bracket-1ev-5", ...
    // WB / Image
    std::wstring whiteBalance;              // "AWB","daylight","color-temp", ...
    int          colorTemp      = 0;        // K (when WB=color-temp)
    std::wstring imageSize;                 // "L","M","S"
    std::wstring fileType;                  // "JPEG","RAW","RAW+JPEG","HEIF","RAW+HEIF"
    std::wstring metering;                  // "multi","center","spot"
    // Output
    std::wstring storeDestination;          // "card","pc","both"
};

using LogFn = std::function<void(const wchar_t*)>;

class CameraController {
public:
    explicit CameraController(LogFn log = nullptr);
    ~CameraController();

    bool Init();
    void Shutdown();
    bool Connect(int enumTimeoutSec = 5, int connectTimeoutMs = 8000);
    void Disconnect();
    bool IsConnected() const { return m_connected; }
    const std::wstring& Model() const { return m_model; }

    // ── Capability ──────────────────────────────────────────────────────────
    bool SupportsProperty(uint32_t code) const;

    // ── Generic access ──────────────────────────────────────────────────────
    // Set any property by raw code + CrDataType value + numeric value
    bool SetProp(uint32_t code, uint32_t dataType, long long value,
                 const wchar_t* desc = nullptr);
    // Get current value of any property
    bool GetPropRaw(uint32_t code, uint64_t& outValue);

    // ── Generic command ──────────────────────────────────────────────────────
    // cmdId = CrCommandId_*, param = CrCommandParam_Down/Up or slot number
    bool SendCmd(int cmdId, int param = 0);

    // ── Exposure helpers (complex encoding) ─────────────────────────────────
    bool SetPCRemotePriority();
    bool SetExposureMode(const wchar_t* mode);   // "M","A","S","P"
    bool SetFocusMode(const wchar_t* mode);      // "MF","AF-S","AF-C","AF-A","DMF"
    bool SetShutterSpeed(const wchar_t* value);  // "1/100","25s","bulb"
    bool SetISO(int iso);
    bool SetFNumber(float f);
    bool SetStoreDestination(const wchar_t* dest); // "card","pc","both"

    // ── Shoot ────────────────────────────────────────────────────────────────
    // expectedCaptures: 1 for single, N for bracket (waits for N CrNotify_Captured_Event)
    bool Shoot(int* latencyMs = nullptr, int timeoutMs = 5000, int expectedCaptures = 1);

    // ── Status ───────────────────────────────────────────────────────────────
    CameraStatus GetStatus();

    // ── Decode helpers ───────────────────────────────────────────────────────
    static std::wstring DecodeShutterSpeed(uint64_t raw);

private:
    class DeviceCallback;
    friend class DeviceCallback;

    void Log(const wchar_t* msg);
    void Logf(const wchar_t* fmt, ...);

    bool     SetPropRaw(unsigned code, unsigned type, long long value, const wchar_t* desc);
    uint32_t NearestFromList16(unsigned propCode, uint32_t target);
    uint32_t NearestFromList32log(unsigned propCode, uint32_t target);
    uint32_t ParseShutterSpeedToRaw(const wchar_t* value);
    uint32_t NearestShutterSpeed(uint32_t targetRaw);
    void     PopulateSupportedCodes();

    bool                         m_initialized  = false;
    bool                         m_connected    = false;
    uint64_t                     m_deviceHandle = 0;
    std::wstring                 m_model;
    DeviceCallback*              m_callback     = nullptr;
    LogFn                        m_log;
    std::unordered_set<uint32_t> m_supportedCodes;

    std::mutex              m_waitMutex;
    std::condition_variable m_waitCv;
    std::atomic<bool>       m_connectedSig { false };
    std::atomic<int>        m_capturedCount { 0 };
    int                     m_capturedTarget { 1 };
};

} // namespace TotalControl
