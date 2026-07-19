#pragma once
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace SCRSDK { class IDeviceCallback; }

namespace TotalControl {

struct CameraInfo {
    std::wstring guid;   // GetGuid() — UUID unikalny per jednostka
    std::wstring model;  // GetModel()
    std::wstring name;   // GetName()
};

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
    std::wstring slot1Writing;              // "idle","writing"
    std::wstring slot2Writing;
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
    // Camera clock vs host clock (for post-processing drift correction)
    std::wstring camTime;                   // camera datetime e.g. "20260812T202900.0" (local)
    std::wstring camTimeArea;               // timezone offset e.g. "+0200"
    int64_t      camTimeHostMs = 0;         // host Unix ms (avg of before/after SDK call)
};

using LogFn = std::function<void(const wchar_t*)>;

class CameraController {
public:
    explicit CameraController(LogFn log = nullptr);
    ~CameraController();

    // ── SDK lifecycle (static) ───────────────────────────────────────────────
    // InitSDK/ReleaseSDK używają refcount — bezpieczne przy wielu kontrolerach.
    static bool InitSDK();
    static void ReleaseSDK();

    // Enumerate wszystkich podłączonych kamer (wymaga wcześniejszego InitSDK).
    static std::vector<CameraInfo> Enumerate(int timeoutSec = 5);

    // ── Instance lifecycle ───────────────────────────────────────────────────
    bool Init();     // wywołuje InitSDK + tworzy callback
    void Shutdown(); // Disconnect + wywołuje ReleaseSDK
    // guid=nullptr → łączy z pierwszą kamerą (compat jednej kamery)
    bool Connect(const wchar_t* guid = nullptr,
                 int enumTimeoutSec = 5, int connectTimeoutMs = 8000);
    void Disconnect();
    bool IsConnected() const { return m_connected; }
    const std::wstring& Model() const { return m_model; }
    const std::wstring& Guid()  const { return m_guid;  }

    // Abort any ongoing Shoot() wait immediately (call before Shutdown on signal).
    void RequestShutdown();

    // ── Capability ──────────────────────────────────────────────────────────
    bool SupportsProperty(uint32_t code) const;

    // ── Generic access ──────────────────────────────────────────────────────
    bool SetProp(uint32_t code, uint32_t dataType, long long value,
                 const wchar_t* desc = nullptr);
    // Set + poll until camera confirms value (or maxWaitMs).
    // Skips both set and verify when already cached from a prior successful call.
    bool SetPropAndVerify(uint32_t code, uint32_t dataType, long long value,
                          const wchar_t* desc, int maxWaitMs = 2000);
    bool GetPropRaw(uint32_t code, uint64_t& outValue);
    // Diagnostic: snapshot every property code+value the camera currently reports.
    // Used to empirically identify undocumented property codes (see CommandHandler
    // "dump_props") — not used on any hot path.
    bool DumpAllProps(std::vector<std::pair<uint32_t, uint64_t>>& out);

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
    bool IsPropCached(uint32_t code, long long value) const; // true → no SDK call needed

    // ── Live View ────────────────────────────────────────────────────────────
    // StartLiveView enables the camera's LV stream and creates named shared memory
    // "TotalControl_LV_<camIdx>" (2 MB + 8 B header) that the GUI reads.
    // StopLiveView disables LV and unmaps the shared memory.
    bool StartLiveView(int camIdx = 0);
    void StopLiveView();

    // ── Shoot ────────────────────────────────────────────────────────────────
    // holdForBurst=true: keeps Release button pressed until all captures arrive,
    // then releases (required for CrDrive_Cont_Bracket_* — camera fires N shots
    // only while button is held). For single-shot and Single_Bracket, leave false.
    bool Shoot(int* latencyMs = nullptr, int timeoutMs = 5000,
               int expectedCaptures = 1, bool holdForBurst = false);

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
    bool     SetPropCached(unsigned code, unsigned type, long long value, const wchar_t* desc);
    uint32_t NearestFromList16(unsigned propCode, uint32_t target);
    uint32_t NearestFromList32log(unsigned propCode, uint32_t target);
    uint32_t ParseShutterSpeedToRaw(const wchar_t* value);
    uint32_t NearestShutterSpeed(uint32_t targetRaw);
    void     PopulateSupportedCodes();
    void     WarmCache();

    bool                         m_initialized  = false;
    bool                         m_connected    = false;
    uint64_t                     m_deviceHandle = 0;
    std::wstring                 m_model;
    std::wstring                 m_guid;
    DeviceCallback*              m_callback     = nullptr;
    LogFn                        m_log;
    std::unordered_set<uint32_t> m_supportedCodes;
    std::unordered_map<uint32_t, long long> m_propSetCache;

    std::mutex              m_waitMutex;
    std::condition_variable m_waitCv;
    std::atomic<bool>       m_connectedSig  { false };
    std::atomic<bool>       m_shutdownReq   { false };
    std::atomic<int>        m_capturedCount { 0 };
    int                     m_capturedTarget { 1 };
    // Bumped by OnPropertyChanged/OnPropertyChangedCodes on every camera-reported
    // property change (incl. MediaSLOT_WritingState writing->idle). SetPropAndVerify
    // waits on this generation counter instead of polling on a fixed cadence.
    std::atomic<uint64_t>   m_propChangeGen { 0 };

    // Live view shared memory (SHM)
    HANDLE                  m_lvMapHandle   = nullptr;
    void*                   m_lvShmView     = nullptr;
    std::atomic<bool>       m_lvActive      { false };  // guards SHM access in callback
    std::vector<uint8_t>    m_lvBuf;                    // pre-allocated JPEG buffer (2 MB)
};

} // namespace TotalControl
