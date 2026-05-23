#include "CameraController.h"

// CrSDK — wszystkie include TYLKO tutaj
#include "CameraRemote_SDK.h"       // SCRSDK::Init/Release/EnumCameraObjects/Connect
#include "IDeviceCallback.h"        // SCRSDK::IDeviceCallback
#include "ICrCameraObjectInfo.h"    // SCRSDK::ICrEnumCameraObjectInfo

#include <chrono>
#include <sstream>

// Wygodny alias — tylko w tym pliku .cpp
namespace SDK = SCRSDK;

namespace TotalControl {

    // ─────────────────────────────────────────────────────────────────────────────
    // DeviceCallback
    // Sygnatury MUSZĄ być identyczne z IDeviceCallback.h — łącznie z "Versioin"!
    // ─────────────────────────────────────────────────────────────────────────────
    class CameraController::DeviceCallback : public SDK::IDeviceCallback
    {
    public:
        explicit DeviceCallback(CameraController* owner) : m_owner(owner) {}

        void OnConnected(SDK::DeviceConnectionVersioin /*ver*/) override
        {
            Fire(L"Connected");
        }

        void OnDisconnected(CrInt32u error) override
        {
            m_owner->m_connected = false;
            std::wostringstream ss;
            ss << L"Disconnected:0x" << std::hex << error;
            Fire(ss.str());
        }

        void OnPropertyChanged() override { Fire(L"PropertyChanged"); }

        void OnPropertyChangedCodes(CrInt32u /*num*/,
            CrInt32u* /*codes*/) override {
        }

        void OnLvPropertyChanged() override {}

        void OnLvPropertyChangedCodes(CrInt32u /*num*/,
            CrInt32u* /*codes*/) override {
        }

        void OnCompleteDownload(CrChar* filename,
            CrInt32u /*type*/) override
        {
            std::wostringstream ss;
            ss << L"Download:" << (filename ? filename : L"?");
            Fire(ss.str());
        }

        // CrOperationResultData i CrContentHandle są w namespace SCRSDK
        void OnCompleteOperation(CrInt32u /*code*/,
            SDK::CrOperationResultData* /*result*/) override {
        }

        void OnNotifyContentsTransfer(CrInt32u /*notify*/,
            SDK::CrContentHandle /*handle*/,
            CrChar* /*filename*/) override {
        }

        void OnWarning(CrInt32u warning) override
        {
            std::wostringstream ss;
            ss << L"Warning:0x" << std::hex << warning;
            Fire(ss.str());
        }

        void OnWarningExt(CrInt32u /*w*/, CrInt32 /*p1*/,
            CrInt32 /*p2*/, CrInt32 /*p3*/) override {
        }

        void OnError(CrInt32u error) override
        {
            std::wostringstream ss;
            ss << L"Error:0x" << std::hex << error;
            Fire(ss.str());
        }

        void OnNotifyFTPTransferResult(CrInt32u /*n*/,
            CrInt32u /*ok*/,
            CrInt32u /*fail*/) override {
        }

        void OnNotifyRemoteTransferResult(CrInt32u /*n*/, CrInt32u /*per*/,
            CrChar* /*fn*/) override {
        }

        void OnNotifyRemoteTransferResult(CrInt32u /*n*/, CrInt32u /*per*/,
            CrInt8u* /*data*/,
            CrInt64u /*sz*/) override {
        }

        void OnNotifyRemoteTransferContentsListChanged(CrInt32u /*n*/,
            CrInt32u /*slot*/,
            CrInt32u /*add*/) override {
        }

        void OnNotifyRemoteFirmwareUpdateResult(CrInt32u /*n*/,
            const void* /*p*/) override {
        }

        void OnReceivePlaybackTimeCode(CrInt32u /*tc*/) override {}

        void OnReceivePlaybackData(CrInt8u /*mt*/, CrInt32 /*ds*/,
            CrInt8u* /*d*/, CrInt64 /*pts*/,
            CrInt64 /*dts*/, CrInt32 /*p1*/,
            CrInt32 /*p2*/) override {
        }

        void OnNotifyMonitorUpdated(CrInt32u /*type*/,
            CrInt32u /*frameNo*/) override {
        }

        void OnNotifyPostViewImage(CrChar* /*fn*/,
            CrInt32u /*sz*/) override {
        }

    private:
        void Fire(const std::wstring& ev)
        {
            ::OutputDebugStringW((L"[CrSDK][CB] " + ev + L"\n").c_str());
            if (m_owner->m_eventCallback)
                m_owner->m_eventCallback(ev);
        }

        CameraController* m_owner;
    };

    // ─────────────────────────────────────────────────────────────────────────────
    // CameraController
    // ─────────────────────────────────────────────────────────────────────────────

    CameraController::CameraController() = default;
    CameraController::~CameraController() { Shutdown(); }

