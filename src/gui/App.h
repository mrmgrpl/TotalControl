#pragma once
#include "PipeClient.h"
#include <string>
#include <atomic>
#include <fstream>
#include <mutex>
#include <thread>

struct ImFont;

namespace TotalControl {

class App {
public:
    App();
    ~App();
    void OnInit();
    void OnFrame();
    bool OnCloseRequest();

private:
    void TryAutoConnect();
    bool TryLaunchDaemon();
    void LogLine(const char* msg);

    PipeClient m_pipe;

    std::thread       m_connectThread;
    std::atomic<bool> m_connectRun{false};

    int         m_reconnectCountdown = 0;
    std::string m_lastResult;

    bool m_showStyleEditor = false;
    bool m_showDemoWindow  = false;

    ImFont* m_fontMono  = nullptr;
    ImFont* m_fontLarge = nullptr;

    std::ofstream m_logFile;
    std::mutex    m_logMutex;
};

} // namespace TotalControl
