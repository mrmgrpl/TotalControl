#include "PipeClient.h"
#include <cstring>

namespace TotalControl {

static constexpr DWORD kReadBuf = 4096;

PipeClient::PipeClient() = default;

PipeClient::~PipeClient() {
    Disconnect();
}

bool PipeClient::Connect() {
    std::lock_guard<std::mutex> lk(m_pipeMutex);
    if (m_pipe != INVALID_HANDLE_VALUE) return true;

    HANDLE h = CreateFileW(
        L"\\\\.\\pipe\\TotalControl",
        GENERIC_READ | GENERIC_WRITE,
        0, nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_PIPE_BUSY) {
            // Pipe exists but all instances busy — try WaitNamedPipe
            WaitNamedPipeW(L"\\\\.\\pipe\\TotalControl", 200);
        }
        return false;
    }

    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(h, &mode, nullptr, nullptr);

    m_pipe = h;
    m_state.store(State::Connected);
    return true;
}

void PipeClient::Disconnect() {
    std::lock_guard<std::mutex> lk(m_pipeMutex);
    if (m_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }
    m_state.store(State::Disconnected);
}

bool PipeClient::SendRequest(const std::string& request, std::string& response) {
    std::lock_guard<std::mutex> lk(m_pipeMutex);
    if (m_pipe == INVALID_HANDLE_VALUE) return false;

    // Write: request + newline
    std::string line = request;
    if (line.empty() || line.back() != '\n') line += '\n';

    DWORD written = 0;
    if (!WriteFile(m_pipe, line.data(), static_cast<DWORD>(line.size()), &written, nullptr)) {
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
        m_state.store(State::Disconnected);
        return false;
    }

    // Read until newline
    response.clear();
    char buf[kReadBuf];
    while (true) {
        DWORD read = 0;
        BOOL ok = ReadFile(m_pipe, buf, sizeof(buf), &read, nullptr);
        if (!ok || read == 0) {
            CloseHandle(m_pipe);
            m_pipe = INVALID_HANDLE_VALUE;
            m_state.store(State::Disconnected);
            return false;
        }
        response.append(buf, read);
        if (response.find('\n') != std::string::npos) break;
        if (response.size() > 1024 * 1024) break; // safety
    }

    // Strip trailing newline
    while (!response.empty() && (response.back() == '\n' || response.back() == '\r'))
        response.pop_back();

    return true;
}

bool PipeClient::Send(const std::string& request) {
    std::string resp;
    return SendRequest(request, resp);
}

} // namespace TotalControl
