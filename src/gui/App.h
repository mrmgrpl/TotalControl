#pragma once
#include "PipeClient.h"
#include <string>
#include <atomic>
#include <chrono>

// ImGui forward declarations (avoid including imgui.h in the header)
struct ImFont;

namespace TotalControl {

// Camera status snapshot — refreshed every ~500 ms via "status" pipe command
struct CameraStatus {
    bool valid       = false;
    std::string guid;
    std::string battery;
    std::string mode;       // exposure mode
    std::string ss;         // shutter speed
    std::string iso;
    std::string fnumber;
    std::string focus;
    std::string drive;
    std::string card1;
    std::string card2;
    std::string store;
    std::string wb;
    std::string cam_time;
    std::string error;      // non-empty = SDK error
};

// Sequencer status snapshot
struct SeqStatus {
    bool active     = false;
    int  stepsDone  = 0;
    int  stepsTotal = 0;
    std::string nextLabel;
    int64_t     nextAtMs   = 0;  // UTC ms of next step
    std::string state;            // "idle" / "running" / "done"
};

class App {
public:
    App();
    ~App();

    // Called once after D3D11 device + ImGui are initialised
    void OnInit();

    // Called every frame — renders the full UI
    void OnFrame();

    // Called when the window is about to close — returns false to cancel
    bool OnCloseRequest();

private:
    void DrawStatusBar();
    void DrawConnectionPanel();
    void DrawCameraPanel();
    void DrawSequencerPanel();

    void PollStatus();          // fire "status" + "seq_status" on background thread
    void StartPollThread();
    void StopPollThread();

    bool TryLaunchDaemon();     // CreateProcess TotalControlSRV.exe

    PipeClient m_pipe;

    // Status
    CameraStatus m_camStatus;
    SeqStatus    m_seqStatus;
    std::mutex   m_statusMutex;
    std::string  m_lastError;

    // Poll thread
    std::thread  m_pollThread;
    std::atomic<bool> m_pollRun{false};

    // Connection management
    int  m_reconnectCountdown = 0;  // frames until next Connect() attempt
    bool m_srvLaunched = false;

    // Fonts (set during OnInit from ImGui context)
    ImFont* m_fontMono = nullptr;
    ImFont* m_fontLarge = nullptr;
};

} // namespace TotalControl
