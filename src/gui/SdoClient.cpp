#include "SdoClient.h"
#include <windows.h>
#include <winhttp.h>
#include <cassert>
#include <format>

namespace TotalControl {

std::vector<uint8_t> FetchHttpsBytes(
    const wchar_t* host,
    const wchar_t* path,
    int maxBytes,
    const std::function<void(std::string_view)>& logger)
{
    assert(host && host[0]);
    assert(path && path[0]);
    constexpr DWORD kTimeoutMs = 20000;

    HINTERNET hSes = WinHttpOpen(L"TotalControl/1.0",
                                  WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSes) {
        if (logger) logger(std::format("HTTP: WinHttpOpen failed ({})", GetLastError()));
        return {};
    }
    WinHttpSetTimeouts(hSes, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);

    HINTERNET hCon = WinHttpConnect(hSes, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hCon) {
        if (logger) logger(std::format("HTTP: WinHttpConnect failed ({})", GetLastError()));
        WinHttpCloseHandle(hSes);
        return {};
    }

    HINTERNET hReq = WinHttpOpenRequest(hCon, L"GET", path,
                                         nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE);
    if (!hReq) {
        if (logger) logger(std::format("HTTP: WinHttpOpenRequest failed ({})", GetLastError()));
        WinHttpCloseHandle(hCon);
        WinHttpCloseHandle(hSes);
        return {};
    }

    bool ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
           && WinHttpReceiveResponse(hReq, nullptr);
    if (!ok) {
        if (logger) logger(std::format("HTTP: request failed ({})", GetLastError()));
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hCon);
        WinHttpCloseHandle(hSes);
        return {};
    }

    DWORD status = 0, statusSz = sizeof(status);
    WinHttpQueryHeaders(hReq,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSz, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hCon);
        WinHttpCloseHandle(hSes);
        return {};
    }

    std::vector<uint8_t> body;
    body.reserve(64 * 1024);
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
        size_t off = body.size();
        body.resize(off + avail);
        DWORD got = 0;
        if (!WinHttpReadData(hReq, body.data() + off, avail, &got)) break;
        body.resize(off + got);
        if ((int)body.size() > maxBytes) break;
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hCon);
    WinHttpCloseHandle(hSes);
    return body;
}

} // namespace TotalControl
