#pragma once
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace SCRSDK { class IDeviceCallback; }

namespace TotalControl {

struct CameraStatus {
    bool         connected      = false;
    std::wstring model;
    int          batteryPct     = 0;
    int          remainingShots = 0;
    std::wstring shutterSpeed;        // "1/100", "25s", "bulb"
    int          iso            = 0;
    float        fNumber        = 0;  // 4.0, 5.6, ...
    std::wstring exposureMode;        // "M","A","S","P"
    std::wstring focusMode;           // "MF","AF-S","AF-C"
    std::wstring storeDestination;    // "card","pc","both"
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

    // Parametry ekspozycji (wartości czytelne dla człowieka)
    bool SetPCRemotePriority();
    bool SetExposureMode(const wchar_t* mode);      // "M","A","S","P"
    bool SetFocusMode(const wchar_t* mode);         // "MF","AF-S","AF-C"
    bool SetShutterSpeed(const wchar_t* value);     // "1/100","25s","bulb"
    bool SetISO(int iso);
    bool SetFNumber(float f);
    bool SetStoreDestination(const wchar_t* dest);  // "card","pc","both"

    bool Shoot(int* latencyMs = nullptr, int timeoutMs = 5000);

    CameraStatus GetStatus();

private:
    class DeviceCallback;

    void Log(const wchar_t* msg);
    void Logf(const wchar_t* fmt, ...);

    bool     SetPropRaw(unsigned code, unsigned type, long long value, const wchar_t* desc);
    uint32_t NearestFromList16(unsigned propCode, uint32_t target);
    uint32_t NearestFromList32log(unsigned propCode, uint32_t target);
    uint32_t ParseShutterSpeedToRaw(const wchar_t* value);
    uint32_t NearestShutterSpeed(uint32_t targetRaw);

    bool                    m_initialized  = false;
    bool                    m_connected    = false;
    uint64_t                m_deviceHandle = 0;
    std::wstring            m_model;
    DeviceCallback*         m_callback     = nullptr;
    LogFn                   m_log;

    std::mutex              m_waitMutex;
    std::condition_variable m_waitCv;
    std::atomic<bool>       m_connectedSig { false };
    std::atomic<bool>       m_capturedSig  { false };
};

} // namespace TotalControl
