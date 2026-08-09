#pragma once
#include <windows.h>
#include <array>
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
    std::wstring shutterType;               // "mechanical","electronic","auto","?" (unconfirmed on hardware — see CLAUDE.md)
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

    // Jak Enumerate(), ale NIE zwalnia surowego wyniku SDK — zwraca go jako
    // opaque handle (ICrEnumCameraObjectInfo* pod spodem, ale nagłówek tego
    // nie ujawnia — reguła izolacji SDK zostaje nietknięta), żeby wielokrotne
    // Connect(guid, handle, ...) mogło go współdzielić zamiast wielokrotnie
    // skanować USB od nowa (main.cpp: 1 enumeracja zamiast 1+N dla N kamer).
    // Wywołujący MUSI zwolnić handle przez ReleaseEnum() po zakończeniu pętli
    // connect (nawet jeśli część połączeń się nie powiodła).
    static void* EnumerateRaw(int timeoutSec, std::vector<CameraInfo>& out);
    static void  ReleaseEnum(void* handle);

    // ── Instance lifecycle ───────────────────────────────────────────────────
    bool Init();     // wywołuje InitSDK + tworzy callback
    void Shutdown(); // Disconnect + wywołuje ReleaseSDK
    // guid=nullptr → łączy z pierwszą kamerą (compat jednej kamery).
    // Enumeruje samodzielnie od zera — wygodne dla pojedynczego ad-hoc connect,
    // ale przy łączeniu wielu kamer pod rząd wolniejsze niż wariant poniżej.
    bool Connect(const wchar_t* guid = nullptr,
                 int enumTimeoutSec = 5, int connectTimeoutMs = 8000);
    // Jak wyżej, ale szuka guid w JUŻ posiadanym wyniku EnumerateRaw() zamiast
    // enumerować USB od nowa — wywołujący nadal jest właścicielem handle
    // (Connect go nie zwalnia; wywołaj ReleaseEnum() po całej pętli connect).
    bool Connect(const wchar_t* guid, void* preEnumeratedHandle, int connectTimeoutMs);
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
    // Skips both set and verify when already cached from a prior successful call
    // -- unless forceRecheck is true, in which case a cache hit still costs one
    // live GetPropRaw read to confirm the camera hasn't silently drifted off the
    // cached value before trusting it (see PriorityKey's caller: PC Remote
    // priority can silently lapse with no error surfaced anywhere else, letting
    // the physical dial move properties like ShutterSpeed out from under a
    // stale cache entry — confirmed on real hardware 2026-07-26, a "8s" shutter
    // speed that SetPropAndVerify had confirmed drifted to 15s on the camera a
    // few seconds later with zero commands sent in between). Does NOT re-issue
    // SetPropRaw when the live read still matches -- only a genuinely stale
    // cache falls through to the normal set+retry path below.
    bool SetPropAndVerify(uint32_t code, uint32_t dataType, long long value,
                          const wchar_t* desc, int maxWaitMs = 2000,
                          bool forceRecheck = false);
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
    // actualCaptures (optional): how many CrNotify_Captured_Event actually
    // arrived by the time Shoot() returns — the real count, not
    // expectedCaptures. Needed by callers using the duration-driven sentinel
    // pattern (expectedCaptures=9999, holdForBurst for a fixed timeoutMs)
    // where the real shot count isn't known in advance, e.g. card write-speed
    // calibration, which must know exactly how many shots it's timing.
    bool Shoot(int* latencyMs = nullptr, int timeoutMs = 5000,
               int expectedCaptures = 1, bool holdForBurst = false,
               int* actualCaptures = nullptr);

    // ── ShootUntilBufferSlowdown ─────────────────────────────────────────────
    // Dedicated to buffer-capacity calibration -- holds the shutter (same
    // Cont_Bracket Release-Down/Up pattern as Shoot(holdForBurst=true)) and
    // releases the INSTANT a live inflection is detected in the capture
    // cadence (one shot landing kSlowdownFactor x slower than the baseline
    // rate established from the first few shots) -- physically, that is the
    // exact moment the buffer just filled and occupancy first reached
    // capacity, so there is no benefit to firing further past it, only
    // wasted shutter actuations/wear. maxTimeoutMs is a ceiling, not a fixed
    // hold: if no slowdown is ever detected within it, that's a real, valid
    // result too (buffer capacity is at least actualCaptures shots -- some
    // camera/card combos never fill their buffer). A separate method rather
    // than a Shoot() parameter, to avoid touching Shoot()'s wait predicate
    // used by every other caller (shoot/bracket/burst/arm).
    bool ShootUntilBufferSlowdown(int* latencyMs, int maxTimeoutMs,
                                   int* actualCaptures);

    // ── Buffer capacity ───────────────────────────────────────────────────────
    struct BufferCapacityResult {
        int    totalShots         = 0;      // actual captures during the whole hold
        int    bufferCapacityShots = 0;     // shots fired before the fps slowdown (== totalShots if none observed)
        double fastFps             = 0.0;   // steady-state fps before any slowdown
        double slowFps             = 0.0;   // sustained fps after the slowdown (== fastFps if none observed)
        bool   slowdownObserved    = false;
    };
    // Analyzes the per-capture timestamps recorded during the most recent
    // Shoot() call (holdForBurst, sentinel target) to find where the shot
    // interval jumps from steady-state to a slower sustained rate — that's
    // the camera's buffer filling up and falling back to card-write-limited
    // speed. actualCaptures must be the same value Shoot() just reported via
    // its actualCaptures out-param. If no slowdown is seen within
    // actualCaptures shots, bufferCapacityShots==totalShots and
    // slowdownObserved==false — a real, useful result ("buffer capacity is
    // at least this many shots"), not a failure.
    BufferCapacityResult AnalyzeBufferCapacity(int actualCaptures) const;

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
    bool     IsPropSettable(unsigned propCode);
    uint32_t NearestFromList16(unsigned propCode, uint32_t target);
    uint32_t NearestFromList32log(unsigned propCode, uint32_t target);
    uint32_t ParseShutterSpeedToRaw(const wchar_t* value);
    uint32_t NearestShutterSpeed(uint32_t targetRaw);
    void     PopulateSupportedCodes();
    void     WarmCache();
    // Shared tail of both Connect() overloads: targetInfo is an
    // ICrCameraObjectInfo* already resolved by the caller (opaque here to
    // keep SDK types out of this header) — issues SDK::Connect, waits for
    // OnConnected, stabilises PopulateSupportedCodes, warms the prop cache.
    bool     ConnectToTarget(void* targetInfo, int connectTimeoutMs);

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
    // Per-capture timestamps (steady_clock ms) for buffer-capacity analysis
    // (see AnalyzeBufferCapacity) — pre-allocated, no heap allocation after
    // init (rule 3). Index i is written once, the instant capture i+1's
    // CrNotify_Captured_Event arrives; capacity bounds any realistic hold
    // (kMaxCaptureTimestamps at ~20fps sustained is well over a minute).
    static constexpr int    kMaxCaptureTimestamps = 2000;
    std::array<int64_t, kMaxCaptureTimestamps> m_captureTimestampsMs{};
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