    bool CameraController::Init()
    {
        if (m_initialized) return true;

        // Init() zwraca bool (nie CrError!)
        if (!SDK::Init(0)) {
            ::OutputDebugStringW(L"[CrSDK] Init() FAILED\n");
            return false;
        }

        m_callback = new DeviceCallback(this);
        m_initialized = true;
        ::OutputDebugStringW(L"[CrSDK] Init() OK\n");
        return true;
    }

    void CameraController::Shutdown()
    {
        Disconnect();
        if (m_initialized) {
            SDK::Release();
            m_initialized = false;
            ::OutputDebugStringW(L"[CrSDK] Release() OK\n");
        }
        delete m_callback;
        m_callback = nullptr;
    }

    int CameraController::EnumerateCameras()
    {
        m_cameras.clear();

        if (!m_initialized) {
            ::OutputDebugStringW(L"[CrSDK] Enumerate: SDK nie zainicjowany!\n");
            return 0;
        }

        SDK::ICrEnumCameraObjectInfo* pEnum = nullptr;
        // CrError jest w namespace SCRSDK
        SDK::CrError err = SDK::EnumCameraObjects(&pEnum);

        if (err != 0 || !pEnum) {
            std::wostringstream ss;
            ss << L"[CrSDK] EnumCameraObjects FAILED err=0x" << std::hex << err << L"\n";
            ::OutputDebugStringW(ss.str().c_str());
            return 0;
        }

        const CrInt32u count = pEnum->GetCount();

        {
            std::wostringstream ss;
            ss << L"[CrSDK] Znaleziono kamer: " << count << L"\n";
            ::OutputDebugStringW(ss.str().c_str());
        }

        for (CrInt32u i = 0; i < count; ++i) {
            const SDK::ICrCameraObjectInfo* info = pEnum->GetCameraObjectInfo(i);
            if (!info) continue;

            CameraInfo ci;
            ci.model = info->GetModel() ? info->GetModel() : L"Unknown";
            ci.connType = info->GetConnectionTypeName() ? info->GetConnectionTypeName() : L"Unknown";
            m_cameras.push_back(ci);

            std::wostringstream ss;
            ss << L"[CrSDK]   [" << i << L"] " << ci.model
                << L"  conn=" << ci.connType << L"\n";
            ::OutputDebugStringW(ss.str().c_str());
        }

        pEnum->Release();
        return static_cast<int>(count);
    }

    bool CameraController::Connect(int index)
    {
        if (!m_initialized) return false;
        if (m_connected)    Disconnect();

        if (index < 0 || index >= static_cast<int>(m_cameras.size())) {
            ::OutputDebugStringW(L"[CrSDK] Connect: nieprawidłowy indeks\n");
            return false;
        }

        SDK::ICrEnumCameraObjectInfo* pEnum = nullptr;
        SDK::CrError err = SDK::EnumCameraObjects(&pEnum);
        if (err != 0 || !pEnum) return false;

        if (static_cast<CrInt32u>(index) >= pEnum->GetCount()) {
            pEnum->Release();
            return false;
        }

        SDK::ICrCameraObjectInfo* info =
            const_cast<SDK::ICrCameraObjectInfo*>(
                pEnum->GetCameraObjectInfo(static_cast<CrInt32u>(index)));

        SDK::CrDeviceHandle handle = 0;

        auto t0 = std::chrono::steady_clock::now();
        err = SDK::Connect(info, m_callback, &handle);
        auto t1 = std::chrono::steady_clock::now();

        m_lastLatencyMs =
            std::chrono::duration<double, std::milli>(t1 - t0).count();

        pEnum->Release();

        if (err != 0 || handle == 0) {
            std::wostringstream ss;
            ss << L"[CrSDK] Connect FAILED err=0x" << std::hex << err << L"\n";
            ::OutputDebugStringW(ss.str().c_str());
            return false;
        }

        // Przechowujemy jako uint64_t (m_deviceHandle w nagłówku)
        m_deviceHandle = static_cast<uint64_t>(handle);
        m_connected = true;
        m_connectedCamera = m_cameras[index];
        m_connectedCamera.connected = true;

        std::wostringstream ss;
        ss << L"[CrSDK] Połączono: " << m_connectedCamera.model
            << L"  latencja=" << m_lastLatencyMs << L"ms\n";
        ::OutputDebugStringW(ss.str().c_str());
        return true;
    }

    void CameraController::Disconnect()
    {
        if (!m_connected || m_deviceHandle == 0) return;

        auto handle = static_cast<SDK::CrDeviceHandle>(m_deviceHandle);
        SDK::Disconnect(handle);
        SDK::ReleaseDevice(handle);

        m_deviceHandle = 0;
        m_connected = false;
        ::OutputDebugStringW(L"[CrSDK] Rozłączono\n");
    }

} // namespace TotalControl