#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

// Celowo BEZ include CrSDK — zero zależności w nagłówku
namespace SCRSDK {
    class IDeviceCallback;
}

namespace TotalControl {

    struct CameraInfo {
        std::wstring model;
        std::wstring connType;
        bool         connected = false;
    };

    using CameraEventCallback = std::function<void(const std::wstring& event)>;

    class CameraController {
    public:
        CameraController();
        ~CameraController();

        bool Init();
        void Shutdown();

        int  EnumerateCameras();
        const std::vector<CameraInfo>& GetCameraList() const { return m_cameras; }

        bool Connect(int index = 0);
        void Disconnect();

        bool              IsConnected()        const { return m_connected; }
        const CameraInfo& GetConnectedCamera() const { return m_connectedCamera; }
        double            GetLastLatencyMs()   const { return m_lastLatencyMs; }

        void SetEventCallback(CameraEventCallback cb) { m_eventCallback = std::move(cb); }

    private:
        class DeviceCallback;

        bool                    m_initialized = false;
        bool                    m_connected = false;
        uint64_t                m_deviceHandle = 0;   // przechowuje CrDeviceHandle
        DeviceCallback* m_callback = nullptr;
        std::vector<CameraInfo> m_cameras;
        CameraInfo              m_connectedCamera;
        CameraEventCallback     m_eventCallback;
        double                  m_lastLatencyMs = 0.0;
    };

} // namespace TotalControl